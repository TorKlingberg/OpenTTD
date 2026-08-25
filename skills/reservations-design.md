# Modular Airport Reservation Design

How taxiing and tile reservations actually work in `src/modular_airport_cmd.cpp`. Source of truth for reservation invariants.

## 1. Segment types

Taxi paths are split into contiguous segments. Segment type still describes routing
and safe-stop behavior, but reservation scope is decided by the aircraft's operation,
not by treating every runway segment as the same resource:

| Type | Where it applies | Reservation model |
|------|------------------|-------------------|
| `Runway` | `IsModularRunwayPiece(piece_type)` | Crossing: traveled tiles only. Explicit landing/takeoff operation: entire contiguous runway |
| `OneWay` | `IsTaxiwayPiece` with `one_way_taxi == true` | Queue tile and forward-horizon boundary |
| `FreeMove` | Everything else (aprons, stands, hangars, fenced apron variants) | Traveled tiles through the forward horizon |

`TaxiSegmentType` is assigned by `ClassifyTile` in
`airport_ground_pathfinder.cpp`. `TryReserveTaxiSegment` builds one reservation
horizon from the current path index regardless of which segment triggered the call.

## 2. Safe stops (key invariant)

**An aircraft on the ground must always have a reserved path to a "safe stop".** A safe stop is a tile where the aircraft can wait indefinitely without holding a shared resource other aircraft need to traverse.

| Tile kind | Safe stop? | Why |
|-----------|-----------|-----|
| Stand | Yes | Parking; per-stand exclusivity, others route around |
| Hangar | Yes | Multi-capacity parking; never hard-blocks |
| Helipad | Yes | Parking for helicopters |
| `OneWay` taxiway tile | Yes | Designed as a queue; per-tile semantics support waiting |
| Runway tile | **No** (in transit) | Shared crossing/operation space; an aircraft may not wait there |
| `FreeMove` grass / apron | **No** | Pure transit; stopping blocks anyone else needing to cross |

A runway tile is special: it *is* the destination during takeoff (`MGT_RUNWAY_TAKEOFF` → state `TAKEOFF`), where the aircraft transitions out of ground movement entirely. So a runway-end takeoff goal is acceptable as a path terminus, but **never** as a mid-path resting place.

The fallout: every reservation step must guarantee that the aircraft, after taking it, still owns a chain reaching some safe stop. Stopping on grass/apron is an invariant violation, not a "stuck" symptom — the system is supposed to deny entry rather than allow the stop.

## 3. Unified forward reservation horizon

`BuildForwardReservationPlan` is the single description used by reservation and
retention. Starting at `taxi_path_index`, it walks through the aircraft's goal or
the first *future* safe stop. The current safe-stop tile cannot immediately end a
departure plan; the aircraft must reserve somewhere to advance to.

The plan partitions that horizon into two kinds of claim:

- **Taxi tiles:** every traveled apron, taxiway, parking, and transit-runway tile.
  A runway crossing is ordinary exclusive path space: only the tile(s) on this
  path are claimed. Two aircraft may therefore cross the same runway at disjoint
  places.
- **Operation runway:** only the runway explicitly used for a landing or runway
  takeoff. It expands to the whole contiguous runway. A takeoff runway is not
  acquired while the horizon still ends at an upstream one-way queue; it joins
  the plan when the horizon actually reaches that runway.

Landing supplies its touchdown runway explicitly. Ground movement identifies a
takeoff operation from `MGT_RUNWAY_TAKEOFF` and `modular_takeoff_tile`; after
touchdown, the aircraft's tracked whole-runway claim identifies the landing
operation until the aircraft steps off it. A runway merely crossed on the way to
another runway never enters `modular_runway_reservation`.

`TryCommitForwardReservationPlan` validates every claim before changing any
ownership. Whole-runway operations check every runway tile; crossings check their
individual path tiles. The map claims make the exclusion symmetric: an operation
is denied by a crossing anywhere on its runway, while a crossing is denied by an
active operation. A denied transaction leaves existing ownership unchanged.

`AirportMoveModular` revalidates this same horizon before every step. Consequently
an aircraft never enters runway or apron transit without already owning a path to
a place where it may wait, but it also never reacquires a runway or other tile that
has fallen behind its current path index.

#### Termination invariant

Every visited tile reaches the termination tests; no tile classification may skip
them. The aircraft's goal is tested separately from the tile's safe-stop property
and before the generic safe-stop test. A goal that is itself a runway tile can
therefore terminate the horizon. It expands to a whole runway only when it is the
explicit landing/runway-takeoff operation; otherwise it remains a taxi-tile
claim.

Because a valid taxi path ends at `ground_path_goal`, `NO_SAFE_STOP` indicates an
inconsistent path/goal rather than ordinary traffic contention. See Pitfall 2.

## 4. Reserve-then-reconcile (Reservation V2)

Per-step movement uses `TryReserveTaxiSegment` to acquire forward, then a deterministic release pass:

