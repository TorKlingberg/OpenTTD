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
| `scripts/parse_airport_template.py` | Visualize template JSON files (`--grid`, `--detail`, `--runways`, `--raw`). See `skills/airport_template_analysis.md`. |

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

## Saveload

Modular tile data is saved via `ModularAirportTileDataDesc` in `src/saveload/station_sl.cpp`. Aircraft modular fields (`taxi_path`, `taxi_reserved_tiles`, `modular_holding_wp_index`, etc.) are **not** saved — recomputed on load. `taxi_path` is a heap pointer and must never be saved.

## Common Pitfalls

- `GetModularTileData(tile)` returns `nullptr` if the tile isn't in the modular layout — always null-check.
- `FindAirportGroundPath` with `v=nullptr` ignores stand occupancy (topology only); with `v=aircraft` avoids occupied stands that aren't the goal.
- Runway flags (`RUF_LANDING`, `RUF_TAKEOFF`, `RUF_DIR_LOW`, `RUF_DIR_HIGH`) propagate to all tiles in a contiguous runway via `CmdSetRunwayFlags`.
- `AirportTiles` IDs `>= NEW_AIRPORTTILE_OFFSET` (74) are treated as NewGRF airport tiles. Do not store new modular default-tile IDs in map gfx; keep canonical gfx IDs and branch drawing from modular metadata.
- Depot windows can outlive tile deletion. Guard depot UI reads with a valid depot tile check.
- Rotation invariants for modular airports:
  - Hangar directional convention is `0=SE, 1=NE, 2=NW, 3=SW` (clockwise in world space). Keep this mapping consistent across build/rotate/draw/pathfinder code.
  - For preview projection, `iso_x` must use `(dy - dx)` (not `(dx - dy)`), otherwise the entire preview is mirrored.
  - Legacy small runway ends (`NEAR`/`FAR`) swap on odd quarter-turns; after template rotation, preview should normalize each contiguous segment so low-end is `FAR` and high-end is `NEAR`, matching placed tiles.

## GUI Pitfalls

- **`PickerWindowBase::Close()` calls `ResetObjectToPlace()`** — child picker windows (hangar, cosmetic, helipad) must override `Close()` with `this->Window::Close()` to avoid stealing the parent's placement cursor.
- **`SetObjectToPlace` triggers `OnPlaceObjectAbort` on the current cursor owner** — when changing cursor ownership from within the same window (e.g. fence tool activation), wrap the call in `this->updating_cursor = true/false` to suppress the abort callback.
- **`CloseWindowByClass` can trigger `ResetObjectToPlace` chains** — closing a `PickerWindowBase` sub-window triggers its `Close()` → `ResetObjectToPlace()` → `OnPlaceObjectAbort` on whoever owns the cursor. Guard with `updating_cursor` or override `Close()`.
- **Sub-tile click position**: use `_tile_fract_coords.x/.y` (0–15 in world X/Y), set by the viewport on every click. Same mechanism as the autoroad tool. Do NOT use `InverseRemapCoords` — it doesn't give tile-relative positions.
- **Widget `SetPIPRatio(left, mid, right)`**: controls how extra space is distributed. `(0,0,1)` = left-aligned, `(1,0,1)` = centered, `(1,0,0)` = right-aligned.
- **Helicopter landing stage**: `AircraftEventHandler_Flying` in `aircraft_cmd.cpp` sets `modular_landing_stage = 0` before `AirportMoveModularLanding` runs. Helipad-specific overrides (like skipping the FAF approach) must go in `aircraft_cmd.cpp` right after that assignment, not in the landing movement code.

## Holding Loop Pitfalls

- Don't use `tick_counter` or `running_ticks` for phase timing — both are `uint8_t` and wrap at 256. Use `TimerGameTick::counter` (uint64_t monotonic).
- Movement must be unconditional — don't guard `UpdateAircraftSpeed` inside `if (dist > 0)`.
- Use ghost for movement, nearest-waypoint only for gate checks.
- Reset `modular_holding_wp_index` to `UINT32_MAX` on landing commit.
