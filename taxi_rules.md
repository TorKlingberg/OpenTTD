# Modular Airport Taxi Reservation Rules

## Tile Classification

Every tile along a computed path falls into one of three categories:

| Category | Condition | Reservation strategy |
|---|---|---|
| **One-way** | `IsTaxiwayPiece(piece_type) && one_way_taxi == true` | Queue: one tile at a time |
| **Runway** | `APT_RUNWAY_*` pieces | Atomic: entire contiguous runway |
| **Free-move** | Everything else taxiable (plain aprons, stands, hangars, fenced aprons) | Atomic: entire segment at once |

## Core Concept: Segments

Given a computed A* path, group consecutive tiles by category into **segments**. At each segment boundary, the aircraft must acquire the next segment before entering it.

Example departure path:
```
[STAND]  [APRON] [APRON]  [1-WAY] [1-WAY] [1-WAY]  [RWY_END ... RWY tiles]
 free-move segment 0       one-way segment 1          runway segment 2
```

## Reservation Protocol

### Entering a free-move segment
**Atomic all-or-nothing.** Before stepping onto the first tile of a free-move segment, the aircraft must reserve:
- All tiles in the free-move segment, PLUS
- The first tile of the NEXT segment (the "exit tile")

The exit tile guarantees you can leave the free-move zone. It could be:
- First one-way tile (most common)
- A stand/helipad/hangar (if that's the destination -- it's the last tile of the free-move segment)
- A runway end (if free-move leads directly to runway -- requires entire runway atomically)

If reservation fails: stay where you are, wait, retry with a cooldown.

### Entering a one-way segment
**One tile at a time.** Reserve the next tile, move onto it, release the previous tile. Classic queuing. Safe because traffic is unidirectional.

### Entering a runway
**Atomic entire runway.** Reserve all contiguous runway tiles before entering (already implemented).

### Releasing tiles
- **Free-move segment**: release ALL tiles in the segment when you physically reach the exit tile (the first tile of the next segment). Hold the whole segment until you're out.
- **One-way tiles**: release the previous tile when you move onto the next one.
- **Runway**: release when fully off the runway (already implemented).

## Movement Patterns

Aircraft only move between these endpoints:
- **Hangar <-> Stand** (parking/servicing)
- **Stand <-> Runway** (departure/arrival)
- **Hangar <-> Runway** (direct departure/arrival when ordered to depot)

Hangars can hold multiple aircraft, so they are never a blocking endpoint.

## Full Departure Flow

Aircraft at stand (or hangar), wants to take off:

1. **Compute topology path** (A* ignoring reservations) from stand to runway queue point.
2. **Classify segments** along the path.
3. **Acquire free-move segment**: Atomically reserve [all free-move tiles + first one-way tile]. Aircraft still holds stand reservation while attempting this -- only releases stand tile when it physically moves off.
   - Fail: try alternate runway/path. If all fail, wait at stand, retry with cooldown.
4. **Taxi through free-move zone** -- no conflict possible, we hold all tiles.
5. **Enter one-way zone**: release all free-move tiles. Now queuing.
6. **Queue through one-way tiles** toward runway, one tile at a time.
7. **At last one-way tile before runway**: atomically reserve entire runway.
   - Fail: wait on one-way tile (queued behind other departures, or waiting for landing to clear).
8. **Enter runway, take off**, release one-way tile, then release runway when airborne.

## Full Arrival Flow

Aircraft is flying/in holding pattern, wants to land:

1. **While flying/holding**: evaluate each runway candidate:
   - Score = distance + exit path feasibility
   - For each candidate runway, compute the exit path to a stand
   - Check if the full pre-landing reservation can be acquired:
     **runway + exit path to first safe queuing point** (first one-way tile after runway, OR entire free-move path to stand if no one-way tiles)
2. **Best feasible runway found**: atomically reserve [runway + exit to first queue point].
   - If no runway's reservation succeeds: stay in holding pattern, retry with cooldown.
3. **Begin approach and land.** Rollout along runway.
4. **Exit runway onto reserved path**: taxi through reserved exit tiles.
5. **Enter one-way queue** (if present): release runway + free-move exit tiles. Queue one tile at a time.
6. **At segment boundary before free-move zone**: atomically reserve [free-move tiles + stand].
   - Fail: wait on one-way tile, retry with cooldown. May try alternate stand.
7. **Taxi through free-move zone to stand.** Release free-move tiles when parked.

If there are NO one-way tiles between runway and stand, the pre-landing reservation in step 2 includes the entire path to the stand.

## Alternative Path Finding

When a reservation attempt fails, try alternative routes before waiting:

### For landings (aircraft in the air)
- Iterate all runway candidates. For each, check if the full arrival reservation (runway + exit) can be acquired.
- Pick the best scoring runway where reservation succeeds.
- If none available, stay in holding pattern and retry periodically.

### For departures (aircraft at stand/hangar)
- Iterate all runway candidates. For each, compute the path and check if the free-move segment can be reserved.
- Pick the best scoring runway where the path is reservable.
- If none available, wait at stand/hangar and retry periodically.

### Mid-taxi
No mid-taxi rerouting. Once a plane has left its stand/hangar, it sticks to the plan. Exception: if an airport layout change invalidates the planned path, recompute.

## Pathfinding Changes

The A* pathfinder:
- **Ignores tile reservations** (the reservation system handles timing)
- **Avoids stands** occupied by other aircraft (unless it's the goal or start)
- **Still respects** one-way directions and taxi connectivity
- **Called once** when the aircraft needs a path, not every tick
- Path is cached. Recompute only if reservation fails (try alternate) or airport layout changes.

## Why This Is Deadlock-Free

1. **Free-move zones**: all-or-nothing reservation means an aircraft never holds a partial path while waiting for more tiles. No circular wait possible.
2. **One-way zones**: unidirectional flow means no head-on collision. Aircraft queue in order.
3. **Runways**: atomic reservation, already implemented.
4. **Cross-segment**: you always secure the exit before entering. No aircraft gets trapped at a segment boundary.
5. **Hangar endpoints**: multi-capacity, never block.
6. **Landing**: runway + exit reserved before committing to land. No plane gets stuck on runway unable to exit.
