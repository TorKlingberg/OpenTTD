# Reservation V2 Plan

## Problem Statement

Current modular airport reservations are conservative and stateful:

- Aircraft can lose takeoff runway reservation while still taxiing toward it.
- Taxi/stand/runway reservation release is inconsistent (special-case heavy).
- Reservations can persist longer than needed, reducing throughput and creating contention.

We want two outcomes:

1. A takeoff-bound aircraft should not lose a runway reservation it still needs.
2. Any reserved tile (taxi, stand, runway) should be released as soon as it is no longer needed.

## Core Principles (V2)

1. Reservation should be "minimal future footprint":
- Keep only tiles needed to safely continue from current tile to next commitment point.

2. Reservation ownership should be "intent-accurate":
- For takeoff intent, runway reservation is tied to selected runway resource, not physical current tile.
  - Retention must verify reservation matches current intended runway resource; mismatched legacy ownership must be released.

3. Release should be deterministic:
- Every movement step computes what to keep and releases everything else.

4. Runway resource semantics remain atomic:
- A contiguous runway is still reserved as one resource when required.
  - If any tile of runway resource `R` is still needed, retain all tiles of `R`.

## Required Invariants

1. `current_tile` is always reserved for the moving aircraft unless flying.
2. For `MGT_RUNWAY_TAKEOFF`:
- If runway resource `R` is selected and still reachable, keep `R` reserved until:
  - runway entry and takeoff completes, or
  - takeoff intent/runway target changes, or
  - explicit abort/recovery.
3. No tile outside the computed keep-set remains reserved by the aircraft.
4. Reservation denials must not clear already-valid ownership of the same resource.
5. Runway turn-back paths are supported:
- If path needs runway tiles again later, those tiles remain in keep-set until final use is passed.

## Scope

In scope:

- `AirportMoveModular` reservation/step loop
- `TryReserveTaxiSegment`
- `TryReserveContiguousModularRunway`
- `ClearTaxiPathReservation` / `ClearModularRunwayReservation` call patterns
- takeoff ground intent handling (`MGT_RUNWAY_TAKEOFF`)

Out of scope:

- changing runway resource granularity (still contiguous atomic)
- changing pathfinder topology rules

## Data Model Changes

Add explicit reservation intent metadata (aircraft-local):

1. `reserved_runway_target` (TileIndex or resource key)
- The runway resource this aircraft intends to use for takeoff/landing.

2. `reservation_keep_set` (derived each tick, not necessarily stored)
- Set of tiles to retain after movement/reserve step.

3. Optional debug-only fields:
- `last_release_count`
- `last_keep_set_size`
- `last_release_reason`

No saveload changes initially; derive on load from current state/path.

## Algorithm: Reserve-Then-Reconcile

Per ground movement tick:

1. Build/validate path and identify current segment.
2. Acquire required forward reservations for immediate progress.
3. Build a keep-set from current state:
- always include `current_tile`.
- include forward tiles for current reservation horizon.
- include boundary/exit tiles required by segment policy.
- include runway resource tiles if takeoff/landing intent still needs them.
- include active landing-chain continuity tiles needed before/while `taxi_path` is (re)established.
4. Reconcile:
- release any currently owned tile not in keep-set.
- keep everything in keep-set.

This replaces broad "clear if not on runway" behavior with explicit retention logic.

Landing-chain note:
- During touchdown transition, keep-set computation must consider `v->landing_chain_path` (when present and valid) so pre-reserved continuity tiles are not released before the main taxi path is active.

## Segment Policy (V2)

### FREE_MOVE

Keep:

- current tile
- forward suffix of current free-move segment from `path_idx + 1`
- one boundary tile into next segment
- if boundary is runway and runway intent applies: full runway resource

Release:

- any previously reserved free-move tile behind current index
- any non-path tile no longer justified by intent

### ONE_WAY

Keep:

- current tile
- next step tile
- runway resource only when entering runway is imminent and selected

Release:

- previous one-way tiles immediately after step

### RUNWAY

Keep:

- current tile
- runway tiles still ahead on active planned runway use
- if path predicts re-use (turn-back), keep required future runway subset/resource

Release:

- runway tiles provably behind and not needed for future planned re-entry

## Runway Turn-Back Handling

Key requirement: runway tile may be needed twice in one path.

Plan:

1. Precompute future runway usage indices for current taxi path:
- for each runway tile/resource, track remaining occurrence positions >= current index.

2. During reconcile:
- release runway tile/resource only when no future occurrence remains.

3. If runway resource is atomic contiguous:
- retain full resource while any future occurrence remains.
- optimize later only if partial runway ownership is introduced (not in this phase).

This preserves correctness at cost of holding contiguous runway longer in turn-back cases.

## Behavior Changes Needed

