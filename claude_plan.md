# Modular Airport Ground Pathfinding - Implementation Plan

## Current Status (2026-01-24)

**Progress**: 4 of 7 phases completed (57%)

- ✅ **Phase 1**: Store Tile Metadata - DONE
- ✅ **Phase 2**: SaveLoad Integration - DONE
- ✅ **Phase 3**: Ground Pathfinder (A*) - DONE
- ✅ **Phase 4**: Aircraft Integration - DONE
- ⏳ **Phase 5**: Tile Reservation - TODO
- ⏳ **Phase 6**: Terminal Types & Takeoff - TODO
- ⏳ **Phase 7**: Polish & Optimizations - TODO

**Current Capabilities**:
- Tile metadata (piece type, rotation, taxi directions) is stored and persisted
- A* pathfinding algorithm finds optimal ground paths respecting taxi directions
- Aircraft detect modular airports and use pathfinding instead of FTA
- Landing aircraft find free terminals and path to them
- SaveLoad support for all new data structures

**Next Steps**: Implement tile reservations for multi-aircraft collision avoidance (Phase 5)

---

## Overview

Implement aircraft ground pathfinding for custom-built modular airports using **Option D (Hybrid)** from routing_options.md: auto-calculate taxi directions from piece geometry, allow user overrides via UI direction controls.

**Current State**: UI fully implemented (26 piece types, rotation, taxi direction toggles) but doesn't pass/store metadata. Aircraft can't taxi on modular airports.

**Goal**: Aircraft automatically pathfind from runway to terminal/hangar and back on player-built airports.

---

## Implementation Phases

### Phase 1: Store Tile Metadata ✅ COMPLETED

**Objective**: Fix the gap where UI collects but doesn't store taxi direction data.

**Files to Modify**:
1. **`src/base_station_base.h`** - Add after line 32:
```cpp
struct ModularAirportTileData {
    TileIndex tile;
    uint8_t piece_type;           // 0-25 (26 piece types from UI)
    uint8_t rotation;              // 0-3
    uint8_t user_taxi_dir_mask;    // bit: 0=N, 1=E, 2=S, 3=W
    bool one_way_taxi;
    uint8_t auto_taxi_dir_mask;    // calculated from piece_type + rotation
};
```

2. **`src/station_base.h`** (line 379+) - Add to Airport struct:
```cpp
std::vector<ModularAirportTileData> *modular_tile_data = nullptr;
const ModularAirportTileData* GetModularTileData(TileIndex tile) const;
void EnsureModularDataExists();
```

3. **`src/station_cmd.h`** (line 27) - Extend command signature:
```cpp
CommandCost CmdBuildModularAirportTile(DoCommandFlags flags, TileIndex tile,
    uint16_t gfx, StationID station_to_join, bool allow_adjacent,
    uint8_t rotation, uint8_t taxi_dir_mask, bool one_way_taxi);  // ADD THESE 3
```

4. **`src/station_cmd.cpp`** (line 2758+) - Implement storage:
   - Accept new parameters
   - Create/store ModularAirportTileData in vector
   - Call `CalculateAutoTaxiDirections(piece_type, rotation)` to compute auto_taxi_dir_mask

5. **`src/airport_gui.cpp`** (line 879) - Pass data to command:
```cpp
// Change line 884-888 to pass rotation, taxi_dir_mask, one_way_taxi
Command<CMD_BUILD_MODULAR_AIRPORT_TILE>::Post(/* ... */,
    this->rotation, this->taxi_dir_mask, this->one_way_taxi);
```

**New Files**:
- **`src/airport_pathfinder.h`** - Declare `CalculateAutoTaxiDirections()` and `GetEffectiveTaxiDirections()`
- **`src/airport_pathfinder.cpp`** - Implement direction calculation with lookup table:
```cpp
static const uint8_t _piece_taxi_directions[26][4] = {
    { 0x05, 0x0A, 0x05, 0x0A },  // RUNWAY: N+S or E+W by rotation
    // ... 25 more entries
};
```

**Test**: Place tiles with different rotations, verify metadata stored correctly.

---

### Phase 2: SaveLoad Integration ✅ COMPLETED

**Objective**: Persist tile data to savegames.

**Files to Modify**:
1. **`src/saveload/station_sl.cpp`** (after line 545) - Add handler:
```cpp
class SlModularAirportTileData : public VectorSaveLoadHandler<...> {
    static inline const SaveLoad description[] = {
        SLE_VAR(ModularAirportTileData, tile, SLE_UINT32),
        SLE_VAR(ModularAirportTileData, piece_type, SLE_UINT8),
        // ... all fields
    };
};
```

