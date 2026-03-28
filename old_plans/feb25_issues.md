# Modular Airport Issues & Ideas - Design Document

_February 25, 2026_

---

## Issue 1: Stock Airport Fences When Building as Modular

**Problem:** When converting a stock FTA airport to modular via `CmdBuildModularAirportFromStock`, fence information is lost. Stock airports use decorative fence tile variants (e.g. `APT_APRON_FENCE_NW`, `APT_RUNWAY_END_FENCE_SE`), but `MapStockGfxToModularPiece` maps them all to their plain functional equivalents (`APT_APRON`, `APT_RUNWAY_END`, etc.). The resulting modular airport has `edge_block_mask = 0` on every tile.

**Current code path:**
- `station_cmd.cpp:3177-3306` - `MapStockGfxToModularPiece()` strips fence variants
- `station_cmd.cpp:3470-3683` - `CmdBuildModularAirportFromStock()` creates tiles with no edge fences
- The modular system uses `edge_block_mask` (4 bits: N=0x01, E=0x02, S=0x04, W=0x08) in `ModularAirportTileData` instead of decorative fence tile variants

**Stock fence inventory:** 98 fence tile placements across all 9 stock airports. Examples:
- City Airport: 8 fence tiles
- International: 9 fence tiles
- Intercontinental: 22 fence tiles

**Design:**

Add a post-conversion pass in `CmdBuildModularAirportFromStock` that sets `edge_block_mask` bits based on the original stock tile's fence variant. Create a mapping table:

```
Stock tile                    -> edge_block_mask bits to set
APT_APRON_FENCE_NW            -> 0x01 (N edge)
APT_APRON_FENCE_SW            -> 0x08 (W edge)
APT_APRON_FENCE_NE            -> 0x02 (E edge)
APT_APRON_FENCE_SE            -> 0x04 (S edge)
APT_APRON_FENCE_NE_SW         -> 0x02 | 0x08 (E+W)
APT_APRON_FENCE_SE_SW         -> 0x04 | 0x08 (S+W)
APT_APRON_FENCE_NE_SE         -> 0x02 | 0x04 (E+S)
APT_APRON_N_FENCE_SW          -> 0x08 (W edge)
APT_RUNWAY_END_FENCE_SE       -> 0x04 (S)
APT_RUNWAY_END_FENCE_NW       -> 0x01 (N)
APT_RUNWAY_END_FENCE_NW_SW    -> 0x01 | 0x08
APT_RUNWAY_END_FENCE_SE_SW    -> 0x04 | 0x08
APT_RUNWAY_END_FENCE_NE_NW    -> 0x02 | 0x01
APT_RUNWAY_END_FENCE_NE_SE    -> 0x02 | 0x04
APT_RUNWAY_FENCE_NW           -> 0x01 (N)
APT_HELIPAD_2_FENCE_NW        -> 0x01
APT_HELIPAD_2_FENCE_NE_SE     -> 0x02 | 0x04
APT_HELIPAD_3_FENCE_NW        -> 0x01
APT_HELIPAD_3_FENCE_NW_SW     -> 0x01 | 0x08
APT_HELIPAD_3_FENCE_SE_SW     -> 0x04 | 0x08
APT_TOWER_FENCE_SW            -> 0x08
APT_RADAR_FENCE_SW            -> 0x08
APT_RADAR_FENCE_NE            -> 0x02
(etc.)
```

**Implementation:**

Set `edge_block_mask` directly on each `ModularAirportTileData` during the single `CmdBuildModularAirportFromStock` command execution. Do NOT call `CmdSetModularAirportEdgeFence` from within the conversion — calling sub-commands breaks command atomicity and has test/network semantics issues.

1. Create a `uint8_t GetStockFenceEdgeMask(uint8_t stock_gfx)` helper that maps stock gfx IDs to edge mask bits.

2. In the tile placement loop (where `MapStockGfxToModularPiece` is called), retain the original stock gfx. After creating the `ModularAirportTileData`, set `data.edge_block_mask = GetStockFenceEdgeMask(original_gfx)`.

3. Mirror fence edges to neighbors: for each bit set in `edge_block_mask`, also set the reverse bit on the adjacent tile's data. This can be done in a post-pass over the tile data vector since all tiles are created within the same command.

**Files to change:**
- `src/station_cmd.cpp` - Add fence edge mask lookup + direct data mutation in `CmdBuildModularAirportFromStock`

