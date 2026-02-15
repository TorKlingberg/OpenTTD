# Review of @claude_plan.md

## 1. Strategic Conflict (UI Direction)
*   **Issue**: The plan states "Current State: UI fully implemented" and references a grid-based designer approach (26 piece types, specific widget behavior). This contradicts the decision in `new_ui_gemini.md` to abandon the off-map "designer window" in favor of native, on-map construction tools (drag-to-build runways, etc.).
*   **Impact**: Phase 1 assumes we are just hooking up backend storage to an existing UI. In reality, we are building a new UI (`BuildAirToolbarWindow`) and need to ensure `CmdBuildModularAirportTile` (or equivalent) supports the drag-building interaction model of the new UI.

## 2. Technical Architecture Risks

### Data Structure Performance
*   **Plan**: Stores tile metadata in `std::vector<ModularAirportTileData>` within the `Station` struct.
*   **Risk**: Pathfinding requires querying neighbor tiles constantly. Searching a vector for a `TileIndex` is **O(N)**. For a large airport (100+ tiles), this will significantly slow down the A* expansion.
*   **Recommendation**: Use `std::unordered_map<TileIndex, ModularAirportTileData>` or a dedicated `TileMap` structure to ensure **O(1)** lookups.

### Tile Memory Collision (`m7`)
*   **Plan**: "Use tile.m6() bit 0 for reservation flag, tile.m7() for vehicle ID".
*   **Risk**: `m7` is standardly used for **animation frames** in OpenTTD stations (`SetAnimationFrame`). Using it for Vehicle ID will likely break animations for radars, windsocks, and runway lights.
*   **Recommendation**: 
    *   Use `m6` bit 0 for the reservation flag (this is safe).
    *   Do *not* store the Vehicle ID on the tile. The pathfinder only needs to know *if* a tile is reserved, not *who* reserved it. If ID is strictly necessary for deadlock resolution, store it in the `ModularAirport` struct (in a map) rather than on the map tile.

### File Locations
*   **Plan**: Puts `ModularAirportTileData` in `src/base_station_base.h`.
*   **Correction**: This struct is specific to `Station` (airports), not generic to `BaseStation` (which includes waypoints/roadstops). It should be placed in `src/station_base.h` or a new header `src/modular_airport.h`.

## 3. Implementation Details
*   **Line Numbers**: The referenced line numbers (e.g., `src/airport_gui.cpp` line 879) appear to correspond to the "Codex" version of the file, not the current codebase state.
*   **Command Granularity**: `CmdBuildModularAirportTile` builds a single piece. For the new on-map dragging UI, we will need to ensure this command can be batched efficiently or create a wrapper command `CmdBuildModularAirportArea`.

## 4. Alignment
*   **Verdict**: The **backend logic** (Metadata -> Pathfinder -> Aircraft Integration) is sound and necessary. However, the **UI assumptions** are outdated, and the **data storage** choice (Vector + m7) needs optimization for performance and compatibility.
