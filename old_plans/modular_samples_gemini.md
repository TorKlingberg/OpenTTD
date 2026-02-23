# Modular Airport Samples - Implementation Plan

This document outlines how to implement a "Build as modular" switch in the stock airport builder, which creates a modular airport approximating the selected stock airport layout.

## 1. GUI Integration

### 1.1. Widget Definition
Add a new checkbox widget `WID_AP_BUILD_AS_MODULAR` to `src/widgets/airport_widget.h` and include it in the `_nested_build_airport_widgets` in `src/airport_gui.cpp`.

### 1.2. Window State
Add a static or window-local boolean `_build_as_modular` to track the state of the switch. Default should be `false`.

### 1.3. Placement Interception
Modify `PlaceAirport(TileIndex tile)` in `src/airport_gui.cpp`:
- If `_build_as_modular` is false, proceed with `CMD_BUILD_AIRPORT`.
- If `_build_as_modular` is true, call a new helper function `PlaceModularApproximation(tile, airport_type, layout)`.

## 2. Implementation of `PlaceModularApproximation`

This function will:
1. Look up the `AirportSpec` for the chosen `airport_type`.
2. Get the tile table for the chosen `layout`.
3. For each tile in the layout:
   - Determine its `AirportTiles` ID and relative offset `(dx, dy)`.
   - Map the ID to a functional modular piece type and rotation if necessary.
   - Call `Command<CMD_BUILD_MODULAR_AIRPORT_TILE>::Post(...)`.
   - The first tile should use `StationID::Invalid()` to create a new station; subsequent tiles should use the `StationID` of the first tile.
4. After building all tiles, call `CmdSetRunwayFlags` for each runway to configure landing/takeoff directions and usage.
5. Call `CmdSetTaxiwayFlags` for specific apron tiles to enable one-way traffic where needed to match stock flow.

## 3. Stock Airport Modular Approximations

### 3.1. Country Airfield (4x3)
- **Runway**: Y=2. Flags: `RUF_LANDING | RUF_TAKEOFF | RUF_DIR_LOW`. (Matches stock NW direction).
- **One-way**: Not strictly needed for 2 terminals, but can be added to the apron loop if desired.

```text
(0,0) B  B  B  H  (3,0)
(0,1) A  S  S  A  (3,1)
(0,2) Rf Rm Rm Rn (3,2)
```

### 3.2. Commuter Airfield (5x4)
- **Runway**: Y=3. Flags: `RUF_LANDING | RUF_TAKEOFF | RUF_DIR_LOW`. (NW).
- **One-way**: One-way loop around terminals (Clockwise) is recommended to avoid deadlocks at the hangar exit.

```text
(0,0) T  B  P  P  H  (4,0)
(0,1) A> A> A> A> A  (4,1)
(0,2) A^ S  S  S  Av (4,2)
(0,3) Re R  R  R  Re (4,3)
```

### 3.3. City Airport (6x6)
- **Runway**: Y=5. Flags: `RUF_LANDING | RUF_TAKEOFF | RUF_DIR_LOW`. (NW).
- **One-way**: One-way aprons around the central terminal area.

### 3.4. Metropolitan Airport (6x6)
- **Runway 1 (Y=4)**: `RUF_TAKEOFF | RUF_DIR_LOW`. (Takeoff NW).
- **Runway 2 (Y=5)**: `RUF_LANDING | RUF_DIR_LOW`. (Landing NW).
- **Analysis**: Stock uses two parallel runways both in NW direction. This provides a modular equivalent with higher throughput.

### 3.5. International Airport (7x7)
- **Runway 1 (Y=0)**: `RUF_TAKEOFF | RUF_DIR_HIGH`. (Takeoff SE).
- **Runway 2 (Y=6)**: `RUF_LANDING | RUF_DIR_LOW`. (Landing NW).
- **One-way**: Essential. Entry to terminals via bottom-right apron, exit via top-left.

### 3.6. Intercontinental Airport (9x11)
- **Runway 1 (Y=0)**: `RUF_LANDING | RUF_DIR_HIGH`. (Landing SE).
- **Runway 2 (Y=1)**: `RUF_TAKEOFF | RUF_DIR_HIGH`. (Takeoff SE).
- **Runway 3 (Y=9)**: `RUF_TAKEOFF | RUF_DIR_LOW`. (Takeoff NW).
- **Runway 4 (Y=10)**: `RUF_LANDING | RUF_DIR_LOW`. (Landing NW).
- **Analysis**: Matches the stock 4-runway layout exactly.

## 4. Verification & Behavior

| Requirement | Evaluation |
|-------------|------------|
| **Will it work?** | Yes. The modular A* pathfinder (`FindAirportGroundPath`) will find paths between any connected `APT_APRON`, `APT_STAND`, and `APT_RUNWAY` tiles. |
| **Taxi to Stand/Hangar?** | Yes. Provided the apron connectivity is maintained. The layouts above ensure every parking spot is reachable from the runway exits. |
| **Same runways as stock?** | Yes. We place modular runway pieces in the same grid locations (Y=0, Y=5, etc.) as the stock layout. |
| **Same direction?** | Yes. By setting `RUF_DIR_LOW` (NW) or `RUF_DIR_HIGH` (SE) in `CmdSetRunwayFlags`, we enforce the same traffic patterns. |
| **One-way aprons?** | Yes. For complex airports (International/Intercontinental), we must set the one-way flags on apron tiles to replicate the stock flow and prevent deadlocks in the terminal groups. |

## 5. Mapping Logic Extensions

| Stock Tile | Modular Piece Type | Directional Notes |
|------------|--------------------|-------------------|
| `APT_GRASS_1/2` | `APT_STAND` | Allows small airfield "grass" parking to function. |
| `APT_APRON_W/S/E/N` | `APT_APRON` + One-way | Maps the implicit directions of stock apron tiles to modular one-way flags. |
| `APT_APRON_HOR/VER` | `APT_APRON` | Maps stock crossing tiles to standard modular aprons (which are crossable by default). |

## 6. Implementation Detail: Runway Rollout

In modular airports, planes exit the runway at the "Nearest exit" to their destination. To mimic stock behavior where planes often taxi to the end of the runway before turning, we may need to place apron tiles only at the specific "exit points" used by the stock layout.

Example for Country Airfield:
- Stock: Lands NW, brakes to NW end, then turns back and taxis to center.
- Modular: If we place an apron at (0,1) and (3,1), and the plane lands NW, it will naturally rollout towards the NW end (0,2) and exit at the apron (0,1). This matches stock behavior.
