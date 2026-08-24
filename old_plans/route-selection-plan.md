# Unified Goal + Route Selection — Implementation Plan

Plan for making "if the chosen exit is blocked, try another one" work for every
modular airport goal type, by unifying the five near-duplicate goal selectors and
adding the missing route axis.

Status: **implemented, measured and enabled.** Stages 0-3 are built and tested; the search
runs with `MODULAR_MAX_ROUTE_ATTEMPTS = 3`. Companion to `skills/reservations-design.md`,
which remains the source of truth for reservation invariants.

> **§7 is the part worth reading.** This work first measured as an 18% *loss* and was
> written up as proof that diverting is inherently expensive. That conclusion was wrong: it
> was a single state-space bug in the pathfinder. The same policy, with the bug fixed, gains
> throughput and removes long-standing deadlocks. The history is kept because the wrong
> conclusion was reached twice, and both times the aggregate numbers supported it.

## 1. The problem

Two separate gaps that turn out to be one gap.

**The pathfinder is reservation-blind.** `FindAirportGroundPath`
(`airport_ground_pathfinder.cpp:453`) runs A* on pure topology — connection
directions, one-way flags, edge fences, buildings, type restrictions. Its only
dynamic input is stand reservation, and only for `IsParkingOnlyTile` tiles
(`airport_ground_pathfinder.cpp:307`). Transit apron and taxiway reservations are
invisible to it.

**Every consumer asks for exactly one path.** `TryReserveLandingChain` calls
`BuildTaxiPath` once (`modular_airport_cmd.cpp:1718`) and commits or fails
(`:1733`). So for a layout with two exits from a runway:

```
Runway -> one-way apron A -> all-direction apron -> stand
Runway -> one-way apron B -> one-way apron C -> all-direction apron -> stand
```

A* always returns the A route. If A is held by another aircraft, the commit fails
and the landing is refused — the B/C route is never considered, because nothing
ever asked about it.

The same hole exists on departure: `FindModularRunwayTileForTakeoff`
(`modular_airport_cmd.cpp:2764`) tries every runway *end* but only one *route* to
each.

### What already works, and must keep working

The reservation horizon terminates at the first future safe stop
(`modular_airport_cmd.cpp:1391`), and a one-way apron *is* a safe stop — the chain
is `IsTaxiwayPiece` (`modular_airport_cmd.h:256`, which covers exactly the
`APT_APRON*` pieces) into `IsModularSafeStopTile` (`modular_airport_cmd.cpp:1292`).

So for the A route the landing plan claims only `{runway tiles, A}`. **Congestion
past A already does not block a landing.** Any change here must preserve that: the
retry must fire only when the horizon itself is blocked.

### The trap: a global tile ban is the wrong shape

The obvious implementation — take the denial tile from the failed commit, ban it,
re-run A* — is subtly wrong. Consider:

```
shortest:  Runway -> A -> stand                    (A blocked)
alternate: Runway -> B -> C(one-way) -> A -> stand
```

The alternate route's horizon is `{runway, B, C}` — it stops at C, and A is not in
it. That route is reservable right now. But banning A globally kills the alternate
too, because it passes through A four tiles later, where nobody cares.

The ban is a fine way to *generate* a candidate route. It is wrong as a constraint
on the whole path, because the thing being validated is only the horizon prefix.

## 2. Design core

### Acceptance test

A candidate `(goal_tile, route)` is acceptable when **both** hold:

1. **The horizon prefix is reservable now** — `ValidateForwardReservationPlan`
   against live reservation state.
2. **The goal is reachable from the horizon terminus eventually** — topology only,
   reservations ignored.

Two questions against two different notions of "blocked". Collapsing them into one
is what produces the trap above.

This is not a new invention. It is the existing no-ground-goal branch of
`TryReserveLandingChain` (`modular_airport_cmd.cpp:1704-1729`), which already
reserves to a one-way buffer and admits a landing without a full route. The plan
promotes that special case to *the* acceptance rule.

### Search space

Every goal type resolves to a set of candidate tiles, and every candidate has a set
of possible routes. `MGT_*` (`modular_airport_cmd.h:27-33`) already names the axis:
`MGT_TERMINAL`, `MGT_HELIPAD`, `MGT_HANGAR`, `MGT_RUNWAY_TAKEOFF`, `MGT_ROLLOUT`,
`MGT_HELI_TAKEOFF_TILE`.

The search is `(goal instance) x (route)`, with acceptance as above. Arrival and
departure are the same search run in different directions.

