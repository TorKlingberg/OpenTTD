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
- if exit is `RUNWAY`, apply the runway segment contract before entry (`TryReserveTaxiSegment` on the runway segment):
  - terminal runway: reserve contiguous runway resource atomically
  - transit runway: reserve runway + hold + continuation chain atomically
- hangar tiles are treated as non-blocking (multi-capacity)

### ONE_WAY

- reserve next tile step-by-step
- if next tile is runway and target is takeoff, require full contiguous runway reservation before entering runway
- hangar next tile is tracked in reservation state but not map-blocking

### RUNWAY

- runway segments are classified by intent:
  - terminal runway mode: runway is the destination for takeoff/takeoff-state progression
  - transit runway mode: runway is used as a taxiway bridge toward a non-runway continuation
- terminal runway mode reserves contiguous runway resources atomically
- transit runway mode requires all-or-nothing pre-entry ownership of:
  - runway resource(s),
  - current hold tile,
  - safe non-runway continuation tile after runway segment
- if no safe continuation can be derived (or it is blocked), runway entry is denied

## 4) Release/retention behavior while moving (Reservation V2)

- movement now uses reserve-then-reconcile:
  - reserve forward tiles with `TryReserveTaxiSegment`
  - compute keep-set with `BuildReservationKeepSet`
  - release everything else with `ReconcileAircraftReservations`
- keep-set includes:
  - current tile
  - active forward segment horizon (+ boundary tile)
  - landing-chain continuity (`landing_chain_path`)
  - runway resources needed by active runway traversal and takeoff-intent runway ownership
  - for active transit-runway traversal: hold tile + continuation tile
- runway resources are atomic:
  - if any tile of runway resource `R` is still needed, all tiles in `R` stay reserved
- runway reservation is no longer auto-cleared just because aircraft is on non-runway tile
  - takeoff intent uses `ShouldRetainRunwayReservation` to keep reservation only when it still matches current intended takeoff runway

`ClearTaxiPathReservation` remains a transition/force-clear utility; normal per-step release is now reconcile-based.

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
- and after each movement step, deterministic release of owned tiles outside the computed keep-set.

Additional runway-transit invariant:

- aircraft must not enter runway-transit movement unless it already owns a continuation chain to a safe non-runway continuation tile.

That invariant is enforced by staged segment reservation (`TryReserveTaxiSegment`) rather than by making intermediate queue tiles into permanent goals.

## 9) Runway deny behavior when already owning runway

- `TryReserveContiguousModularRunway` no longer clears runway ownership on deny if the aircraft already owns the exact requested contiguous runway resource.
- deny clears happen only when existing runway ownership is stale/mismatched with the requested runway resource.
