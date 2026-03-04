# Modular Airport Taxi Reservation Rules (Current Code)

This document reflects how taxiing/reservations currently work in code (`src/modular_airport_cmd.cpp`, `src/aircraft_cmd.cpp`), not an idealized design.

## 1) Tile/segment types

Taxi paths are split into contiguous segments of:

- `FREE_MOVE`
- `ONE_WAY`
- `RUNWAY`

Reservation behavior is segment-specific.

## 2) Goal semantics

`ground_path_goal` is usually a real destination:

- terminal / helipad / hangar for ground service movement
- runway tile for takeoff (`MGT_RUNWAY_TAKEOFF`)

Important exceptions that still exist:

- rollout/no-goal holding can set a temporary holding tile (typically one-way)
- some recovery/wait states may temporarily set goal to current tile to pause movement

Recent change: takeoff goal selection no longer uses free-move queue tiles; runway takeoff goals are now runway tiles.

## 3) Reservation rules by segment

### FREE_MOVE

When reserving a `FREE_MOVE` segment:

- reserve all forward tiles in the segment atomically
- if already inside the same free-move segment, only reserve/check forward tiles (not tiles behind current index)
- also reserve one "exit" tile (first tile of next segment)
- if exit is `RUNWAY`, reserve full contiguous runway atomically before entering it
- hangar tiles are treated as non-blocking (multi-capacity)

### ONE_WAY

- reserve next tile step-by-step
- if next tile is runway and target is takeoff, require full contiguous runway reservation before entering runway
- hangar next tile is tracked in reservation state but not map-blocking

### RUNWAY

- full contiguous runway reservation is atomic via `TryReserveContiguousModularRunway`
- runway crossing path (`non-runway -> runway -> non-runway`) reserves crossing resources atomically and also keeps current tile + first exit tile reserved

## 4) Release/retention behavior while moving

- one-way: release old tile when stepping forward
- stands: released immediately after aircraft leaves stand tile
- runway: release tiles behind as aircraft progresses (`ReleaseRunwayReservationTile`)
- on runway exit: runway reservation is cleared when no longer on runway
- free-move segment exit:
  - on `FREE_MOVE -> non-RUNWAY` boundary, clear path reservations except keep current tile and preserved non-path reservations
  - on `RUNWAY -> non-RUNWAY` boundary, do **not** clear full taxi reservation set (keeps pre-reserved post-runway tiles from landing chain continuity)

`ClearTaxiPathReservation` preserves non-runway reservations that are not on the current active taxi path when not force-clearing (to keep landing-chain continuity across path rebuilds/touchdown transitions).

## 5) Landing chain (pre-landing reservation)

Before landing commit:

- runway is reserved atomically
- post-runway chain is pre-reserved to first safe queueing point:
  - if first non-runway segment is `ONE_WAY`: reserve entry tile
  - if first non-runway segment is `FREE_MOVE`: reserve that segment + one exit tile
- the computed chain path is stored in `landing_chain_path` and reused after touchdown when possible

If no ground goal exists, landing is only allowed when there is a safe one-way buffer available after runway.

## 6) Path rebuilding and retargeting

- taxi path is rebuilt when invalid/out-of-sync with current tile/goal
- aircraft can retarget while waiting (every 64 ticks after wait threshold), depending on target type:
  - terminal / helipad / hangar / rollout retarget supported
- reservations are kept unless retarget actually succeeds

So the system is not strictly "no mid-taxi reroute"; controlled retargeting is present.

## 7) Hangar handling

- hangars are multi-capacity in reservation logic
- entering hangar intent is tracked, but hangar tiles are not treated as hard blocking reservations

## 8) Practical invariant used now

Ground movement attempts to maintain:

- reservation of the forward free-move chunk,
- plus one non-free-move boundary tile to step off the free-move region,
- with special rule that runway entry for takeoff requires full contiguous runway reservation.

That invariant is enforced by staged segment reservation (`TryReserveTaxiSegment`) rather than by making intermediate queue tiles into permanent goals.
