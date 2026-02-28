# Plan: Button Size Fix + Ideas from ideas.md

## Context

The modular airport toolbar was recently updated to use `ScaleGUITrad(64)×ScaleGUITrad(48)` for button sizing. This is too large for the actual icon sprites (small preview sprites ~32px wide) — icons are right-sized but surrounded by excessive empty space. The fix is to measure actual icon sizes.

`ideas.md` lists 7 ideas for the modular airport system. 4 are reasonably implementable now.

---

## ideas.md Feasibility

| Idea | Verdict |
|------|---------|
| 1. Runway dropdown (big/small) | Defer — less important once #2 removes the end button |
| **2. Auto runway ends / remove end button** | **Yes — medium effort** |
| 3. Air bridge stand adjacent to round terminal | Out of scope — no suitable art assets identified |
| 4. Pre-built airport templates | Out of scope — too complex |
| **5. Nicer holding patterns** | **Yes — low effort** |
| 6. Broken planes follow same pattern | Already works (same code path as #5) |
| **7. Throughput display (planes/month)** | **Yes — low-moderate effort** |

---

## Part 1 — Button Size Fix

**File**: `src/airport_gui.cpp`

Replace `UpdateWidgetSize` in `BuildModularAirportWindow`. Instead of `ScaleGUITrad(64)×ScaleGUITrad(48)` (oversized fixed value), measure actual icon sizes and take the max across all pieces so all buttons are uniform:

```cpp
void UpdateWidgetSize(WidgetID widget, Dimension &size, ...) override
{
    if (widget < WID_MA_PIECE_FIRST || widget > WID_MA_PIECE_LAST) return;
    Dimension max_d = {};
    for (int i = 0; i < PIECE_COUNT; ++i) {
        const auto &p = _modular_airport_pieces[i];
        Dimension d = (i == MODULAR_AIRPORT_PIECE_ERASE_INDEX)
            ? GetSpriteSize(p.icon)                    // matches DrawWidget (no zoom arg)
            : GetSpriteSize(p.icon, nullptr, ICON_ZOOM);
        max_d.width  = std::max(max_d.width,  d.width);
        max_d.height = std::max(max_d.height, d.height);
    }
    size.width  = std::max(size.width,  max_d.width  + WidgetDimensions::scaled.bevel.Horizontal() + ScaleGUITrad(4));
    size.height = std::max(size.height, max_d.height + WidgetDimensions::scaled.bevel.Vertical()   + ScaleGUITrad(4));
}
```

`ScaleGUITrad(4)` = 2 px margin each side, DPI-stable. This is the standard OpenTTD approach for toolbar button sizing and ensures buttons exactly fit their icons.

---

## Part 2 — Auto Runway Ends (Remove Runway End Button)

**Goal**: Large runway drags automatically place `APT_RUNWAY_END` tiles at both endpoints. The manual runway-end button (piece 1) is removed from the toolbar.

**Why**: Small runway already auto-places ends (`APT_RUNWAY_SMALL_NEAR_END`/`FAR_END`) in the smart-drag loop. Large runway should do the same.

**Files**: `src/airport_gui.cpp`, `src/widgets/airport_widget.h`, `src/lang/english.txt`

### Piece re-numbering after removing piece 1

| New index | Piece | Old index |
|-----------|-------|-----------|
| 0 | Runway (large) | 0 |
| 1 | Small runway | 2 |
| 2 | Cosmetic (picker) | 3 |
| 3 | Hangar large | 4 |
| 4 | Small hangar | 5 |
| 5 | Helipad | 6 |
| 6 | Stand | 7 |
| 7 | Apron | 8 |
| 8 | Grass | 9 |
| 9 | Empty | 10 |
| 10 | Erase | 11 |

→ 11 buttons total (was 12). Update `WID_MA_PIECE_LAST = WID_MA_PIECE_10` in `airport_widget.h`.

### Auto-end placement logic

After the large runway drag places `APT_RUNWAY_5` tiles in `OnPlaceMouseUp`, call a new helper `PlaceLargeRunwayEnds(start_tile, end_tile, rotation)`:

1. Determine runway axis from `rotation` (0/2 = X-axis, 1/3 = Y-axis)
2. Walk from `start_tile` backward along the axis until no more runway tiles — that's the near endpoint
3. Walk from `end_tile` forward along the axis until no more runway tiles — that's the far endpoint
4. Place `APT_RUNWAY_END` at near endpoint via `CmdBuildModularAirportTile` (with `rotation`)
5. Place `APT_RUNWAY_END` at far endpoint (with `rotation ^ 2` to flip direction)

The contiguous runway walking logic mirrors what `CmdSetRunwayFlags` in `station_cmd.cpp` already does — examine that function (around line 3027) for the exact `IsRunwayPieceOnAxis` helper to reuse.

### Other changes

- Remove `_modular_airport_pieces[1]` (runway end entry)
- Remove `case 1: return APT_RUNWAY_END;` from `GetModularAirportPieceGfx`
- Remove `STR_STATION_BUILD_MODULAR_AIRPORT_PIECE_RUNWAY_END` string if unreferenced
- Update all `selected_piece` comparisons in `OnClick`, `PlaceSingleTileWithDialog`, `OnPlaceObject` for new indices
- `wants_picker` check: was `new_piece == 3 || 4 || 5`, now `new_piece == 2 || 3 || 4`
- `is_apron/grass/empty` checks: shift down by 1
- Hangar rotation check: `selected_piece == 3 || selected_piece == 4`

---

## Part 3 — Throughput Display (Planes/Month)

**Goal**: Show aircraft arrivals and departures per month in the airport station window.

**Files**: `src/base_station_base.h`, `src/aircraft_cmd.cpp`, `src/station_cmd.cpp` (monthly tick), `src/station_gui.cpp`, `src/saveload/station_sl.cpp`, `src/lang/english.txt`

### Step 1: Counters in Station

In `src/base_station_base.h` or `Station` struct, add four `uint16_t` fields:

```cpp
uint16_t airport_arrivals_this_month   = 0;
uint16_t airport_arrivals_last_month   = 0;
uint16_t airport_departures_this_month = 0;
uint16_t airport_departures_last_month = 0;
```

### Step 2: Increment in aircraft_cmd.cpp

- **Arrivals**: Find the `ENDLANDING`/`HELIENDLANDING` handler where the aircraft transitions from runway to ground taxi. Increment `st->airport_arrivals_this_month++`.
- **Departures**: Find the `ENDTAKEOFF` handler where state transitions to `FLYING`. Increment `st->airport_departures_this_month++`.

(Both should only count modular airports: guard with `st->airport.blocks.Test(AirportBlock::Modular)`.)

### Step 3: Monthly reset

Search `src/station_cmd.cpp` for the monthly station callback (look for `StationMonthlyLoop` or `OnNewMonth`):

```cpp
st->airport_arrivals_last_month   = st->airport_arrivals_this_month;
st->airport_departures_last_month = st->airport_departures_this_month;
st->airport_arrivals_this_month   = 0;
st->airport_departures_this_month = 0;
```

### Step 4: Display in station window (src/station_gui.cpp)

Add a line to the station window for modular airports, near where cargo or rating info is shown:

```cpp
if (st->airport.blocks.Test(AirportBlock::Modular)) {
    SetDParam(0, st->airport_arrivals_last_month);
    SetDParam(1, st->airport_departures_last_month);
    DrawString(r, STR_STATION_AIRPORT_THROUGHPUT);
}
```

String: `STR_STATION_AIRPORT_THROUGHPUT  :{BLACK}Airport: {NUM} arr / {NUM} dep last month`

### Step 5: Saveload

Add the four counters to `station_sl.cpp`'s station chunk descriptor. Non-critical — default to 0 if missing on load.

---

## Part 4 — Nicer Holding Patterns

**File**: `src/aircraft_cmd.cpp` (search for `hold_radius` or `hold_dx`)

### Changes

**1. More phases (smoother circle)**:
Replace 8-entry orbit table with 16-entry version covering 22.5° steps instead of 45°:

```cpp
static const int8_t hold_dx[16] = { 2, 2, 1, 1, 0,-1,-1,-2,-2,-2,-1,-1, 0, 1, 1, 2};
static const int8_t hold_dy[16] = { 0, 1, 1, 2, 2, 2, 1, 1, 0,-1,-1,-2,-2,-2,-1,-1};
uint8_t phase = ((v->tick_counter / 8) + v->index.base()) & 15;
// (halve tick_counter divisor from 16→8 to keep same orbital period)
```

**2. Vary radius per aircraft** (prevents visual overlap):

```cpp
int hold_r = (6 + (v->index.base() & 3)) * TILE_SIZE;
// = 6, 7, 8, or 9 tiles depending on aircraft ID
```

Scale `hold_dx[phase] * hold_r / (2 * TILE_SIZE)` to get actual pixel offset.

**3. Increase base radius** from 5 to 6 tiles minimum (less cramped visual).

**Note**: Broken-down planes already use the same code path — no separate fix needed.

---

## Implementation Order

1. **Button size fix** — trivial, do first (15 min)
2. **Auto runway ends** — independent, medium effort
3. **Throughput display** — independent, touches more files
4. **Holding pattern** — cosmetic, easy to verify visually

## Verification

After each step, build with `/Users/tor/ttd/OpenTTD/scripts/build_and_sign.sh`:

1. **Button fix**: Toolbar buttons are snug around icons (no excessive padding), all same width
2. **Runway ends**: Drag large runway → both ends auto-place; no manual end button in toolbar
3. **Throughput**: Open station window after aircraft operate → shows arrival/departure counts
4. **Holding pattern**: Multiple aircraft in holding pattern circle smoothly in wider, slightly offset orbits
