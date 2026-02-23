# Show Holding Loop Overlay — Implementation Design

## Goal

Improve the current concept into an implementation-ready design with:
1. A reliable way to draw connecting lines between holding-loop waypoints.
2. A non-straight gate-to-runway connector that looks like a real approach turn + short final.

## Key Decision

Do all holding-loop overlay drawing in a **single viewport post-draw pass**.

Reason: the connecting-line problem is fundamentally cross-tile geometry. Tile draw callbacks are tile-local and fight clipping/ordering. A viewport pass gets one coherent coordinate space and natural line clipping.

## Existing Data (already enough)

`ModularHoldingLoop` already has:
- `waypoints` for the loop path (world pixels).
- per-gate `approach_x/y`, `threshold_x/y`, and `approach_dir`.

No new persistent data is required.

## Rendering Architecture

### 1. UI toggle

Add `WID_MA_TOGGLE_SHOW_HOLDING` next to the existing direction toggle in `airport_widget.h`.

- New global flag: `bool _show_holding_overlay` (initialized to `false`).
- `OnClick`: flip bool, set lowered state, `MarkWholeScreenDirty()`.

### 2. Viewport hook (core fix for connecting lines)

In `ViewportDoDraw` (`src/viewport.cpp`), just before `vp.overlay` or string drawing, while the blitter is set up for the viewport:

```cpp
if (_show_holding_overlay) DrawModularHoldingOverlay(vp);
```

`DrawModularHoldingOverlay(const Viewport &vp)` should be implemented in `src/modular_airport_gui.cpp`. It does:
1. Iterate all stations (`Station::Iterate()`).
2. Skip if station has no modular airport data.
3. Quick reject station by bounding box against viewport world rect.
4. For each loop, draw loop polyline between consecutive waypoints.
5. For each gate, draw:
   - gate marker at threshold
   - curved approach path (not straight) from approach fix to threshold.

### 3. Coordinate conversion

Use a helper equivalent to the static `MapXYZToViewport` in `viewport.cpp`:
```cpp
Point MapToScreen(const Viewport &vp, int wx, int wy) {
    Point p = RemapCoords(wx, wy, 0);
    p.x = UnScaleByZoom(p.x - vp.virtual_left, vp.zoom) + vp.left;
    p.y = UnScaleByZoom(p.y - vp.virtual_top, vp.zoom) + vp.top;
    return p;
}
```

## Curved Gate-to-Runway Path

Straight line replacement: **quadratic Bezier turn + short straight final**.

### Geometry

Given gate `A = approach` and runway threshold `T = threshold`:

1. Get runway inbound unit direction `d` from `approach_dir` (use a 0-7 direction vector lookup).
2. Choose final segment length:
   - `final_len = Clamp(dist(A, T) / 3, 2 * TILE_SIZE, 6 * TILE_SIZE)`.
3. Compute final-start point `F`:
   - `F = T - d * final_len`.
4. Compute lateral unit `n = perp_left(d)` (i.e., `(-d.y, d.x)`).
5. Choose bend side from cross-product sign:
   - `side = sign((T.x - A.x) * d.y - (T.y - A.y) * d.x)`, fallback `+1` if near zero.
6. Control point `C`:
   - `C = F + n * side * (final_len * 0.7)`.
7. Build path:
   - Sample Bezier `A -> C -> F` (12 segments).
   - Straight segment `F -> T`.

This creates a plausible intercept then final approach.

### Sampling + drawing

- Convert sampled world points to screen points using `MapToScreen`.
- Draw segment-by-segment with `GfxDrawLine`.
- Recommended style:
  - loop path: `PC_WHITE`, width 2 (or 1 at high zoom)
  - curved approach: `PC_YELLOW`, width 2
  - threshold marker: small square/cross in `PC_RED`

## Culling and Performance

- Quick reject station by bbox against viewport world rect.
- Skip loops with fewer than 2 waypoints.
- Let `GfxDrawLine` handle final screen clipping.
- Optional: if zoomed out too far (`vp.zoom` above threshold), draw only gate markers (no full polylines).

Expected overhead is low: loops are cached; drawing is lightweight line work.

## Files to Touch

- `src/widgets/airport_widget.h`
  - Add `WID_MA_TOGGLE_SHOW_HOLDING`.
- `src/modular_airport_gui.h`
  - Export `_show_holding_overlay` and `DrawModularHoldingOverlay`.
- `src/modular_airport_gui.cpp`
  - Implement toggle widget, flag, and `DrawModularHoldingOverlay`.
- `src/viewport.cpp`
  - Add viewport hook in `ViewportDoDraw`.

Remove gate-marker logic from per-tile `DrawTile_Station`; keep all holding overlay rendering in viewport pass to avoid duplicated logic and coordinate mismatch.

## Acceptance Criteria

1. Toggling holding overlay shows/hides immediately without artifacts.
2. Loop appears as continuous connected lines across tiles.
3. Each gate shows a curved connector that ends aligned into runway threshold.
4. No per-tile clipping glitches when panning/zooming.
5. Overlay is readable at normal zoom and does not tank frame rate.

## Risks / Gotchas

- `approach_dir` interpretation must match inbound runway direction; if reversed, curve bends the wrong way.
- Very short `A->T` distances can create ugly curves; clamp `final_len` and reduce side offset when distance is small.
- Keep all holding overlay coordinates in world-pixel space until final transform; mixing tile/world units causes skew.