---

## Issue 2: One-Way Taxi Arrows on Stock-to-Modular Conversion

**Problem:** Stock FTA airports have implicit taxi direction through their state machine movement data (aircraft always taxi in specific directions on specific tiles). When converted to modular, all tiles get `one_way_taxi = false` and `user_taxi_dir_mask = 0x0F` (all directions), losing the directional flow.

**Current code path:**
- FTA movement data in `src/table/airport_movement.h` defines explicit (x, y, heading) waypoints per airport
- `station_cmd.cpp:3470-3683` - conversion creates tiles with no one-way flags
- Modular one-way system: `one_way_taxi` bool + `user_taxi_dir_mask` (N=0x01, E=0x02, S=0x04, W=0x08)
- Only `IsTaxiwayPiece()` types can be one-way (aprons, crossings - not stands, hangars, runways)

**Design:**

This requires per-airport analysis of the FTA movement data to identify tiles where aircraft only ever taxi in one direction. For each stock airport, create a static table of `(dx, dy, direction_bit)` tuples specifying which tiles should be one-way.

**Per-airport one-way analysis needed:**

The stock airports that benefit most from one-way arrows are the larger ones with separate taxi-in/taxi-out paths:
- **International airport** - has parallel taxiways, one for taxi-out (toward runway), one for taxi-in (toward terminals)
- **Intercontinental airport** - complex dual-runway layout with directed taxi flow
- **Metropolitan airport** - some directional flow around terminal area

Smaller airports (Country, City, Commuter) generally have simpler layouts where aircraft taxi back and forth on the same path, so one-way arrows may not apply or may be minimal.

**Implementation:**

1. For each stock airport type, define a static array of one-way configurations:
   ```cpp
   struct StockOneWayConfig {
       uint8_t dx, dy;           // offset in layout grid
       uint8_t taxi_dir_mask;    // which direction is allowed (single bit)
   };
   ```

2. In `CmdBuildModularAirportFromStock`, set `one_way_taxi` and `user_taxi_dir_mask` directly on each `ModularAirportTileData` during the same command execution. Do NOT call `CmdSetTaxiwayFlags` as a sub-command — mutate the data directly for command atomicity.

3. The one-way configs must be manually determined by studying each airport's FTA movement data and identifying tiles where all aircraft movements go in the same direction.

**Files to change:**
- `src/station_cmd.cpp` - Add per-airport one-way config tables + application pass in `CmdBuildModularAirportFromStock`

**Work required:** Manual analysis of FTA movement paths for each stock airport to determine one-way tiles. This is design work, not just coding.

---

## Issue 3: Airships and UFOs with Modular Airports

**Problem:** How should airships (Zeppelins) and UFOs interact with modular airports?

**Current state:**

Both Zeppelins and UFOs are `DisasterVehicle` instances (`VEH_DISASTER`), not `Aircraft` (`VEH_AIRCRAFT`). They have completely separate vehicle types, no engines, no orders, no cargo, and no player control.

- **Zeppelins** (`ST_ZEPPELINER` in `disaster_vehicle.cpp:225-308`): Fly to a random airport and _crash_ on it. The crash check at line 245 uses `IsAirportTile()` which matches _any_ airport tile (including modular) — it's just `IsTileType(Station) && IsAirport()`. However, `Disaster_Zeppeliner_Init()` (line 741) only targets `AT_SMALL` or `AT_LARGE` FTA airports, so Zeppelins never aim at modular airports. They could still accidentally crash on one if they fly over it.

  **Existing bug:** The Zeppelin is supposed to block the runway via `AirportBlock::Zeppeliner`, but the code only ever _resets_ this flag (lines 267, 304) — it never _sets_ it. The `Set` call is missing entirely. So even on classic airports, Zeppelin crashes don't actually block the runway as intended.

- **Small UFOs** (`ST_SMALL_UFO` in `disaster_vehicle.cpp:318-360`): Hunt road vehicles. No airport interaction whatsoever.

- **Big UFOs** (`ST_BIG_UFO` in `disaster_vehicle.cpp:463-608`): Land on railroad tracks. No airport interaction.

**Design: Make Zeppelin crashes work on modular airports**

The Zeppelin disaster has two problems: (1) targeting only classic airports, and (2) a pre-existing bug where the runway block flag is never set. The fix needs to address both:

1. **Fix the missing Set call (classic + modular):** In `DisasterTick_Zeppeliner()`, when the Zeppelin enters crash state (line 246, `v->state = 1`), add `st->airport.blocks.Set({AirportBlock::Zeppeliner, AirportBlock::RunwayIn})` to actually block the runway. The Reset calls at lines 267 and 304 already exist for cleanup.

2. **Expand targeting** in `Disaster_Zeppeliner_Init()` (line 741): Currently only targets `AT_SMALL` or `AT_LARGE`. Add a check for stations with `AirportBlock::Modular` flag, picking a runway tile from the modular layout as the crash target.

3. **Modular runway blocking:** For modular airports, `AirportBlock::RunwayIn` may not be meaningful (modular uses segment reservation, not FTA blocks). Options:
   - Reserve the modular runway segment for the crash duration via the segment reservation system
   - Or set a station-level flag that the modular landing code checks

UFOs do not interact with airports — no changes needed for them.

**Files to change:**
- `src/disaster_vehicle.cpp` - Zeppelin init targeting + crash tile detection + missing Set call

---

## Issue 4: 3-Tile Terminal Building (Small Country Airport)

**Problem:** The small country airport has a 3-tile terminal building (`APT_SMALL_BUILDING_1`, `APT_SMALL_BUILDING_2`, `APT_SMALL_BUILDING_3`), but in the modular builder these are individual cosmetic pieces placed one at a time. The user wants to place all 3 tiles at once with a single click, no rotation.

**Current layout** (from `airport_defaults.h:25-43`):
```
X=0                  X=1                  X=2                  X=3
APT_SMALL_BUILDING_1 APT_SMALL_BUILDING_2 APT_SMALL_BUILDING_3 APT_SMALL_DEPOT_SE
APT_GRASS_FENCE_NE   APT_GRASS_1          APT_GRASS_2          APT_GRASS_FENCE_SW
APT_RUNWAY_SMALL_*   APT_RUNWAY_SMALL_*   APT_RUNWAY_SMALL_*   APT_RUNWAY_SMALL_*
```

The 3 building tiles run left-to-right: building_1 (left wing), building_2 (center), building_3 (right wing).

**Current multi-tile support:** The modular builder already has drag-to-build for runways (`OnPlaceMouseUp` in `modular_airport_gui.cpp:569-668`), which auto-places end pieces. But cosmetic pieces (index 3 in the palette, which opens the sub-picker) are placed one tile at a time.

**Design:**

Add a new "compound piece" concept to the cosmetic picker. When the user selects the "Small Terminal" compound piece and clicks a tile, place all 3 tiles in a fixed row.

**Implementation:**

1. Add a new entry in `_cosmetic_pieces[]` (or a separate compound pieces list) for "Small Terminal Building" that maps to `{APT_SMALL_BUILDING_1, APT_SMALL_BUILDING_2, APT_SMALL_BUILDING_3}` placed at offsets `{(0,0), (1,0), (2,0)}`.

2. When this compound piece is selected and the user clicks, the GUI posts 3 sequential `CmdBuildModularAirportTile` commands for the 3 tiles at the clicked position + offsets.

3. No rotation means the offsets are fixed (always extends in the +X direction from the click point). The user requirement explicitly says "no rotation."

4. Validation: all 3 target tiles must be buildable (flat, not occupied, etc.). If any tile fails, none should be placed. Use `DC_NO_WATER` + test mode first, then execute.

5. The compound piece button could show a preview of the 3-tile building in the GUI.

**Template rotation constraint:** If a user places a 3-tile terminal, saves it as a template, then loads the template with rotation, the asymmetric building tiles would be incorrectly oriented (they have no rotated variants). Block rotated template placement for any template containing compound pieces like the 3-tile terminal. In the template placement GUI, disable the rotation control and show a tooltip explaining that the template contains non-rotatable pieces. In `CmdPlaceModularAirportTemplate`, reject placement with rotation != 0 if any tile in the template is `APT_SMALL_BUILDING_1/2/3`.

**Files to change:**
- `src/modular_airport_gui.cpp` - Add compound piece to cosmetic picker, handle multi-tile placement on click
- `src/modular_airport_template_cmd.cpp` - Reject rotated placement of templates containing non-rotatable compound pieces
- `src/airport_template_gui.cpp` - Disable rotation control when template has non-rotatable pieces

---

