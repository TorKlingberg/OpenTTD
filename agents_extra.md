# Modular Airport — Detailed Reference

Extended reference material moved from CLAUDE.md. See CLAUDE.md for the concise version.

## Key Data Structures

```cpp
// Per-tile metadata stored in station (base_station_base.h)
struct ModularAirportTileData {
    TileIndex tile;
    uint8_t piece_type;         // AirportTiles enum value
    uint8_t rotation;           // 0-3
    uint8_t user_taxi_dir_mask; // One-way direction bits (N=1,E=2,S=4,W=8)
    bool one_way_taxi;          // Whether this tile has one-way constraint
    uint8_t auto_taxi_dir_mask; // Computed from piece_type + rotation
    uint8_t runway_flags;       // RUF_LANDING|RUF_TAKEOFF|RUF_DIR_LOW|RUF_DIR_HIGH
    uint8_t edge_block_mask;    // Fence edges blocking taxi (N=0x01, E=0x02, S=0x04, W=0x08)
};

// Classified taxi path (airport_ground_pathfinder.h)
struct TaxiPath {
    std::vector<TileIndex> tiles;      // A* path from start to goal
    std::vector<TaxiSegment> segments; // Segment decomposition of the tile list
    bool valid;
};

// Aircraft modular fields (aircraft.h:87-101)
TaxiPath *taxi_path;              // Current active path (owned, heap-allocated)
uint16_t taxi_path_index;         // Current tile index in taxi_path->tiles
uint8_t  taxi_current_segment;    // Current segment index
std::vector<TileIndex> taxi_reserved_tiles; // Tiles this aircraft has reserved
uint16_t taxi_wait_counter;       // Ticks waiting for a segment reservation
TileIndex ground_path_goal;       // Destination tile
uint8_t  modular_ground_target;   // MGT_NONE/TERMINAL/HELIPAD/HANGAR/RUNWAY_TAKEOFF/ROLLOUT
TileIndex modular_landing_tile;   // Runway tile committed for landing
TileIndex modular_landing_goal;   // Stand pre-selected at landing commit time
TileIndex modular_takeoff_tile;   // Runway tile for takeoff
std::vector<TileIndex> modular_runway_reservation; // Reserved runway tiles
uint32_t modular_holding_wp_index; // Per-aircraft position on holding loop (UINT32_MAX = uninitialised, not saved)
```

## Aircraft State Machine

```
FLYING
  → AirportMoveModularFlying: holding pattern + approach
  → AircraftEventHandler_Flying: tries TryReserveLandingChain(runway + exit path)
  → if success: state = LANDING, modular_landing_tile set

LANDING / HELILANDING
  → AirportMoveModularLanding: two-stage approach (stage 0=approach, 1=final)
  → on touchdown: AircraftEventHandler_Landing → ENDLANDING

ENDLANDING / HELIENDLANDING
  → AircraftEventHandler_EndLanding: sets ground_path_goal + modular_ground_target
  → MGT_ROLLOUT: taxi to runway exit, then find stand
  → MGT_TERMINAL / HELIPAD / HANGAR: taxi directly to parking

TERM1 / HANGAR  (ground_path_goal set)
  → AirportMoveModular: segment-based taxi

TERM1 / HANGAR  (ground_path_goal == INVALID_TILE, parked)
  → AircraftEventHandler_AtTerminal / InHangar: waits for orders
  → on departure: FindModularRunwayTileForTakeoff, sets MGT_RUNWAY_TAKEOFF

HandleModularGroundArrival(MGT_RUNWAY_TAKEOFF)
  → TryReserveContiguousModularRunway → if success: state = TAKEOFF

TAKEOFF / STARTTAKEOFF / ENDTAKEOFF
  → AirportMoveModularTakeoff: runway roll + climb → state = FLYING
```

## Segment-Based Reservation

`AirportMoveModular` uses `BuildTaxiPath` (A* topology-only, no reservation checks) then moves tile-by-tile through segments:

**Free-move segment**: All tiles reserved atomically before entering + the first tile of the next segment ("exit guarantee"). Released when the aircraft crosses into the next segment. If reservation fails, aircraft waits at the boundary and retries.

**One-way segment**: Reserve next tile, move, release previous. Safe for queuing because traffic is unidirectional.

**Runway**: `TryReserveContiguousModularRunway` atomically reserves all tiles of the contiguous runway. Landing uses `TryReserveLandingChain` (runway + exit path) before committing to land.

Key helper functions in `modular_airport_cmd.cpp`:
- `TryReserveLandingChain` — reserves runway + exit path before landing commit
- `SetTaxiReservation` / `ClearTaxiReservation` — per-tile reservation wrappers
- `ClearTaxiPathState` — releases all non-runway reservations and frees `taxi_path`
- `TryRetargetModularGroundGoal` — on pathfinding failure, tries alternate stand/hangar
- `FindModularRunwayTileForTakeoff` — scores runways, checks free-move segment reservability
- `FindFreeModularTerminal` / `FindFreeModularHelipad` / `FindFreeModularHangar` — pick best available parking

## Additional Pitfalls

- Modular hangar visuals are rotation-driven in draw code; pathfinding/logic must use `piece_type` from modular tile metadata, not inferred sprite gfx.
- For modular hangars, the rotation-to-exit mapping used for robust E/W behavior is inverse-style (`rot=0..3 -> SE, NE, NW, SW`) when assigning directional hangar piece type.

## LLDB Quick Workflow

```bash
# Run debug build under scripted LLDB breakpoints (__assert_rtn/abort/__cxa_throw)
/Users/tor/ttd/OpenTTD/scripts/run_lldb_debug.sh

# Attach to a running game when it's stuck
ps aux | grep openttd
lldb -p <pid>
```
In LLDB, use `thread backtrace all` (or `btall` in scripted sessions) to capture all stacks.