2. **`src/saveload/saveload.h`** - Add version constant:
```cpp
SLV_MODULAR_AIRPORT_PATHFINDING, ///< Modular airport pathfinding data
```

3. Register in `_station_desc` with `SLEG_CONDSTRUCTLIST`

**Test**: Save/load game with modular airport, verify tiles restored.

---

### Phase 3: Ground Pathfinder ✅ COMPLETED

**Objective**: Implement A* pathfinder for ground movement.

**New Files**:
- **`src/airport_ground_pathfinder.h`** - Declare `FindAirportGroundPath()`
- **`src/airport_ground_pathfinder.cpp`** - Implement A* algorithm:
  - Node structure with tile, g_cost, f_cost, parent
  - `GetReachableNeighbors()` - checks CanTilesConnect() based on taxi_dir_mask
  - `CalculateHeuristic()` - Manhattan distance to goal
  - Priority queue based A* search
  - Returns path as `std::vector<TileIndex>`

**Key Functions**:
```cpp
struct AirportGroundPath {
    std::vector<TileIndex> tiles;
    int cost;
    bool found;
};

AirportGroundPath FindAirportGroundPath(
    const Station *st, TileIndex start, TileIndex goal, const Aircraft *v);
```

**Algorithm**:
1. Open set = priority queue ordered by f_cost
2. For each node: expand neighbors that are taxi-connected
3. Use effective taxi directions (auto & user) from GetEffectiveTaxiDirections()
4. Terminate when goal reached or MAX_ITERATIONS (1000)
5. Reconstruct path from parent pointers

**Test**: Build simple L-shaped path, verify pathfinder finds route.

---

### Phase 4: Aircraft Integration ✅ COMPLETED

**Objective**: Replace FTA logic with pathfinding for modular airports.

**Files to Modify**:
1. **`src/aircraft.h`** (line 72+) - Add to Aircraft struct:
```cpp
std::vector<TileIndex> *ground_path = nullptr;
uint16_t ground_path_index = 0;
TileIndex ground_path_goal = INVALID_TILE;
```

2. **`src/aircraft_cmd.cpp`**:
   - **Line 1821** `AirportMove()` - Add modular airport detection:
```cpp
if (st != nullptr && st->airport.blocks.Test(AirportBlock::Modular)) {
    return AirportMoveModular(v, st);  // NEW
}
// ... existing FTA code unchanged
```

   - **NEW** `AirportMoveModular()` function:
     - Check if `ground_path` exists, else call `FindAirportGroundPath()`
     - Follow path tile-by-tile, update aircraft position
     - Return true when path complete

   - **Line 1728** `AircraftEventHandler_EndLanding()` - Set ground_path_goal:
```cpp
if (st->airport.blocks.Test(AirportBlock::Modular)) {
    v->ground_path_goal = FindFreeModularTerminal(st, v);
    v->state = TERM1;
    return;
}
```

   - **NEW** `FindFreeModularTerminal()` - Search for piece_type 7 or 8 (terminals) that's unoccupied

**Test**: Land aircraft, verify it paths to terminal. Test with 2 aircraft simultaneously.

---

### Phase 5: Tile Reservation ⏳ TODO

**Objective**: Prevent collisions between multiple aircraft.

**Files to Modify**:
1. **`src/station_map.h`** - Add reservation bit accessors (like rail PBS):
```cpp
inline bool HasAirportTileReservation(Tile t);
inline void SetAirportTileReservation(Tile t, bool reserved);
inline VehicleID GetAirportTileReserver(Tile t);
inline void SetAirportTileReserver(Tile t, VehicleID vid);
```

2. **`src/aircraft_cmd.cpp`** - Implement reservation:
   - `TryReserveGroundPath()` - Reserve all tiles in path or fail
   - `ClearGroundPathReservation()` - Clear on path completion
   - Modify `AirportMoveModular()` to check reservations before moving
   - Clear reservations in Aircraft destructor

**Storage**: Use tile.m6() bit 0 for reservation flag, tile.m7() for vehicle ID (like rail PBS)

**Test**: Two aircraft on same airport, verify collision avoidance via reservations.

---

### Phase 6: Terminal Types & Takeoff ⏳ TODO

**Objective**: Support hangars, helipads, and takeoff routing.

