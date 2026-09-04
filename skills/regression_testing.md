# Modular Airport Regression Testing

Everything about `scripts/regression_test.sh` and the throughput fixtures: what the run
checks, what each fixture covers, how to compare two runs, and why the committed floors sit
where they do. `CLAUDE.md` carries only the short version.

## What the run checks

`scripts/regression_test.sh` runs headless 5-year simulations and checks each fixture two
ways: total airport movements against the committed minimum (the `min_movements=` floor in
`scripts/testdata/*.expected`), **and** the run's log against the `FAILURE_PATTERNS` array in
the script -- should-never-happen lines like `landing-chain-invariant`, both `[FALLBACK]`
markers, `invalid ground state` and `[AircraftLost]`. Ordinary contention
(`stuck(reserve)`, `runway-rest-invariant`, `retarget failed`, ...) is explicitly not gated.
The log check exists because throughput hides small correctness faults: a few aircraft on a
broken path cost a handful of movements out of thousands, well inside the floor's headroom.
Fixtures run at `-d misc=2` so the `[FALLBACK]` markers are visible; that costs ~2.7x the log
volume and does not change any total.

## Invocation

- `scripts/regression_test.sh` -- the bare run: `T5j2`, `mass7-inair`, `helis2` concurrently,
  under a minute apiece. The right check for almost everything, including before a commit.
- `scripts/regression_test.sh --full` -- adds `T7d`, ~13 min total, nearly all of it `T7d`.
  Only when a change could break ground/taxi pathfinding, and always for routing work. Not a
  per-commit gate.
- Other flags: `--no-build` reuses whatever `./build/openttd` already is, `--sequential` runs
  the fixtures one at a time, `--log-dir DIR` moves the per-fixture logs off their default
  `/tmp` paths.

Fixtures run concurrently: the sim is single-threaded and the fixtures share no writable
state, so `--full` costs roughly one fixture's wall time on a multi-core machine rather than
four. Each fixture's output is buffered and replayed in fixture order after everything
finishes, so the report never interleaves.

**Two concurrent *suite* runs still collide**, because the log path is derived from the fixture
name: the second run truncates the first one's log out from under it and both sets of
`[AirportStats]` lines are lost. That surfaces as "no countable years" and reads exactly like a
paused fixture. Pass `--log-dir` to whichever run is the guest. (This is real -- it happened on
2026-08-23 with two agents driving the suite in the same tree at once.)

## The fixtures

- `scripts/testdata/T7d.sav` -- route diversity: Pladingbury Airport has multiple paths off one landing runway under heavy arrival demand, Sledinghead Cross Airport has multiple paths to one takeoff runway, and other airports mix large and small runways. Busiest fixture (~7.4k movements/year), so with the fixtures running concurrently it is the one that sets the suite's wall time.
- `scripts/testdata/T5j2.sav` -- **default.** Real player layout under sustained contention (~26k `stuck(reserve)` reports over 8 years with no permanent stall); 16 modular airports, mixed fleet.
- `scripts/testdata/mass7-inair.sav` -- **default.** Mixed fixed-wing throughput; every airport is large-safe.
- `scripts/testdata/helis2.sav` -- **default.** Helicopter-heavy stress; every airport is large-safe.

**Aircraft crashes are off in all four fixtures** (`vehicle.plane_crashes = 0`, set by
`scripts/disable_crashes.sh`), so which aircraft happen to die no longer perturbs a total. That
setting gates only the general per-brake-tick roll in `RollAirplaneCrashCheck`. The elevated
short-strip overrun for fast jets (fixed prob 3276) **ignores it** and is gated by the
`no_jetcrash` *cheat* instead, which no console command reaches and which would also change
which airports jets may be sent to -- so it is left alone. `mass7-inair` and `helis2` are
entirely large-safe, so that path cannot fire there at all; `T5j2` and `T7d` mix runway sizes
and could in principle, but neither logs a crash today. `[AircraftLost]` at `misc=1` stays the
check: a fixture that starts logging one has reached the overrun path, not the general roll.

**`T5j2.sav` is still not flat** with crashes off: throughput declines ~3% across the window
from fleet ageing alone (1306 / 1297 / 1239 / 1235 / 1263 movements per counted year). It is
exact and reproducible, because the sim is deterministic for a fixed save plus tick count and
the window is always the same -- but read a `T5j2` drop as "compared with the committed
baseline", not "compared with last year". Its value is contention coverage the other two do not
provide.