1. Remove/replace non-runway auto-clear in movement loop.
- Current logic at modular movement step that clears runway reservation when tile is not runway must be replaced with keep-set reconcile.

2. Remove/replace `MGT_RUNWAY_TAKEOFF` non-runway "clear and requeue" clear.
- Keep runway ownership if same target runway still intended.

3. Change denial behavior in runway reserve:
- On deny, do not clear existing runway reservation if it already matches required runway resource.
- Only clear when reservation is stale/incorrect for current intent.

4. Centralize release:
- Use one reconcile function as authoritative releaser to avoid distributed clears.

## Migration Strategy

### Phase 1: Safety Refactor (no behavior change)

1. Add helper declarations in [`src/modular_airport_cmd.h`](src/modular_airport_cmd.h):
- `BuildReservationKeepSet(const Aircraft *v, const Station *st, std::vector<TileIndex> &keep_set)`
  - preferred contract: caller-owned vector passed by reference and filled in-place.
  - acceptable alternative: return `std::vector<TileIndex>` by value if call sites stay simple.
- `ReconcileAircraftReservations(Aircraft *v, const Station *st, std::span<const TileIndex> keep_set, const char *reason)`
- `ShouldRetainRunwayReservation(const Aircraft *v, const Station *st)` (intent guard)
  - helper should validate runway-resource equality: current `modular_runway_reservation` resource == intended resource (`modular_takeoff_tile` / active intent).

2. Add helper implementations in [`src/modular_airport_cmd.cpp`](src/modular_airport_cmd.cpp), near existing reservation helpers:
- keep-set builder should read:
  - `v->taxi_path`, `v->taxi_path_index`, `v->taxi_current_segment`
  - `v->taxi_reserved_tiles`
  - `v->modular_runway_reservation`
  - `v->modular_ground_target`, `v->modular_takeoff_tile`
- reconcile function should only release tiles currently owned by `v->index` and not in keep-set.
- runway keep logic should operate at contiguous-runway resource level (not per tile):
  - if any future need references resource `R`, keep full `R`.
- keep-set must be deduplicated before reconcile:
  - `std::sort(keep_set.begin(), keep_set.end(), by tile id)`
  - `keep_set.erase(std::unique(...), keep_set.end())`
  - this is required because overlapping rules (segment horizon, intent tiles, landing-chain tiles) can add the same tile multiple times.

3. Keep behavior unchanged initially:
- Call helpers only in debug mode from `LogModularVehicleReservationState` path to compare:
  - current owned set vs computed keep-set
  - release candidates count
- Do not mutate ownership yet in this phase.

4. Struct additions (optional, debug-only) in [`src/aircraft.h`](src/aircraft.h):
- `uint16_t debug_last_keep_set_size`
- `uint16_t debug_last_release_count`
- Keep these non-saveload, same as other modular runtime fields.

### Phase 2: Preserve Takeoff Runway Ownership

1. Remove non-runway auto-clear in movement loop:
- Edit [`AirportMoveModular`](src/modular_airport_cmd.cpp) around current post-step block that does:
  - `if (!v->modular_runway_reservation.empty()) ... ClearModularRunwayReservation(v);`
- Replace with:
  - retain when `ShouldRetainRunwayReservation(v, st)` is true
  - otherwise clear as before.

2. Stop clearing runway reservation in takeoff requeue path:
- Edit `HandleModularGroundArrival` case `MGT_RUNWAY_TAKEOFF` in [`src/modular_airport_cmd.cpp`](src/modular_airport_cmd.cpp), current `if (!on_runway)` block.
- Remove unconditional:
  - `ClearModularRunwayReservation(v);`
  - `ClearModularAirportReservationsByVehicle(st, v->index, v->tile);`
- Keep only current-tile reservation refresh and goal assignment.
- Additionally, if existing runway reservation does not match current intended takeoff runway resource, release the mismatched runway resource explicitly.

3. Fix deny path in `TryReserveContiguousModularRunway`:
- In [`src/modular_airport_cmd.cpp`](src/modular_airport_cmd.cpp), branches for:
  - `state-held`
  - `occupied`
  - `reserved`
- do not clear existing runway reservation if `v->modular_runway_reservation` already equals requested contiguous runway set.
- only clear when reservation differs/stale.
- if deny occurs for requested runway `R_new` while owning different `R_old`, release `R_old` (intent changed) before requeue.

4. Protect emergency clears:
- Keep existing clears in:
  - crash/destructor paths in [`src/aircraft_cmd.cpp`](src/aircraft_cmd.cpp)
  - takeoff completion in `AirportMoveModularTakeoff`
  - zeppeliner and recovery paths.

5. Validate:
- check `/tmp/openttd.log` for aircraft with `tgt=4` retaining `owned_rw > 0` while off-runway.
- ensure no increase in long `stuck(reserve)` on same routes.