**Enhancements**:
1. **Terminal finding**: Distinguish terminals (7,8), hangars (9,10), helipads (11)
   - Helicopters prefer helipads
   - Depot orders route to hangar
   - Fallback: helicopters can use terminals

2. **Runway finding**: `FindModularRunway()` - locate pieces 0-4 (runways)

3. **Takeoff sequence**: Set ground_path_goal to runway, then switch to existing FTA for airborne

**Test**: Send aircraft to depot (routes to hangar). Land helicopter (uses helipad).

---

### Phase 7: Polish ⏳ TODO

**Features**:
- Visual taxi direction arrows (overlay when show_taxi_arrows enabled)
- Connectivity validator (warn if terminals unreachable from runways)
- Performance: spatial index using `std::unordered_map<TileIndex, ModularAirportTileData*>`
- Stuck detection: recalculate path if no progress after 50 ticks

**Test**: Large airport (50+ tiles), 10+ aircraft, verify smooth operation.

---

## Critical Integration Points

### Aircraft Movement Flow:
```
Aircraft::Tick() [2131]
  → AircraftEventHandler() [2095]
    → AirportGoToNextPosition() [1809]
      → AirportMove() [1821] ← PRIMARY INTEGRATION POINT
        → if modular: AirportMoveModular() [NEW]
        → else: existing FTA logic [unchanged]
```

### Landing Sequence (Modular):
```
FLYING → LANDING → ENDLANDING [1728]
  → FindFreeModularTerminal() [NEW]
  → Set ground_path_goal
  → AirportMoveModular() finds path
  → Follow path tile-by-tile → TERMINAL
```

### Backward Compatibility:
- All existing airports use FTA (detection: `!blocks.Test(AirportBlock::Modular)`)
- No changes to FTA code paths
- Saveload version guard: old saves load fine (modular_tile_data = nullptr)

---

## Critical Files Summary

1. **`src/station_cmd.cpp`** (2758+) - Tile placement command, metadata storage
2. **`src/station_base.h`** (379+) - Airport struct with modular_tile_data vector
3. **`src/aircraft_cmd.cpp`** (1821+) - AirportMove(), AirportMoveModular(), terminal finding
4. **`src/airport_pathfinder.cpp`** (NEW) - Direction calculation, connectivity checks
5. **`src/airport_ground_pathfinder.cpp`** (NEW) - A* pathfinder implementation
6. **`src/saveload/station_sl.cpp`** (545+) - SaveLoad handler for tile data

---

## Testing Strategy

### Phase-by-Phase Tests:
1. **Phase 1**: Place tiles, inspect with debugger, verify metadata stored
2. **Phase 2**: Save/load game, verify persistence
3. **Phase 3**: Console command to test pathfinder with start/goal tiles
4. **Phase 4**: Land single aircraft, follow with debugger, verify movement
5. **Phase 5**: Land two aircraft, verify second waits for first to clear
6. **Phase 6**: Test depot order, helicopter landing, takeoff sequence
7. **Phase 7**: Stress test with 20 aircraft on 100-tile airport

### End-to-End Verification:
1. Build modular airport: runway + 3 taxiways + 2 terminals + 1 hangar
2. Land 3 aircraft - verify all find different terminals
3. Send 1 to depot - verify routes to hangar
4. All takeoff simultaneously - verify queueing
5. Save/load game - verify everything works after load

---

## Timeline

- **Week 1**: Phase 1-2 (data model + saveload) - 3-5 days
- **Week 2-3**: Phase 3-4 (pathfinder + integration) - 7-9 days
- **Week 4**: Phase 5 (reservations) - 2-3 days
- **Week 5-6**: Phase 6-7 (terminals + polish) - 4-6 days

**Total**: 5-6 weeks for complete feature

---

## Risk Mitigation

**High Risk**: Phase 4 (aircraft integration) - complex state machine
- **Mitigation**: Extensive logging, step-through debugging, test with single aircraft first

**Medium Risk**: Phase 5 (collision avoidance) - deadlock potential
- **Mitigation**: Timeout detection, forced reroute after stuck

**Low Risk**: Phases 1-3 - isolated from existing systems
- Can be tested independently before aircraft integration

---

## Success Criteria

✅ Aircraft land and taxi to terminals on modular airports
✅ Multiple aircraft don't collide (reservation system works)
✅ Airports saved/loaded correctly
✅ Existing preset airports still work (backward compatible)
✅ Performance: pathfinding <10ms even on large airports
✅ Visual feedback: taxi arrows show allowed directions