## Issue 5: Helicopters Take Off from Stands (Should Use Runway)

**Problem:** When a helicopter is parked at a stand (because no helipad exists), it takes off vertically from the stand instead of taxiing to a runway first. Fixed-wing aircraft properly route through runway queues for takeoff; helicopters skip this entirely.

**Root cause:** `aircraft_cmd.cpp:1749-1751`:
```cpp
if (v->subtype == AIR_HELICOPTER && !go_to_hangar) {
    v->state = HELITAKEOFF;
    return;
}
```

This unconditionally enters `HELITAKEOFF` for all helicopters regardless of what tile they're on. The `HELITAKEOFF` state (`modular_airport_cmd.cpp:1555-1570`) just climbs vertically from the current position.

**Contrast with fixed-wing takeoff** (`aircraft_cmd.cpp:1754-1783`):
1. `FindModularRunwayTileForTakeoff(st, v)` - finds a runway
2. `FindModularTakeoffQueueTile(st, v, runway)` - finds queue position
3. Sets `modular_ground_target = MGT_RUNWAY_TAKEOFF`
4. Aircraft taxis to runway, enters `TAKEOFF` state only at runway

**Design:**

Modify the helicopter takeoff path in `AircraftEventHandler_AtTerminal` to check the current tile:

```
if helicopter is on a helipad:
    v->state = HELITAKEOFF   (vertical takeoff from pad - current behavior, correct)
else if helicopter is on a stand:
    find runway with FindModularRunwayTileForTakeoff()
    if runway found:
        taxi to runway, use runway takeoff (same as fixed-wing)
    else:
        v->state = HELITAKEOFF  (fallback: vertical takeoff if no runway exists at all)
```

**Implementation:**

There are **two code paths** where helicopters unconditionally enter `HELITAKEOFF` without runway routing:

1. **Terminal departure** (`aircraft_cmd.cpp:1749-1751`): Helicopter at a stand enters `HELITAKEOFF`.
2. **Hangar exit** (`aircraft_cmd.cpp:1647-1650`): Helicopter leaving a hangar enters `HELITAKEOFF`.

Both need the same fix:

For each path, before setting `HELITAKEOFF`:
- Get the tile's `ModularAirportTileData`
- Check `IsModularHelipadPiece(data->piece_type)` (for terminal path) or check if exiting a hangar (for hangar path)
- If on helipad: `HELITAKEOFF` (existing behavior, correct)
- If on stand or hangar: use the fixed-wing takeoff path (find runway via `FindModularRunwayTileForTakeoff`, set queue tile, set `MGT_RUNWAY_TAKEOFF`)
- If no runway exists at all: fall back to `HELITAKEOFF` (vertical takeoff as last resort)

Helicopters using a runway for takeoff would enter `TAKEOFF` state (runway roll) rather than `HELITAKEOFF` (vertical climb). This may look odd visually. Alternative: add a `HELI_RUNWAY_TAKEOFF` state that taxis to the runway, then does a vertical climb from the runway end. But this is extra complexity - consider whether the visual matters enough.

**Files to change:**
- `src/aircraft_cmd.cpp` - Modify helicopter takeoff logic in both `AircraftEventHandler_AtTerminal` (line 1749) and the hangar exit path (line 1647)

---

## Issue 6: Multiple Helicopters on Same Helipad

**Problem:** Multiple helicopters can occupy the same helipad simultaneously. Helipads should be exclusive (one helicopter at a time), like stands are for planes.

**Root cause:** Three bugs that compound:

### Bug A: `FindFreeModularHelipad` missing occupancy check
**Location:** `modular_airport_cmd.cpp:1874-1917`

The function checks tile reservation (`HasAirportTileReservation`) but does NOT check physical occupancy. Compare with `FindFreeModularTerminal` (line 1850) which does:
```cpp
if (v != nullptr && IsModularTileOccupiedByOtherAircraft(st, data.tile, v->index)) continue;
```

The helipad function is missing this check. Two helicopters can be assigned to the same helipad if the first hasn't reserved it yet (e.g., still airborne during descent).

### Bug B: `HandleModularGroundArrival` desync check skips helipads
**Location:** `modular_airport_cmd.cpp:2441-2463`

The arrival safety check that prevents stacking only fires for `MGT_TERMINAL`:
```cpp
if (v->modular_ground_target == MGT_TERMINAL && IsModularTileOccupied...) { ... }
```

