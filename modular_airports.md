# Modular Airports - Current Implementation Guide

This document describes the **implemented** modular-airport system in this branch. It replaces the old design draft.

## Scope

Modular airports are now a first-class airport mode built on top of station airport tiles plus modular metadata.

- Players place airport pieces tile-by-tile.
- Ground movement uses modular routing/reservation logic.
- The classic airport FTA is still present for non-modular airports.
- Saved templates (JSON) can be created, previewed, rotated, and placed atomically.

## Key Files

- `src/station_cmd.cpp`
- `src/modular_airport_cmd.cpp`
- `src/modular_airport_cmd.h`
- `src/modular_airport_build.cpp`
- `src/modular_airport_draw.cpp`
- `src/modular_airport_gui.cpp`
- `src/airport_ground_pathfinder.cpp`
- `src/airport_pathfinder.cpp`
- `src/airport_template.cpp`
- `src/airport_template_gui.cpp`
- `src/modular_airport_template_cmd.cpp`
- `src/base_station_base.h`
- `src/aircraft.h`
- `src/saveload/station_sl.cpp`
- `src/saveload/vehicle_sl.cpp`

## Data Model

### Per-tile modular metadata

`ModularAirportTileData` (`src/base_station_base.h`) stores:

- `tile`
- `piece_type`
- `rotation` (`0..3`)
- `user_taxi_dir_mask`
- `one_way_taxi`
- `auto_taxi_dir_mask`
- `runway_flags` (`RUF_*`)
- `edge_block_mask`

This metadata is authoritative for modular logic; map tile gfx remains canonical airport gfx.

### Per-airport modular state

In `Airport` (`src/base_station_base.h`):

- `modular_tile_data`
- `modular_tile_index` + dirty bit
- cached `modular_holding_loop` + dirty bit

### Per-aircraft modular runtime state

In `Aircraft` (`src/aircraft.h`):

- taxi path and segment progress
- non-runway reserved tiles
- runway reservation vector
- modular landing/takeoff targets and stages
- holding waypoint index

Important: runtime path/reservation state is mostly recomputed, not fully persisted.

## Commands and Editing Flow

Defined in `src/station_cmd.h`:

- `CMD_BUILD_MODULAR_AIRPORT_TILE`
- `CMD_SET_RUNWAY_FLAGS`
- `CMD_SET_TAXIWAY_FLAGS`
- `CMD_SET_MODULAR_AIRPORT_EDGE_FENCE`
- `CMD_BUILD_MODULAR_AIRPORT_FROM_STOCK`
- `CMD_PLACE_MODULAR_AIRPORT_TEMPLATE`

### Build tile

`CmdBuildModularAirportTile` (`src/station_cmd.cpp`):

- checks piece availability by year (`IsModernModularPiece` / `GetModularPieceMinYear`)
- enforces flat-level consistency within an existing modular airport
- allows safe replacement of modular grass/empty tiles
- stores directional hangar metadata in `piece_type`
- validates one-way taxi settings against auto directions
- inherits runway flags from contiguous runway if present; otherwise defaults
- normalizes runway segment visuals after placement

### Build from stock airport

`CmdBuildModularAirportFromStock` converts stock layouts to modular metadata, applies stock overrides, sets runway flags per airport type, and mirrors fence edges.

### Edit runway/taxi/fence

`src/modular_airport_gui.cpp` posts:

- runway flag changes (`CMD_SET_RUNWAY_FLAGS`)
- one-way taxi direction changes (`CMD_SET_TAXIWAY_FLAGS`)
- edge fence toggles (`CMD_SET_MODULAR_AIRPORT_EDGE_FENCE`)

### Remove tile

`RemoveModularAirportTile` (`src/modular_airport_build.cpp`):

- handles multi-tile small terminal demolition coherently
- clears and rebuilds affected runway visuals around removed segments
- updates modular indices and holding loop cache
- tears down airport facility if no modular tiles remain

