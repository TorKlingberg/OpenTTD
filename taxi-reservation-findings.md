# Modular Airport Ground Reservation - Findings

Findings from code review of `src/modular_airport_cmd.cpp` and related files.

## How Reservations Work

### Three segment types

| Type | Protocol | Release |
|------|----------|---------|
| **RUNWAY** | Atomic: entire contiguous runway reserved at once. Crossings use FIFO queue (`CanGrantRunwayCrossingNow`) | Released when aircraft leaves runway segment |
| **ONE_WAY** | Single tile at a time: check+reserve next tile only | Previous tile released immediately on move |
| **FREE_MOVE** | All-or-nothing: every tile in segment + exit tile must be free. Hangars exempted (multi-capacity) | All segment tiles released on entering next segment |

### Landing sequence

1. **Holding pattern** — Dubins-curve loop, phase-offset per vehicle
2. **Runway selection** (`FindModularLandingTarget`, line 731) — scores by `flight_distance + 4 * taxi_distance_to_terminal`
3. **Landing chain reservation** (`TryReserveLandingChain`, line 617) — atomic pre-commitment: runway + first non-runway exit segment. Rolled back entirely on failure.
4. **Approach & touchdown** (`AirportMoveModularLanding`, line 1488) — FAF then glide slope
5. **Rollout & ground taxi** (`AirportMoveModular`, line 3032) — segment-by-segment reservation

### Takeoff sequence

1. **Runway selection** (`FindModularRunwayTileForTakeoff`, line 2157) — three-tier fallback: reachable+enterable > reachable > Manhattan distance
2. **Queue tile** (`FindModularTakeoffQueueTile`, line 2296) — last non-runway, non-service, non-blocked tile before runway
3. **Normal taxi** to queue tile via `AirportMoveModular`
4. **Arrival at queue** (`HandleModularGroundArrival` case `MGT_RUNWAY_TAKEOFF`, line 2841) — if not on runway yet, keeps taxiing; if on runway, tries `TryReserveContiguousModularRunway`
5. **Takeoff roll** (`AirportMoveModularTakeoff`, line 1673) — accelerate, climb, release runway on reaching flight level

### Stand/hangar movements

- **Stand → hangar**: `FindFreeModularHangar`, normal taxi. Hangars are multi-capacity (never block reservations).
- **Hangar → stand**: aircraft waits inside hangar until `FindFreeModularTerminal` finds a free stand. Conservative: won't exit without a destination.
- **Hangar → takeoff**: `FindModularRunwayTileForTakeoff` + `FindModularTakeoffQueueTile`, normal taxi to queue, then runway reservation.
- **Stand → takeoff**: same as hangar → takeoff but from a stand.

---

## Known Issues

### Heuristic stale reservation clearing

`TryClearStaleModularReservation` (line 2089) is called every time `IsTaxiTileReservedByOther` checks a tile. If the reserving vehicle doesn't appear to "need" the tile (not on it, not pathing through it, not in any tracking vector), the reservation is forcibly cleared.

**Risk:** If any state transition has a moment where the vehicle legitimately holds a reservation but none of the tracked indicators are set, another aircraft could steal the tile. This is a heuristic band-aid, not a proof-based invariant. Could interact badly with the landing chain fix (preserved tiles might get cleared by the stale heuristic if the indicators aren't set).

### Orphan reservation scans use shared static counters

`ClearTaxiPathReservation` (line 2365) and `ClearModularRunwayReservation` (line 326) both use `static uint8_t` counters shared across ALL vehicles. Every 16th call from *any* vehicle triggers a full station scan. The scan frequency per vehicle is unpredictable. The existence of these scans indicates the primary reservation tracking (`taxi_reserved_tiles`, `modular_runway_reservation` vectors) can desync from tile-level bits.

### Free-move zone with multiple stands: mutual blocking

If aircraft A is at stand X and aircraft B is at stand Y, both within the same free-move segment, and both want to leave: A's reservation attempt is blocked by B's stand tile, and vice versa. The `tile != v->tile` exemption (line 2637) means each aircraft skips its own tile, but the other aircraft's tile still blocks.

After 64 ticks, `TryRetargetModularGroundGoal` fires but won't help if both goals require the same blocked zone. **Mitigation:** one-way taxiways between stands and exits convert the zone into safe queuing segments.

### Retarget livelock

When `taxi_wait_counter > 64`, `TryRetargetModularGroundGoal` picks a new goal. If the new goal has the same blocking problem, the counter resets to 0, waits 64 more ticks, retargets again — potentially ping-ponging between equally blocked goals indefinitely. Not a deadlock but effective livelock.

### Unneccessary fairness queue

Runway crossings have a FIFO queue (`CanGrantRunwayCrossingNow`) to prevent starvation. I think this is not really needed, and complicates things.

### Three-tier fallback in takeoff runway selection

`FindModularRunwayTileForTakeoff` (line 2219) keeps a `best_fallback_tile` scored by pure Manhattan distance with no pathfinding. Could send an aircraft toward a runway it can't reach via the taxi network. The aircraft would get stuck and retarget after 64 ticks.

### Expensive rollout fallback

`FindNearestModularRunwayExitTile` (called from `HandleModularGroundArrival` MGT_ROLLOUT) iterates all runway tiles, checks 4 neighbors each, calls `FindAirportGroundPath` to every service tile. O(runway_length * 4 * service_tiles) pathfinder calls. Only fires in edge cases but could be expensive on large airports.