When `modular_ground_target == MGT_HELIPAD`, no occupancy check runs. A helicopter arriving at an occupied helipad is not redirected.

### Bug C: Pathfinder doesn't treat helipads as parking tiles
**Location:** `airport_ground_pathfinder.cpp:75-90`

`IsParkingOnlyTile()` only returns true for `APT_STAND`, `APT_STAND_1`, `APT_STAND_PIER_NE`. Helipad types are not included, so the pathfinder's occupancy avoidance (lines 147-152) doesn't apply to helipads.

**Design:**

### Fix A: Add occupancy check to `FindFreeModularHelipad`
After the reservation check, add:
```cpp
if (v != nullptr && IsModularTileOccupiedByOtherAircraft(st, data.tile, v->index)) continue;
```

### Fix B: Extend desync check to `MGT_HELIPAD`
Change the condition from:
```cpp
if (v->modular_ground_target == MGT_TERMINAL && ...)
```
to:
```cpp
if ((v->modular_ground_target == MGT_TERMINAL || v->modular_ground_target == MGT_HELIPAD) && ...)
```
And add helipad re-routing (try another helipad, then fall back to terminal).

### Fix C: Add helipads to `IsParkingOnlyTile`
Add all helipad piece types to the switch (or call `IsModularHelipadPiece(piece_type)` from the pathfinder check alongside the existing stand check).

**Note:** Fix C alone is insufficient. The pathfinder check at line 150-152 uses `HasAirportTileReservation` (reservation flag), not physical occupancy (`IsModularTileOccupiedByOtherAircraft`). This means there's a race window between when a helicopter is assigned to a helipad and when it actually writes the reservation. Fix A (occupancy check at assignment time) and Fix B (arrival desync check) close that race. Fix C prevents other aircraft from _routing through_ an occupied/reserved helipad, which is a different (complementary) protection. All three fixes together are needed.

**Files to change:**
- `src/modular_airport_cmd.cpp` - Fix A (FindFreeModularHelipad) and Fix B (HandleModularGroundArrival)
- `src/airport_ground_pathfinder.cpp` - Fix C (IsParkingOnlyTile or the occupancy check)

---

## Idea 1: Diagonal Taxiing

**Status: DEFERRED** — Do not implement yet.

Significant architectural change (pathfinder, 4-bit to 8-bit direction masks, edge/corner blocking, new sprites, saveload). High effort relative to benefit. Revisit after core modular system is stable.

---

## Implementation Status

| # | Issue | Status | Notes |
|---|-------|--------|-------|
| 6 | Helipad exclusivity bug | DONE | All 3 fixes (A/B/C) implemented. FindFreeModularHelipad occupancy check, HandleModularGroundArrival desync check for MGT_HELIPAD, IsParkingOnlyTile includes helipads. |
| 5 | Helicopter takeoff from stands | DONE | Terminal path: falls through to runway flow when not on helipad. Hangar path: tries runway first, vertical fallback. Both paths have vertical fallback if no runway exists. |
| 1 | Stock airport fences | DONE | `GetStockFenceEdgeMask()` mapping table + direct `edge_block_mask` mutation during `CmdBuildModularAirportFromStock` + fence mirroring post-pass. |
| 2 | Stock airport one-way arrows | TODO | Requires manual per-airport FTA movement analysis. |
| 4 | 3-tile terminal building | DONE | Compound piece in cosmetic picker via template command. Rotation blocked in `CmdPlaceModularAirportTemplate` and template GUI for templates with `APT_SMALL_BUILDING_*` pieces. |
| 3 | Zeppelin crashes on modular | PARTIAL | Targeting expanded to include modular airports. Missing `Set` call for `AirportBlock::Zeppeliner` fixed. Still TODO: modular-specific runway blocking (modular uses segment reservation, not FTA blocks). |
| D1 | Diagonal taxi | DEFERRED | — |

### Additional fixes in this pass
- **Template command atomicity**: Refactored to 3-pass approach (Test tiles, Build tiles, Apply metadata). Removed unsafe validation bypasses from `CmdSetRunwayFlags`/`CmdSetTaxiwayFlags`/`CmdSetModularAirportEdgeFence` that skipped checks in test mode.
- **Template station joining**: `GetStationAroundModular` helper for finding stations during template placement. Station ID tracked across tile placements to ensure all join the same station.
