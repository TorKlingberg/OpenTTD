# Dubins-Path Holding Loop for Modular Airports

## Context

The current modular airport holding pattern is a fixed 8-waypoint rectangle around the airport bounding box. The gate-firing mechanism is fundamentally broken: approach fixes always land on a loop edge that travels **perpendicular** to the approach direction, so `DirectionsWithin45` fails by 90° in every case. Planes never commit to landing via the gate mechanism.

The root cause: a rectangular perimeter loop has 4 heading directions (±X, ±Y). For each runway, the nearest waypoints to the approach fix are on the loop edge facing the runway, and that edge's heading is always perpendicular to the approach direction.

The fix: replace the rectangular loop with **Dubins paths** connecting runway approach gates. A Dubins path from "exit point after gate i" to "gate i+1" arrives at gate i+1 heading exactly in the approach direction — by construction. The heading check then passes.

Also addressing: the user wants runways to default to one-way landing (up-screen direction) to ensure each runway contributes exactly one gate.

---

## Part 1: Default Runway Direction

**File**: `src/station_cmd.cpp`, `CmdBuildModularAirportTile`

When a new isolated runway tile is placed (no adjacent runway neighbor), the struct initializer in `base_station_base.h` sets `runway_flags = RUF_DEFAULT` (0x0F = both operations, both directions). Change this to default to one-way landing "up-ish":

- Horizontal runway (rotation % 2 == 0, X-axis): land toward lower X → aircraft lands at the HIGH-X end → needs `RUF_DIR_LOW`
- Vertical runway (rotation % 2 == 1, Y-axis): land toward lower Y → aircraft lands at the HIGH-Y end → needs `RUF_DIR_LOW`

Both cases: default to `RUF_LANDING | RUF_TAKEOFF | RUF_DIR_LOW`.

**Change**: In `CmdBuildModularAirportTile` (station_cmd.cpp ~line 2973), after creating `tile_data` for a runway piece with no adjacent runway neighbor, add:
```cpp
// Default to one-way up-screen landing direction
tile_data.runway_flags = RUF_LANDING | RUF_TAKEOFF | RUF_DIR_LOW;
```
This only applies when the new tile has no runway neighbor on either side; when extending an existing runway, the neighbor's flags are still inherited (existing behavior).

---

## Part 2: Dubins-Path Holding Loop

### 2a. Constants — `src/airport.h`

Add:
```cpp
static constexpr int    MODULAR_HOLDING_OVERSHOOT_TILES      = 10;  // tiles past gate before turning
static constexpr int    MODULAR_HOLDING_TURN_RADIUS_TILES    = 6;   // Dubins min radius
static constexpr int    MODULAR_HOLDING_SAMPLE_INTERVAL_PX   = 48;  // ~3 tiles per waypoint
```

Remove `MODULAR_HOLDING_WP_COUNT` (no longer fixed).
Keep `MODULAR_HOLDING_TICKS_PER_WP`, `MODULAR_HOLDING_MARGIN_TILES`, `MODULAR_LANDING_GATE_MAX_DIST_TILES`.

### 2b. Data Structure — `src/airport.h` `ModularHoldingLoop`

Change from fixed array to variable vector:
```cpp
struct ModularHoldingLoop {
    struct Waypoint { int x; int y; };
    struct Gate {
        TileIndex runway_tile;
        uint32_t  wp_index;       // was uint8_t; now indexes into waypoints vector
        int approach_x, approach_y;
        int threshold_x, threshold_y;
        Direction approach_dir;
    };

    std::vector<Waypoint> waypoints;  // replaces wp[MODULAR_HOLDING_WP_COUNT]
    std::vector<Gate>     gates;
};
```

### 2c. New Dubins Helper Functions — `src/aircraft_cmd.cpp`

All working in pixel-space doubles; using y-down coordinate system.

**Direction enum → unit vector**:
```cpp
static void DirToVec(Direction d, double &dx, double &dy);
// Uses dir_dx[]/dir_dy[] table, normalized to unit length.
```

