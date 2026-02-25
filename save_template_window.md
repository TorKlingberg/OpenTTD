# Modular Airport Template Manager Window: Design

## Summary
Move template workflow out of the stock airport picker into a dedicated window opened from the modular builder `Save template` button.

New window provides three actions:
1. `Save` (pick an existing modular airport on map, name it, write JSON)
2. `Load` (select template, choose 4-way rotation, place on map)
3. `Delete` (remove selected template file with confirmation)

## Difficulty
Medium.

Rough effort:
- GUI + state wiring: 1 day
- Backend delete/list refresh hardening: 0.5 day
- Placement preview/interaction polish + testing: 0.5 to 1 day

Total: about 2 to 3 days including debugging.

## Scope decision
Do this as a single cutover, not phased coexistence:
- Remove `APC_SAVED_CUSTOM` stock-picker integration in the same change.
- Add the new template manager window in the same change.
- Keep one user path for templates at all times after merge: modular builder -> template manager.

## UX Flow

### Entry
- In `BuildModularAirportWindow`, clicking `WID_MA_TEMPLATE_MANAGER` opens `Template Manager` window.
- Rename the toolbar button from "Save template" to "Templates" (or "Template manager") to match its expanded scope.
- The old direct "save mode" from the modular builder is removed.
- If stock airport picker is open, it stays open, but no template class is available there anymore.

### Template Manager window
Layout:
- Vertical OpenTTD-style layout:
- Top: template list (name, size, availability) + scrollbar
- Middle: row of `Save`, `Load`, `Delete` buttons
- Next row: rotation controls (`0/90/180/270`) used only by `Load`
- Bottom: info line (`WxH`, tile count, NewGRF missing warning)

### Save flow
1. Click `Save`.
2. Cursor mode activates: "click an existing modular airport you own".
3. Click airport tile.
4. Name query opens.
5. Confirm writes template JSON, refreshes list, keeps window open.

### Load flow
1. Select template in list.
2. Pick rotation.
3. Click `Load`.
4. Cursor mode activates: place mode on main map.
5. White-square footprint preview follows cursor (same style as stock airport placement).
6. Rotation can be changed while placing via both window controls and rotate hotkeys (e.g. `R`/`E`), with preview updating immediately.
7. Click map: posts `CMD_PLACE_MODULAR_AIRPORT_TEMPLATE`.

### Delete flow
1. Select template.
2. Click `Delete`.
3. Confirmation dialog.
4. Remove JSON file, refresh list, clear selection if needed.

## Architecture

## 1) New window class
Add `BuildModularTemplateManagerWindow` (prefer `src/airport_template_gui.cpp` and `src/airport_template_gui.h`).
Use a dedicated window class:
- `WC_AIRPORT_TEMPLATE_MANAGER` (new), window number `0`.

Responsibilities:
- Own template selection index for this window
- Own manager mode state: `None`, `SavingPickAirport`, `LoadingPlace`
- Handle query callbacks for save name
- Handle place callbacks for save-pick and load-place
- Manage preview cache updates for load placement

This avoids bloating `BuildModularAirportWindow`.

Window lifetime decision:
- Manager closes when modular builder closes/toggles off.
- Manager also closes on airport toolbar abort (`BuildAirToolbarWindow::OnPlaceObjectAbort`) to avoid dangling placement state.
- Right-click/abort while manager is actively placing should not close the manager; it should only exit `SavingPickAirport`/`LoadingPlace` mode and clear preview.

## 2) Backend template manager additions
`src/airport_template.h/.cpp`:
- Keep current `Refresh`, `GetTemplates`, `SaveTemplate`
- Add:
  - `static bool DeleteTemplateByFileStem(const std::string &file_stem);`

Recommended model update for reliability:
- Extend `AirportTemplate` with `std::string file_stem` (no directory, no extension).
- Delete uses stem, never display name.

Refresh behavior:
- Keep `_templates_dirty` cache.
- Do not add `ForceRefresh()`.
- `SaveTemplate` and `DeleteTemplateByFileStem` both:
  - set `_templates_dirty = true`
  - call `Refresh()`

## 3) Placement and preview reuse
Reuse existing components:
- Placement command: `CMD_PLACE_MODULAR_AIRPORT_TEMPLATE` in `modular_airport_template_cmd.cpp`
- Rotation math: `AirportTemplateTile::Rotate` / existing command rotation path
- White squares:
  - Reuse `_saved_template_preview_offsets`, `_saved_template_preview_active`
  - Make preview visibility independent of stock picker class selection

Needed adjustment:
- Replace current `ShouldDrawSavedTemplatePreviewAtTileInternal(...selected_airport_class...)` gate to rely on `_saved_template_preview_active` (plus existing drag/cursor checks), not airport class.
- Update call site in `src/viewport.cpp` accordingly (drop airport-class argument).

This keeps the main-map drawing behavior identical while decoupling from stock airport picker state.

## 4) Remove stock picker dependency
Remove current load path in `src/airport_gui.cpp` (virtual class `Saved custom airports`) in the same patch.

Final state:
- Stock airport picker shows only real airport classes.
- Template placement is only from template manager window.

## State Model

Window-local fields:
- `int selected_template_index`
- `uint8_t selected_rotation`
- `enum class TemplateManagerMode { None, SavingPickAirport, LoadingPlace } mode`
- `TileIndex save_pick_tile` (set when airport tile is clicked in save mode)
- `bool has_save_pick_tile`

Global/shared (existing):
- preview offsets/active flag (manager window owns activation/reset)

Cursor ownership decision:
- Opening template manager from modular builder immediately deactivates modular builder placement state (`selected_piece`, fence tool, old save mode) and releases its cursor ownership.
- Template manager becomes sole cursor owner for template save/load placement modes.
- Modular builder's `OnPlaceObjectAbort` should treat this handoff as expected and only clear its own tool state (no attempts to reclaim cursor).
- Closing template manager does not auto-restore a previous modular piece selection.