**`T7d.sav` is the route-diversity fixture**, and the reference for what a quiet fixture looks
like: it already had `plane_crashes = 0` when it was captured, so it was crash-free before the
other three were, and turning off crashes left its total bit-for-bit unchanged (27033 both
ways, measured 2026-08-23; it reads 26974 at `1dbcc94756`). Its four counted years were
6699 / 6796 / 6793 / 6745 -- a 1.4% spread, with the dip in the first counted year, so compare
totals rather than reading one year against another. It is
the only fixture with genuine route diversity -- two or more routes between the same endpoints
-- so it is the one that can detect alternate-exit routing work at all. It also mixes large and
small runways without ever reaching the overrun path, because the strict large-runway
preference keeps fast jets off the short strips, which makes it the only probe of the
wait-don't-downgrade tier (`mass7-inair` and `helis2` are entirely large-safe). Those two gaps
are exactly what the default run cannot see, and why routing work needs `--full`.

**The fixtures have no AI companies.** `T5j2` and `T7d` carried three each, and which
airports an AI decides to build shifts with any change that consumes the synced `Random()`
differently -- worth a couple of hundred movements with no routing cause. `scripts/strip_ai.sh
<save>` deletes every AI company from a save (taking its stations and vehicles with it) and
sets `difficulty.max_no_competitors = 0` so no replacements spawn during the run. It re-saves
through the same `game_start.scr` hook as `resave.sh`, with zero ticks simulated.
`mass7-inair` and `helis2` never had any.

`helis2.sav` was written by a build predating the modular touchdown clearing
`VehicleAirFlag::HelicopterDirectDescent`, so ~52 of its 90 helicopters carry a stale descent
flag -- real pre-fix data for the touchdown clear to chew on. An earlier note warned never to
re-save it because that would destroy the coverage. **That was wrong.** `Aircraft::flags` is
ordinary saved vehicle state (`SLE_CONDVAR(Aircraft, flags, ...)` in
`saveload/vehicle_sl.cpp`) and no afterload step clears the bit, so the flags round-trip. The
fixture was re-saved on 2026-08-23 to turn crashes off, which is also what re-baselined it.

The runner excludes the **first reported year** as a warmup (its length depends on the save's
start date), so the saves count different calendar windows -- that's expected.

**Test saves must be saved unpaused.** A paused save still consumes the tick budget while the
game loop does nothing, so the run finishes in seconds having simulated nothing.
`airport_stats_history.sh` now fails loudly when a run reports no countable years, because the
raw result of a paused save is `movements=0`, which parses fine and would otherwise read as a
total throughput collapse (or pass silently against a low floor). Check with `_pause_mode` in a
debugger if a new fixture behaves oddly; the committed saves all read 0.

## Builds and logs

`n_years_plus2.sh` runs `scripts/build_and_sign.sh` before **every** fixture unless
`OPENTTD_SKIP_BUILD=1` is set. `regression_test.sh` builds once up front and sets that, both
because concurrent fixtures would otherwise run concurrent `make` invocations against the same
build directory, and because a per-fixture rebuild lets a mid-run source edit silently split
the result across two builds. That hazard is still live for anything that drives
`n_years_plus2.sh` or `airport_stats_history.sh` in a loop: the early fixtures measure the old
code and the later ones the new, nothing warns, and the totals look perfectly ordinary. Let
such a run finish, or kill it, before touching sources.

Batch runs log to `/tmp/openttd_regression_<save>.log`, one per fixture, overwritten each run
(override with `OPENTTD_REGRESSION_LOG`). They deliberately do **not** use `/tmp/openttd.log`:
that path belongs to the interactive runners, and a game started by `build_and_run*.sh` holds
it open as its stdout for as long as it runs. Truncating it from a batch run does not move the
live game's file offset, so the two interleave and the `[AirportStats]` lines get overwritten
-- which shows up as "no countable years" and reads exactly like a paused fixture. So a
regression run is safe to start while a game is open, but read the `log:` path the runner
prints rather than `/tmp/openttd.log`.

## Comparing two runs

The sim is deterministic for a fixed save + tick count, so a single run is exactly
reproducible and the floors are exact -- the same code on the same fixture gives the same
total every time. That does **not** by itself make two runs of *different code* cleanly
comparable: any change that shifts timing consumes the synced `Random()` differently, which
changes:

- **which aircraft crash** -- removed by setting `vehicle.plane_crashes = 0` in every fixture.
  While crashes were on, an airport served by three aircraft lost ~22% of its movements when
  one of them died, which is indistinguishable from a routing regression. `[AircraftLost]`
  logs every crash at `misc=1`; all four fixtures log zero, and one appearing means the
  elevated short-strip overrun fired, which that setting does not gate.
- **which airports the AI companies build** -- removed as a noise source by stripping the AIs
  from the fixtures, but this is what it looked like while it was there: T7d had zero crashes
  yet eight small airports present in one run were never built in another, worth 238
  movements. If a fixture ever regains an AI, watch for an airport going to 0 while a
  similarly-sized one with a different name appears -- that is one airport rebuilt elsewhere,
  not a loss.

