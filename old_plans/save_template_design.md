# Saved Modular Airport Templates: Design

## Goal
Add a player workflow to:
1. Click a button in the modular airport builder.
2. Click an existing modular airport.
3. Enter a template name and save.
4. Build from those templates in the stock airport picker via a new class shown after `Helicopter airports`: `Saved custom airports`.

This design assumes existing modular placement, runway/taxi/fence metadata, and UI behavior are stable.

## Non-goals
- No savegame-format changes.
- No dynamic `AirportSpec` registration.
- No multiplayer file sync of templates.

## High-level Architecture
Use a dedicated template manager + a new atomic placement command.

- `airport_template.*`: local file persistence and in-memory template catalog.
- `modular_airport_gui.cpp`: save interaction (`Save Template` button + click station + name dialog).
- `airport_gui.cpp`: virtual stock-picker class `Saved custom airports` backed by template catalog.
- `station_cmd.*` + `command_type.h`: `CMD_PLACE_MODULAR_AIRPORT_TEMPLATE` for all-or-nothing execution.

Templates remain a UI/data-layer concept; simulation changes only via command execution.

## Data Model
Add `src/airport_template.h/.cpp`:

```cpp
struct AirportTemplateTile {
    uint16_t dx;
    uint16_t dy;
    uint8_t piece_type;
    uint8_t rotation;
    uint8_t runway_flags;
    bool one_way_taxi;
    uint8_t user_taxi_dir_mask;
    uint8_t edge_block_mask;
    uint32_t grfid;   // 0 for base-set tiles
    uint16_t local_id; // only meaningful when grfid != 0
};

struct AirportTemplate {
    std::string id;          // stable filename stem
    std::string display_name;
    uint16_t width;
    uint16_t height;
    std::vector<AirportTemplateTile> tiles;
    uint32_t schema_version; // start at 1
};
```

Storage path: `FioGetDirectory(SP_PERSONAL_DIR, BASE_DIR) + "airport_templates/"`.

File format: one template per JSON file (`<id>.json`), human-readable (`dump(2)`), with explicit schema version.

`grfid/local_id` are saved for forward-compat with NewGRF airport tiles. For base-set tiles keep `grfid=0`.

## Save Flow (Modular Builder)

### UI behavior
Add a new toolbar button in `BuildModularAirportWindow`:
- Widget: `WID_MA_SAVE_TEMPLATE`
- Tooltip: `Save template`
- Cursor mode: same selection cursor as airport build tools.

Interaction:
1. Player clicks `Save template`.
2. Window enters `save_template_mode` (mutually exclusive with piece/fence modes).
3. Player clicks a tile.
4. If tile is not a modular airport station tile (or not owned by local company): show error and stay in mode.
5. If valid: capture station template data, open `ShowQueryString(..., this, CS_ALPHANUMERAL, ...)`.
6. `OnQueryTextFinished` validates name and writes JSON through template manager.
7. Success toast/message; refresh template catalog; exit save mode.

### Extraction rules
From `st->airport.modular_tile_data`:
- Compute min `x/y` across all tiles; normalize to `(0,0)` offsets.
- Preserve fields exactly:
  - `piece_type`
  - `rotation`
  - `runway_flags`
  - `one_way_taxi`
  - `user_taxi_dir_mask`
  - `edge_block_mask`
- Sort serialized tiles by `(dy, dx)` for deterministic file output.

### Naming/file policy
- Display name is user text.
- Filename stem is sanitized slug + suffix on collision (`name`, `name-2`, ...).
- Empty/whitespace-only names rejected.

## NewGRF Compatibility Policy
- On load, each tile must resolve to a valid `piece_type` in the current game context.
- If a referenced NewGRF tile is missing, mark the template as unavailable in UI.
- Stock picker list renders unavailable templates in an error style, e.g. `Name (NewGRF missing)`.
- Placement of unavailable templates is blocked with a clear error.
- Do not silently skip missing tiles during placement.

## Stock Picker Integration

### Key decision
Do **not** add a real NewGRF airport class ID. Instead, inject a **virtual class entry** in `BuildAirportWindow` dropdown after existing classes. This avoids coupling to `AirportClass` internals and keeps templates UI-only.

### UI model changes
In `BuildAirportWindow`:
- Replace direct `AirportClassID`-only state with a view state:
  - Built-in class (`AirportClassID`), or
  - Virtual class `Saved custom airports`.
- Dropdown composition:
  - Existing classes in current order.
  - Append `Saved custom airports` entry after helicopter airports.
- List behavior:
  - Built-in class: current airport list behavior unchanged.
  - Saved class: list template display names from manager.

### Selection + preview
When saved-template class selected:
- `WID_AP_AIRPORT_LIST`: shows templates.
- `UpdateSelectSize`: uses template `width/height` to set tile selection footprint.
- `WID_AP_AIRPORT_SPRITE`: no sprite requirement; keep blank or draw simple footprint hint.
- `WID_AP_EXTRA_TEXT`: show size/tile count summary.
- Layout controls enabled and repurposed as template rotation (0/90/180/270).

