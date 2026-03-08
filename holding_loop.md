# Holding Loops and Approach Gates

## Overview

Aircraft waiting to land at a modular airport fly a **Dubins holding loop** — a closed path that passes through one **approach gate** per landing runway. When a plane crosses a gate with the right heading and the runway is free, it leaves the loop and descends to land.

## The Holding Loop Shape

The loop is a closed curve built from Dubins paths (arc–straight–arc segments) connecting the approach gates in angular order around the airport.

**Construction** (`ComputeModularHoldingLoop` in `modular_airport_cmd.cpp`):

1. Collect all runway end tiles that have the `RUF_LANDING` flag set.
2. For each qualifying end, compute an **approach point** 12 tiles out from the threshold (the gate position) and a **heading vector** pointing inward toward the runway.
3. **Colocated merging**: parallel same-direction runways within 5 tiles lateral / 3 tiles along-axis get a single merged gate at their midpoint (since the gate only triggers descent — actual runway selection happens independently).
4. Sort gates **counter-clockwise** around the airport center using integer quadrant + cross-product (no floating point, deterministic for multiplayer).
5. For each consecutive gate pair, emit:
   - The gate waypoint
   - An **overshoot** waypoint 10 tiles past the gate (in the approach heading direction)
   - A Dubins path (turn radius = 6 tiles) from overshoot to the next gate
6. Dubins arcs are sampled at 48 px (~3 tile) intervals into the waypoint list. Straight segments only emit their endpoint.

If there are no qualifying runways, a rectangular fallback loop at 12-tile margin is used.

## Waypoints and Movement

The loop is stored as `ModularHoldingLoop::waypoints` — a flat list of (x, y) pixel positions forming the closed path. Aircraft advance through these waypoints independently.

**Waypoint targeting** (`GetModularHoldingWaypointTarget`):
- Lookahead of 2 waypoints ahead of the current base index
- When the aircraft comes within 3 tiles of the lookahead target, the base index advances by 1
- This means the aircraft self-advances as it flies — no external clock needed

**Initial placement**: when an aircraft first enters the loop, its starting waypoint is offset by `(vehicle_index % n_wp) * 64 ticks` to spread multiple aircraft around the loop.

**Flight** (`AirportMoveModularFlying`):
- Speed: `SPEED_LIMIT_HOLD = 425` (× `plane_speed` setting, typically 4 → effective 1700)
- Direction changes are rate-limited: `turn_counter = 2 * plane_speed` ticks between turns (same feel as the classic FTA `SlowTurn`)
- Altitude tracks `GetAircraftFlightLevel` (the standard cruise band), stepping ±1 per sub-tick
- Movement is unconditional — `UpdateAircraftSpeed` always runs, even if the target is reached

## Approach Gates

Each gate is a point on the holding loop associated with a specific runway. A gate has:

| Field | Meaning |
|-------|---------|
| `runway_tile` | The runway end tile this gate serves |
| `wp_index` | Index into the waypoint list where this gate sits |
| `approach_x/y` | The gate's pixel position (12 tiles from threshold) |
| `threshold_x/y` | Runway end tile center (the touchdown target) |
| `approach_dir` | Direction the aircraft should be heading when passing |

## Gate-Fire Logic

Every FLYING tick, after the plane moves, `AircraftEventHandler_Flying` checks all gates. A gate fires when **all** of these pass:

1. **Waypoint proximity**: the aircraft's nearest waypoint is at or one step before the gate's `wp_index`
2. **Heading**: aircraft direction is within ±45° of `gate.approach_dir`
3. **In front of threshold**: the aircraft is on the approach side (dot product signs match between aircraft-to-threshold and approach-to-threshold vectors)
4. **Distance**: Manhattan distance from aircraft to threshold ≤ 25 tiles
5. **Runway free**: no other aircraft holds a reservation on this contiguous runway
6. **No queued takeoff**: no aircraft is queued for takeoff on this runway

The first gate passing all checks wins. The system then:
- Calls `TryReserveLandingChain` to reserve the runway + taxi path to a stand
- Sets `v->modular_landing_tile = runway_tile`
- Sets `v->state = LANDING`

If reservation fails, the plane stays in the holding loop.

## Landing Approach

Once the gate fires (`AirportMoveModularLanding`):

- **Target**: the runway threshold tile center (single-stage, straight-line approach)
- **Speed**: `SPEED_LIMIT_APPROACH = 230` (× `plane_speed`)
- **Movement**: pixel-by-pixel Manhattan stepping toward the target (no turn rate limiting during approach — direction snaps to target immediately)
- **Descent**: proportional glide slope — `delta_z / max(1, dist - 4)` per step, reaching ground level exactly at the threshold
- **Touchdown**: when position matches target and altitude ≤ ground level, transitions to runway rollout

## Visual Artifact: Apparent Freeze on Approach

When the approach direction is exactly diagonal (world dx = dy), the aircraft has **zero screen-x movement** in the isometric view (since screen_x ∝ world_x − world_y). Combined with descent partially cancelling the screen-y movement, the plane can appear frozen for 10–15 ticks despite moving in 3D. The shadow (projected to ground level) continues to move during this period.

## Constants

| Constant | Value | Meaning |
|----------|-------|---------|
| `SPEED_LIMIT_HOLD` | 425 | Holding loop speed cap (before plane_speed multiplier) |
| `SPEED_LIMIT_APPROACH` | 230 | Landing approach speed cap |
| `MODULAR_HOLDING_MARGIN_TILES` | 12 | Fallback rectangle padding |
| `MODULAR_HOLDING_OVERSHOOT_TILES` | 10 | How far past the gate before turning |
| `MODULAR_HOLDING_TURN_RADIUS_TILES` | 6 | Dubins minimum turn radius |
| `MODULAR_HOLDING_SAMPLE_INTERVAL_PX` | 48 | Arc waypoint spacing (~3 tiles) |
| `MODULAR_LANDING_GATE_MAX_DIST_TILES` | 25 | Max gate-fire distance |
| `MODULAR_HOLDING_TICKS_PER_WP` | 64 | Ghost clock period (initial placement only) |
