# Build-As-Modular Stock Airports

## Goal
Add a switch on the stock airport picker (`Build airport` window) named **Build as modular**.

When enabled, selecting a stock airport should place a **modular airport approximation** of that stock layout, instead of a classic fixed-layout airport.

## Where to hook this

### UI entry point
- `src/airport_gui.cpp`
- `src/widgets/airport_widget.h`
- `src/lang/english.txt`

The stock picker is `BuildAirportWindow` in `src/airport_gui.cpp`.

Implementation sketch:
1. Add a new widget ID in `AirportPickerWidgets`, e.g. `WID_AP_BUILD_AS_MODULAR`.
2. Add a checkbox/text button row in `_nested_build_airport_widgets`.
3. Add a string, e.g. `STR_STATION_BUILD_AIRPORT_AS_MODULAR` in `src/lang/english.txt`.
4. Store state as a static/global bool near `_selected_airport_*` (same pattern as coverage toggle).
5. In `PlaceAirport(TileIndex tile)`, branch:
   - `false`: current `CMD_BUILD_AIRPORT`
   - `true`: new modular-template build command.

## Command-level implementation (recommended)

Do **not** emit dozens of `CMD_BUILD_MODULAR_AIRPORT_TILE` posts from UI. That would be non-atomic and hard to keep consistent.

Add one command, e.g.:
- `CmdBuildModularAirportFromStock(DoCommandFlags flags, TileIndex tile, uint8_t airport_type, uint8_t layout, StationID station_to_join, bool allow_adjacent)`

Likely files:
- `src/station_cmd.h` (declaration + `DEF_CMD_TRAIT`)
- `src/station_cmd.cpp` (implementation)
- `src/command_type.h` (new command enum)

### Why one command
- Single test/execute pass
- Proper cost aggregation
- Clean failure semantics
- Easier multiplayer determinism
- Easier to preserve town-noise and authority checks from `CmdBuildAirport`

## Critical parity requirements

### 1) Preserve airport economics/spec identity
Current modular tile placement initializes new modular airports as `AT_SMALL` in `CmdBuildModularAirportTile`.

For this feature, set station airport type to the selected stock type (`airport_type`) so catchment/noise/maintenance remain correct:
- catchment and noise UI/logic use `st->airport.GetSpec()`
- maintenance uses `st->airport.GetSpec()->maintenance_cost`

### 2) Reuse stock airport prechecks
Reuse the same checks from `CmdBuildAirport`:
- airport availability/layout bounds
- flatness/map bounds
- station spread
- town authority/noise restrictions
- join rules

### 3) Functional tile conversion pass
Stock layouts include decorative runway/fence tiles that are not functional in modular mode.

In modular conversion, apply substitutions:
- `APT_RUNWAY_END_FENCE_*` -> `APT_RUNWAY_END`
- `APT_RUNWAY_FENCE_NW` -> `APT_RUNWAY_5` (only where you intend a functional runway lane)
- `APT_HELIPORT` -> `APT_HELIPAD_2` (functional helipad for modular logic)
- `APT_STAND_PIER_NE` -> `APT_STAND` (count as terminal stand)

Everything else can stay as stock gfx unless it breaks connectivity.

### 4) Metadata pass
For each placed modular tile, populate `ModularAirportTileData` exactly as modular builder does:
- `piece_type`
- `rotation`
- `auto_taxi_dir_mask`
- `user_taxi_dir_mask`
- `one_way_taxi`
- `runway_flags`

Then mark:
- `modular_tile_index_dirty = true`
- `modular_holding_loop_dirty = true`

## Suggested stock -> modular templates

Legend:
- `R` = large runway middle (`APT_RUNWAY_5`/`APT_RUNWAY_2` etc.)
- `E` = runway end (`APT_RUNWAY_END`)
- `r` = small runway middle (`APT_RUNWAY_SMALL_MIDDLE`)
- `e` = small runway end (`APT_RUNWAY_SMALL_FAR_END` / `APT_RUNWAY_SMALL_NEAR_END`)
- `A` = taxi/apron
- `S` = stand/terminal stand
- `H` = hangar (large or small)
- `P` = helipad
- `B` = cosmetic/building/tower/radar
- `G` = grass/empty

Coordinates: x grows left->right, y grows top->bottom.

### 1) Small (Country) 4x3
```text
y0  B B B H
y1  G G G G
y2  e r r e
    0 1 2 3 (x)
```
Notes:
- Already mostly modular-compatible.
- Keep small runway as-is.

### 2) Commuter 5x4
```text
y0  B B P P H
y1  A A A A A
y2  A S S S A
y3  E R R R E
    0 1 2 3 4
```
Notes:
- Convert runway fence ends to `E`.

### 3) City 6x6
```text
y0  B A S A A H
y1  B B B S A A
y2  B S B A A A
y3  B A A A A B
y4  G A A A A B
y5  E R R R R E
    0 1 2 3 4 5
```
Notes:
- Map `APT_STAND_PIER_NE` to `S`.
- Convert runway fence ends to `E`.

### 4) Metropolitan 6x6
```text
y0  B A S A A H
y1  B B B S A A
y2  B S B A A A
y3  B A A A A B
y4  E R R R R E
y5  E R R R R E
    0 1 2 3 4 5
```
Notes:
- Upper runway already uses functional pieces.
- Lower runway needs fence-end conversion.

### 5) International 7x7
```text
y0  B B B B B B B
y1  B A A A A A H
y2  B A S B S A A
y3  H A S B S A P
y4  A A S B S A P
y5  A A A A A A B
y6  E R R R R R E
    0 1 2 3 4 5 6
```
Notes:
- Keep top fence strip cosmetic for first pass.
- Functional runway is bottom row with end conversion.

