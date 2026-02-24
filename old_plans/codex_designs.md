# Modular Airport: Implementation Plan

## Source files reference

| Area | Files |
|------|-------|
| Core logic | `src/modular_airport_cmd.cpp`, `src/modular_airport_cmd.h` |
| Ground pathfinding | `src/airport_ground_pathfinder.cpp`, `src/airport_ground_pathfinder.h` |
| Taxi direction calc | `src/airport_pathfinder.cpp` |
| Build commands | `src/station_cmd.cpp`, `src/station_cmd.h`, `src/command_type.h` |
| Drawing | `src/station_cmd.cpp` (`DrawTile_Station`, modular fence overlay) |
| Modular builder UI | `src/modular_airport_gui.cpp`, `src/widgets/airport_widget.h` |
| Stock airport picker | `src/airport_gui.cpp`, `src/newgrf_airport.h`, `src/newgrf_airport.cpp` |
| Station capability | `src/vehicle.cpp`, `src/order_cmd.cpp`, `src/aircraft_cmd.cpp` |
| Tile data struct | `src/base_station_base.h` (`ModularAirportTileData`) |
| Tile IDs | `src/table/airporttile_ids.h` |
| Save/load | `src/saveload/station_sl.cpp`, `src/saveload/saveload.h` |

Current savegame version: `SL_MAX_VERSION = 368` (after `SLV_AIRPORT_THROUGHPUT = 367`).

## Milestones

```
Milestone 1 — quick fixes:
  1. Helicopter staging fix
  2. Hold-loop wiggle fix
  3. Runway-end visual normalization

Milestone 2 — game rules:
  4. Piece-year gating
  5. Large-plane safety + mixed runway routing

Milestone 3 — pathfinding + barriers (together, since grass routing needs fences):
  6. Grass taxiable with cost weighting
  7. Edge fence system

Milestone 4 — content workflow:
  8. Template save/load (phase 1)
  9. Stock picker integration (phase 2, defer)
```

---

## 1) Helicopter staging fix

### Problem

Helicopters fly to an arbitrary point 12 tiles NE of the airport before approaching the pad.

Root cause: the helicopter branch in `AirportMoveModularFlying()` (line ~2823) calls `GetModularLandingApproachPoint()`, which is runway-centric. It offsets 12 tiles along the runway axis. For helipads this produces a meaningless staging point.

### Fix

Don't call `GetModularLandingApproachPoint()` for helipad targets. Use pad tile center directly.

In `AirportMoveModularFlying()` helicopter branch (line ~2823):

```cpp
runway = FindModularLandingTarget(st, v);
if (runway != INVALID_TILE) {
    const ModularAirportTileData *td = st->airport.GetModularTileData(runway);
    if (td != nullptr && IsModularHelipadPiece(td->piece_type)) {
        // Helipads: fly directly to pad center, no FAF offset
        target_x = TileX(runway) * TILE_SIZE + TILE_SIZE / 2;
        target_y = TileY(runway) * TILE_SIZE + TILE_SIZE / 2;
    } else {
        // Runways: use standard approach point
        GetModularLandingApproachPoint(st, runway, &target_x, &target_y);
    }
} else {
    target_x = TileX(target) * TILE_SIZE + TILE_SIZE / 2;
    target_y = TileY(target) * TILE_SIZE + TILE_SIZE / 2;
}
```

In `AirportMoveModularLanding()` stage 0: if `modular_landing_tile` is a helipad, skip stage 0 entirely (set `modular_landing_stage = 1` immediately). Helicopters descend vertically — no FAF glide needed.

All helipad styles (1/2/3) use the same approach behavior. Heliport rooftop `+60` z offset is unaffected (applied in stage 1 touchdown).

### Files

- `src/modular_airport_cmd.cpp`

### Acceptance

- [ ] Helicopters fly directly toward pad area, no NE drift.
- [ ] Fixed-wing approach logic unchanged.
- [ ] Heliport rooftop touchdown height still correct.