1. `BuildReservationKeepSet(v, st, keep_set)` computes the tiles that should remain reserved.
2. `ReconcileAircraftReservations(v, st, keep_set, "post-step")` releases everything owned that isn't in the keep-set, and prunes `taxi_reserved_tiles` / `modular_runway_reservation` to match.

`keep_set` includes:
- Current tile (`v->tile`).
- The taxi tiles and operation runway produced by the same forward plan used for acquisition.
- Landing-chain continuity (every tile in `v->landing_chain_path`, until the rollout transition discards it) and its committed operation runway.
- A committed safe-stop claim while a just-landed aircraft is still standing on a runway without an active saved path.
- Full `modular_runway_reservation` when `ShouldRetainRunwayReservation(v, st)` confirms an active takeoff operation on the intended runway.

Transit-runway claims are **not** expanded during retention; they remain ordinary
taxi tiles. An operation runway remains whole while current or inside the active
forward horizon, then falls out of the recomputed plan immediately after the
aircraft steps onto another resource. `ClearTaxiPathReservation` is reserved for
transitions and force-clear; normal per-step release is reconciler-driven.

## 5. Landing chain (pre-touchdown reservation)

`TryReserveLandingChain` builds and commits one transaction before descent:

- The whole contiguous landing runway as the operation claim.
- Every traveled tile from the rollout end through the ground goal or first safe queueing point. Any runway crossed after rollout contributes only its traveled path tiles.

If any operation or taxi claim is unavailable, nothing in the landing transaction
is acquired. This prevents an aircraft from landing with a reserved touchdown
runway but no reserved route through an adjacent runway or apron to safety.

The computed path is stored in `landing_chain_path` and reused after touchdown when possible. If no ground goal exists, landing is only allowed when there is a safe `OneWay` buffer after the runway.

## 6. Path rebuilding and retargeting

- Taxi path is rebuilt when invalid or out of sync with current tile/goal.
- After waiting >64 ticks, `TryRetargetModularGroundGoal` can reroute to a different terminal/helipad/hangar/rollout, and re-picks the takeoff end for `MGT_RUNWAY_TAKEOFF`. Reservations stay until retarget actually succeeds and replaces them; the stale whole-runway claim is released on the next tick because `ShouldRetainRunwayReservation` compares it against the new `modular_takeoff_tile`.

## 7. Hangars

- Multi-capacity in reservation logic: never set map-level reservation bits, never block another aircraft.
- Vehicle-level intent is still tracked in `taxi_reserved_tiles` so path cleanup works.

## 8. Common pitfalls

### Pitfall 1: Transit-runway entry must guarantee a safe stop beyond (resolved)

Entering a transit runway commits every traveled tile through the first safe stop
(see §3), so an aircraft never steps onto a runway — or off it onto unreserved
grass — without owning a path to a place it can wait. The crossing itself remains
tile-level; only an explicit landing/takeoff operation claims the whole runway.
This is enforced uniformly for fixed-wing and helicopters by the common forward
planner; there is **no** vehicle-class or runway-segment special case at the
`AirportMoveModular` boundary.

History: this used to pin only the first continuation tile, leaving the rest of
the downstream `FreeMove` segment unreserved, so fixed-wing could strand on
grass. A helicopter-only revalidation at the segment boundary patched the
symptom; the forward-horizon contract removed the root cause and let the special
case be deleted. Tightening the fixed-wing contract cost ~2% on the
`mass6-inair.sav` baseline (floor lowered 9200 → 9000) — the price of the
provable safe-stop guarantee.

### Pitfall 2: Never let classification skip the horizon terminator

Every tile in the forward walk must reach "is this the goal / a safe stop?" and
must never `continue` past those tests. The aircraft goal remains a separate test
from the tile's safe-stop property.

This has now caused the same permanent deadlock twice. `FindRunwayTransitContinuationTile`
(deleted) skipped runway tiles before its goal check; that was latent, because it
only ever produced a hint. `a7346e86c8` copied the ordering into the real
old crossing walk, where it became fatal: a helicopter whose computed pad had
fallen back to a runway end could never satisfy the contract, and waited forever
on an *empty* airport with every blocker reading false. The replacement
`BuildForwardReservationPlan` has one code path, no classification `continue`,
and tests the aircraft goal separately before the generic safe-stop condition.

Two smells to watch for when touching this area:

- A predicate that silently folds the aircraft's goal into a tile-property test
  (the old three-argument `IsModularSafeStopTile(st, tile, goal)`). It hides the
  goal behind a piece-type check that an earlier branch can skip. The goal is a
  property of the *aircraft*, not of the tile — keep it at the call site.
- Two functions that both answer "what must I own before advancing". They will disagree,
  and the log will confidently print the wrong one's answer while the other denies
  entry. That is exactly how this bug hid in plain sight.

### Pitfall 3: A reservation off a safe stop is not the reconciler's to reclaim