### The duplication being removed

Five near-identical "enumerate instances, A* each, filter, score, pick best" loops,
none of which searches the route axis:

| Function | Enumerates |
|---|---|
| `FindFreeModularTerminal` (`modular_airport_cmd.cpp:2421`) | stands |
| `FindFreeModularHelipad` | helipads |
| `FindFreeModularHangar` | hangars |
| `FindModularLandingTarget` (`:1749`) | runway ends + helipads |
| `FindModularRunwayTileForTakeoff` (`:2764`) | takeoff-legal runway ends |

### Where the symmetry genuinely breaks

Four asymmetries the unification must respect rather than flatten:

1. **Deliberate waiting is a tier, not a failure.** `FindModularRunwayTileForTakeoff`
   resolves in strict priority — non-runway-crossing route, then any reservable
   route, then `best_blocked_tile`, a reachable-but-currently-blocked end returned
   *on purpose* so the aircraft waits (`modular_airport_cmd.cpp:2918-2921`). That is
   the strict large-runway preference: a large aircraft waits for a good runway
   rather than downgrading to a short one. A naive "first candidate that validates
   wins" destroys it silently.
2. **Set-level filters, not per-candidate predicates.** `good_takeoff_runway_exists`
   (`:2823`) is computed across the whole candidate set before ranking. The
   interface must be "filter the set, then rank", not `bool IsAcceptable(candidate)`.
3. **The route graph is directed.** One-way aprons mean stand→runway is not the
   reverse of runway→stand. The search always runs in the direction of travel; a
   route can never be computed once and flipped.
4. **Landing has two free endpoints, takeoff has one.** Landing picks entry runway
   *and* parking goal *and* the route between, with entry candidates gated by
   approach geometry. Takeoff's origin is wherever the aircraft is parked. Takeoff
   is a strict sub-problem of landing, not a mirror of it.

Denial cost differing between the two — a circling arrival burns a lap, a parked
departure does not — is real, but it is a policy parameter (retry budget), not a
structural difference.

## 3. Stages

Each stage is independently committable and independently measurable.

### Stage 0 — Extract the validator

Split `TryCommitForwardReservationPlan` (`modular_airport_cmd.cpp:3119`) into:

- `ValidateForwardReservationPlan(v, st, plan, TaxiReserveResult *out)` — all the
  checks, no mutation, reports the exact denial reason and tile.
- `TryCommitForwardReservationPlan` — validate, then apply.

Then replace the `path_enterable` lambda in `FindModularRunwayTileForTakeoff`
(`:2779`) with a call to it, deleting the duplicate. Its guessed `pe_reason` debug
string goes away in favour of the real reason and tile.

**Why first:** `path_enterable` is a second, hand-rolled implementation of the same
validation. Pitfall 2 in `reservations-design.md` warns exactly about this — *"Two
functions that both answer 'what must I own before advancing'. They will disagree,
and the log will confidently print the wrong one's answer."*

**This refactor is a detector.** It is intended to be movement-neutral. If the
regression floors move at all, the two implementations disagreed in live play, and
that disagreement must be understood before proceeding — not absorbed by bumping a
floor.

Expected: floors unchanged, unit tests unchanged.

### Stage 1 — Unify goal-instance selection

- `EnumerateModularGoalCandidates(st, v, mgt, origin) -> vector<TileIndex>`, one
  enumerator per `MGT_*`, absorbing the type-specific filters from the five loops
  (including the helicopter quirks: computed landing/service tiles, hangar-reachable
  pad filtering, "helicopters use stands only where no helipad exists").
- Set-level pre-filters kept as an explicit step.
- Tiering made explicit — a `CandidateTier` enum rather than the current
  parallel `best_*_tile` locals.
- The five existing functions become thin wrappers preserving their exact current
  tier ordering.

Movement-neutral by intent. Floors must not move.

### Stage 2 — Add the route axis

- `avoid_tiles` span parameter on `FindAirportGroundPath` / `BuildTaxiPath`,
  enforced as an early reject in `GetReachableNeighbors`.
- **The crossing cache is bypassed entirely — read and write — whenever the
  avoid-set is non-empty.** `_modular_airport_crossing_required_path_cache` is keyed
  on `(start, goal, restriction)` only (`airport_ground_pathfinder.cpp:36`) and is
  *saved*; a strict-mode failure caused purely by an avoid-set would otherwise teach
  it "this pair needs a runway crossing" permanently and wrongly.
- Route generation per candidate: shortest first, then up to N regenerations that
  ban the denial tile reported by the validator.