---

## 2) Hold-loop wiggle fix

### Problem

Aircraft on straight hold-loop segments oscillate left-right every few ticks.

Root cause: `GetDirectionTowards()` returns one of 8 compass directions. When the actual bearing to the target falls between two adjacent directions, the quantized result flips back and forth. The `turn_counter` delay (2 * plane_speed ticks) slows the flip but doesn't prevent it — after cooldown, it flips again.

The doc's lookahead is already 5 waypoints ahead, so the target position isn't the issue. Multi-waypoint averaging won't help because the problem is direction quantization, not target selection.

### Fix

Suppress flip-flop using the existing `last_direction` and `number_consecutive_turns` fields. If the new direction would return to the previous heading (indicating oscillation), suppress the turn.

In `AirportMoveModularFlying()` direction update block (line ~2846):

```cpp
if (dist > 0) {
    if (v->turn_counter > 0) {
        v->turn_counter--;
    } else {
        Direction new_dir = GetDirectionTowards(v, target_x, target_y);
        if (new_dir != v->direction) {
            if (new_dir == v->last_direction && v->number_consecutive_turns > 0) {
                // Suppress: this is a flip-flop back to the previous heading.
                // Increase turn_counter to delay the next check.
                v->turn_counter = 2 * _settings_game.vehicle.plane_speed;
            } else {
                v->number_consecutive_turns = (new_dir == v->last_direction) ? 0 : v->number_consecutive_turns + 1;
                v->turn_counter = 2 * _settings_game.vehicle.plane_speed;
                v->last_direction = v->direction;
                v->direction = new_dir;
                SetAircraftPosition(v, v->x_pos, v->y_pos, v->z_pos);
            }
        }
    }
}
```

The key insight: `number_consecutive_turns` already tracks alternating turns. When it exceeds 0 and the new direction matches `last_direction`, we know it's oscillating, so we hold current heading and wait.

### Files

- `src/modular_airport_cmd.cpp`

### Acceptance

- [ ] Straight hold-loop segments show stable heading, no micro oscillation.
- [ ] Curves still turn normally (suppression only triggers on flip-flop pattern).
- [ ] No missed landing gate opportunities.
- [ ] No aircraft freeze or reversal.

---

## 3) Runway-end visual normalization

### Problem

After adding/removing runway tiles, the end-piece visuals don't update. Extending a runway leaves the old end piece as-is; removing an end piece doesn't promote the next tile to an end.

### Design

Add `NormalizeRunwaySegmentVisuals(Station *st, TileIndex changed_tile)` in `station_cmd.cpp`. Called after runway tile build and runway tile removal.

Algorithm:
1. Determine the axis from the changed tile's rotation (or from surviving neighbors if tile was removed).
2. Walk both directions along the axis, collecting all contiguous runway tiles.
3. Split into sub-segments by family:
   - **Large family:** `APT_RUNWAY_1-5`, `APT_RUNWAY_END`
   - **Small family:** `APT_RUNWAY_SMALL_NEAR_END`, `APT_RUNWAY_SMALL_MIDDLE`, `APT_RUNWAY_SMALL_FAR_END`
   - A contiguous runway cannot mix families. If mixed tiles are found, treat as separate segments at the family boundary.
4. For each sub-segment, assign piece types:
   - **Large:** ends get `APT_RUNWAY_END`, interior gets `APT_RUNWAY_5`.
   - **Small:** low-coordinate end gets `APT_RUNWAY_SMALL_NEAR_END`, high-coordinate end gets `APT_RUNWAY_SMALL_FAR_END`, interior gets `APT_RUNWAY_SMALL_MIDDLE`.
5. For each changed tile, update both `ModularAirportTileData.piece_type` and call `SetStationGfx(tile, new_gfx)` so the viewport redraws correctly.
6. Preserve `runway_flags` and `rotation` — do not reset them.
7. After piece-type changes, call the runway-flag propagation logic (same walk as `CmdSetRunwayFlags`) to ensure flags are consistent across the segment.