## Ground Routing and Reservation Model

### Pathfinding

`FindAirportGroundPath` in `src/airport_ground_pathfinder.cpp` uses A* across modular airport tiles.

Connectivity is based on:

- auto taxi directions (`CalculateAutoTaxiDirectionsForGfx`)
- optional user one-way restriction mask
- runway axis checks
- runway operation restrictions
- explicit edge fences (`edge_block_mask`)

### Segment classes

`TaxiSegmentType` (`src/airport_ground_pathfinder.h`):

- `FREE_MOVE`
- `ONE_WAY`
- `RUNWAY`

Path segments drive reservation behavior:

- runways reserve contiguous runway tiles atomically
- one-way sections allow queueing behavior
- free-move sections use segment-level reservation behavior

### Runway flags

`RUF_*` flags (`src/base_station_base.h`):

- `RUF_LANDING`
- `RUF_TAKEOFF`
- `RUF_DIR_LOW`
- `RUF_DIR_HIGH`

`CmdSetRunwayFlags` applies flags across the entire contiguous same-axis runway.

## Rendering and UI

### Shared modular airport tile layout overrides

`GetAirportTileLayoutWithModularOverrides` (`src/station_cmd.cpp`) centralizes modular sprite decisions for:

- directional hangars
- NS runway override sprites
- modular windsock/helipad variants
- radar/flag animated airport tile layouts

This helper is used by normal tile drawing and template preview paths to reduce divergence.

### Builder UI

`src/modular_airport_gui.cpp` includes:

- piece toolbar and sub-pickers (hangar/cosmetic/helipad)
- smart runway drag placement (auto end pieces)
- taxi/runway overlay editing mode
- edge-fence tool mode
- holding overlay toggle
- template manager launch

### Template manager and preview

`src/airport_template_gui.cpp` provides:

- save template from selected modular airport
- load/place rotated template
- in-window isometric preview with zoom-down for large templates
- preview runway-end normalization for legacy small runway segments

Template storage is JSON in personal dir `airport_templates/` (`src/airport_template.cpp`).

## Rotation Invariants (Critical)

These are the most common source of bugs.

- Hangar directional convention: `0=SE, 1=NE, 2=NW, 3=SW`.
- Keep mappings aligned across:
  - `SwapBuildingPieceForRotation` (`src/modular_airport_cmd.h`)
  - `GetModularHangarTileLayoutByPiece` (`src/station_cmd.cpp`)
  - hangar taxi-direction handling in `CalculateAutoTaxiDirectionsForGfx` (`src/airport_pathfinder.cpp`)
- Preview isometric handedness uses `iso_x = (dy - dx) * half_w`; flipping this mirrors preview.
- Legacy small runway `NEAR/FAR` ends swap on odd rotations and must be normalized by contiguous segment in preview/build visual passes.

## Save/Load

- Modular tile metadata is saved in station save data (`SlModularAirportTileData`, `src/saveload/station_sl.cpp`).
- Aircraft modular runtime path/reservation internals are not fully serialized; they are re-established by runtime logic (`src/saveload/vehicle_sl.cpp`).

## Large-Aircraft Capability

`ModularAirportSupportsLargeAircraft` (`src/modular_airport_cmd.cpp`) currently requires:

- tower
- big terminal piece
- at least one safe large runway (length/family constraints)

## Debugging Notes

Useful debug areas:

- `src/modular_airport_cmd.cpp` has dense `[ModAp]` logging around reservation and movement.
- `src/airport_ground_pathfinder.cpp` has additional detailed neighbor/connectivity traces at higher debug levels.

## What Changed vs Old Draft

The old draft discussed options (FTA generation vs hybrid). The current codebase already implements a modular ground-routing/reservation system, modular editing commands, stock-to-modular conversion, and template workflows. Use this file as the current behavior reference, not a future plan.