- **Acceptance stays on the horizon prefix plus topological reachability**, so a
  regenerated route that rejoins through the banned tile beyond the horizon is still
  accepted. This is the fix for the trap in §1.
- Ban only on `ReservedByOther` / `OccupiedByOther`. Never ban on `RunwayBusy` for
  the operation runway — that is a different candidate, handled by the outer loop.

### Stage 3 — Wire the consumers

**Departures first.** The `T7d.sav` baseline (§6) splits `stuck(reserve)` as 5373
takeoff-goal against 3358 terminal-goal, so `FindModularRunwayTileForTakeoff` is
where the throughput is. That inverts the intuitive ordering — a refused landing
costs a whole holding lap while a refused pushback costs nothing — but the intuition
is about cost per event, and the counts say departures win on volume.

- `FindModularRunwayTileForTakeoff` — route axis for departures. Do this one first.
- `TryReserveLandingChain` — route axis for arrivals.
- Retry budget as a policy parameter: larger for arrivals (a refused landing costs a
  full holding lap), smaller for departures (the aircraft is parked on a safe stop).
  Size the arrival budget off a `-d misc=2` run, per §6.

Watch the cost. `AircraftEventHandler_Flying` (`aircraft_cmd.cpp:2205`) runs per
aircraft per tick and already does several A* runs per attempt —
`FindFreeModularTerminal` alone runs one per stand. `MAX_PATHFINDER_ITERATIONS` is
1000 (`airport_ground_pathfinder.cpp:27`). Profile with `sample` on `T5j2.sav`
before and after; see `skills/performance_profiling.md`.

### Stage 4 — Mid-taxi reroute (optional, measure first)

Probably not worth it. A one-way corridor has already committed the aircraft by the
time it is inside one (Pitfall 8), so a reroute can only help at a stand or a
free-move junction — and by then Stage 3 has already chosen well. If attempted:
hook the denial site in `AirportMoveModular` (~`:3845`), follow the
`TryRetargetModularGroundGoal` discipline of disturbing nothing until the
replacement plan validates, and use a threshold well below the current 64 ticks.

## 4. Invariants to hold

From `CLAUDE.md` and `skills/reservations-design.md`:

- **Determinism.** Nothing that changes a path choice may depend on
  `_interactive_random`, `_debug_misc_level`, or rate-limit gating. Retry counts must
  be identical on every client.
- **Crossing cache.** Never written from an avoid-set run. It is saved state.
- **Saveload.** `taxi_path` and `landing_chain_path` are not saved. If route choice
  becomes a function of any new persistent field, it versions on the fork's own axis
  (`XVER` / `MODULAR_AIRPORT_SL_VERSION`), never `SaveLoadVersion`.
- **Safe-stop invariant.** Every accepted plan still terminates at a safe stop.
- **Never park on a one-way tile** (Pitfall 8).
- **`MarkLayoutDirty()`** after any direct `ModularAirportTileData` mutation in tests.

## 5. Testing

### Unit tests — the real correctness net

`src/tests/test_modular_airport.cpp` already builds synthetic layouts with
`AddModularTile`, sets `one_way_taxi` / `user_taxi_dir_mask`, creates aircraft, and
exercises `FindAirportGroundPath`, `BuildTaxiPath` and the landing chain — see
`ModularAirportPathfinding` (`:1055`, one-way sections around `:1184`) and
`ModularAirportLandingChain` (`:1742`). Everything below is expressible there,
deterministically, with no savegame involved.

1. **Parallel exits.** Runway → A → apron → stand, plus runway → B → C → apron →
   stand. Reserve A to another aircraft; assert the landing chain commits via B/C.
2. **Rejoin case.** Shortest runway → A → stand, alternate runway → B → C(one-way) →
   A → stand, A blocked. Assert the landing commits with horizon `{runway, B, C}`.
   *This is the regression test for the §1 trap* — a global ban fails it.
3. **No false admission.** Single exit, blocked → still denied.
4. **Tier preservation.** Large aircraft, one large-safe runway blocked, one short
   runway free → still waits, does not downgrade. Guards the deliberate design
   decision behind `good_takeoff_runway_exists`.
5. **Takeoff mirror** of 1 and 2 from a stand.
6. **Directedness.** Confirm a route valid stand→runway is not assumed valid
   reversed through a one-way corridor.

### Regression saves

Run `scripts/regression_test.sh` at every stage. Stages 0 and 1 must not move the
floors; a movement there is a finding, not a number to bump.