**Dubins path**: compute all 4 CSC path types (RSR, LSL, RSL, LSR); pick shortest valid one. Return as a struct with arc/straight segments.

```cpp
struct DubinsArc    { double cx, cy, r, a0, sweep; }; // CCW positive
struct DubinsSeg    { bool is_arc; DubinsArc arc; double x0,y0,x1,y1; };
struct DubinsPath   { std::vector<DubinsSeg> segs; double length; bool valid; };

// Compute shortest Dubins path between two oriented points.
// hdx1/hdy1, hdx2/hdy2 are unit heading vectors.
static DubinsPath ComputeDubins(double x1, double y1, double hdx1, double hdy1,
                                double x2, double y2, double hdx2, double hdy2,
                                double radius);
```

**No single-gate special case**: For N=1, the loop is: gate G₀ → overshoot D tiles to exit E₀ → Dubins from (E₀, H₀) to (G₀, H₀). This is well-posed because E₀ ≠ G₀ (they differ by D tiles along heading H₀). RSR or LSL produces the symmetric racetrack oval. No special handling required.

**Path sampling**:
```cpp
static void SampleDubinsPath(const DubinsPath &path, double step_px,
                              std::vector<ModularHoldingLoop::Waypoint> &out);
// Walks each segment and appends sampled points at step_px intervals.
// Arcs: sample by angle step = step_px / radius.
// Straights: sample by distance.
// Does NOT add the endpoint (to avoid duplicate with next gate's start).
```

**Gate angle sorting**:
```cpp
struct GateInfo {
    TileIndex runway_tile;
    int gate_x, gate_y;         // approach fix (pixels)
    int threshold_x, threshold_y;
    Direction approach_dir;
    double hdx, hdy;            // unit approach heading vector
    double sort_angle;          // atan2 from airport center
};
static void GatherAndSortGates(const Station *st, std::vector<GateInfo> &gates);
```
Sort by `sort_angle` (counterclockwise around airport center). This produces a non-crossing loop that naturally orbits the airport.

### 2d. Rewrite `ComputeModularHoldingLoop` — `src/aircraft_cmd.cpp` (~line 2929)

```
1. Gather bounding box (same as current).
2. Call GatherAndSortGates() → sorted vector of N gates.
3. If N == 0: emit fallback 8-point rectangular loop (same as current code) so airports
   with no valid runways still have some orbit. Clear gates vector.
4. For i = 0 to N-1:
     a. Record gate[i].wp_index = waypoints.size().
     b. Append waypoint for gate position G_i (the approach fix).
     c. Compute exit point E_i = G_i + OVERSHOOT * heading_i (in pixels).
     d. Sample straight G_i → E_i at SAMPLE_INTERVAL_PX (the overshoot leg).
     e. Compute Dubins path from (E_i, heading_i) to (G_{(i+1)%N}, heading_{(i+1)%N}).
     f. Sample Dubins path into waypoints (endpoint excluded — it will be gate[i+1]).
5. Fill each Gate struct: runway_tile, wp_index, approach_x/y, threshold_x/y, approach_dir.
```

### 2e. Update Phase Functions — `src/aircraft_cmd.cpp`

**`GetModularHoldingWaypointIndex`** (line 3008):
```cpp
const auto &loop = GetModularHoldingLoop(st);
const uint32_t n_wp = static_cast<uint32_t>(loop.waypoints.size());
if (n_wp == 0) return 0;
const uint32_t offset = (v->index.base() % n_wp) * MODULAR_HOLDING_TICKS_PER_WP;
return static_cast<uint32_t>((v->tick_counter + offset) / MODULAR_HOLDING_TICKS_PER_WP) % n_wp;
```

**`GetModularHoldingWaypointTarget`** (line 3022):
```cpp
const uint32_t n_wp = loop.waypoints.size();
if (n_wp == 0) { *target_x = ...; *target_y = ...; return; } // fallback to station center
const uint32_t wp      = ...  // same formula as above
const uint32_t next_wp = (wp + 1) % n_wp;
// linear interp same as current, using loop.waypoints[wp] and loop.waypoints[next_wp]
```