### Trigger points

- After execute path in `CmdBuildModularAirportTile` when the placed tile is a runway piece.
- After execute path in `RemoveModularAirportTile` — pass in the removed tile's axis so the normalizer knows which direction to walk from neighbors.
- Not needed after stock-to-modular conversion (drag-builder already places correct ends).

### Files

- `src/station_cmd.cpp`

### Acceptance

- [ ] Extending a runway moves the end visual to the new tip.
- [ ] Removing an end tile promotes the next tile to end visual.
- [ ] Removing a middle tile splits into two correctly-ended segments.
- [ ] Mixed large/small runway tiles at a boundary become separate segments.
- [ ] Runway flags and rotation preserved through normalization.
- [ ] Runway operational logic (landing/takeoff selection) unchanged.

---

## 4) Piece-year gating

### Design

Modern pieces unlock in the same year as the city airport (`AirportSpec::Get(AT_LARGE)->min_year`, which is 1955 in vanilla).

#### Piece classification

**Legacy (always available):**
- Small runway: `APT_RUNWAY_SMALL_NEAR_END`, `APT_RUNWAY_SMALL_MIDDLE`, `APT_RUNWAY_SMALL_FAR_END`
- Basic apron: `APT_APRON` and fenced variants
- Basic stand: `APT_STAND`
- Small hangars: `APT_SMALL_DEPOT_SE/SW/NW/NE`
- Grass: all `APT_GRASS_*` variants
- Empty: `APT_EMPTY`, `APT_EMPTY_FENCE_NE`
- Windsock: `APT_GRASS_FENCE_NE_FLAG`, `APT_GRASS_FENCE_NE_FLAG_2`

**Modern (city airport year):**
- Large runway: `APT_RUNWAY_1-5`, `APT_RUNWAY_END`
- Large hangars: `APT_DEPOT_SE/SW/NW/NE`
- Tower: `APT_TOWER`, `APT_TOWER_FENCE_SW`
- Radar: `APT_RADAR_FENCE_SW`, `APT_RADAR_FENCE_NE`, `APT_RADAR_GRASS_FENCE_SW`
- Round terminal: `APT_ROUND_TERMINAL`
- Advanced stands: `APT_STAND_1`, `APT_STAND_PIER_NE`, `APT_PIER`, `APT_PIER_NW_NE`
- Low building: `APT_LOW_BUILDING`, `APT_LOW_BUILDING_FENCE_N`, `APT_LOW_BUILDING_FENCE_NW`
- Helipads: `APT_HELIPAD_*`, `APT_HELIPORT`
- All decorative buildings: `APT_BUILDING_1/2/3`, `APT_SMALL_BUILDING_1/2/3`
- Radio tower: `APT_RADIO_TOWER_FENCE_NE`

#### Implementation

1. Add `GetModularPieceMinYear(uint8_t piece_type)` in `src/modular_airport_cmd.h` (inline, shared between GUI and command validation).
   - Returns `TimerGameCalendar::Year{0}` for legacy pieces.
   - Returns `AirportSpec::Get(AT_LARGE)->min_year` for modern pieces.

2. GUI enforcement in `src/modular_airport_gui.cpp`:
   - Disable toolbar buttons and picker entries for locked pieces.
   - Tooltip shows unlock year.

3. Command enforcement in `CmdBuildModularAirportTile` (`src/station_cmd.cpp`):
   - Reject with `STR_ERROR_MODULAR_PIECE_NOT_YET_AVAILABLE` if `TimerGameCalendar::year < GetModularPieceMinYear(gfx)`.

4. Stock-to-modular conversion (`CMD_BUILD_MODULAR_AIRPORT_FROM_STOCK`): exempt from year check. Conversion represents an existing layout.

### Files