Retention is normally justified by a path — the active `taxi_path` or the stored
`landing_chain_path`. A landing committed through the **no-ground-goal** branch
of `TryReserveLandingChain` has neither: it reserves the runway plus a one-way
buffer to queue on, then deliberately resets the path. Nothing justified the
buffer, so the next reconcile released it, and the aircraft reached the rollout
end owning nothing — standing on a runway with the guarantee that permitted its
landing already thrown away.

`BuildReservationKeepSet` therefore keeps reserved **safe-stop** tiles whenever
the aircraft is standing on a runway. Landing is only allowed against a reserved
route to a safe stop (§5), so until it is on one, that claim is what makes its
position legal.

Keep the condition narrow. Retaining *everything* an aircraft holds while off a
safe stop costs ~10% throughput on `mass7-inair`, and retaining safe stops from
any non-safe-stop tile still costs ~4%; restricting it to aircraft standing on a
runway fixes the invariant and *gains* throughput, because it targets the landing
case instead of every apron transit. Measure before widening it.

### Pitfall 4: `taxi_reserved_tiles` vs map state

`SetTaxiReservation` blindly overwrites the map-level reserver bit; the caller must have already verified that no other vehicle owns the tile. Likewise, the reconciler edits both the map bits and the vehicle vectors — divergence between vector and map state usually means something wrote map state without going through `SetTaxiReservation`, or vice versa.

The reconciler now *relies* on the vectors being authoritative: its release pass walks `taxi_reserved_tiles` + `modular_runway_reservation` (not the whole airport) to find map bits to clear, so per-step reconcile is O(reserved) not O(tiles). Every map-bit setter (`SetTaxiReservation`, `TryReserveContiguousModularRunway`, `TryCommitForwardReservationPlan`) records into one of these vectors — if you add a new setter, it **must** track here or its bit can leak (the reconciler will never see it).

The `owned-reservations` log line reads map state; `tracked-runway` reads `modular_runway_reservation`. A mismatch is a useful red flag. See `skills/stuck_plane_debugging.md`.

### Pitfall 5: Runway deny is sticky if you already own the resource

`TryReserveContiguousModularRunway` does not clear runway ownership on deny if the aircraft already owns the exact requested contiguous runway. Deny clears happen only when existing runway ownership is stale or mismatched. Don't write recovery code that assumes a deny implies a clean slate.

### Pitfall 6: Reservation clears are rendering invalidation events

The reservation overlay is drawn from map-level reservation bits, but changing those bits does not automatically repaint the viewport. Modular airport code that clears a reservation (`SetAirportTileReservation(t, false)`) must also dirty the affected tile, otherwise the colored overlay line can remain visible until some later redraw happens.

Use the local reservation-clear helper in `src/modular_airport_cmd.cpp` for modular airport cleanup paths instead of calling `SetAirportTileReservation(..., false)` directly. This applies to normal reconciliation, taxi/runway transition cleanup, and stale-reservation fallback cleanup.

### Pitfall 7: Per-tick targeting that reads shared-resource state without claiming it

If a per-tick movement function (`AirportMoveModular*`) sets its target based on "is the shared resource free?" while the actual reservation happens later in a separate handler (e.g. `AircraftEventHandler_Flying` → `TryReserveLandingChain`), then in the window between one aircraft freeing the resource and the next aircraft committing, **every** pre-commit aircraft simultaneously sees "available" and redirects to the same point. Visible as synchronized convergence at state transitions — a cluster of holding helicopters all flying to the landing tile in unison, scattering back to holding once one of them commits.

Cure: don't let movement read the shared-resource bit unless movement also reserves. Keep movement targeting on a private waypoint (holding pattern, current path) until the commit handler claims the resource and changes state — after which a different movement function takes over off the committed state. Helicopters in `AirportMoveModularFlying` now always target the holding waypoint; commit happens in `AircraftEventHandler_Flying`, and post-commit movement runs in `AirportMoveModularLanding` driven by `HELILANDING` state.

### Pitfall 8: Never park an aircraft on a `OneWay` tile

A one-way tile is a queueing corridor, not a parking space. Holding an aircraft
there (computed heli pad, fallback holding spot, giving up mid-corridor) is a
permanent deadlock: it blocks the corridor, and it can only move in the flow
direction — so if that leads somewhere it can't use, it has no legal move at
all. `ComputeModularHeliTiles` hit this and now skips `one_way_taxi` tiles.

Note when touching that heuristic: making its building-adjacency filter a soft
penalty (to get a central pad instead of a runway-end fallback) costs ~500
movements on `helis2.sav`. Tried and reverted; re-measure before retrying.

## 9. Debugging policy

When evaluating proposed fixes for stuck aircraft:

- Prefer strict "can I enter?" rules before movement into `FreeMove` or runway-transit sections.
- Prevent unsafe entry rather than trying to "unstick" later.
- `[FALLBACK]` cleanup paths (stale-clear, orphan-clear, force-clear-all) are safety nets — frequent occurrences mean an upstream contract is wrong, not that the fallbacks need tuning.
- If a plane stops on a `FreeMove` tile, the first question is whether entry should have been denied earlier.
