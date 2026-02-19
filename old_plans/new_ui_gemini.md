# New UI Plan: On-Map Modular Airport Construction

## 1. Vision
Instead of a "designer window" where players layout an airport in an abstract grid, players will construct airports directly on the game map, tile-by-tile, similar to how they currently build rail networks or road stations. This feels more native to OpenTTD and allows for organic integration with surrounding infrastructure.

## 2. Interaction Model

### The Entry Point
*   The main toolbar's **Airport** button calls `ShowBuildAirToolbar()`.
*   This opens `BuildAirToolbarWindow` (defined in `src/airport_gui.cpp`).
*   Currently, this toolbar is sparse (Airport Picker, Modular Builder Window, Demolish).
*   **Plan:** Expand `BuildAirToolbarWindow` to be a full-featured construction toolbar, similar to `BuildRailToolbarWindow`.

### The Construction Toolbar (`BuildAirToolbarWindow`)
The expanded toolbar will feature specific tools for modular components.

**New Widget IDs (to add to `src/widgets/airport_widget.h`):**
1.  **`WID_AT_TAXIWAY`**:
    *   **Behavior:** Drag-to-place line (like Road/Rail).
    *   **Icon:** A taxiway sprite (blue/grey pavement).
2.  **`WID_AT_RUNWAY`**:
    *   **Behavior:** Drag-to-place line. Constrained to straight lines.
    *   **Icon:** A runway sprite (dark grey with markings).
3.  **`WID_AT_TERMINAL`**:
    *   **Behavior:** Click-to-place (single tile). Rotatable.
    *   **Icon:** Terminal building sprite.
4.  **`WID_AT_HANGAR`**:
    *   **Behavior:** Click-to-place (single tile). Rotatable.
    *   **Icon:** Hangar sprite.
5.  **`WID_AT_DEMOLISH`**:
    *   **Behavior:** Standard dynamite tool (already exists).

**Deprecated:**
*   `WID_AT_MODULAR`: The button that opens the "designer window" will be removed.
*   `BuildModularAirportWindow`: The class in `src/airport_gui.cpp` implementing the off-map designer will be removed.

## 3. Station Logic Integration

*   **Adjacency:**
    *   When placing a component next to an existing airport tile, it automatically joins that station (standard OpenTTD behavior).
    *   **Ctrl+Click:** Allows joining non-adjacent stations or creating a separate station next to an existing one.
*   **Visual Feedback:**
    *   When hovering with a tool, if the cursor is near an existing station, the station sign highlights to indicate "joining".

## 4. Implementation Steps

### Step 1: Widget Definitions (`src/widgets/airport_widget.h`)
*   Update `AirportToolbarWidgets` enum.
*   Add `WID_AT_TAXIWAY`, `WID_AT_RUNWAY`, `WID_AT_TERMINAL`, `WID_AT_HANGAR`.
*   Remove `WID_AT_MODULAR`.

### Step 2: Toolbar Window Layout (`src/airport_gui.cpp`)
*   Modify `_nested_air_toolbar_widgets` to include the new buttons.
*   Arrange them logically (e.g., Infrastructure group: Taxiway/Runway; Buildings group: Terminal/Hangar).
*   Assign appropriate sprites (use placeholders from `spr_img_rail` or similar if specific airport sprites aren't available yet).

### Step 3: Event Handling (`src/airport_gui.cpp`)
*   Update `BuildAirToolbarWindow::OnClick`:
    *   Handle clicks for the new widgets.
    *   Set the correct cursor (e.g., `SPR_CURSOR_RAIL_NS` or similar placeholders) and tool mode (`HT_LINE`, `HT_RECT`).
*   Update `BuildAirToolbarWindow::OnPlaceObject`:
    *   Map the new tools to the new commands (Phase 2/3 commands).
    *   **Taxiway:** Call `CmdBuildTaxiway`.
    *   **Runway:** Call `CmdBuildRunway` (to be implemented).
    *   **Terminal/Hangar:** Call `CmdBuildAirportComponent`.

### Step 4: Cleanup
*   Remove `BuildModularAirportWindow` class and `ShowBuildModularAirportWindow` function.
*   Remove the `_modular_airport_pieces` array and related constants.

## 5. Future Considerations
*   **Upgrading:** Allow converting a "Classic" airport to "Modular" by clicking it with a modular tool? (Maybe too complex for v1).
*   **Terraforming:** The toolbar should likely include standard landscaping tools (Raise/Lower land) for convenience, just like Rail/Road toolbars.