- `src/modular_airport_cmd.h` (year helper)
- `src/modular_airport_gui.cpp` (GUI disable)
- `src/station_cmd.cpp` (command validation)
- `src/lang/english.txt` (error string)

### Acceptance

- [ ] Locked pieces are unplaceable via both UI and command path.
- [ ] Unlock happens exactly on year boundary.
- [ ] Stock-to-modular conversion works regardless of year.
- [ ] Picker windows show lock state and unlock year tooltip.

---

## 5) Large-plane safety + mixed runway routing

These are one feature: large planes need safe runways, and when an airport has both safe and unsafe runways, large planes use only the safe ones.

### Aircraft classification

`AIR_FAST` flag (bit 1 of `AircraftVehicleInfo.subtype`). This matches the existing crash logic in `aircraft_cmd.cpp:162` and `1419` which already uses `AIR_FAST` to determine crash risk on short strips.

Helper: `IsLargeAircraft(const Aircraft *v)` — checks `AircraftVehInfo(v->engine_type)->subtype & AIR_FAST`.
Engine variant: `IsLargeAircraftEngine(EngineID e)` — same check from engine info.

### Runway safety criteria

A runway is "safe for large aircraft" when ALL of:
1. **Large runway family:** all tiles are `APT_RUNWAY_1-5` or `APT_RUNWAY_END` (not `APT_RUNWAY_SMALL_*`).
2. **Contiguous length >= 6 tiles.**

Helper: `IsRunwaySafeForLarge(const Station *st, TileIndex runway_end)`. Walks the contiguous segment (reuse the same walk logic as `CmdSetRunwayFlags`), checks family and counts tiles.

### Station-level eligibility

A modular airport "supports large aircraft" when ALL of:
1. At least one safe runway exists (per above).
2. At least one tower tile: `APT_TOWER` or `APT_TOWER_FENCE_SW`.
3. At least one big terminal tile: `APT_ROUND_TERMINAL`, `APT_BUILDING_1`, `APT_BUILDING_2`, `APT_BUILDING_3`, `APT_STAND_1`, `APT_STAND_PIER_NE`. (Not `APT_LOW_BUILDING*`.)

Helper: `ModularAirportSupportsLargeAircraft(const Station *st)`. Scans `modular_tile_data` once, checking all three criteria.

### Enforcement points

**Do NOT modify `CanVehicleUseStation()`.** Blocking there would prevent order assignment to airports under construction (tower not placed yet) and invalidate orders if the tower is temporarily removed.

Instead:

1. **Order creation warning** (not block) in `order_cmd.cpp`: when assigning a large aircraft to a modular airport that doesn't meet criteria, show a warning similar to the existing short-strip warning at line 1713. Allow the order.

2. **Runtime landing filter** in `FindModularLandingTarget()`: when `IsLargeAircraft(v)`, skip runway candidates where `!IsRunwaySafeForLarge(st, tile)`. If no safe runway is available, return `INVALID_TILE` — aircraft stays in holding pattern indefinitely (per decision).

3. **Runtime takeoff filter** in `FindModularRunwayTileForTakeoff()`: same safety filter for large aircraft.

4. **Small aircraft:** no filtering. They can use any runway.

### Files

- `src/modular_airport_cmd.cpp` (helpers, landing/takeoff filters)
- `src/modular_airport_cmd.h` (helper declarations)
- `src/order_cmd.cpp` (warning at order creation)
- `src/lang/english.txt` (warning strings)

### Acceptance

- [ ] Large aircraft only land on/take off from safe runways.
- [ ] Large aircraft hold indefinitely when no safe runway is free.
- [ ] Small aircraft can use any runway, unaffected.
- [ ] Orders can be assigned to unsafe airports (with warning).
- [ ] Criteria update live: placing/removing tower/terminal/runway tiles immediately changes eligibility.
- [ ] Mixed airports correctly route large planes to safe runways and small planes to any runway.

---

