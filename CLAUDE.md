# Working on OpenTTD (macOS)

## Build

The build directory is `OpenTTD/build/`. To rebuild after source changes:

```bash
/Users/tor/ttd/OpenTTD/scripts/build_and_sign.sh
```

Equivalent manual command (if needed):
```bash
make -j8 -C /Users/tor/ttd/OpenTTD/build && codesign -s - --deep --force /Users/tor/ttd/OpenTTD/build/openttd
```

If you need to reconfigure (e.g. after cmake file changes):
```bash
cd /Users/tor/ttd/OpenTTD/build
cmake .. \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_OSX_SYSROOT=/Library/Developer/CommandLineTools/SDKs/MacOSX15.4.sdk \
  -DCMAKE_CXX_FLAGS="-I/Library/Developer/CommandLineTools/SDKs/MacOSX15.4.sdk/usr/include/c++/v1" \
  -DCMAKE_OBJCXX_FLAGS="-I/Library/Developer/CommandLineTools/SDKs/MacOSX15.4.sdk/usr/include/c++/v1"
```

For a debug build (slower, full symbols, unoptimised):
```bash
cmake .. -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_CXX_FLAGS="-I/Library/Developer/CommandLineTools/SDKs/MacOSX15.4.sdk/usr/include/c++/v1" \
  -DCMAKE_OBJCXX_FLAGS="-I/Library/Developer/CommandLineTools/SDKs/MacOSX15.4.sdk/usr/include/c++/v1"
```

**Common build failures:**
- `algorithm file not found` — missing `-DCMAKE_CXX_FLAGS` above
- `cannot find libatomic` — apply fix to `cmake/3rdparty/llvm/CheckAtomic.cmake`: change `if(MSVC)` to `if(MSVC OR APPLE)` at lines 52 and 75

## Run and Debug

```bash
# Load a savegame with full modular airport logging
/Users/tor/ttd/OpenTTD/build/openttd -g ~/Documents/OpenTTD/save/SAVENAME.sav -d misc=3 2>/tmp/openttd.log
```

Useful flags: `-g savegame.sav` (load save), `-d misc=3` (debug level), `-f` (fullscreen), `-r 1920x1080` (resolution).

Log files:
- Debug output: `/tmp/openttd.log` (from the command above)
- Crash logs: `~/Documents/OpenTTD/crash*.json.log`
- Game saves: `~/Documents/OpenTTD/save/`

Filter modular airport log lines:
```bash
grep '\[ModAp\]' /tmp/openttd.log | tail -100
```

LLDB quick workflow (what worked):
```bash
# Run debug build under scripted LLDB breakpoints (__assert_rtn/abort/__cxa_throw)
/Users/tor/ttd/OpenTTD/scripts/run_lldb_debug.sh

# Attach to a running game when it's stuck
ps aux | grep openttd
lldb -p <pid>
```
In LLDB, use `thread backtrace all` (or `btall` in scripted sessions) to capture all stacks.

Key log patterns:
- `V{id} stuck(no-path)` — pathfinder found no topology path (bad airport layout or genuine dead end)
- `V{id} seg-wait FREE_MOVE` — waiting for atomic free-move segment reservation (normal queuing)
- `V{id} seg-wait ONE_WAY` — waiting for next one-way tile (normal queuing)
- `V{id} landing-chain fail` — can't land; runway+exit both blocked; stays in holding pattern

## Coordinate System

OpenTTD uses an isometric view. Tiles are on a rectangular (X, Y) grid; each tile is `TILE_SIZE` = 16 pixel-units wide.

| Coordinate change | Screen appearance |
|---|---|
| X increases | moves right-down diagonally |
| Y increases | moves left-down diagonally |

Use screen-relative terms (up/down/left/right), not compass directions — all axis-aligned moves appear diagonal on screen. See `coords.md` for details.

---

# Modular Airports

The modular airport system lets players build airports tile-by-tile. The reservation design is in `taxi_rules.md`.

## Key Source Files

