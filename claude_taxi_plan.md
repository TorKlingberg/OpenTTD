# Segment-Based Taxi Reservation System

## Context

The current modular airport ground taxi system uses tile-by-tile incremental reservation. Aircraft reserve one tile ahead as they move, which causes deadlocks when two aircraft meet head-on on bidirectional taxiways. The full design spec is in `/Users/tor/ttd/OpenTTD/taxi_rules.md`.

The new system classifies path tiles into **segments** (free-move, one-way, runway) and uses **atomic reservation** for free-move segments while allowing **queuing** on one-way segments. Aircraft must reserve their full free-move path before entering it, preventing head-on deadlocks. Landing aircraft must reserve runway + exit path before committing to land.

## Files to Modify

| File | Changes |
|------|---------|
| `src/aircraft.h` | New fields on Aircraft struct for segment tracking |
| `src/aircraft_cmd.cpp` | Rewrite AirportMoveModular, modify landing/departure flows |
| `src/airport_ground_pathfinder.cpp` | Remove reservation checks from A*, add segment classification |
| `src/airport_ground_pathfinder.h` | New structs/functions for segments |

## Step 1: New Data Structures

### 1a. Segment types and path segment struct (`airport_ground_pathfinder.h`)

```cpp
enum class TaxiSegmentType : uint8_t {
    FREE_MOVE,  // Bidirectional taxiways, aprons, stands, hangars
    ONE_WAY,    // One-way taxiways (queue-safe)
    RUNWAY,     // Runway tiles (atomic reservation)
};

struct TaxiSegment {
    TaxiSegmentType type;
    uint16_t start_index;  // Index into path vector (first tile of this segment)
    uint16_t end_index;    // Index into path vector (last tile of this segment, inclusive)
};

struct TaxiPath {
    std::vector<TileIndex> tiles;          // Full A* path
    std::vector<TaxiSegment> segments;     // Classified segments
    bool valid = false;
};
```

### 1b. New tile classification function (`airport_ground_pathfinder.cpp`)

Add `IsOneWayTaxiTile(station, tile)` — checks `IsTaxiwayPiece(piece_type) && one_way_taxi`. This replaces the inline checks scattered through the code.

Add `ClassifyTaxiSegments(station, path)` — walks the path, classifies each tile, groups consecutive same-type tiles into segments. Uses:
- `IsModularRunwayPiece(piece_type)` → RUNWAY
- `IsOneWayTaxiTile(st, tile)` → ONE_WAY
- Everything else → FREE_MOVE

### 1c. Aircraft struct changes (`aircraft.h`)

Replace existing ground path fields with:
```cpp
// Replace: ground_path, ground_path_index, ground_path_last_tile, ground_path_stall_counter
TaxiPath *taxi_path = nullptr;               // Current taxi path with segments
uint16_t taxi_path_index = 0;                // Current position in path
uint8_t taxi_current_segment = 0;            // Which segment we're currently in
std::vector<TileIndex> taxi_reserved_tiles{}; // Tiles we currently hold reservations on
uint16_t taxi_wait_counter = 0;              // Ticks waiting for segment reservation
```

Keep existing: `ground_path_goal`, `modular_ground_target`, `modular_landing_tile`, `modular_landing_stage`, `modular_takeoff_tile`, `modular_takeoff_progress`, `modular_runway_reservation`.

## Step 2: Modify A* Pathfinder (Topology Only)

**File:** `src/airport_ground_pathfinder.cpp`

### 2a. Remove reservation check from `CanTilesConnect`

Delete lines 177-185 (the `if (v != nullptr)` block that checks `HasAirportTileReservation`). The pathfinder should find topology paths regardless of current reservations.

### 2b. Keep stand avoidance but change it

Change the `IsParkingOnlyTile` check (line 156) to avoid stands that are occupied by other aircraft (physical position, not reservation). Pass a flag or use the aircraft's goal to determine which stands to allow:
- Always allow the goal tile
- Always allow the current tile
- Block stands where another aircraft is physically parked (check `v->tile` of all Aircraft at the station — but only for stands, not transient tiles)

Actually, simpler approach: keep the existing logic (`to != v->tile && to != v->ground_path_goal`). Stands that are reserved but not our goal are already blocked by `IsParkingOnlyTile` preventing pass-through. This is the right behavior — we don't want to route THROUGH stands.

### 2c. Add `ClassifyTaxiSegments` function

```cpp
TaxiPath BuildTaxiPath(const Station *st, TileIndex start, TileIndex goal);
```

This calls `FindAirportGroundPath(st, start, goal, nullptr)` (topology only), then classifies the resulting path into segments. Returns a `TaxiPath` with both the tile list and segment list.

## Step 3: Segment Reservation Functions

**File:** `src/aircraft_cmd.cpp` (new helper functions)

### 3a. `TryReserveSegment`

```cpp
static bool TryReserveSegment(Aircraft *v, const TaxiPath &path,
                               uint8_t segment_idx, TileIndex exit_tile)
```

