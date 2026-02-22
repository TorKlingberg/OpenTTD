# Modular Airport Holding Pattern — Design Notes

## Problem Statement

When multiple aircraft are waiting to land at a modular airport, they cluster in a
small area and execute sharp, frequent direction changes. With three or more aircraft
they effectively move as a flock, all targeting the same approach point.

### Root Causes

1. **Single target point.** `FindModularLandingTarget` scores runways by distance to
   the *aircraft*. When planes are already bunched together they all get the same
   nearest runway, so they all orbit the same approach fix.

2. **Tiny orbit.** The octagonal orbit has a radius of only 5 tiles. With 8 phase
   slots of 16 ticks each the full lap is 128 ticks ≈ 2 seconds.  A plane arrives
   at a new waypoint every 16 ticks — far too rapidly, causing rapid direction
   changes.

3. **Phase bunching.** The phase formula `((tick_counter / 16) + v->index) & 7` uses
   sequential vehicle IDs. Aircraft bought at the same time get consecutive IDs and
   therefore consecutive phases — they are separated by only 1/8 of the tiny orbit,
   i.e. roughly 5 tiles, much less than safe visual separation.

4. **No visit of other runways.** Each aircraft orbits the approach fix of *its*
   chosen runway and never approaches other runways. With a layout that has two
   runways on opposite ends of the airport, all aircraft pile up on one end.

5. **Landing commit distance too short.** The 16-tile commit distance means a plane
   only commits when it is already inside the orbit, causing an abrupt dive toward
   the runway from inside the holding circle.

---

## How Stock Airports Handle Holding

Stock airports use a hardcoded Finite State Automaton (`AirportFTAbuildup`).
Holding waypoints are set with the `AirportMovingDataFlag::Hold` flag (which
activates `SPEED_LIMIT_HOLD = 425` and altitude clamping).

### Example — City Airport (6×6)

```
Pos 18  (160,  87)  NE corner of loop
Pos 25  (145,   1)  NW corner of loop
Pos 20  (257,   1)  SW corner of loop
Pos 21  (273,  49)  SE corner of loop
Pos 13  (177,  87)  final approach fix
Pos 14  ( 89,  87)  runway threshold (Land flag)
```

The four Hold positions form a wide racetrack (`~256 × 88` pixels ≈ 16 × 5.5 tiles)
*outside* the airport bounding box.  Aircraft loop through these four corners in
order; the landing check fires at pos 18, which is adjacent to the runway threshold.

Key properties:
- Only **4 corners** → gentle 45° turns every ~half-second, not rapid oscillation.
- Loop is **large** (~16+ tiles across), so planes naturally spread out.
- Every aircraft visits the **same set of waypoints**, so they all check the runway
  at the same position in the loop — natural queue order is preserved by loop
  position.
- The `SlowTurn` flag prevents heading changes > 45° per tick for smooth visuals.

### Example — Country Airfield (4×3)

Four holding waypoints at pixel coordinates:
```
(  1, 193)  (  1,   1)  (257,   1)  (273,  47)
```
A very large 256×193 pixel racetrack — roughly 16×12 tiles — far outside the airport.
Only one runway; planes circle and land whenever the runway becomes free.

---

## Proposed New Design: Airport Perimeter Loop

### Concept

Compute a **rectangular holding loop** that encloses the entire airport's runway
footprint with a configurable outset.  Generate a small number of waypoints (4–8)
distributed around this rectangle.  Every waiting aircraft follows the same loop;
each aircraft's *current phase slot* advances over time, naturally spacing them.

```
      WP0 (NW approach)          WP1 (N midpoint)     WP2 (NE approach)
       ┌────────────────────────────────────────────────┐
       │                                                │
  WP7  │                   airport tiles               │  WP3
 (W)   │   ██ ██  runway 1  ██ ██                      │  (E)
       │                                                │
  WP6  │                          ██ ██  runway 2  ██  │  WP4
 (SW)  │                                                │  (SE)
       └────────────────────────────────────────────────┘
      WP5 (S midpoint)
```