`T7d.sav` is the benefit detector — the only fixture with real route diversity, and
flat enough to read a gain directly (§6). `T5j2.sav` is the harm detector — real
layout, sustained contention, ~26k `stuck(reserve)` reports. `mass7-inair.sav` and
`helis2.sav` should stay flat throughout.

Adding `T7d.sav` roughly doubles suite wall time, to ~7 minutes.

## 6. Fixture coverage

### `T7d.sav` — the targeted fixture

Purpose-built for this work and it lands both of the gaps identified below:

- **Pladingbury Airport** — multiple paths off a single landing runway, with heavy
  arrival demand. The arrival case from §1.
- **Sledinghead Cross Airport** — multiple paths to a single takeoff runway. The
  departure case, i.e. Stage 3's second consumer.
- **Mixed large and small runways** across various airports, which closes the
  wait-don't-downgrade coverage hole described below. It mixes them *without*
  crashing, because the strict large-runway preference keeps fast jets off the short
  strips — so the tier is exercised through routing, with no crash attrition
  muddying the movement signal.

It is by some distance the busiest fixture: ~7.4k movements per year against ~1.9k
for `mass7-inair` and ~2.8k for `helis2`. Committed as
`scripts/testdata/T7d.sav` with `min_movements=29300` against a **29590** baseline
at `702326a151`. It roughly doubles regression-suite wall time (~3.5 min on its own).

**It is flat, which is what makes it usable as a floor.** Counted years came in at
7376 / 7409 / 7380 / 7425 — a 0.3% spread — because the baseline run logs zero
crashes. So it delivers contention coverage *and* year-to-year comparability, which
no existing fixture does at once (`T5j2.sav` has the contention but declines from
fleet ageing). A movement drop here is attributable to routing.

Two independent runs produced byte-identical per-year totals, so the floor is exact
and reproducible in the same sense as the existing fixtures.

The baseline log is otherwise clean: 0 `[FALLBACK]` cleanups, 0
`runway-rest-invariant` reports, 5 `stuck(no-path)`. Per §9 of
`reservations-design.md`, frequent fallbacks would mean an upstream contract is
wrong; there are none to start from.

**Confirmed headroom.** A baseline run at `-d misc=1` shows the contention this
feature targets is real and abundant:

| Signal | Count |
|---|---|
| `deny=reserved_by_other` | 8712 |
| `deny=runway_busy` | 1083 |

`reserved_by_other` is precisely the class the route axis addresses — a taxi tile
held by another aircraft, where an alternate route may exist. It outnumbers runway
contention 8:1.

Broken down by goal type (`stuck(reserve)` reports):

| Goal | `MGT_*` | Count |
|---|---|---|
| Takeoff runway | 4 | 5373 |
| Terminal | 1 | 3358 |
| Hangar | 3 | 658 |
| Helipad | 2 | 300 |
| Heli takeoff tile | 6 | 112 |

Two things follow. Departures dominate, so **Stage 3's takeoff consumer is where the
throughput is**, not the arrival side — worth knowing before sequencing the work.
And `MGT_HANGAR` shows 658 reports, so the hangar-traffic coverage listed as
optional-(c) below is already present; no separate save is needed for it.

### Measuring the arrival side needs a louder run

The standard runner uses `-d misc=1` (`scripts/n_years_plus2.sh:28`), but every
arrival-admission diagnostic — `landing-chain fail` and `landing-chain reject` — is
emitted at `Debug(misc, 2, ...)`. They are therefore **invisible in a normal
regression log**, which is why the baseline above shows zero of them despite heavy
landing demand.

Do not read that zero as "arrivals never get refused". Size the arrival opportunity
with a one-off run at `-d misc=2` before committing to Stage 3's retry budget.

Note also that a refused *taxi* step after a successful landing is not an admission
failure: the landing chain only reserves to the first safe stop, so an aircraft that
lands cleanly and then blocks on the way to its stand shows up as a `tgt=1`
`stuck(reserve)`, not a landing-chain failure. Those are largely **not** addressable
by the route axis — once the aircraft is inside a one-way corridor the choice is
already spent (Pitfall 8).

### Remaining coverage notes

`mass7-inair.sav` and `helis2.sav` are documented as having every airport large-safe,
so the strict large-runway preference — the wait-don't-downgrade tier that Stage 1
most risks destroying — was previously exercised by `T5j2.sav` at most. `T7d.sav`'s
mixed-runway airports now cover it directly, and unit test 4 in §5 pins it
deterministically.

