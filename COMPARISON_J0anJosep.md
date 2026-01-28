# Comparison: Our Modular Airport Implementation vs J0anJosep's PR #5

## Overview

**Our Implementation**: Simple piece-by-piece modular airport builder with custom ground pathfinding
**J0anJosep's PR**: Complete airport system redesign with track-based infrastructure (analogous to railways)

---

## Core Architecture Differences

### Our Approach: Piece-Based System
- **26 predefined piece types** (runway, taxiway, terminal, hangar, helipad, apron, grass, fence, etc.)
- Each piece has a GFX ID, rotation (0-3), and taxi direction mask
- Store `ModularAirportTileData` per tile with:
  - `piece_type` (GFX ID)
  - `rotation` (0-3)
  - `user_taxi_dir_mask` (user overrides via UI toggles)
  - `auto_taxi_dir_mask` (calculated from piece type + rotation)
- **Pathfinding**: Custom A* implementation for ground movement
  - 4-bit bitmask for allowed directions (0x01=N, 0x02=E, 0x04=S, 0x08=W)
  - Pathfinder respects `GetEffectiveTaxiDirections(auto & user)`

### J0anJosep's Approach: Track-Based System
- **AirType system** (like RailType): Different surface types
  - GRAVEL, ASPHALT, WATER, DARK, YELLOW
  - NewGRF-extensible with custom air types
- **AirportTileType**: Functional tile classifications
  - Infrastructure (with/without catchment)
  - Simple track
  - Waiting points
  - Aprons (normal, helipad, heliport, built-in heliport)
  - Hangars (standard, extended)
  - Runways (middle, end, start with/without landing)
- **Track-based movement**: Aircraft follow **Trackdir** like trains
  - Uses OpenTTD's `Track` enum (12 directions + diagonal bits)
  - Aircraft have a `path` vector of trackdirs
- **Pathfinding**: YAPF (Yet Another PathFinder)
  - `YapfAircraftFindPath()` - same sophisticated system used for trains
  - Path caching for performance
  - Handles complex route finding through track network

---

## Building Interface

### Our System
- **UI**: Piece selector with 26 pieces
- Rotation button (0-3) for each piece
- Taxi direction toggles (N/E/S/W checkboxes) for user overrides
- Build pieces one-by-one
- No drag-building (click individual tiles)

### J0anJosep's System
- **Multiple build modes**:
  - `CmdAddRemoveAirportTiles` - Add/remove airport infrastructure
  - `CmdAddRemoveTracksToAirport` - Add/remove specific tracks
  - `CmdChangeAirportTiles` - Change tile types/configuration
  - `CmdAirportChangeTrackGFX` - Change visual track appearance
  - `CmdAirportToggleGround` - Toggle ground tile visibility
- **Drag-building support** with diagonal option
- **AirType conversion** - Convert existing airport surface types
- **Climate-aware sprites** - Different graphics for different climates

---

## Aircraft Behavior

### Our System
- **Landing**:
  - Find runway/helipad via `FindModularLandingTarget()`
  - Two-stage approach: FAF (Final Approach Fix) → final
  - Custom `AirportMoveModularLanding()` with pixel-by-pixel movement
  - Speed limits: HOLD (425 km/h) for approach, APPROACH (230 km/h) for final
- **Ground Movement**:
  - `FindAirportGroundPath()` using A* from current tile to goal
  - Path stored as `vector<TileIndex>`
  - Follow path tile-by-tile with reservation system
- **Takeoff**:
  - Find runway, taxi to it, accelerate on runway
  - Custom `AirportMoveModularTakeoff()` sequence
- **Terminals**:
  - Find free terminal/helipad/hangar by piece type
  - Helicopters prefer helipads, depot orders go to hangars

### J0anJosep's System
- **New Aircraft Controller** (complete rewrite of aircraft behavior)
- **Track-based movement**:
  - Aircraft follow trackdirs like train movement
  - `YapfAircraftFindPath()` calculates optimal route
  - Path caching: `v->path` stores computed route
  - `v->HandlePathfindingResult()` processes path validity
- **State management**:
  - Complex state machine for different flight phases
  - Holding patterns integrated with track network
  - Entry points defined by track connections
