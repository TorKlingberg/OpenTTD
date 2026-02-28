# Design Doc: Modular Airport Templates

## Overview
This feature allows players to save custom modular airport designs to disk and reuse them later via the standard airport builder window.

## Proposed Components

### 1. Template Manager (`src/airport_template.h/cpp`)
- Handles loading, saving, and listing modular airport templates using `nlohmann/json`.
- **Storage Location**: `airport_templates/*.json` in the user's personal directory.
- **Data Model**:
    - `AirportTemplateTile`: `dx`, `dy`, `piece_type`, `rotation`, `runway_flags`, `one_way_taxi`, `user_taxi_dir_mask`, `edge_block_mask`, `grfid`, `local_id`.
    - `AirportTemplate`: `name`, `width`, `height`, `tiles` (vector), `schema_version`.

### 2. Saving UI (`src/modular_airport_gui.cpp`)
- Add a "Save template" button (`WID_MA_SAVE_TEMPLATE`) to the modular builder.
- **Extraction**: Normalizes tile coordinates relative to the top-left tile of the selection. Sorts tiles by `(dy, dx)` for deterministic output.

### 3. Loading UI (`src/airport_gui.cpp`)
- **Virtual Class**: Injects "Saved custom airports" (`STR_AIRPORT_CLASS_SAVED_CUSTOM`) into the builder dropdown.
- **NewGRF Missing**: Templates with missing NewGRFs are shown in red with a suffix. Placement skips missing tiles (as requested).
- **Thumbnail**: Live-rendered thumbnails in the preview area.

### 4. 4-Way Rotation Logic
Rotation `r` (0-3 clockwise) is applied during placement:
- **Offsets**: 
    - `r=0: (x, y)`
    - `r=1: (H-1-y, x)`
    - `r=2: (W-1-x, H-1-y)`
    - `r=3: (y, W-1-x)`
- **Tile Rotation**: `rotation' = (rotation + r) & 3`.
- **Taxi Mask**: Rotate NESW bits clockwise by `r`.
- **Runway Flags**: Swap `RUF_DIR_LOW` and `RUF_DIR_HIGH` if the axis is reversed (r=2,3 for X-axis; r=1,2 for Y-axis).

### 5. Atomic Placement Command
- `CMD_PLACE_MODULAR_AIRPORT_TEMPLATE`: Sends the full tile payload for multiplayer safety.
- Validates bounds, terrain, and year-gating before placing any tiles.
- Skips tiles whose NewGRFs are not present in the current game.