## Commands and callbacks

### Save
- On map click in `SavingPickAirport`:
  - validate station tile, ownership, modular block
  - store clicked tile in `save_pick_tile`
  - `ShowQueryString(...)`
- On query finish:
  - validate non-empty name
  - rebuild station/template from `save_pick_tile` at callback time
  - `AirportTemplateManager::SaveTemplate(...)`
  - refresh list and select newly saved item

### Load
- On `Load` button:
  - validate selected template exists and available
  - enter placing mode and set object cursor
  - recompute preview offsets for rotation
- On map click in `LoadingPlace`:
  - construct `ModularTemplatePlacementData`
  - post `CMD_PLACE_MODULAR_AIRPORT_TEMPLATE` with a dedicated callback that does not depend on stock-picker globals (`CcBuildAirportTemplateManager`), then keep/exit load mode per persistent-build-tool setting

### Delete
- On `Delete` click:
  - call `ShowQuery(...)` confirmation
- On confirm callback:
  - `AirportTemplateManager::DeleteTemplateByFileStem(...)`
  - refresh list
  - restore selection to next sensible item:
    - prefer the item that moved into the deleted row
    - otherwise previous row if deleted item was last
    - clear selection if list became empty

## Selection behavior
- On window open:
  - if list non-empty, select first item by default.
- After save refresh:
  - auto-select newly created template.
- After delete refresh:
  - select next available item as described above.
- After external parse/load changes (if any):
  - clamp selection index into valid range or clear if empty.

## Widget and string additions

Likely new widgets (`src/widgets/airport_widget.h`):
- `WID_TM_CAPTION`
- `WID_TM_TEMPLATE_LIST`
- `WID_TM_SCROLLBAR`
- `WID_TM_SAVE`
- `WID_TM_LOAD`
- `WID_TM_DELETE`
- `WID_TM_ROTATE_LEFT`
- `WID_TM_ROTATE_RIGHT`
- `WID_TM_INFO`

String additions (`src/lang/english.txt`):
- `STR_STATION_BUILD_MODULAR_AIRPORT_TEMPLATE_MANAGER`
- `STR_STATION_BUILD_MODULAR_AIRPORT_TEMPLATE_SAVE`
- `STR_STATION_BUILD_MODULAR_AIRPORT_TEMPLATE_LOAD`
- `STR_STATION_BUILD_MODULAR_AIRPORT_TEMPLATE_DELETE`
- `STR_QUERY_DELETE_AIRPORT_TEMPLATE_CAPTION`
- `STR_QUERY_DELETE_AIRPORT_TEMPLATE_TEXT`
- `STR_ERROR_AIRPORT_TEMPLATE_DELETE_FAILED`

## Shared helper/API changes
Current helpers in `airport_template_gui.*` use global selection state. With selection moved window-local:
- Replace `GetSelectedAirportTemplate()` with index-based helper: `GetAirportTemplateByIndex(int index)`.
- Replace `UpdateSavedTemplatePreviewCache(uint8_t rotation)` with `UpdateSavedTemplatePreviewCache(const AirportTemplate *templ, uint8_t rotation)`.
- Simplify preview check signature to avoid stock class coupling: `ShouldDrawSavedTemplatePreviewAtTileInternal(TileIndex tile)`.
- Remove/dead-code `_selected_airport_template_index` and any stock-picker-only template globals.

## Risks and mitigations

1. Cursor ownership conflicts (`SetObjectToPlace` triggers abort callbacks)
- Mitigation: copy the same `updating_cursor` guard pattern used in modular builder.

2. Preview squares not showing in new flow
- Mitigation: preview gate must rely on `_saved_template_preview_active` (with existing cursor checks), not stock picker class.

3. Stale list after file operations
- Mitigation: set `_templates_dirty = true`, call `Refresh()`, then `SetDirty()`.

4. Wrong template deleted
- Mitigation: delete by `file_stem`, not display name.

5. Rotation UX inconsistency during placement
- Mitigation: support rotation hotkeys in `LoadingPlace` mode and keep window rotation buttons in sync.

## File touch list
- `src/airport_template_gui.h`
- `src/airport_template_gui.cpp`
- `src/modular_airport_gui.cpp`
- `src/airport_template.h`
- `src/airport_template.cpp`
- `src/airport_gui.cpp` (remove/de-scope saved-template class path)
- `src/viewport.cpp`
- `src/window_type.h` (new `WC_AIRPORT_TEMPLATE_MANAGER`)
- `src/widgets/airport_widget.h`
- `src/lang/english.txt`

## Implementation plan (single PR)
1. Add manager window (list/buttons/rotation/info) and wire modular builder save button to open it.
   - Include widget/string rename: `WID_MA_SAVE_TEMPLATE` -> `WID_MA_TEMPLATE_MANAGER`, tooltip/text updated to template-manager wording.
2. Move save flow into manager window (`Save` -> pick tile -> name query -> save).
3. Move load flow into manager window (`Load` -> place mode -> command post + white-square preview).
4. Add delete flow with `ShowQuery` confirm + file-stem deletion.
5. Remove `APC_SAVED_CUSTOM` path and related stock-picker/template globals no longer needed.
6. Regression test cursor abort, preview draw, station join dialog, and 4-way rotation.

## Acceptance criteria
- Clicking modular builder save button opens template manager window.
- Save/load/delete all work from this one window.
- Loading shows white footprint squares on main map before placement.
- Placement supports 4-way rotation and preserves runway/taxi/fence metadata.
- No template workflow remains required in stock airport picker.