No further saves are needed. `T5j2.sav` remains the "real layout under contention"
harm detector; `T7d.sav` is the benefit detector.

### Fixture hygiene

Both must stay **unpaused** — a paused save consumes the tick budget while
simulating nothing, and `airport_stats_history.sh` fails loudly on no countable
years for exactly that reason. `T7d.sav` is confirmed unpaused (it reports real
per-year movement totals). See `skills/savegame_fixture_resave.md` for format
migration, and note the warning on `helis2.expected` about not "cleaning" a fixture
by re-saving it — that deletes coverage and re-baselines the floor.

## 7. Result

### What shipped

| Change | Effect |
|---|---|
| **Stage 0** — split validation out of `TryCommitForwardReservationPlan` | Provably movement-neutral; all fixtures reproduce baseline exactly |
| **`avoid_tiles`** on `FindAirportGroundPath` / `BuildTaxiPath` | Empty avoid-set is bit-identical to the old tile-keyed search |
| **`FindReservableRoute`** wired at three call sites | The alternate-route search itself |
| **No revisiting a tile** in the horizon-scoped search | The fix that made the policy viable |
| **One-way entry rule** — no head-on entry against the arrow | Bit-identical on every fixture; closes a real gap |
| **Whole-route detour cap of +2 tiles** | Recovered T5j2 from failing its floor to above baseline |
| **Parking is not a safe stop unless it is the goal** (stands + helipads) | Stops aircraft waiting on other aircraft's stands and pads |

Stage 1 (unifying the five goal selectors) and Stage 4 (mid-taxi reroute) were **not** done.

### The mistake, because it is the useful part

The first full measurement showed T7d at 24268 against a 29590 baseline, −18%. Setting
attempts to 1 reproduced the baseline exactly, so the refactor was neutral and the loss was
entirely the policy. The write-up concluded that diverting is inherently expensive: one-way
corridors are queues doing useful work, diverting commits the aircraft, spreads load and
destroys arrival order. It fitted every number available.

It was wrong. Per-airport logging showed the loss was not diffuse degradation but **ten
airports deadlocking outright** while the airports the feature targets *gained*. The cause:
the horizon-scoped ban splits A* state into `(tile, passed_safe_stop)`, and one-way tiles
are safe stops — so the search could leave a one-way tile, turn round, re-enter it (flipping
the flag and lifting the ban) and continue. That produced routes visiting the same tile
twice: the aircraft drove out, doubled back against the arrow, and drove out again, holding
both tiles throughout. 46% of permanently-stuck aircraft were walking such a route. These
routes are unreachable with the search off, because `track_horizon` is then false and the
state collapses to the bare tile.

| T7d configuration | movements | pinned aircraft |
|---|---|---|
| baseline (search off) | 29590 | 19 |
| search on, revisit bug | 24268 | 82 |
| search on, revisits forbidden | 30230 | **0** |

Forbidding revisits removed every permanently-stuck aircraft *including the 19 that predate
this feature*.

### Final fixture totals

| Fixture | baseline | shipped | |
|---|---|---|---|
| mass7-inair | 9251 | 9118 | −1.4% |
| helis2 | 13816 | 13775 | −0.3% |
| T5j2 | 6309 | 6253 | −0.9% |
| T7d | 29590 | 29904 | +1.1% |

### Why those small deltas are not worth chasing

Comparing two runs of a deterministic sim is not as clean as it looks. Any change that
shifts timing reshuffles the synced `Random()`, which changes **which aircraft crash** and
**which airports the AI companies build**. Both move fixture totals by 1-2% with no routing
cause:

- Frunnpool East lost 22% of its movements. It is served by three aircraft; one crashed.
- Sunley Airport "lost" 147 movements — the AI rebuilt it under a different station id,
  where it gained 195.
- T7d has **zero crashes**, yet eight small airports present in one run were never built in
  another, worth 238 movements between them.

So: trust deltas well outside 2%, and treat anything inside it as unresolved until the
per-airport `[AirportStats]` lines and the `[AircraftLost]` crash list have been read.
Airports served by few aircraft are the worst offenders and should be discounted first.
`skills/stuck_plane_debugging.md` documents the diagnostic workflow.

### Measured dead ends

- **Don't divert from a one-way tile** (irreversible commitment): 30230 → 29923 and one
  aircraft re-pinned. The reasoning was sound *given the revisit bug* and wrong without it.
- **Suppress the search for takeoff goals**: made all four fixtures pass, but it is a rule
  fitted to these saves rather than a principle, and was rejected on those grounds.
