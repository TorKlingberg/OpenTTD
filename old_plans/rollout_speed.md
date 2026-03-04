# Modular Runway Rollout Speed Parity Design

## Goal
Make modular-airport runway rollout match stock-airport rollout behavior for fixed-wing aircraft, so a plane landing on a runway of equivalent length spends the same time decelerating and clearing the runway.

## Scope
In scope:
- Fixed-wing landing rollout after touchdown.
- Speed profile while still in runway flow.
- Transition from touchdown to runway egress.

Out of scope (for this change):
- Airborne holding logic.
- Runway selection heuristics.
- Helicopter landing behavior.
- Full pre-touchdown path geometry parity with each stock airport layout.

## Current Behavior Summary

### Stock airports (reference behavior)
Stock landing rollout uses `AirportMovingDataFlag::Brake` nodes in the FTA movement tables.

Behavior in `AircraftController`:
- `Brake` sets speed limit to taxi (`SPEED_LIMIT_TAXI = 50`) with `hard_limit = false`.
- This means **gradual** deceleration, not an immediate clamp.
- Runway rollout movement continues with braking until the state machine reaches runway-exit taxi nodes.

Relevant code:
- `src/aircraft_cmd.cpp`:
  - `UpdateAircraftSpeed(...)`
  - `AircraftController(...)` Brake flag handling
- `src/table/airport_movement.h`:
  - Per-airport `Land` then `Brake` runway nodes.

### Modular airports (current)
Modular landing flow:
1. `AirportMoveModularLanding` performs approach/final and touchdown.
2. On touchdown, aircraft is switched into ground routing with `modular_ground_target = MGT_ROLLOUT` and `state = TERM1`.
3. `AirportMoveModular` handles movement to rollout target (`FindModularRunwayRolloutPoint` = far runway end).

Important difference:
- `AirportMoveModular` currently hard-clamps speed at entry:
  - if `cur_speed > taxi_limit`, set directly to taxi speed and reset subspeed.
- This removes stock-like gradual brake deceleration.

Result:
- Modular rollout speed profile differs from stock.
- Time-on-runway after touchdown does not match stock behavior.

## Root Cause
The modular ground mover includes a global safety clamp intended to prevent flight-speed leftovers on ground routes. That clamp is also applied to `MGT_ROLLOUT`, but stock rollout behavior is specifically a gradual brake phase.

## Design Requirements
1. Keep modular anti-deadlock protections for non-rollout ground movement.
2. Restore stock-like gradual deceleration during rollout.
3. Avoid new reservation races or runway occupancy bugs.
4. Preserve existing modular runway reservation semantics.
5. Keep behavior deterministic and cheap per tick.
6. During rollout braking, do not reset `subspeed`; preserve fractional deceleration behavior from `UpdateAircraftSpeed`.
7. Braking mode must be tied to physical runway occupancy, not only `MGT_ROLLOUT` state.
8. At runway exit, re-enable normal hard taxi clamp immediately to avoid high-speed apron/taxi movement.
9. Stock parity target is the same brake call semantics as stock `Brake` nodes: `UpdateAircraftSpeed(..., SPEED_LIMIT_TAXI, false)`.

## Chosen Approach
We will implement **Option A**:
- Rollout-specific gradual brake mode in `AirportMoveModular`.
- Fidelity target: match stock **speed profile** only.
- Scope: minimal-risk rollout-only patch.

## Implementation Design (Option A)

Approach:
- Special-case `MGT_ROLLOUT` when aircraft is still on runway tiles.
- Replace hard speed clamp with stock-like braking update:
  - call `UpdateAircraftSpeed(v, SPEED_LIMIT_TAXI, false)`
  - do not force `cur_speed = taxi_limit`.
  - do not reset `subspeed` in this mode.
- Continue tile-target movement toward rollout path target as today.
- Keep current hard-clamp behavior for all other modular ground targets and for rollout after leaving runway.

Implementation sketch:
1. In `AirportMoveModular`, compute `rollout_on_runway` from both:
   - `v->modular_ground_target == MGT_ROLLOUT`
   - current tile is still a modular runway piece.
2. Gate the pre-ground hard-clamp:
   - skip both `cur_speed` snap and `subspeed = 0` when `rollout_on_runway` is true.
3. During movement update for the step toward next tile:
   - if `rollout_on_runway`, use `UpdateAircraftSpeed(v, SPEED_LIMIT_TAXI, false)`.
   - otherwise keep existing taxi update/clamp behavior.
4. On transition from runway segment to non-runway segment:
   - normal hard clamp re-engages immediately (intentional one-time speed snap if needed).
5. Keep behavior unchanged for retarget and recovery paths:
   - if state remains `MGT_ROLLOUT` but tile is non-runway, rollout braking mode must be off.
   - this prevents high-speed movement on taxiways during emergency redirection.

Expected behavior:
- Same deceleration shape as stock Brake phase.
- No immediate speed snap at touchdown->rollout handoff.
- Potential speed snap at runway exit is allowed by design for safety.

## Options Considered And Rejected

### Rejected Option B: Dedicated modular rollout state machine (new runtime phase)

- Rejected because:
- More code and state complexity.
- Higher regression surface.
- More transition points to validate.

### Rejected Option C: Remove global hard clamp for all modular ground movement

- Rejected because:
- Conflicts with existing comments and safety intent around deadlock/leftover-flight-speed recovery.
- Highest risk for non-rollout pathing regressions.

## Validation Plan

### Functional checks
1. Stock vs modular stopwatch comparison:
   - same aircraft type
   - same plane-speed setting
   - same runway length (tile count)
   - measure ticks from touchdown event to first non-runway tile.
2. Confirm modular speed decreases gradually after touchdown (not instant snap).
3. Confirm runway reservation remains held during rollout and releases as aircraft exits.
4. Verify `subspeed` is not reset while on-runway rollout braking is active.
5. Verify first non-runway tick re-applies normal clamp behavior.
6. Verify soft-brake call semantics match stock Brake usage (`speed_limit = SPEED_LIMIT_TAXI`, `hard_limit = false`).

### Regression checks
1. No new stuck aircraft in queue/one-way segments.
2. No runway deadlock increase under high traffic.
3. No behavior change for non-rollout taxi paths.

### Suggested instrumentation
- Add temporary debug lines (or use existing debug channel) for:
  - touchdown speed
  - rollout speed each N ticks
  - rollout `subspeed` each N ticks
  - rollout_on_runway mode toggle
  - runway-exit tick
  - total rollout ticks