For a FREE_MOVE segment:
1. Iterate all tiles in the segment (path.tiles[seg.start_index..seg.end_index])
2. For each tile: if reserved by another aircraft → return false
3. Also check `exit_tile` (first tile of next segment): if reserved by another → return false
4. If all clear: set reservations on all segment tiles + exit_tile, add to `v->taxi_reserved_tiles`
5. Return true

For a ONE_WAY segment: reserve just the first tile (path.tiles[seg.start_index]).

For a RUNWAY segment: delegate to existing `TryReserveContiguousModularRunway`.

### 3b. `ReleaseSegmentReservations`

```cpp
static void ReleaseSegmentReservations(Aircraft *v)
```

Release all tiles in `v->taxi_reserved_tiles` that the aircraft has passed (not the current tile). Or release all segment tiles when transitioning to the next segment.

### 3c. `TryReserveLandingChain`

```cpp
static bool TryReserveLandingChain(Aircraft *v, const Station *st,
                                    TileIndex runway_end, TileIndex stand)
```

For landing: atomically reserve runway + exit path to first safe queuing point:
1. Reserve contiguous runway
2. Compute path from rollout point to stand
3. Classify segments
4. If first segment after runway is ONE_WAY: also reserve first one-way tile
5. If first segment after runway is FREE_MOVE: also reserve entire free-move segment + its exit
6. If any step fails: release everything, return false

## Step 4: Rewrite `AirportMoveModular`

**File:** `src/aircraft_cmd.cpp`, lines 3886-4326 — **full rewrite**

The new function is much simpler. Pseudocode:

```
AirportMoveModular(v, st):
    if goal == INVALID_TILE: return true  // no goal
    if v->tile == goal: HandleModularGroundArrival(v); return true  // already there

    // Phase 1: Get or compute path
    if v->taxi_path == nullptr:
        path = BuildTaxiPath(st, v->tile, v->ground_path_goal)
        if !path.valid:
            // No topology path exists
            taxi_wait_counter++
            if taxi_wait_counter > 128: abandon goal, clear target
            return false
        v->taxi_path = new TaxiPath(path)
        v->taxi_path_index = 0
        v->taxi_current_segment = 0
        taxi_wait_counter = 0

    // Phase 2: Acquire current segment if not yet reserved
    seg = taxi_path->segments[taxi_current_segment]

    if need_to_reserve_segment:
        exit_tile = first tile of next segment (or INVALID if last)
        if seg.type == FREE_MOVE:
            if !TryReserveSegment(v, *taxi_path, taxi_current_segment, exit_tile):
                taxi_wait_counter++
                if taxi_wait_counter > 64:
                    // Try alternate path
                    delete taxi_path; taxi_path = nullptr
                    taxi_wait_counter = 0
                return false
        else if seg.type == ONE_WAY:
            next_tile = taxi_path->tiles[taxi_path_index + 1]
            if reserved by other: return false  // wait in queue
            reserve next_tile
        else if seg.type == RUNWAY:
            // Should not normally happen in ground movement
            // (runway entry is via HandleModularGroundArrival MGT_RUNWAY_TAKEOFF)

    // Phase 3: Follow path
    next_tile = taxi_path->tiles[taxi_path_index + 1]

    if seg.type == ONE_WAY:
        // Check next tile is free before moving
        if HasAirportTileReservation(next_tile) && reserver != v->index:
            return false  // queue wait
        // Reserve next tile
        SetAirportTileReservation(next_tile, true)
        SetAirportTileReserver(next_tile, v->index)

    // Move toward next tile
    direction = toward next_tile
    count = UpdateAircraftSpeed(v, SPEED_LIMIT_TAXI)
    move via GetNewVehiclePos

    if reached next_tile:
        old_tile = v->tile
        v->tile = next_tile
        taxi_path_index++

        // Release old tile
        if seg.type == ONE_WAY:
            release old_tile reservation

        // Check segment transition
        if taxi_path_index > seg.end_index:
            // Crossed into next segment
            if seg.type == FREE_MOVE:
                release all free-move tiles (taxi_reserved_tiles) except exit_tile
            taxi_current_segment++
            if taxi_current_segment >= segments.size():
                // Reached goal
                HandleModularGroundArrival(v)
                cleanup taxi_path
                return true

    return false
```

### What to DELETE from current AirportMoveModular:
- All `no_path_retry_tick` / exponential backoff logic
- All `replan_bursts` / stall detection / 50-tick replan
- Emergency runway exit logic
- Hangar deadlock breaker (64 no-path)
- Takeoff deadlock breaker (128 no-path)
- `ReconcileModularRunwayReservationTracking` calls (simplify)
- `LogModularPathfindingFailure` rate-limited topology checks
- The entire tile-by-tile path following with incremental reservation

## Step 5: Modify Landing Flow

### 5a. `AircraftEventHandler_Flying` (lines 1877-1968)

Current flow at line 1892-1923:
1. `FindModularLandingTarget` → get runway
2. Check distance
3. `TryReserveContiguousModularRunway` → reserve runway
4. Set LANDING state

New flow:
1. `FindModularLandingTargetWithExit` → get runway + exit path info
2. Check distance
3. `TryReserveLandingChain(v, st, runway, stand)` → reserve runway + exit to first safe point
4. If reservation fails → stay FLYING, try next runway candidate
5. Set LANDING state only if reservation succeeded