Planes fly counter-clockwise.  The landing check fires whenever an aircraft's
current waypoint is near a runway's approach fix.

### Loop Geometry

```
bbox = bounding box of all modular tiles (or runway tiles only)
margin = 12 tiles (configurable, cf. current 12-tile approach distance)

loop_x0 = (bbox.min_x - margin) * TILE_SIZE
loop_y0 = (bbox.min_y - margin) * TILE_SIZE
loop_x1 = (bbox.max_x + margin) * TILE_SIZE
loop_y1 = (bbox.max_y + margin) * TILE_SIZE
```

**Waypoints (CCW starting NW corner):**

| # | x        | y        | description            |
|---|----------|----------|------------------------|
| 0 | loop_x0  | loop_y0  | NW corner              |
| 1 | cx       | loop_y0  | N midpoint             |
| 2 | loop_x1  | loop_y0  | NE corner              |
| 3 | loop_x1  | cy       | E midpoint             |
| 4 | loop_x1  | loop_y1  | SE corner              |
| 5 | cx       | loop_y1  | S midpoint             |
| 6 | loop_x0  | loop_y1  | SW corner              |
| 7 | loop_x0  | cy       | W midpoint             |

8 waypoints gives 45° direction changes at most — equivalent to the stock airport
loop behaviour with `SlowTurn`.

Optionally, waypoints nearest to each runway's approach fix can be snapped to that
fix (clamped to within 1 tile of the loop edge), so aircraft naturally pass each
runway on their circuit.

### Phase / Spacing

```cpp
// Number of waypoints on the loop
static constexpr int LOOP_WP_COUNT = 8;

// How many ticks per waypoint advance (tune for visual smoothness)
static constexpr int LOOP_TICKS_PER_WP = 64; // ~2 seconds per waypoint = ~16 s per lap

// Per-aircraft phase:
//   - v->index spreads aircraft around the loop by ID
//   - tick_counter advances all aircraft at the same rate
int global_phase = v->tick_counter / LOOP_TICKS_PER_WP;
int aircraft_offset = (v->index.base() * LOOP_WP_COUNT) / MAX_AIRCRAFT; // even spread
int wp_index = (global_phase + aircraft_offset) % LOOP_WP_COUNT;
```

With `LOOP_TICKS_PER_WP = 64` and 8 waypoints the full lap is 512 ticks (~8 s).
A `MAX_AIRCRAFT = 64` spread gives adjacent planes a 1/8 lap gap ≈ 1 waypoint ≈
64 ticks.  For actual aircraft counts of 3–6 the spacing increases to 2–3 waypoints.

Alternative: use the number of currently-waiting aircraft to compute spacing so
the gap is always at least 1 waypoint regardless of ID gaps.  This requires a
small count pass before the phase calculation.

### Landing Attempt Trigger — and the Direction Alignment Problem

This is the trickiest part of the design.  A naive gate based on proximity alone
**does not work** — it causes a visible snap at the moment of commitment.

**Why:**

For a horizontal runway whose approach fix is on the loop's east midpoint, the
aircraft arrives there traveling *southward* (CCW: NE→SE leg).  The aircraft must
fly *westward* to land.  The moment it commits, the landing code turns it 90°
instantly (no `turn_counter` in `AirportMoveModularLanding` stage 0 — see line
2886 in aircraft_cmd.cpp, `v->direction = desired_dir` with no delay).

**How stock airports avoid this:**

Stock airports place holding waypoints so that the *last loop leg before the gate*
is **co-linear with the runway axis**.  For the City Airport the gate (pos 18) is
at the end of a leg that runs exactly parallel to the runway heading, so no
direction change is needed at commitment.

```
WP25 ─────── fly east ─────── WP18 (gate)
                                ↓ already flying east
                               WP13 (approach fix)
                                ↓ already flying east
                               WP14 (runway threshold)
```