| File | Purpose |
|------|---------|
| `src/modular_airport_cmd.cpp` | All modular airport movement, reservation, holding-loop, and pathfinding logic (~2500 lines). |
| `src/modular_airport_cmd.h` | Declarations + inline helpers (`IsModularRunwayPiece`, `IsRunwayPieceOnAxis`, MGT_* constants). |
| `src/modular_airport_gui.cpp` | Modular airport builder UI (`BuildModularAirportWindow` + hangar/cosmetic pickers). |
| `src/modular_airport_gui.h` | `ShowBuildModularAirportWindow` + shared GUI globals. |
| `src/aircraft_cmd.cpp` | Classic FTA state machine, event handlers, shared mechanics (`UpdateAircraftSpeed`, etc.). |
| `src/aircraft.h` | Aircraft struct. Modular fields at lines 87-101. |
| `src/airport_ground_pathfinder.cpp` | A* ground pathfinder + segment classification |
| `src/airport_ground_pathfinder.h` | `TaxiPath`, `TaxiSegment`, `TaxiSegmentType`, `BuildTaxiPath` |
| `src/base_station_base.h` | `ModularAirportTileData` struct (per-tile metadata) |
| `src/station_map.h` | Tile reservation functions: `HasAirportTileReservation`, `SetAirportTileReservation`, `GetAirportTileReserver` |
| `src/table/airporttile_ids.h` | `AirportTiles` enum: `APT_STAND`, `APT_APRON`, `APT_RUNWAY_*`, `APT_DEPOT_*`, etc. |
| `src/station_cmd.cpp` | `CmdBuildModularAirportTile`, `CmdSetTaxiwayFlags`, `CmdSetRunwayFlags` |
| `src/airport_gui.cpp` | Shared airport toolbar + classic FTA airport picker UI |

## Tile Classification

Every taxiable tile is one of three types used by the segment reservation system:

| Type | Condition | Reservation |
|------|-----------|-------------|
| `RUNWAY` | `IsModularRunwayPiece(piece_type)` — `APT_RUNWAY_1-5`, `APT_RUNWAY_END`, `APT_RUNWAY_SMALL_*` | Atomic: entire contiguous runway |
| `ONE_WAY` | `IsTaxiwayPiece(piece_type) && one_way_taxi == true` | Queue: one tile at a time |
| `FREE_MOVE` | Everything else (aprons, stands, hangars, fenced apron variants) | Atomic: entire segment at once |

Notes:
- Runway end fence variants (`APT_RUNWAY_END_FENCE_*`) are **not** in `IsModularRunwayPiece` — they're decorative. Only `APT_RUNWAY_END`, `APT_RUNWAY_SMALL_NEAR_END`, `APT_RUNWAY_SMALL_FAR_END` are landing targets.
- Hangars: `APT_DEPOT_SE/SW/NW/NE` (large) and `APT_SMALL_DEPOT_SE/SW/NW/NE` (small) — four rotations each. Hangars are multi-capacity (multiple aircraft can park in one).
- One-way flags only apply to `IsTaxiwayPiece` types. Stands, hangars, and runways cannot be one-way.

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

## Saveload

Modular tile data is saved via `ModularAirportTileDataDesc` in `src/saveload/station_sl.cpp`. Aircraft modular fields (`taxi_path`, `taxi_reserved_tiles`, `modular_holding_wp_index`, etc.) are **not** saved — recomputed on load. `taxi_path` is a heap pointer and must never be saved.

## Common Pitfalls

- `GetModularTileData(tile)` returns `nullptr` if the tile isn't in the modular layout — always null-check.
- `FindAirportGroundPath` with `v=nullptr` ignores stand occupancy (topology only); with `v=aircraft` avoids occupied stands that aren't the goal.
- Runway flags (`RUF_LANDING`, `RUF_TAKEOFF`, `RUF_DIR_LOW`, `RUF_DIR_HIGH`) propagate to all tiles in a contiguous runway via `CmdSetRunwayFlags`.
- `AirportTiles` IDs `>= NEW_AIRPORTTILE_OFFSET` (74) are treated as NewGRF airport tiles. Do not store new modular default-tile IDs in map gfx; keep canonical gfx IDs and branch drawing from modular metadata.
- Modular hangar visuals are rotation-driven in draw code; pathfinding/logic must use `piece_type` from modular tile metadata, not inferred sprite gfx.
- For modular hangars, the rotation-to-exit mapping used for robust E/W behavior is inverse-style (`rot=0..3 -> SE, NE, NW, SW`) when assigning directional hangar piece type.
- Depot windows can outlive tile deletion. Guard depot UI reads (`GetDepotDestinationIndex`, caption setup, vehicle list invalidation) with a valid depot tile check.
- **Holding loop — don't use `tick_counter` or `running_ticks` for phase timing**: both are `uint8_t` and wrap at 256 ticks. Use `TimerGameTick::counter` (uint64_t monotonic global) for loop phase calculations.
- **Holding loop — movement must be unconditional**: `UpdateAircraftSpeed` and the position-update loop must NOT be inside `if (dist > 0)`. If guarded, the plane freezes for up to 64 ticks whenever it catches its lookahead target.
- **Holding loop — use ghost for movement, nearest-waypoint only for gate checks**: using nearest-waypoint as the movement target locks each plane to the nearest section of the loop, causing separate orbits per runway. The ghost (time-based `TimerGameTick::counter`) ensures planes traverse the full unified Dubins loop past all runway gates.
- **Holding loop — reset `modular_holding_wp_index` on landing commit**: set to `UINT32_MAX` when entering LANDING state so the next FLYING entry reinitialises from the ghost phase at the correct position.
