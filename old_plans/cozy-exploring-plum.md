# Plan: Holding Loop Overlay Toggle

## Context

The modular airport uses a Dubins-path holding loop. Debugging and tuning that loop requires visually inspecting the waypoints and approach paths, currently invisible in-game. This adds a toggle button next to "show directions" that draws:
- The full loop polyline (white lines between consecutive waypoints).
- A quadratic-Bezier approach curve per gate (approach fix → runway threshold, yellow).
- A red threshold marker at each runway landing spot.

## Architecture Decision

All drawing happens in a single **viewport post-draw pass** (not per-tile). This is the same mechanism used by the LinkGraph overlay (`vp.overlay->Draw`) — after `ViewportDrawParentSprites` all tiles are blitted, and `GfxDrawLine` draws directly in screen space. We do NOT use the `vp.overlay` slot (that's owned by the link graph system); instead we add a standalone `if` block right after it.

## Coordinate System

World pixel `(wx, wy)` → screen pixel:
```cpp
Point p = RemapCoords(wx, wy, 0);                              // landscape.h
p.x = UnScaleByZoom(p.x - vp.virtual_left, vp.zoom) + vp.left;
p.y = UnScaleByZoom(p.y - vp.virtual_top,  vp.zoom) + vp.top;
```
This is exactly the pattern used by `GetViewportStationMiddle` (`viewport.cpp:3584`). z=0 is fine for a debug overlay.

## Files to Change

| File | What |
|------|------|
| `src/widgets/airport_widget.h:50` | Add `WID_MA_TOGGLE_SHOW_HOLDING` after `WID_MA_TOGGLE_SHOW_ARROWS` |
| `src/lang/english.txt:3020` | Add string `STR_STATION_BUILD_MODULAR_AIRPORT_TOGGLE_SHOW_HOLDING` |
| `src/modular_airport_gui.h` | Export `_show_holding_overlay` and declare `DrawModularHoldingOverlay` |
| `src/modular_airport_gui.cpp` | Global bool, member bool, widget, OnClick, implement `DrawModularHoldingOverlay` |
| `src/viewport.cpp:1868` | Add `DrawModularHoldingOverlay` call after link-graph overlay block |

## Step-by-Step Changes

### 1. `src/widgets/airport_widget.h`
Add after line 50:
```cpp
WID_MA_TOGGLE_SHOW_HOLDING,  ///< Show holding loop overlay toggle.
```

### 2. `src/lang/english.txt`
Add after `STR_STATION_BUILD_MODULAR_AIRPORT_TOGGLE_SHOW_ARROWS`:
```
STR_STATION_BUILD_MODULAR_AIRPORT_TOGGLE_SHOW_HOLDING           :{BLACK}Show holding loop
```

### 3. `src/modular_airport_gui.h`
Add alongside the existing `_show_runway_direction_overlay` extern:
```cpp
extern bool _show_holding_overlay;
void DrawModularHoldingOverlay(const Viewport &vp, DrawPixelInfo *dpi);
```

### 4. `src/modular_airport_gui.cpp`

**Global:**
```cpp
bool _show_holding_overlay = false;
```

**Member in `BuildModularAirportWindow`:**
```cpp
bool show_holding_loop = false;
```

**Constructor init** (alongside the existing arrows init):
```cpp
this->SetWidgetLoweredState(WID_MA_TOGGLE_SHOW_HOLDING, this->show_holding_loop);
_show_holding_overlay = this->show_holding_loop;
```

**`Close()` override** (alongside existing):
```cpp
_show_holding_overlay = false;
```

**`OnClick` case:**
```cpp
case WID_MA_TOGGLE_SHOW_HOLDING:
    this->show_holding_loop = !this->show_holding_loop;
    _show_holding_overlay = this->show_holding_loop;
    this->SetWidgetLoweredState(WID_MA_TOGGLE_SHOW_HOLDING, this->show_holding_loop);
    MarkWholeScreenDirty();
    break;
```

**Widget** (append to the NWidget list, after the arrows button):
```cpp
NWidget(WWT_IMGBTN, COLOUR_DARK_GREEN, WID_MA_TOGGLE_SHOW_HOLDING), SetFill(0, 1), SetToolbarMinimalSize(1),
    SetSpriteTip(SPR_ONEWAY_BASE + 2, STR_STATION_BUILD_MODULAR_AIRPORT_TOGGLE_SHOW_HOLDING),
```
(Use same sprite as placeholder; can be updated to a dedicated icon later.)

**Implement `DrawModularHoldingOverlay`** (new free function, before or after other free functions):

```cpp
// Convert world pixel to screen point. z=0 (flat-ground approximation is fine for overlay).
static Point HoldingWorldToScreen(const Viewport &vp, int wx, int wy)
{
    Point p = RemapCoords(wx, wy, 0);
    p.x = UnScaleByZoom(p.x - vp.virtual_left, vp.zoom) + vp.left;
    p.y = UnScaleByZoom(p.y - vp.virtual_top,  vp.zoom) + vp.top;
    return p;
}

// Conservative AABB visibility — GfxDrawLine clips anyway, this just skips obviously off-screen pairs.
static bool SegVis(Point a, Point b, const DrawPixelInfo *dpi)
{
    int l = dpi->left, r = l + dpi->width, t = dpi->top, bot = t + dpi->height;
    return !(   (a.x < l   && b.x < l)
             || (a.y < t   && b.y < t)
             || (a.x > r   && b.x > r)
             || (a.y > bot && b.y > bot));
}

// Draw quadratic-Bezier approach curve + straight final + threshold marker.
static void DrawApproachCurve(const Viewport &vp, const DrawPixelInfo *dpi,
    int ax, int ay, int tx, int ty, Direction approach_dir)
{
    // dir_dx/dir_dy: same table as in modular_airport_cmd.cpp DirToVec
    static constexpr int8_t dir_dx[DIR_END] = {-1, -1, -1, 0, 1, 1, 1, 0};
    static constexpr int8_t dir_dy[DIR_END] = {-1,  0,  1, 1, 1, 0,-1,-1};

    double ddx = dir_dx[approach_dir], ddy = dir_dy[approach_dir];
    double dlen = std::hypot(ddx, ddy);
    if (dlen > 0.0) { ddx /= dlen; ddy /= dlen; }

    double dist  = std::hypot((double)(tx - ax), (double)(ty - ay));
    double flen  = std::clamp(dist / 3.0, 2.0 * TILE_SIZE, 6.0 * TILE_SIZE);

    // F = threshold retreated along inbound heading
    double fx = tx - ddx * flen, fy = ty - ddy * flen;

    // Control point: perpendicular to heading, on the correct bend side
    double cross = (tx - ax) * ddy - (ty - ay) * ddx;
    double side  = (cross >= 0.0) ? 1.0 : -1.0;

    double cx = fx + (-ddy) * side * flen * 0.7;
    double cy = fy + ( ddx) * side * flen * 0.7;

    // Sample Bezier A→C→F
    constexpr int N = 12;
    Point prev = HoldingWorldToScreen(vp, ax, ay);
    for (int i = 1; i <= N; ++i) {
        double t  = (double)i / N;
        double mt = 1.0 - t;
        double bx = mt*mt*ax + 2*mt*t*cx + t*t*fx;
        double by = mt*mt*ay + 2*mt*t*cy + t*t*fy;
        Point cur = HoldingWorldToScreen(vp, (int)bx, (int)by);
        if (SegVis(prev, cur, dpi)) GfxDrawLine(prev.x, prev.y, cur.x, cur.y, PC_YELLOW, 1);
        prev = cur;
    }
    // Straight F→T
    Point pf = HoldingWorldToScreen(vp, (int)fx, (int)fy);
    Point pt = HoldingWorldToScreen(vp, tx, ty);
    if (SegVis(pf, pt, dpi)) GfxDrawLine(pf.x, pf.y, pt.x, pt.y, PC_YELLOW, 1);

    // Red threshold marker
    GfxFillRect(pt.x - 3, pt.y - 3, pt.x + 3, pt.y + 3, PC_RED);
}

void DrawModularHoldingOverlay(const Viewport &vp, DrawPixelInfo *dpi)
{
    for (const Station *st : Station::Iterate()) {
        if (!st->airport.blocks.Test(AirportBlock::Modular)) continue;

        const ModularHoldingLoop &loop = GetModularHoldingLoop(st);
        const size_t n = loop.waypoints.size();
        if (n < 2) continue;

        // Loop polyline
        for (size_t i = 0; i < n; ++i) {
            const auto &a = loop.waypoints[i];
            const auto &b = loop.waypoints[(i + 1) % n];
            Point pa = HoldingWorldToScreen(vp, a.x, a.y);
            Point pb = HoldingWorldToScreen(vp, b.x, b.y);
            if (SegVis(pa, pb, dpi)) GfxDrawLine(pa.x, pa.y, pb.x, pb.y, PC_WHITE, 1);
        }

        // Gate approach curves + threshold markers
        for (const auto &gate : loop.gates) {
            DrawApproachCurve(vp, dpi,
                gate.approach_x, gate.approach_y,
                gate.threshold_x, gate.threshold_y,
                gate.approach_dir);
        }
    }
}
```

**Required includes** in `modular_airport_gui.cpp` (add if not already present):
- `landscape.h` (for `RemapCoords`)
- `zoom_func.h` (for `UnScaleByZoom`)
- `viewport_type.h` (for `Viewport`, `DrawPixelInfo`)
- `gfx_func.h` (for `GfxDrawLine`, `GfxFillRect`)
- `station_base.h` (for `Station::Iterate`, `AirportBlock::Modular`)
- `modular_airport_cmd.h` (for `GetModularHoldingLoop`)

### 5. `src/viewport.cpp`

In `ViewportDoDraw`, after line 1868 (closing `}` of the link-graph overlay block), add:

```cpp
if (_show_holding_overlay) {
    dp.left = x;
    dp.top  = y;
    DrawModularHoldingOverlay(vp, &dp);
}
```

`x` and `y` are already in scope (computed at line 1831–1832 as the screen-space top-left of the viewport). Add the include for `modular_airport_gui.h`.

## Verification

Build and run:
```bash
/Users/tor/ttd/OpenTTD/scripts/build_and_sign.sh
/Users/tor/ttd/OpenTTD/build/openttd -g ~/Documents/OpenTTD/save/SAVENAME.sav -d misc=3 2>/tmp/openttd.log
```

1. Open the modular airport builder. Confirm a new button appears next to the direction arrows button.
2. Toggle it on. The holding loop should appear as a white polyline connecting the Dubins waypoints.
3. Each runway gate should show a yellow curve bending from the approach fix toward the runway, ending at the threshold with a red square.
4. Toggle off — all lines disappear immediately.
5. Pan and zoom — lines follow the viewport correctly without glitches.
6. Close the builder window — overlay disappears (flag reset in `Close()`).
7. Airport with no runways (fallback rectangular loop): white polyline still draws from the fallback waypoints; no gate curves since `loop.gates` is empty.