### 6) Intercontinental 9x11
```text
y00  B B B B B B B B B
y01  E R R R R R R E A
y02  A G A A A A A B A
y03  A A A B P P A A A
y04  A A A S B S A B H
y05  H B A S B S A A A
y06  A A A S B S A A A
y07  A A A S B S A A A
y08  A G A A A A A G A
y09  B B B B B B B B B
y10  E R R R R R R E G
      0 1 2 3 4 5 6 7 8
```
Notes:
- Use rows `y01` and `y10` as the two functional runways.
- Keep rows `y00` and `y09` as decorative runway-shoulder visuals.

### 7) Heliport 1x1
```text
y0  P
    0
```
Notes:
- Convert `APT_HELIPORT` to modular-supported helipad (`APT_HELIPAD_2`).

### 8) Helidepot 2x2
```text
y0  B H
y1  P A
    0 1
```
Notes:
- Keep helipad variant functional.

### 9) Helistation 4x2
```text
y0  H B P P
y1  A A A P
    0 1 2 3
```
Notes:
- Helipad fence variants are modular-supported.

## Per-airport viability and parity check

Legend for answers:
- `Yes` = expected to match target behavior.
- `Approx` = playable, but not exact stock behavior.
- `No` = does not meet requirement yet.

### 1) Small (Country)
- Will it work end-to-end (land -> stand -> hangar -> takeoff): `Yes`
- Uses same runway(s) as stock: `Yes` (single small runway)
- Same landing/takeoff direction as stock: `Approx` unless runway flags are explicitly set
- Right apron tiles one-way: `No` (recommend none)

### 2) Commuter
- Will it work end-to-end: `Yes`
- Uses same runway(s) as stock: `Yes` (single bottom runway)
- Same direction as stock: `Approx` unless runway flags are explicitly set
- Right apron tiles one-way: `Approx`; recommend one-way on row `y1` eastbound and row `y2` westbound

### 3) City
- Will it work end-to-end: `Yes`
- Uses same runway(s) as stock: `Yes` (single bottom runway)
- Same direction as stock: `Approx` unless runway flags are explicitly set
- Right apron tiles one-way: `Approx`; recommend clockwise loop:
  - `y3` eastbound, `x4` southbound, `y4` westbound, `x1` northbound

### 4) Metropolitan
- Will it work end-to-end: `Yes`
- Uses same runway(s) as stock: `Yes` (two parallel bottom runways)
- Same direction as stock: `Approx` unless each runway gets explicit flags
- Right apron tiles one-way: `Approx`; recommend clockwise apron loop between terminal area and runways

### 5) International
- Will it work end-to-end: `Yes`
- Uses same runway(s) as stock: `No` in MVP template (only bottom runway functional)
- Same direction as stock: `No` in MVP template
- Right apron tiles one-way: `Approx`; recommend one-way perimeter loop to avoid apron head-on conflicts

### 6) Intercontinental
- Will it work end-to-end: `Yes`
- Uses same runway(s) as stock: `Yes` (two functional runways: `y01` and `y10`)
- Same direction as stock: `Approx` unless per-runway flags are set to mirror stock
- Right apron tiles one-way: `Approx`; strongly recommend one-way loop on center apron corridors due to traffic volume

### 7) Heliport
- Will it work end-to-end: `N/A` for fixed-wing; `Yes` for helicopters
- Uses same runway(s) as stock: `N/A`
- Same direction as stock: `N/A`
- Right apron tiles one-way: `N/A`

### 8) Helidepot
- Will it work end-to-end: `N/A` for fixed-wing; `Yes` for helicopters
- Uses same runway(s) as stock: `N/A`
- Same direction as stock: `N/A`
- Right apron tiles one-way: `No` (not needed)

### 9) Helistation
- Will it work end-to-end: `N/A` for fixed-wing; `Yes` for helicopters
- Uses same runway(s) as stock: `N/A`
- Same direction as stock: `N/A`
- Right apron tiles one-way: `No` (not needed)

## Direction + one-way policy needed for real parity

To satisfy your four questions strictly (not just approximately), the stock->modular command should include two explicit post-passes:

1. Runway direction parity pass:
- Apply `CmdSetRunwayFlags` per runway segment after all runway tiles exist.
- Keep this data-driven per stock airport template (runway id -> landing/takeoff + low/high direction).

2. One-way apron parity pass:
- Apply `CmdSetTaxiwayFlags` on selected apron tiles.
- Use a per-template list of `(x, y, dir)` entries for tiles that must be one-way.
- Keep all other apron tiles unrestricted.

Without these two passes, airports will generally be playable, but directionality and apron flow will only be approximate.

## Build order strategy

Inside the new command:
1. Resolve and validate full area first.
2. Create/join station exactly once.
3. Place all tiles in deterministic scan order (top-left to bottom-right).
4. Build modular metadata for every tile.
5. Run a final runway normalization pass (contiguous flag propagation).

## Known risks / open choices

1. Runway direction policy:
- MVP: keep default modular runway flags.
- Better: set per-template direction defaults to match stock behavior.

2. International top strip:
- MVP keeps it cosmetic.
- If desired, second pass can convert top strip into a second functional runway.

3. Visual fidelity vs connectivity:
- Some edge apron-fence tiles are cosmetic and non-taxiable in modular logic; preserve look first, then adjust only if pathfinding dead-ends appear in testing.

## Minimal test matrix

1. Build each of 9 stock airports with toggle off: behavior unchanged.
2. Build each of 9 stock airports with toggle on: station is modular and functional.
3. Aircraft lifecycle for each modularized airport:
- enter
- land
- taxi to stand/helipad
- taxi to runway
- takeoff
4. Check catchment/noise/maintenance match selected stock type.
5. Save/load and verify modular metadata survives.