- **Compatibility**:
  - Aircraft engines filtered by compatible air types
  - Airport infrastructure requirements
  - Savegame migration: "redeploy aircraft from old savegames"

---

## Hangar System

### Our System
- **Single hangar GFX**: `APT_DEPOT_SE` (visual always SE-oriented)
- **Rotation affects pathfinding**: Door direction changes
  - rot=0: South (dy+1)
  - rot=1: West (dx-1)
  - rot=2: North (dy-1)
  - rot=3: East (dx+1)
- **Exit direction**: Calculate based on rotation
  - `GetModularHangarExitDirection()` returns `DIR_SE + rotation*2`
- **Pathfinding restriction**: Single-direction entry/exit

### J0anJosep's System
- **Moveable hangars**: Hangars can be placed anywhere
- **Two hangar types**:
  - `ATT_HANGAR_STANDARD` (8)
  - `ATT_HANGAR_EXTENDED` (10)
- **Track-connected**: Hangars connect to airport track network
- **No directional restriction**: Use standard track pathfinding

---

## Pathfinding Comparison

| Feature | Our A* Implementation | J0anJosep's YAPF |
|---------|----------------------|------------------|
| Algorithm | A* (Manhattan heuristic) | YAPF (OpenTTD's train pathfinder) |
| Representation | Tile-to-tile (4 orthogonal neighbors) | Trackdir-based (12 directions) |
| Path format | `vector<TileIndex>` | `vector<Trackdir>` (implicit in path cache) |
| Performance | Simple, fast for small airports | Sophisticated caching, optimized for large networks |
| Features | Basic connectivity checking | Penalty system, one-way tracks, signals |
| Complexity | ~200 lines (A* implementation) | Reuses existing YAPF infrastructure (thousands of lines) |
| Max iterations | 1000 (hardcoded) | YAPF's sophisticated limits |
| Diagonal movement | No (orthogonal only) | Yes (Trackdir supports diagonals) |

---

## Tile Reservation / Collision Avoidance

### Our System
- **Simple reservation bits**:
  - `HasAirportTileReservation(tile)`
  - `GetAirportTileReserver(tile)` returns VehicleID
- **Reservation scope**: Individual tiles
- **Checking**: Before pathfinding, check if tiles are reserved by other aircraft
- **Clearing**: Clear reservations when path complete or aircraft destroyed

### J0anJosep's System
- **Track-based reservation**: Likely uses trackdir reservations like railways
- **Path signals**: May support one-way tracks or waiting points
- **Integration**: Leverages existing track reservation system

---

## NewGRF Support

### Our System
- **None**: Uses hardcoded 26 piece types with fixed GFX IDs
- **No extensibility**: Cannot add new piece types via NewGRF
- **Graphics**: Uses default OpenTTD airport sprites

### J0anJosep's System
- **Full NewGRF support**:
  - Custom air types (like rail types)
  - Custom airport tiles via NewGRF
  - OpenGFX+Airports compatibility mentioned
- **Extensible**:
  - AirType labels for NewGRF identification
  - Custom track graphics
  - Climate-aware sprite rendering

---

## Rotation System

### Our System
- **Rotation parameter**: 0-3 (90° increments)
- **Functional rotation** (pathfinding):
  - Hangars: Direction changes with rotation
  - Runways/taxiways: Currently allow all directions (0x0F) regardless of rotation (bug in our implementation)
- **Graphical rotation**:
  - Taxiways: 2 variants (horizontal/vertical)
  - Taxiway intersections: 2 variants
  - Apron edges: 4 variants
  - Fences: 4 variants (just fixed!)
  - Hangars, runways, helipads: No graphical rotation

### J0anJosep's System
- **DiagDirection rotation**: More sophisticated (NE, SE, SW, NW)
- **Track orientation**: Tracks naturally have direction/orientation
- **Rotated layouts**: Default airports can be built in multiple orientations

---

## Savegame Compatibility

### Our System
- **SaveLoad integration**:
  - `SlModularAirportTileData` handler
  - Stores `vector<ModularAirportTileData>`
  - Version guard for backward compatibility
- **Aircraft state**:
  - New fields: `ground_path`, `ground_path_goal`, `modular_landing_tile`, etc.
  - Clear on new game or when not using modular airports

### J0anJosep's System
- **Migration system**: "Redeploy aircraft from old savegames"
- **State conversion**: New aircraft controller requires state updates
- **Flooded airports**: Special handling for water-based airports

---

## Code Complexity

### Our Implementation
- **New files**:
  - `src/airport_pathfinder.cpp` (~180 lines)
  - `src/airport_pathfinder.h` (~36 lines)
  - `src/airport_ground_pathfinder.cpp` (~325 lines)
  - `src/airport_ground_pathfinder.h` (~30 lines)
  - `coords.md` (documentation)
- **Modified files**: ~15 files
- **Total additions**: ~1500-2000 lines
- **Approach**: Minimal changes, bolt-on to existing system

### J0anJosep's PR #5
- **Files changed**: 213 files
- **Additions**: ~20,858 lines
- **Deletions**: ~5,521 lines
- **Commits**: 143
- **Approach**: Comprehensive redesign, new infrastructure

---

## Strengths and Weaknesses

### Our System

**Strengths:**
- ✅ Simple to understand and maintain
- ✅ Minimal changes to existing codebase
- ✅ Works well for small-medium airports
- ✅ Direct tile-based thinking (easy to visualize)
- ✅ Fast pathfinding for simple layouts
- ✅ User-friendly piece-by-piece building

**Weaknesses:**
- ❌ No NewGRF support
- ❌ Limited to 26 predefined pieces
- ❌ Simple A* may struggle with very complex layouts
- ❌ No diagonal movement
- ❌ Less sophisticated than railway pathfinding
- ❌ Rotation implementation incomplete (runways/taxiways should restrict directions)

### J0anJosep's System

**Strengths:**
- ✅ Full NewGRF support (extensible)
- ✅ Sophisticated YAPF pathfinding
- ✅ Track-based system allows complex routing
- ✅ Diagonal movement support
- ✅ Multiple air types (like rail types)
- ✅ Climate-aware graphics
- ✅ Drag-building support
- ✅ Professional-grade implementation

**Weaknesses:**
- ❌ Very complex (20k+ lines changed)
- ❌ Steep learning curve for developers
- ❌ May be harder for players to understand (tracks vs pieces)
- ❌ Requires understanding of YAPF and trackdir system
- ❌ Larger merge/maintenance burden

---

## Conceptual Differences

### Philosophy

**Our approach**: "Airport Minecraft"
- Players place tiles like building blocks
- Each piece is discrete and understandable
- Simple mental model: "put runway here, taxiway there, connect terminal"

**J0anJosep's approach**: "Airports as Railways"
- Airports are track networks
- Aircraft are trains on airport tracks
- Complex mental model: understand trackdirs, air types, tile types

### User Experience

**Our system**:
- Easier for casual players
- More forgiving (pieces work independently)
- Limited by predefined pieces

**J0anJosep's system**:
- More powerful for advanced players
- Requires understanding of track system
- Highly customizable via NewGRF

---

## Recommendations

### If you want simplicity and quick implementation:
- **Stick with our approach**
- Fix the rotation bugs (runways/taxiways should restrict directions based on rotation)
- Consider adding more piece types
- Maybe add simple drag-building

### If you want a professional, extensible system:
- **Study J0anJosep's implementation**
- Consider merging/adapting their system
- Be prepared for significant complexity
- Gain NewGRF support and YAPF pathfinding

### Hybrid approach:
- Keep our simple piece-based UI
- Internally convert pieces to tracks for YAPF pathfinding
- Best of both worlds: simple UX, sophisticated pathfinding

---

## Key Takeaways

1. **J0anJosep's system is fundamentally different** - it's not just "better pathfinding", it's a complete redesign using track infrastructure

2. **Our system is simpler but less powerful** - good for a prototype or mod, but limited long-term

3. **The track-based approach is more "correct"** from an architectural standpoint - it leverages OpenTTD's existing sophisticated infrastructure

4. **NewGRF support is a huge advantage** - J0anJosep's system is extensible, ours is hardcoded

5. **Both systems work** - the question is scope and maintenance burden

---

**Bottom line**: J0anJosep's PR is a professional, comprehensive reimplementation. Our system is a working prototype that demonstrates the concept with minimal complexity. For a production feature, J0anJosep's approach is superior. For learning/prototyping, ours is more accessible.