## 6) Grass taxiable with paved preference

### Design

Make all grass tiles taxiable with a 4x movement cost penalty so paved routes are preferred when available.

Grass tiles are taxiable. Empty tiles (`APT_EMPTY`, `APT_EMPTY_FENCE_NE`) are NOT taxiable.

#### Grass tile IDs (all taxiable)

`APT_GRASS_FENCE_SW` (36), `APT_GRASS_2` (37), `APT_GRASS_1` (38), `APT_GRASS_FENCE_NE_FLAG` (39), `APT_GRASS_FENCE_NE_FLAG_2` (73).

NOT taxiable: `APT_RADAR_GRASS_FENCE_SW` (31) — has a building on it.

#### Implementation

1. In `CalculateAutoTaxiDirectionsForGfx()` (`src/airport_pathfinder.cpp`), return `0x0F` for all grass GFX values listed above (currently returns `0x00`).

2. Add cost function in `src/airport_ground_pathfinder.cpp`:

```cpp
static int GetGroundMoveCost(uint8_t piece_type)
{
    switch (piece_type) {
        case APT_GRASS_FENCE_SW:
        case APT_GRASS_2:
        case APT_GRASS_1:
        case APT_GRASS_FENCE_NE_FLAG:
        case APT_GRASS_FENCE_NE_FLAG_2:
            return 4;
        default:
            return 1;
    }
}
```

3. In `FindAirportGroundPath()`, change the neighbor cost calculation:

```cpp
// Was: int tentative_g = current->g + 1;
int tentative_g = current->g + GetGroundMoveCost(to_data->piece_type);
```

The Manhattan distance heuristic remains admissible (it underestimates since all costs >= 1), so A* correctness is preserved.

#### A* iteration limit consideration

The pathfinder has a hard limit of `MAX_PATHFINDER_ITERATIONS = 1000`. With grass tiles becoming traversable, airports with large grass areas have bigger search spaces. The 4x cost helps (A* naturally explores fewer grass tiles) but very large grass-surrounded airports could hit the limit. Monitor this — may need to increase the limit if it becomes a practical issue.

### Files

- `src/airport_pathfinder.cpp` (taxi direction enable)
- `src/airport_ground_pathfinder.cpp` (cost function + A* cost change)

### Acceptance

- [ ] Paved route chosen when available.
- [ ] Grass route used as fallback when paved is unavailable.
- [ ] Empty tiles remain non-taxiable.
- [ ] Radar-on-grass tile remains non-taxiable.
- [ ] No pathfinder performance regression on normal airports.

---

## 7) Edge fence system

### Problem

No way to block aircraft from crossing between specific tiles. Players need barriers between apron and runway, or between parallel taxiways.

### Data model

Add `uint8_t edge_block_mask` to `ModularAirportTileData` in `src/base_station_base.h`. Bits: N=0x01, E=0x02, S=0x04, W=0x08 (matching existing direction bit conventions).

Default: 0 (no fences).

### Command

New command `CMD_SET_MODULAR_AIRPORT_EDGE_FENCE` in `command_type.h`.

Parameters: `TileIndex tile`, `uint8_t edge_bit` (single edge: 0x01/0x02/0x04/0x08), `bool set` (true=add fence, false=remove).

Handler logic:
1. Validate tile belongs to a modular airport.
2. Validate `edge_bit` is exactly one bit.
3. Toggle the bit in `edge_block_mask`.
4. If the adjacent tile in that direction belongs to the same station, toggle the opposite bit on the neighbor (`N<->S`, `E<->W`).
5. If the adjacent tile is outside the airport (perimeter edge), set/clear only on the source tile.

### Pathfinding integration

In `CanTilesConnect()` (`src/airport_ground_pathfinder.cpp`), add a check after the existing direction validation:

```cpp
// After getting dir_bit for from->to direction:
if (from_data->edge_block_mask & dir_bit) return false;
// Reverse direction bit for the 'to' side:
uint8_t reverse_bit = ((dir_bit << 2) | (dir_bit >> 2)) & 0x0F;
if (to_data->edge_block_mask & reverse_bit) return false;
```

Fences coexist with one-way taxi arrows — they are orthogonal constraints. A tile can have both a one-way restriction and fences on specific edges.

### Rendering

In `DrawTile_Station`, unify fence rendering:
- When `edge_block_mask` has a bit set for an edge, always draw a fence sprite on that edge.
- When `edge_block_mask` is 0 for an edge, the existing auto-perimeter logic applies.
- This prevents double fences or visual conflicts between explicit and auto-perimeter fences.

### GUI

Add a fence tool button in the modular builder toolbar (`src/modular_airport_gui.cpp`, new widget in `src/widgets/airport_widget.h`).

When active, clicks on tiles determine the target edge by cursor position relative to tile center (same technique as road stop direction in `road_gui.cpp`):
- Cursor in N quadrant of tile → N edge
- Cursor in E quadrant → E edge
- etc.

Click posts `CMD_SET_MODULAR_AIRPORT_EDGE_FENCE`. Second click on same edge removes it (toggle).

### Save/load

1. New savegame version `SLV_MODULAR_AIRPORT_FENCE = 368` in `src/saveload/saveload.h`. Update `SL_MAX_VERSION` to 369.
2. Add `SLE_CONDVAR(ModularAirportTileData, edge_block_mask, SLE_UINT8, SLV_MODULAR_AIRPORT_FENCE, SL_MAX_VERSION)` in `SlModularAirportTileData` (`src/saveload/station_sl.cpp`).
3. Old saves: field defaults to 0 (no fences).

This is the only feature in this plan that requires a new savegame version. All other features modify behavior without adding persistent fields.

### Files

- `src/base_station_base.h` (field)
- `src/command_type.h` (command ID)
- `src/station_cmd.h` (command trait)
- `src/station_cmd.cpp` (command handler)
- `src/airport_ground_pathfinder.cpp` (pathfinding block)
- `src/modular_airport_gui.cpp` (fence tool)
- `src/widgets/airport_widget.h` (widget ID)
- `src/station_cmd.cpp` (`DrawTile_Station` fence rendering)
- `src/saveload/saveload.h` (version)
- `src/saveload/station_sl.cpp` (field save/load)
- `src/lang/english.txt` (tooltip)

### Acceptance

- [ ] Fence edge blocks traversal in both directions.
- [ ] Mirror update works on adjacent tile within same airport.
- [ ] Perimeter fences (no neighbor) work on source tile only.
- [ ] Fences coexist with one-way taxi arrows.
- [ ] Fences allowed between any tile types (apron-apron, runway-runway, apron-runway).
- [ ] Save/load preserves fence edges.
- [ ] Old saves load with no fences (mask = 0).
- [ ] Multiplayer clients remain in sync.

---

## 8) Template save/load (phase 1)

### Design

Save modular airport layouts as named JSON templates. Place templates with all-or-nothing semantics and a white-footprint preview.

### Template format

JSON file in user data dir (`~/.openttd/airport_templates/` or platform equivalent).

```json
{
    "name": "My Airport",
    "version": 1,
    "width": 12,
    "height": 8,
    "tiles": [
        {
            "dx": 0, "dy": 0,
            "piece_type": 46,
            "rotation": 0,
            "runway_flags": 15,
            "one_way_taxi": false,
            "user_taxi_dir_mask": 0,
            "edge_block_mask": 0
        }
    ]
}
```

`dx`/`dy` are offsets from the top-left corner of the template bounding box. `piece_type` uses `AirportTiles` enum values. `edge_block_mask` included for future-proofing (defaults to 0 if fence feature not yet implemented).

OpenTTD already has `nlohmann/json` as a dependency (used in crash logs), so JSON parsing is straightforward.

### Save flow

