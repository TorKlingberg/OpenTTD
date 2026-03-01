# Plan: Clean Up Modular Airport Reservation Heuristics

## Context

Modular airport ground reservations use tile-level bits (in the map array) paired with per-vehicle tracking vectors (`taxi_reserved_tiles`, `modular_runway_reservation`). The vectors are **not saved to disk**, so on load every reserved tile is orphaned. Multiple heuristic sweeps and fallbacks exist to paper over this and other desync scenarios. This plan removes the root cause (save/load orphans) and simplifies the code.

## Changes

### 1. Afterload sweep — clear all modular airport tile reservations on load

**File:** `src/saveload/station_sl.cpp` — `AfterLoadStations()` (line 111)

Add a block after the existing station iteration loop (after line 132) that iterates all `Station`s, checks for `modular_tile_data`, and unconditionally clears every airport tile reservation bit. On load, no vehicle has populated its tracking vectors yet, so every tile reservation is orphaned. Aircraft will re-establish reservations as they resume movement.

```cpp
/* Clear modular airport tile reservations — tracking vectors are not saved,
 * so all map-level reservation bits are orphaned on load. Aircraft will
 * re-establish them as they resume ground movement. */
for (Station *st : Station::Iterate()) {
    if (st->airport.modular_tile_data == nullptr) continue;
    for (const ModularAirportTileData &data : *st->airport.modular_tile_data) {
        if (!IsValidTile(data.tile)) continue;
        Tile t(data.tile);
        if (!IsAirportTile(t)) continue;
        if (HasAirportTileReservation(t)) {
            SetAirportTileReservation(t, false);
        }
    }
}
```

Needs `#include "../station_map.h"` added to station_sl.cpp (for `HasAirportTileReservation` / `SetAirportTileReservation`). Other includes (`station_base.h`, `vehicle_base.h`) are already present.

### 2. Remove "untracked intent" branch from `TryClearStaleModularReservation`

**File:** `src/modular_airport_cmd.cpp` — `TryClearStaleModularReservation()` (line 2146)

Keep the three safe branches:
- **Invalid vehicle** (lines 2154-2159) — always correct
- **Not normal aircraft** (lines 2162-2167) — always correct
- **Not on ground / not tied to station** (lines 2203-2210) — safe, covers departed aircraft

Remove the "untracked intent" heuristic (lines 2212-2219) — the risky branch that clears reservations when the tile isn't in any tracking vector. Replace the clear+return with `return false` (don't clear — let the reservation stand).

The remaining code structure after the change:
- Lines 2146-2167: invalid vehicle / not normal aircraft → clear (safe)
- Lines 2169-2200: runway flow exemption + explicit tracking checks → return false (not stale)
- Lines 2202-2210: not tied to station / not on ground → clear (safe, `[FALLBACK]` logged)
- New line ~2212: `return false;` (aircraft is on ground here, tile untracked but we trust it)

### 3. Remove orphan reservation scans from both functions

**File:** `src/modular_airport_cmd.cpp`

**`ClearModularRunwayReservation()`** (line 316): Delete lines 328-346 (the `static uint8_t scan_counter` block and entire periodic scan). Keep lines 316-327 (vector-based clear).

**`ClearTaxiPathReservation()`** (line 2469): Delete lines 2526-2548 (the `static uint8_t taxi_scan_counter` block and entire periodic scan). Keep everything else (preservation logic, force-clear logging, keep-tile re-add).

### 4. Remove runway crossing FIFO queue

**File:** `src/modular_airport_cmd.cpp`

Delete:
- `ModularCrossingQueueState` struct (lines 2589-2591)
- `_modular_crossing_queues` global (line 2593)
- `BuildModularCrossingQueueKey()` (lines 2595-2606)
- `CanGrantRunwayCrossingNow()` (lines 2608-2645)
- `MarkRunwayCrossingGranted()` (lines 2647-2653)

Modify `TryReserveTaxiSegment()` runway crossing path (lines 2786-2801):
- Remove the `CanGrantRunwayCrossingNow` check (line 2794)
- Remove the `MarkRunwayCrossingGranted` call (line 2800)
- Keep: exit tile reservation check (2788-2791), `TryReserveRunwayResourcesAtomic` (2795), `SetTaxiReservation` for hold+exit tiles (2798-2799)

Result: crossing uses pure reservation contention — whoever reserves first, wins. No FIFO queue, no timestamp tracking, no stale-entry sweeps.

### 5. Remove Tier 4 from takeoff runway selection

**File:** `src/modular_airport_cmd.cpp` — `FindModularRunwayTileForTakeoff()` (line 2226)

Delete:
- `best_compatible_fallback_tile` / `best_compatible_fallback_score` declarations (lines 2270-2271)
- The fallback scoring block (lines 2306-2327) — the `fallback_reachable` check, Manhattan distance scoring, and `best_compatible_fallback_tile` update
- The Tier 4 return block (lines 2395-2398) — the debug log and `return best_compatible_fallback_tile`

After the change, the return sequence becomes:
```cpp
if (best_non_runway_taxi_tile != INVALID_TILE) return best_non_runway_taxi_tile;
if (best_path_tile != INVALID_TILE) return best_path_tile;
if (best_blocked_tile != INVALID_TILE) return best_blocked_tile;
return INVALID_TILE;
```

When `INVALID_TILE` is returned, call sites already handle it:
- `AirportMoveModularTakeoff` (line 1712): transitions aircraft to terminal state
- `ModularAircraftMovement` (line 3054): aircraft stays at current tile

## Verification

1. Build: `scripts/build_and_sign.sh`
2. Load a savegame with active ground traffic: verify no reservations are stuck from load (grep for `stale-clear` in logs — should see none from save/load orphans)
3. Test normal taxi operations: aircraft land, taxi to stands, taxi to runways, take off
4. Test runway crossing: aircraft cross active runways without FIFO queue (pure contention)
5. Test takeoff with all runways blocked: aircraft should wait at stand rather than picking a Manhattan-distance fallback
6. Verify `[FALLBACK]` log lines — remaining ones (`invalid_vehicle`, `not_normal_aircraft`, `not_on_ground`, `force-clear-all`) indicate real issues to investigate