### Phase 3: Full Minimal-Reservation Reconcile

1. Switch movement loop to reconcile-based release:
- In [`AirportMoveModular`](src/modular_airport_cmd.cpp):
- after successful step and arrival checks, call `BuildReservationKeepSet(...)`
- then `ReconcileAircraftReservations(...)`.
- During touchdown/rollout transition, pass context so keep-set includes landing-chain requirements until `taxi_path` is installed.

2. Integrate with segment reservation path:
- In `TryReserveTaxiSegment`:
  - keep acquisition logic (FREE_MOVE/ONE_WAY/RUNWAY) mostly intact
  - remove ad-hoc retention assumptions; rely on post-step reconcile for releasing extras.

3. Update runway-specific release helper:
- In `ReleaseRunwayReservationTile`, ensure it does not drop tiles still present in keep-set/future-use for turn-back paths.
- If needed, rename to reflect behavior (`ReleaseRunwayTileIfUnused`).

4. Normalize stand/taxi release:
- Replace stand-specific immediate release blocks in `AirportMoveModular` with reconcile outcome.
- Keep stand release special-case only if reconcile cannot run (early-return/error paths).

5. Update `ClearTaxiPathReservation` semantics:
- Preserve as explicit "state transition / force clear" API.
- avoid using it as normal per-step releaser once reconcile is active.

### Phase 4: Cleanup

1. Remove duplicated clear sites superseded by reconcile in [`src/modular_airport_cmd.cpp`](src/modular_airport_cmd.cpp):
- movement-loop off-runway clear
- takeoff non-runway pre-clear
- other local "clear then re-reserve" patterns that become redundant.

2. Keep only explicit transition clears:
- `AirportMoveModularTakeoff` completion
- crash/remove/teleport/layout-invalid transitions
- force clear fallback paths.

3. Strengthen diagnostics in `LogModularVehicleReservationState`:
- include computed keep-set size and owned-minus-keep count.
- emit warning when retained tiles are outside keep-set (post-phase target: zero).

4. Final file touch list review:
- [`src/modular_airport_cmd.cpp`](src/modular_airport_cmd.cpp) (primary logic)
- [`src/modular_airport_cmd.h`](src/modular_airport_cmd.h) (new helper declarations)
- [`src/aircraft.h`](src/aircraft.h) (optional debug fields / intent field if added)
- [`src/aircraft_cmd.cpp`](src/aircraft_cmd.cpp) (only if transition clears need alignment)
- docs (required): [`taxi_rules.md`](taxi_rules.md) must be updated in the same PR to reflect:
  - takeoff runway retention semantics
  - reserve-then-reconcile release model
  - runway turn-back retention rule and release conditions.

5. Documentation gate (required before merge):
- Update [`taxi_rules.md`](taxi_rules.md) with a "Reservation V2" section.
- Include old-vs-new behavior notes for:
  - free-move non-runway reservation handling
  - denial behavior when already owning runway resource
  - per-step deterministic release policy.

## Logging and Diagnostics

Add rate-limited debug lines:

- `reserve-v2 keep-set size=X release=Y reason=...`
- `reserve-v2 runway-retain target=... why=takeoff_intent|future_reuse`
- `reserve-v2 runway-release target=... why=no_future_need|intent_changed`

Extend reserve-state dump with:

- `keep_set_count`
- `future_runway_uses`
- `takeoff_runway_target`

## Test Plan

1. Unit-like behavior checks (simulation-driven):
- takeoff queue with two aircraft competing for same runway
- free-move to runway with temporary state-held deny
- runway turn-back path where same runway tile is used twice
- runway crossing path preserving crossing atomicity

2. Regression scenarios from existing logs:
- aircraft stuck at pre-runway free-move tile
- runway reservation flapping deny/grant cycles
- landing-chain continuity after touchdown and path rebuild

3. Invariant assertions (debug builds):
- no owned tile outside keep-set after reconcile
- no unowned tile in tracked runway list
- no map reservation owned by aircraft after terminal/hangar completion unless justified

## Risks and Mitigations

1. Risk: over-release causes oscillation/reacquire churn
- Mitigation: keep-set includes short forward horizon and runway intent resource.

2. Risk: under-release causes starvation
- Mitigation: explicit reconcile and invariant checks each step.

3. Risk: runway turn-back false-negative release
- Mitigation: future-occurrence tracking before release.

4. Risk: hidden dependency on legacy clear side effects
- Mitigation: phased rollout with verbose diagnostics and comparison logs.

## Completion Criteria

1. No takeoff-intent aircraft loses required runway reservation unless intent/target changes.
2. Taxi, stand, and runway reservations are released immediately after last required use.
3. `tracked_not_owned == 0` and `owned_rw_not_tracked == 0` in steady-state logs.
4. Existing stuck/fallback rates do not regress relative to baseline scenarios.