### Placement path
`PlaceAirport(tile)` dispatches:
- Built-in selection: existing behavior.
- Template selection: post new `CMD_PLACE_MODULAR_AIRPORT_TEMPLATE` with selected template payload, clicked origin tile, and selected rotation.

## 4-Way Rotation Rules (Templates)
Rotation is clockwise in 90-degree steps: `r in {0,1,2,3}`.

Offset transform (from stored template-local `x=dx`, `y=dy`, width `W`, height `H`):
- `r=0`: `(x', y') = (x, y)`, size `(W, H)`
- `r=1`: `(x', y') = (H - 1 - y, x)`, size `(H, W)`
- `r=2`: `(x', y') = (W - 1 - x, H - 1 - y)`, size `(W, H)`
- `r=3`: `(x', y') = (y, W - 1 - x)`, size `(H, W)`

Per-tile metadata transform:
- `rotation`: `rotation' = (rotation + r) & 3`
- `user_taxi_dir_mask`: rotate NESW bitmask clockwise by `r`
- `runway_flags` low/high direction: swap `RUF_DIR_LOW <-> RUF_DIR_HIGH` when axis-order is reversed by the transform; otherwise keep as-is
- `piece_type`:
  - base-set modular tiles: preserve type, rely on rotated `rotation` and placement logic
  - NewGRF tiles: resolve from `grfid/local_id`; fail if unresolved

Runway low/high swap condition (for correctness under rotation):
- Determine original runway axis from stored tile rotation (`0/2 = X`, `1/3 = Y`).
- Swap low/high if transformed axis coordinate order is reversed:
  - original X-axis: reverse for `r=2` or `r=3`
  - original Y-axis: reverse for `r=1` or `r=2`

## Atomic Template Placement Command
Add new command:
- `command_type.h`: `CMD_PLACE_MODULAR_AIRPORT_TEMPLATE`
- `station_cmd.h`: declaration + `DEF_CMD_TRAIT`
- `station_cmd.cpp`: implementation

Suggested signature:
```cpp
CommandCost CmdPlaceModularAirportTemplate(
    DoCommandFlags flags,
    TileIndex origin,
    StationID station_to_join,
    bool allow_adjacent,
    const std::vector<AirportTemplateTileCmdData> &tiles,
    uint16_t width,
    uint16_t height,
    uint8_t rotation);
```

`AirportTemplateTileCmdData` mirrors required fields for deterministic application.

### Validation/execution contract
Single command handles full operation with all-or-nothing semantics.

Validation pass:
- Resolve absolute tile for each template tile (`origin + dx/dy`).
- Bounds, ownership, terrain, station-join, year-gating, and per-piece legality checks.
- Enforce same-height constraints consistent with modular airport rules.
- Resolve/validate NewGRF tile mapping before any execute-side mutation.

Execution pass:
1. Place all tiles.
2. Apply runway flags.
3. Apply taxiway one-way flags.
4. Apply edge fences.

If any validation fails, map remains unchanged.

## Multiplayer/Determinism
- Template files are local-only UI assets.
- Networked action is command payload (explicit tile data), not filename or local path.
- Canonical ordering `(dy, dx)` before send/execute.
- Integer-only simulation decisions.

## Error Handling
Add explicit user-facing errors for:
- Invalid selection (not modular airport tile).
- Empty template name.
- I/O failures creating directory/writing file.
- Template parse/schema errors when loading.
- Template requires missing NewGRF content.
- Placement failure reasons from command validation.

## String Additions
`src/lang/english.txt`:
- `STR_AIRPORT_CLASS_SAVED_CUSTOM` = `Saved custom airports`
- `STR_STATION_BUILD_MODULAR_AIRPORT_SAVE_TEMPLATE`
- `STR_STATION_BUILD_MODULAR_AIRPORT_SAVE_TEMPLATE_TOOLTIP`
- `STR_QUERY_SAVE_AIRPORT_TEMPLATE_CAPTION`
- `STR_ERROR_AIRPORT_TEMPLATE_INVALID_SELECTION`
- `STR_ERROR_AIRPORT_TEMPLATE_NAME_EMPTY`
- `STR_ERROR_AIRPORT_TEMPLATE_IO`

## File Touch List
- `src/airport_template.h` (new)
- `src/airport_template.cpp` (new)
- `src/modular_airport_gui.cpp`
- `src/airport_gui.cpp`
- `src/widgets/airport_widget.h`
- `src/station_cmd.h`
- `src/station_cmd.cpp`
- `src/command_type.h`
- `src/lang/english.txt`

## Rollout Plan
1. Implement template manager + save flow button in modular builder.
2. Add virtual stock-picker class + template list rendering.
3. Add atomic placement command and hook stock-picker placement.
4. Add error strings and polish preview text.

## Acceptance Criteria
- Player can save a modular airport by: button -> click airport -> name -> saved file created.
- `Saved custom airports` appears in stock picker after helicopter airports.
- Saved templates appear in that class list.
- Selecting a template sets correct footprint and can place it.
- Template placement supports 0/90/180/270 rotation with correct taxi/runway behavior.
- Templates with missing NewGRFs are visible but blocked with explicit reason.
- Failed placement causes zero map changes.
- Works in multiplayer deterministically (command-payload-based placement).
- No savegame version bump.