With both gone, no fixture has a known noise source left, so read a total delta as a result of
the code under test, and treat two different builds as comparable as long as `[AircraftLost]`
is still absent from both runs. The one direct measurement of the re-save itself is `T7d`,
which already had crashes off: rewriting the file changed its total by 0 movements. Keep the
floors' headroom regardless -- it costs nothing and covers whatever has not been found yet.

To attribute a delta:

- `[AirportStats] Year N station S "Name"` (at `misc=1`) gives per-airport movements.
- Landing-chain fail/reject diagnostics are emitted at `misc=2`, so they are **absent** from a
  normal regression log. Their absence is not evidence that arrivals are never refused -- do a
  one-off `-d misc=2` run to measure that.
- `stuck(reserve) st=S` groups stuck reports by airport.
- Discount airports served by few aircraft first; large airports are far more trustworthy.
- Average **ground time per leg** is robust to how many aircraft exist -- but it conflates
  "taxied further" with "landed instead of holding", so rising ground time alongside rising
  movements is the feature working, not a cost.

Per-commit attribution: `scripts/airport_stats_history.sh <start_commit> <out_dir> <years>`
checks out + rebuilds each commit in `<start>^..HEAD` and records movements to CSV (history
mode runs **only** the default save). `--current <years> [save]` runs just the working tree.
Underlying runner: `scripts/n_years_plus2.sh <years> [save]` (default save = mass7-inair.sav).

## Floor history

Every floor sits 50-200 movements under its fixture's current total: `mass7-inair` 110,
`helis2` 115, `T5j2` 140, `T7d` 174. That is the margin to aim for when bumping one. `T5j2`
wants the wider end of it: migrating that save to version 375 shifted it by 26 movements (6283
before the re-save, 6309 after) while `mass7-inair` and `helis2` both round-tripped to
identical totals, so whatever state does not survive `T5j2`'s save/load is worth about half a
percent there and a 30-movement margin would sit inside the noise.