**Fix 1 — Direction + approach-side gate (required):**

> ⚠️ **Do not use proximity to the approach fix as a gate condition.**  If the runway
> end is set back from the airport's bounding box edge (common with interior or
> parallel runways), the approach fix is inside the loop.  The aircraft on the
> correct direction leg may be 12+ tiles away from it and the gate never fires —
> a permanent deadlock for that runway.

**Concrete deadlock example:**
- 10×10 tile airport, horizontal runway at Y=10 (centre), threshold at X=10
- Loop outset = 12 tiles → top leg at Y = −2
- Approach fix at (X=−2, Y=10) — 12 tiles south of top leg
- Aircraft on top leg at X=−2 (correct direction ✓) but 12 tiles from fix → gate miss

**Correct gate condition:** heading within 45° of approach direction AND aircraft
is *in front of* the runway threshold on the approach axis.

For a +X (eastward) approach to a threshold at tile X = Xt:
```cpp
// Decompose into approach-axis and lateral-axis coordinates
Direction approach_dir = GetRunwayApproachDirection(st, runway);
bool heading_ok = DirectionsWithin45(v->direction, approach_dir);

// "In front of" check: on the approach side of the threshold
bool in_front;
if (approach is +X)  in_front = v->x_pos < Xt * TILE_SIZE;
if (approach is -X)  in_front = v->x_pos > Xt * TILE_SIZE;
if (approach is +Y)  in_front = v->y_pos < Yt * TILE_SIZE;
if (approach is -Y)  in_front = v->y_pos > Yt * TILE_SIZE;

// Optional: cap to avoid committing from the far side of the airport
// (e.g. 25 tiles from threshold keeps approaches reasonably short)
bool in_range = ManhattanDist(v->x_pos, v->y_pos, threshold_px, threshold_py)
                < 25 * TILE_SIZE;

if (heading_ok && in_front && in_range) {
    // attempt TryReserveLandingChain ...
}
```

This fires for any point on the loop leg that faces the runway, regardless of
how far the runway end is set back from the airport bounding box edge.

With the large perimeter loop this naturally creates a real traffic pattern: the
aircraft orbits until it's on the leg that faces the runway, then commits and
flies straight in.  Multiple aircraft are visible on different legs simultaneously.

**Fix 2 — Smooth turns in approach stage 0 (required regardless):**

`AirportMoveModularLanding` stage 0 currently snaps direction immediately.  Add
`turn_counter` (same as flying code) to allow gradual 45°/tick turns.  This
smooths out any residual heading difference at commitment, and will also make
the glide slope look better.

**Resulting visual sequence (horizontal E→W landing):**

```
 4. loop south leg: aircraft flies WEST ← ← ← (correct heading)
                                                ↓ gate fires here
 5. commitment: already flying west, no snap
 6. stage 0: flies directly to approach fix (slight Y correction only)
 7. stage 1: glide slope to runway threshold
```

Compare to a plane approaching while on the east leg (flying south):
- direction gate rejects: keep orbiting
- next lap, plane reaches south leg heading west → gate fires cleanly

### `GetRunwayApproachDirection` helper

New function needed to return the `Direction` an aircraft travels when landing on
a given runway end:

```cpp
static Direction GetRunwayApproachDirection(const Station *st, TileIndex runway_end)
{
    const ModularAirportTileData *data = st->airport.GetModularTileData(runway_end);
    if (data == nullptr) return DIR_N;
    bool horizontal = (data->rotation % 2) == 0;
    bool is_low     = IsRunwayEndLow(st, runway_end);
    // Landing at low end: aircraft travels in +axis direction
    if (horizontal) return is_low ? DIR_E : DIR_W;   // +X = E,  -X = W
    else            return is_low ? DIR_S : DIR_N;   // +Y = S,  -Y = N
}
```