**`IsHoldingGateActive`** (line 3015):
Add `uint32_t n_wp` parameter:
```cpp
static bool IsHoldingGateActive(uint32_t aircraft_wp, uint32_t gate_wp, uint32_t n_wp)
{
    if (n_wp == 0) return false;
    const uint32_t diff = (aircraft_wp + n_wp - gate_wp) % n_wp;
    return diff == 0 || diff == (n_wp - 1);
}
```
Update callers to pass `loop.waypoints.size()`.

**Gate-checking loop in `AircraftEventHandler_Flying`** (line 1928):
Retrieve `n_wp = loop.waypoints.size()` once, pass to `IsHoldingGateActive`. No other changes needed — `DirectionsWithin45` will now pass correctly since the Dubins path arrives at each gate heading in the approach direction.

**`AirportMoveModularFlying`** (line 4514):
If `loop.waypoints.empty()`: use fallback (fly toward station center). Otherwise same logic but uses `loop.waypoints.size()` instead of `MODULAR_HOLDING_WP_COUNT`.

---

## Critical Files

| File | Section | Change |
|------|---------|--------|
| `src/airport.h` | `ModularHoldingLoop` struct, constants | Vector waypoints, new constants |
| `src/aircraft_cmd.cpp` | `ComputeModularHoldingLoop` | Full rewrite with Dubins |
| `src/aircraft_cmd.cpp` | `GetModularHoldingWaypointIndex/Target`, `IsHoldingGateActive` | Use `n_wp` from vector |
| `src/aircraft_cmd.cpp` | `AircraftEventHandler_Flying` gate loop | Pass `n_wp` to gate check |
| `src/aircraft_cmd.cpp` | `AirportMoveModularFlying` | Guard for empty waypoints |
| `src/station_cmd.cpp` | `CmdBuildModularAirportTile` | Default `RUF_DIR_LOW` for new isolated runway |

No changes needed to: `station_base.h`, `airport_gui.cpp`, saveload (waypoints are not saved, recomputed on load).

**Arrival from other airports**: Planes arrive in `FLYING` state with `targetairport` set. `AirportMoveModularFlying` is called each tick and immediately uses `GetModularHoldingWaypointTarget` to steer toward the phase-assigned waypoint. There is no explicit entry path for modular airports — the plane sweeps in from wherever it is, joins the loop within a fraction of a lap, and gets its first gate opportunity on the next pass. No change needed; same behaviour as current implementation.

**Cache behaviour (unchanged)**: `GetModularHoldingLoop` checks `modular_holding_loop_dirty || nullptr` on every call. The dirty flag is set only by `CmdBuildModularAirportTile`, `CmdSetRunwayFlags`, and tile-removal paths — i.e., only on airport edits, never on flying ticks. The per-tick call from `AirportMoveModularFlying` is therefore just a cheap bool+pointer check; `ComputeModularHoldingLoop` (the expensive Dubins computation) runs only when the airport changes. No change needed here.

---

## Verification

1. **Build**: `make -j8 -C build` — no compile errors.
2. **Single runway**: Place a runway on a blank pad. The holding loop should be a racetrack oval; observe in-game that planes orbit and commit to land on the correct approach leg.
3. **Two parallel runways, same direction** (the failing case): Both runways should receive gates. Planes should commit to whichever runway is free as they pass each gate.
4. **Two runways, opposite directions**: Planes should orbit and approach each runway from its correct side.
5. **Airport edit during flight**: Remove a runway tile, verify dirty flag triggers loop recompute, planes reroute cleanly.
6. **No runway**: Planes in holding pattern should use fallback loop without crashing.
7. **Log check**: `grep '\[ModAp\]' /tmp/openttd.log | tail -100` — should see landing commits, not permanent `stuck(no-path)` loops.

Run with: `/Users/tor/ttd/OpenTTD/build/openttd -g ~/Documents/OpenTTD/save/SAVENAME.sav -d misc=3 2>/tmp/openttd.log`