`mass7-inair` (9100) and `helis2` (14000) were lowered to 8400 and 13400 on
2026-08-29. `cd876d1b676162e60ed80454a9db935332d8edda` ("Refactor: Simplify ground pathfinder
and holding pattern calculations", 2026-08-24) swapped the ground-pathfinder A* heuristic from
a locally-defined `CalculateHeuristic()` to the shared `DistanceManhattan()`, which dropped
`mass7-inair`/`helis2` throughput ~8% (9229 -> 8500, 14127 -> 13505) -- every other change in
that commit was verified inert by reverting each individually and rebuilding. The floors were
lowered to match rather than reverting the swap. See the next section for what the old
heuristic actually was and where the throughput went.

They were raised again to 8850 and 13900 on 2026-09-05, once the loss stopped being
attributable to the heuristic. Throughput did not recover -- 8960 and 14015 are still under the
pre-swap 9229 and 14127 -- but floors of 8400 and 13400 left 560 and 615 movements of slack, so
the two default-run fixtures that were widened for this loss could no longer have caught a
repeat of it. Both totals were measured at `1dbcc94756` and reproduced exactly by a bare
`scripts/regression_test.sh` run at `292c1384ec`, which is where the new floors were set.

## What the A* heuristic swap actually did

`CalculateHeuristic()` did **not** compute Manhattan distance and was not merely "weak". Its
`abs()` resolved to OpenTTD's own `abs<T>` template (`src/core/math_func.hpp`), not `std::abs`;
with `T = uint` from `TileX`/`TileY` the `a < 0` test can never fire, so it returned the wrapped
value unchanged and assigning that to `int` recovered the *signed* difference. The function was

    h_old(n) = (TileX(n) - TileX(goal)) + (TileY(n) - TileY(goal))

-- a signed potential phi(n) - phi(goal) with phi = x + y, not a distance. `h_old(goal) = 0` and
each step changes it by exactly +/-1 (moves are 4-orthogonal) against a minimum edge cost of 1,
so it is **consistent** -- which makes plain A* under it cost-optimal. The `avoid_tiles` path is
not plain A*, since `current_path_contains` prunes on the route built so far, so treat the
measured "never a different cost" below as the real evidence rather than the proof. Under the
effective edge reweighting w' = w + h(v) - h(u) it is Dijkstra with a fixed pull toward the
low-x/low-y (screen-up) corner, with no reference to where the goal is. As of 2026-09-04 this
was the only place the fork tripped over that template: a `static_assert(!std::is_unsigned_v<T>)`
inside `abs<T>` compiled the whole tree clean.

So the swap could not change path *quality*. Measured directly, by running both heuristics on
every identical query inside one simulation (~1.25M paired queries on `mass7-inair`):

- the two **never** return a different cost, and neither ever finds a route the other misses;
- `caphits = 0` throughout, so `MAX_PATHFINDER_ITERATIONS` is not involved either;
- of the queries where both find a route (~95% of them; the rest are refused by both), 52-57%
  return the identical route and **43-48% return a different route of identical cost and
  identical tile count**, with a same-cost different-length result under 0.15%;
- first-safe-stop position, i.e. reservation-horizon length, differs by 0.17% in aggregate.

The only thing the heuristic changes is **which of several equal-cost routes wins**, via node
expansion order. That choice is worth far more than 8%. Isolate it by scaling the whole f-cost
and hanging a sub-unit tie-break term underneath: `f = 64*(g + |dx| + |dy|) +/- phi`. Scaling `g`
by the same 64 is what keeps this cost-optimal -- scaling only the heuristic would be weighted A*
at weight 64, i.e. greedy -- and phi stays under 64 because both tiles are inside one airport.
That gives (5 counted years, `T7d` 4, measured 2026-09-04 at `1dbcc94756`):

| tie-break | mass7-inair | helis2 | T5j2 | T7d |
|---|---|---|---|---|
| none (`DistanceManhattan`, current) | 8960 | 14015 | 6340 | 26974 |
| `h_old` (pre-cd876d1b) | 8990 | 14428 | 6343 | 26648 |
| prefer small x+y | **9558** | **14526** | 6247 | **27127** |
| prefer large x+y | 7973 | 13007 | 6301 | -- |
| prefer small x-y | 8880 | 13996 | -- | -- |
| prefer large x-y | 8061 | 13209 | -- | -- |
| prefer reaching a safe stop sooner | 8953 | 13915 | -- | -- |
| h = 0 (Dijkstra) | 8425 | -- | -- | -- |

`stuck(reserve)` on `mass7-inair` tracks it monotonically and inversely -- 30161 / 35964 / 38288 /
57036 for small-x+y / `h_old` / current / large-x+y -- so the channel is ground contention, as the
old working theory guessed. Three things that theory got wrong or left open are now settled:
the effect is not about heuristic strength (Dijkstra is 6% *worse*, so goal-direction still pays),
it is not about the reservation horizon (deliberately minimising it is a wash), and the accidental
bias in `h_old` was a weak version of a much larger effect. Restoring `h_old` at `1dbcc94756`
gains +0.3%/+2.9%/+0.05% on `mass7-inair`/`helis2`/`T5j2` and *loses* 1.2% on `T7d`, so there is
no live regression to revert -- keep `DistanceManhattan`.

Read the rest as headroom, not as a fix to apply. The winning bias is an arbitrary geometric
preference with no reason to generalise: it gains 6.7%/3.6%/0.6% on `mass7-inair`/`helis2`/`T7d`
but loses 1.5% on `T5j2`, and on `mass7-inair` 88% of its gain is two airports (Drefingbridge
+301, Harhill East +229). What it establishes is that A* tie-breaking silently picks the traffic
pattern, that nothing in the code picks it deliberately, and that the best and worst arbitrary
choice are 1585 movements apart on `mass7-inair` -- 18% of the current baseline. A principled
rule that separates opposing flows would be the real fix. The measurements used an
`OPENTTD_PF_HEURISTIC` selector and an `OPENTTD_PF_SHADOW` paired-query comparison bolted onto
`src/airport_ground_pathfinder.cpp`; that scaffolding was never committed, so reproducing this
means writing it again.

That regression went unseen for five days because the bare run was `T5j2` alone until
2026-08-29. `T5j2`, `mass7-inair` and `helis2` all run by default now, so the same kind of
regression fails the bare run rather than needing `--full` or a manual bisect to catch.

## The NoAI script regression (a different thing)

`cmake --build build --target regression -j8` is the script regression, part of the
before-committing checklist and **not** covered by `openttd_test`. It replays
`regression/regression/main.nut` and diffs the output against the committed
`regression/regression/result.txt`, so any change to script-visible behaviour -- a piece's
availability year, whether a rotation may be built, a noise or catchment number, a new API
answer -- makes that file stale and turns every CI job red on every platform at once. The
whole diff is in test data, so nothing fails locally until this target runs. Regenerate
rather than hand-edit: a failing run writes the actual output to
`build/regression_regression_output.txt`, and the fix is to `diff` that against the committed
file, confirm every changed line is an intended behaviour change, then copy it over.

**Use the `regression` target, not bare `ctest`.** The ctest entry runs the comparison script
directly, while the copy of `result.txt` it reads is staged into `build/regression/` by a
separate `regression_files` target that only the build target depends on
(`cmake/CreateRegression.cmake`). So `ctest -R regression_regression` after editing
`result.txt` re-reads the *previous* copy and reports the same failure -- which reads exactly
like the edit not having worked. CI is unaffected because it builds before it tests.