`DirectionsWithin45` just checks `|dir_a - dir_b| <= 1 (mod 8)`.

### No New Aircraft Fields Needed

The holding state is fully computed from:
- `v->tick_counter` (already exists)
- `v->index` (already exists)
- The airport's tile data (already read per tick in `AirportMoveModularFlying`)

No additions to `aircraft.h` or saveload code are needed.

---

## Changes Required

### `AirportMoveModularFlying` (aircraft_cmd.cpp ~4299)

Replace the 8-point octagon logic with:

1. `ComputeModularHoldingLoop(st, &loop)` — bounding box + waypoints (cache or
   recompute; it's cheap).
2. Compute `wp_index` from `tick_counter + v->index` as above.
3. Set `(target_x, target_y)` to `loop[wp_index]`.
4. Keep the existing smooth-turn / speed / altitude update logic unchanged.

```cpp
static void AirportMoveModularFlying(Aircraft *v, const Station *st)
{
    // Build (or retrieve) the perimeter loop for this airport
    HoldingLoop loop;
    ComputeModularHoldingLoop(st, &loop); // fills loop.wp[8] + loop.approach_gates[]

    // Determine which waypoint this aircraft is currently targeting
    int phase   = (v->tick_counter / LOOP_TICKS_PER_WP);
    int offset  = v->index.base() % LOOP_WP_COUNT; // spread by ID
    int wp      = (phase + offset) % LOOP_WP_COUNT;
    int target_x = loop.wp[wp].x;
    int target_y = loop.wp[wp].y;

    // existing smooth turn + move + altitude code unchanged ...
}
```

### `AircraftEventHandler_Flying` (aircraft_cmd.cpp ~1907)

Replace distance-to-nearest-runway check with approach-gate check:
- Iterate `loop.approach_gates` (one per runway end with `RUF_LANDING`).
- Gate is "active" if aircraft's current waypoint matches or is adjacent to it.
- Only attempt `TryReserveLandingChain` when gate is active.

### New helper: `ComputeModularHoldingLoop`

```cpp
struct HoldingLoop {
    struct WP { int x, y; };
    WP wp[8];
    struct Gate { int x, y; TileIndex runway; };
    std::vector<Gate> approach_gates;
};

static void ComputeModularHoldingLoop(const Station *st, HoldingLoop *out);
```

Computes bounding box of all modular tiles, expands by margin, populates 8 corner +
midpoint waypoints, and for each `RUF_LANDING` runway end calls
`GetModularLandingApproachPoint` to add a gate.

This can be computed once and cached (e.g., in a `mutable` field on `AirportSpec`
or `StationAirport`, or simply recomputed each tick — it is O(N tiles) and small).

---

## Visual Comparison

| Aspect              | Current (octagon)           | Proposed (perimeter loop)        |
|---------------------|-----------------------------|----------------------------------|
| Loop size           | 5-tile radius (~10 tiles dia)| 12-tile outset around entire airport |
| Waypoints           | 8 per orbit                 | 8 (same), but much further apart |
| Ticks/waypoint      | 16                          | 64                               |
| Direction changes   | ~8 per 128 ticks            | ~8 per 512 ticks (4× less often) |
| Landing point       | Any part of orbit           | Only at approach gates           |
| Multi-runway        | Only nearest runway checked | All runways visited on each lap  |
| Aircraft separation | ~5 tiles (ID+1 phase)       | ~lap_length / aircraft_count     |
| New fields          | None                        | None                             |

---

## Open Questions / Tuning

- **LOOP_TICKS_PER_WP**: 64 ticks is roughly right for a jet at cruise speed.
  Should probably scale with runway distance (larger airports → longer lap →
  more ticks per segment needed to keep visible speed constant).

- **Margin**: 12 tiles is current approach distance.  Could reduce to 8 for small
  airports so the loop stays on-screen.  Could also be set dynamically to
  `max(8, sqrt(airport_area))`.

- **Waypoint snapping**: Whether to snap waypoints to runway approach gates or
  keep them strictly on the rectangle.  Snapping looks more realistic but requires
  care to avoid waypoints crossing the airport.

- **Helicopter vs plane**: Helicopters should probably use a smaller loop or a
  dedicated inner loop.  Currently they have the same 8-point orbit with a different
  distance threshold (50 tiles vs 16 tiles).

- **Multi-runway approach order**: Should aircraft check gates in loop order (so
  the "first" gate encountered determines which runway they land on), or should they
  still prefer the least-busy runway regardless of gate position?  The latter avoids
  all planes choosing runway 1 on every loop.

- **Aircraft stacked at same waypoint**: If two aircraft have the same
  `(phase + offset) % 8`, they share a target.  The existing `v->index * spacing /
  MAX_AIRCRAFT` formula only guarantees uniqueness if aircraft IDs are evenly
  distributed.  A safer approach: at the start of each tick, count waiting aircraft
  at this airport and assign slot = `waiting_rank % LOOP_WP_COUNT` using a sorted
  list.  Expensive, but prevents stacking.

---

## Notes for Implementer

**Direction enum pitfall.** Don't hardcode `DIR_E`/`DIR_W` etc in
`GetRunwayApproachDirection` — OpenTTD's direction enum maps +X to a diagonal, not
a cardinal. Derive the approach direction from existing geometry instead:

```cpp
// Reuse GetModularLandingApproachPoint to get fix coords, then compute direction
// from fix toward threshold. That IS the landing direction.
int ax, ay;
GetModularLandingApproachPoint(st, runway, &ax, &ay);
int tx = TileX(runway) * TILE_SIZE + TILE_SIZE/2;
int ty = TileY(runway) * TILE_SIZE + TILE_SIZE/2;
Direction approach_dir = GetDirectionToward(ax, ay, tx, ty); // fix→threshold
```

**"In front of" check without decomposing axes.** Use the approach fix itself:
aircraft is "in front of" the threshold if it's on the same side as the fix.

```cpp
// sign of (fix - threshold) in the approach axis tells you which side is "in front"
// aircraft is in front if sign(aircraft - threshold) == sign(fix - threshold)
bool in_front =
    ((ax - tx) == 0 || ((v->x_pos - tx) * (ax - tx) > 0)) &&
    ((ay - ty) == 0 || ((v->y_pos - ty) * (ay - ty) > 0));
```

This works for any runway orientation with no axis decomposition.

**Speed on the loop.** `AirportMoveModularFlying` currently uses `SPEED_LIMIT_NONE`.
Change to `SPEED_LIMIT_HOLD` (425) so holding aircraft slow to pattern speed, matching
stock airports.

**`turn_counter` in landing stage 0.** The direction snap is at line 2886 in
`AirportMoveModularLanding`. Add the same `turn_counter = 1` guard used in
`AirportMoveModularFlying` (line 4363).

**Landing commit check to replace.** The existing proximity check to replace is at
`aircraft_cmd.cpp` lines 1917–1922 (`landing_commit_dist` block inside
`AircraftEventHandler_Flying`). Replace that entire block with the new gate.

**Loop traversal direction.** Choose CW or CCW consistently and document it.
The choice determines which loop leg aligns with each runway approach — as long as
the "in front of" + heading gate is used correctly, either direction works.

## Files to Change

| File                            | Change                                              |
|---------------------------------|-----------------------------------------------------|
| `src/aircraft_cmd.cpp`          | `AirportMoveModularFlying` (loop waypoints), `AircraftEventHandler_Flying` (direction gate), `AirportMoveModularLanding` stage 0 (add `turn_counter`), add `ComputeModularHoldingLoop`, `GetRunwayApproachDirection` |
| `src/aircraft.h`                | No changes needed                                   |
| `src/saveload/station_sl.cpp`   | No changes needed                                   |
| `src/airport_ground_pathfinder.*` | No changes needed                                 |