### 5b. New `FindModularLandingTargetWithExit`

Modify `FindModularLandingTarget` (lines 2533-2644) to also compute and cache the exit path for each runway candidate. For each candidate runway end:
1. Get rollout point
2. Find best stand (reuse `FindFreeModularTerminal`)
3. Compute path from rollout to stand (topology only)
4. Classify segments — determine the "safe point" (first one-way tile, or stand if no one-way)
5. Try atomic reservation of runway + path to safe point
6. If reservation succeeds: this is a viable runway, score it
7. If reservation fails: try next candidate

Pick the best-scoring viable runway. If none, stay in holding pattern.

### 5c. After touchdown

When the aircraft touches down (in `AirportMoveModularLanding`, around line 2804), it already has its runway + exit path reserved from step 5b. Set up the taxi path:
- Create TaxiPath from rollout point to stand (already computed)
- The free-move exit segment is already reserved
- Begin ground movement immediately

## Step 6: Modify Departure Flow

### 6a. `FindModularRunwayTileForTakeoff` (lines 3285-3354)

Add free-move segment reservability check. For each runway candidate:
1. Compute path from current position to runway queue point
2. Classify segments
3. Try reserving the first free-move segment + its exit (first one-way tile)
4. If reservation succeeds: this runway is viable
5. If reservation fails: try next runway candidate

If no runway is viable, wait at stand/hangar.

### 6b. `HandleModularGroundArrival` MGT_RUNWAY_TAKEOFF case (lines 3549-3648)

Simplify significantly. The aircraft arrives at the last one-way tile before the runway. It needs to:
1. `TryReserveContiguousModularRunway` — already implemented
2. If succeeds: enter TAKEOFF state
3. If fails: wait (queued on one-way tile)

Remove: the complex clearance checking, the yield-to-landing logic (the segment system prevents this deadlock), the stale reservation cleanup.

## Step 7: Cleanup — Delete Dead Code

Remove from `aircraft_cmd.cpp`:
- `static std::map<VehicleID, uint64_t> no_path_retry_tick` and all uses
- `static std::map<VehicleID, uint16_t> no_path_bursts` and all uses
- `static std::map<VehicleID, uint16_t> replan_bursts` and all uses
- `LogModularPathfindingFailure` function (replace with simpler logging)
- `LogModularVehicleReservationState` function (simplify)
- `HasModularReservationOutsideTile` helper
- `ReconcileModularRunwayReservationTracking` — simplify or remove
- Emergency runway exit logic
- Hangar/takeoff deadlock breakers
- `ground_path_last_tile` field from aircraft.h
- `ground_path_stall_counter` field from aircraft.h (replaced by `taxi_wait_counter`)
- `ClearGroundPathReservation` (replaced by `ReleaseSegmentReservations`)
- `TryReserveGroundPath` (replaced by `TryReserveSegment`)

## Implementation Order

### Phase A: Foundation (no behavior change yet)
1. Add `TaxiSegmentType`, `TaxiSegment`, `TaxiPath` structs to `airport_ground_pathfinder.h`
2. Add `IsOneWayTaxiTile()` function to `airport_ground_pathfinder.cpp`
3. Add `ClassifyTaxiSegments()` function
4. Add `BuildTaxiPath()` that calls A* + classification
5. Add new fields to Aircraft struct
6. **Build and verify** — no runtime changes yet

### Phase B: Core ground movement rewrite
1. Add `TryReserveSegment()` and `ReleaseSegmentReservations()`
2. Rewrite `AirportMoveModular()` using segment-based logic
3. Remove reservation check from `CanTilesConnect` in the pathfinder
4. Update `HandleModularGroundArrival` for MGT_ROLLOUT to use BuildTaxiPath
5. **Build and test** — ground taxi should work with new reservation logic

### Phase C: Landing flow
1. Add `TryReserveLandingChain()`
2. Modify `AircraftEventHandler_Flying` to require exit path reservation
3. Modify `AirportMoveModularLanding` touchdown to use pre-computed path
4. **Build and test** — landing should work with pre-reserved exit

### Phase D: Departure flow
1. Modify `FindModularRunwayTileForTakeoff` to check segment reservability
2. Simplify `HandleModularGroundArrival` MGT_RUNWAY_TAKEOFF case
3. **Build and test** — departure should try alternate runways

### Phase E: Cleanup
1. Delete dead code (old maps, old retry logic, old stall detection)
2. Remove unused Aircraft fields
3. Final build and test

## Verification

1. Build with `cmake --build .` from `/Users/tor/ttd/OpenTTD/build`
2. Load the savegame with 44 aircraft that previously deadlocked/crashed
3. Verify: aircraft land, taxi to stands, load/unload, taxi to runway, take off
4. Verify: no deadlocks on bidirectional taxiways (aircraft wait rather than enter)
5. Verify: one-way taxiways allow queuing (multiple aircraft in line)
6. Verify: aircraft in holding pattern when no runway+exit reservation available
7. Verify: aircraft try alternate runways when primary blocked
8. Verify: no crash after several minutes of gameplay