1. Click "Save Template" in modular builder toolbar.
2. Click on an existing modular airport station.
3. Enter a name in a text input dialog.
4. System captures all `ModularAirportTileData` entries, normalizes offsets to (0,0) origin, writes JSON.

### Load/place flow

1. Click "Load Template" in modular builder toolbar.
2. Template list window opens (reads files from templates dir).
3. Select a template. Cursor changes to placement mode.
4. **Preview:** Show white-footprint rectangle (same style as stock airport placement) covering the full template bounding box. Player positions it on the map.
5. **Validation (client-side):** On click, run `CmdBuildModularAirportTile` with test flag for every tile. If any fails, show error, don't place.
6. **Execution:** If all pass, execute all tile placement commands, then flag commands.

### Command ordering

Template placement must respect dependency order:
1. Place all tiles (`CMD_BUILD_MODULAR_AIRPORT_TILE`).
2. Set runway flags (`CMD_SET_RUNWAY_FLAGS`) — flags inherit from neighbors during placement, but template may need specific overrides.
3. Set taxiway one-way flags (`CMD_SET_TAXIWAY_FLAGS`).
4. Set edge fences (`CMD_SET_MODULAR_AIRPORT_EDGE_FENCE`) if fence feature exists.

### Atomicity consideration

The test-then-execute approach is not atomic in multiplayer (state can change between validation and execution). For true atomicity, a `CMD_PLACE_MODULAR_AIRPORT_TEMPLATE` command that validates and places all tiles in one handler would be better. This requires a variable-length command payload (tile list), which OpenTTD's command system supports via `std::vector` parameters. Recommend this approach over sequential commands.

### Files

- `src/modular_airport_gui.cpp` (UI buttons, placement preview, template list window)
- `src/widgets/airport_widget.h` (new widget IDs)
- New: `src/airport_template.cpp`, `src/airport_template.h` (template manager: load/save/list/delete)
- `src/station_cmd.cpp` (atomic placement command if using single-command approach)
- `src/station_cmd.h` (command trait)
- `src/command_type.h` (command ID)
- `src/lang/english.txt` (strings)

### Acceptance

- [ ] Saved template round-trips exactly (all fields preserved).
- [ ] Failed placement leaves map unchanged.
- [ ] White-footprint preview shows full template extent before placement.
- [ ] Template files are human-readable JSON.
- [ ] Template files can be copied between installs and loaded by another player.
- [ ] Error messaging indicates why placement failed (terrain, ownership, collision).

---

## 9) Stock picker integration (phase 2, defer)

"Saved custom airports" section appears in the stock airport build window after helicopter airports.

### Design approach

- Extend `BuildAirportWindow` list model with a virtual section populated from the template manager.
- On selection, show template preview and use the same placement path from phase 1.
- Do not register dynamic `AirportSpec` entries — keep templates as a UI-level concept.

### Files

- `src/airport_gui.cpp`
- `src/widgets/airport_widget.h`
- Template manager integration

Defer until phase 1 is stable.

---

## Cross-cutting notes

### Determinism/multiplayer

- All simulation-state changes must go through commands. Never mutate game state from draw/UI code.
- Use integer arithmetic only in pathfinding and scoring. No floating-point branching on simulation paths.
- The holding loop computation uses floating-point for Dubins curves but the waypoint list is integer-coordinate. This is fine — loop shape is cosmetic, gate checks use integer tile coordinates.

### Performance

- Reuse `modular_tile_index` (hash map) for all tile lookups. Don't scan the full `modular_tile_data` vector when a specific tile is needed.
- `ModularAirportSupportsLargeAircraft()` scans all tiles — acceptable since it's called at order creation and at landing/takeoff candidate evaluation (infrequent). Don't cache; the result changes when tiles are added/removed.
- `IsRunwaySafeForLarge()` walks the contiguous runway (max ~20 tiles). Called per-candidate during landing/takeoff selection. Acceptable cost.
