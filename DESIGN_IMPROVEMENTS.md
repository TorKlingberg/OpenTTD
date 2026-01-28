# Modular Airports: Design Improvements from Community Research

This document synthesizes design ideas from years of OpenTTD modular airport discussions, extracting actionable improvements for current and future implementation.

---

## 1. Core Architecture Improvements

### 1.1 Pathfinding & Movement

**From Dimme's Prototype & Blog Analysis:**
- ✅ **Already implemented**: We use A* pathfinding similar to their reservation-based approach
- 🔄 **Consider**: Implement **breadth-first search** instead of recursive pathfinding for large airports (mentioned as fixing lag issues)
- 🔄 **Future**: Add **holding pattern automation** - aircraft circle the tower while waiting for runway/gate assignments instead of just hovering

**From YAPF Discussion:**
- 🔄 **Future**: Consider migrating to YAPF (Yet Another PathFinder) for sophisticated train-like pathfinding
  - Enables diagonal movement
  - Path caching for performance
  - Already used in J0anJosep's implementation
- ⚠️ **Trade-off**: YAPF requires track-based system (major architectural change)

### 1.2 Deadlock Prevention

**From Multiple Threads:**
- ✅ **Already implemented**: Tile reservation system prevents basic collisions
- 🆕 **Add**: **Hangar overflow holding** - landing aircraft go to hangar when all gates occupied (Dimme's solution)
  - Hangars can hold "infinite" aircraft (actually a queue)
  - Aircraft reserve gates before leaving hangar
  - Prevents runway-to-gate deadlocks
- 🆕 **Add**: **Two-tile minimum reservation** - force aircraft to reserve 2+ tiles ahead (prevents roundabout gridlock)
- 🆕 **Add**: **Teleport escape hatch** - if deadlock detected after N ticks, teleport aircraft to hangar (last resort failsafe)

---

## 2. User Interface Enhancements

### 2.1 Construction Tools

**From Forum Consensus:**
- 🆕 **Add**: **Drag-building** for runways and taxiways
  - Current: Click individual tiles
  - Proposed: Click-drag like building railways
  - Implementation: Already shown in "That Guy's" graphics work
- 🆕 **Add**: **Helper overlays during construction** (from Wiki design draft):
  - Show taxiway connections with colored lines
  - Highlight runways (green for takeoff, blue for landing, yellow for both)
  - Display gate accessibility (which gates can reach which runways)
  - Show movement direction arrows on taxiways
- 🔄 **Improve**: **Airport selection dropdown** (from thread discussion)
  - Click-hold on airport button for menu
  - Options: "Pre-made airports" vs "Modular builder"
  - Similar to road/tram type selection UI

### 2.2 Validation & Feedback

**From Wiki Draft & Dimme's Validator:**
- 🆕 **Add**: **Real-time validation checker** that warns:
  - "No runway for takeoff" ❌
  - "No runway for landing" ❌
  - "Terminal X cannot reach runway Y" ⚠️
  - "Orphaned hangar (no runway access)" ⚠️
  - "Airport may deadlock under heavy traffic" ⚠️
- 🆕 **Add**: **Pathfinding overlay tool** - click a gate/hangar to see which runways it can reach
- 🆕 **Add**: **Noise level indicator** - show predicted noise based on runway count/length (from Wiki draft)

### 2.3 Templates & Saving

**From Thread Consensus:**
- 🆕 **Add**: **Save custom airport designs** as reusable templates
  - Store in user directory like savegames
  - Share-able via files
  - Select from dropdown when building
- 🆕 **Add**: **Pre-built template library** shipped with game
  - Small, Medium, Large templates
  - Example layouts to teach players
  - Based on real-world airports (Schiphol, LaGuardia, etc.)
- 🔄 **Future**: **Town-created templates** - towns pre-build airport layouts, players add terminals (advanced feature)

---

## 3. Piece Types & Functionality

### 3.1 Runway System

**From Multiple Sources:**
- 🆕 **Add**: **Runway type classification**:
  - Takeoff-only (green marking)
  - Landing-only (blue marking)
  - Combined/both (yellow or dual marking)
  - Currently: All runways do both
- 🆕 **Add**: **Runway length requirements** (from Physics discussion):
  - Calculate minimum length based on aircraft weight + max speed
  - Block aircraft from using too-short runways
  - Show error: "Runway too short for this aircraft type"
- 🆕 **Add**: **ICAO runway numbering** (from thread consensus):
  - Display runway heading (05, 14, 23, 32 for cardinal directions)
  - Show in aircraft status: "Landing on runway 23R"
  - Parallel runways get L/R/C suffixes
  - Optional feature, can be disabled
- 🆕 **Add**: **Diagonal runways** (mentioned as uncertain in Wiki draft)
  - Would require diagonal pathfinding
  - Low priority, complex implementation

### 3.2 Taxiway Improvements

**From Graphics & Logic Discussions:**
- ✅ **Partially done**: Rotation affects taxi directions
- 🆕 **Add**: **One-way taxiways** (high community interest):
  - User sets direction with arrow overlay
  - Aircraft can only travel in allowed direction
  - Simplifies airport design, prevents head-on collisions
- 🆕 **Add**: **Taxiway intersections with priorities**:
  - Some taxiways marked as "priority" (aircraft don't stop)
  - Others marked as "yield" (aircraft wait at intersection)
  - Similar to road priority signs
- 🔄 **Improve graphics**: Center taxiway sprites properly (mentioned as needed work)
  - Current issue: Aircraft partially clip buildings
  - Sprites need redrawing with proper centering

### 3.3 Terminal & Gate System

**From Design Discussions:**
- 🆕 **Add**: **Multiple gate types**:
  - Standard gates (current terminals)
  - Helicopter-only gates (helipad with restrictions)
  - Cargo gates (cargo aircraft priority)
  - Quick-turnaround gates (no passenger boarding, faster)
- 🆕 **Add**: **Multi-tile terminals** (mentioned in Wiki):
  - Large terminal buildings spanning 2x2 or 3x3 tiles
  - Multiple gates per terminal
  - More realistic for large airports
- 🆕 **Add**: **Gate reservation before landing** (Dimme's system):
  - Aircraft reserve gate while in approach
  - If no gates available, divert to hangar
  - Prevents "land then search for gate" behavior
- 🔄 **Future**: **Catchment area mechanics** (contentious):
  - Reduce airport catchment to force feeder services
  - Terminal-only pickups (realistic but complex)
  - Difficulty setting to balance realism vs playability

### 3.4 Hangar Enhancements

**From Multiple Sources:**
- ✅ **Already done**: Rotatable hangars with directional entry/exit
- 🆕 **Add**: **Hangar capacity limits** (from kamnet's realism proposal):
  - Default: 1 aircraft per hangar (realistic)
  - Optional: Infinite capacity (current behavior)
  - Game setting for realism level
- 🆕 **Add**: **Hangar overflow queue** (from Dimme):
  - When no gates available, aircraft land and taxi to hangar
  - Hangar acts as waiting area
  - Aircraft depart hangar when gate becomes free

### 3.5 Additional Piece Types

**From Discussions:**
- 🆕 **Add**: **Control tower** (Wiki requirement):
  - Mandatory 1 per airport (validation rule)
  - Aircraft circle tower when waiting
  - Visual landmark for holding pattern
- 🆕 **Add**: **Radar station** (Wiki mention):
  - Optional, improves airport capacity
  - Allows more simultaneous operations
  - Reduces delays in holding pattern
- 🆕 **Add**: **Waiting points** (from J0anJosep's tile types):
  - Designated spots where aircraft can hold
  - Like railway signals "wait here"
  - Prevents congestion at runway entrances
- 🆕 **Add**: **Environmental obstacles** (from Dimme's v2.02):
  - Water tiles (permanent obstacles)
  - Farmland/grass (removable for expansion)
  - Adds visual appeal and design challenge

---

## 4. Aircraft Behavior Improvements

### 4.1 Landing Sequence

**From Physics & Realism Discussions:**
- ✅ **Already done**: Two-stage landing (FAF → final approach)
- 🔄 **Improve**: **Holding pattern rings** (from Dimme's algorithm):
  - Auto-calculate concentric circles around airport
  - Multiple rings for multi-runway airports
  - Aircraft stack at different altitudes
  - More realistic than current "hover and wait"
- 🆕 **Add**: **IAF (Initial Approach Fix) calculation**:
  - Automatically position IAF based on runway heading
  - Scale with runway count (3 runways = 3 IAFs)
  - Aircraft approach from correct direction for runway
- 🆕 **Add**: **Simultaneous operations on combined runways**:
  - Allow landing + takeoff on same runway if distance > X tiles
  - Requires separation checking
  - More realistic, higher throughput

### 4.2 Takeoff Sequence

**From Design Proposals:**
- ✅ **Already done**: Basic takeoff pathfinding to runway
- 🆕 **Add**: **Takeoff queue management**:
  - Aircraft line up at runway threshold
  - Wait for clearance before entering runway
  - Currently: Aircraft just taxi onto runway
- 🆕 **Add**: **Acceleration physics** (from physics proposal):
  - Calculate required runway length for takeoff speed
  - Validate before assigning aircraft to runway
  - Block heavy aircraft from short runways
  - Formula: `length = (target_speed² - 0²) / (2 * acceleration)`

### 4.3 Ground Movement

**From Multiple Sources:**
- ✅ **Already done**: A* pathfinding with tile reservations
- 🆕 **Add**: **Speed limits by tile type**:
  - Taxiways: Current speed (50 km/h)
  - Aprons: Slower (30 km/h)
  - Runways during rollout: Faster (100+ km/h decelerating)
- 🆝 **Add**: **Perpendicular crossing rules** (from Dimme's v2.0):
  - Aircraft crossing taxiway perpendicular to arrows must check reservations
  - Prevents T-bone collisions
  - Adds realism to intersection behavior

---

## 5. Surface Types & Graphics

### 5.1 Air Type System (from J0anJosep)

**NewGRF-extensible surface types:**
- 🔄 **Future**: Implement **AirType** enum similar to RailType:
  - GRAVEL (dirt airports, prop planes only)
  - ASPHALT (standard jets)
  - WATER (seaplanes, helicopters)
  - DARK_ASPHALT (visual variant)
  - YELLOW_ASPHALT (visual variant)
- 🔄 **Future**: **Aircraft-surface compatibility**:
  - Jets require ASPHALT or better
  - Props work on GRAVEL or ASPHALT
  - Seaplanes require WATER
  - Helicopters compatible with all types
- ⚠️ **Trade-off**: Requires significant architecture changes

### 5.2 Graphics Improvements

**From Technical Discussions:**
- ✅ **Already done**: Rotation affects fence/apron edge graphics
- 🆕 **Add**: **Climate-aware sprites** (from J0anJosep):
  - Snow-covered runways in arctic climate
  - Desert-colored tarmac in desert
  - Tropical vegetation around tropical airports
- 🆕 **Add**: **Transparent taxiway backgrounds** (from richk67):
  - Taxiway sprites have no grass behind them
  - Ground shows through (allows snow/desert/grass)
  - More flexible visual appearance
- 🆕 **Add**: **Realistic taxiway markings**:
  - Yellow centerlines
  - Directional arrows painted on pavement
  - Runway number markings (05, 23, etc.)
  - Hold-short lines at runway intersections

---

## 6. Construction Constraints & Validation

### 6.1 Required Components (from Wiki Draft)

**Validation rules before airport can open:**
- ✅ **Already validated**: Need runway or helipad
- 🆕 **Add**: **Mandatory control tower** (1 per airport)
- 🆕 **Add**: **Every landing runway needs gate/hangar route**
- 🆕 **Add**: **Every gate needs takeoff runway route**
- 🆕 **Add**: **Orphaned hangar check** (warn if hangar can't reach runway)

### 6.2 Land Purchase & Boundaries

**From Design Discussions:**
- 🆕 **Add**: **Airport boundary system**:
  - Players purchase land area first (like fencing)
  - Build pieces only within boundary
  - Prevents city sprawl into airport
  - Clean separation between airport and surroundings
- 🆕 **Add**: **Expansion slots** (from template discussion):
  - Pre-designated areas for future expansion
  - Large airports have 2-3 expansion zones
  - Click "Expand" to unlock additional building area

### 6.3 Modification Rules

**From Wiki Draft:**
- ✅ **Already done**: Can build/modify airports anytime
- 🆕 **Add**: **Airport closure requirement for modifications**:
  - Must close airport (no aircraft present)
  - Prevents construction while aircraft taxiing
  - Re-open after validation passes
- 🆕 **Add**: **Noise restrictions** (Wiki draft):
  - Calculate noise level from runways (length × count)
  - Block opening if noise exceeds local authority limit
  - Requires player to remove/shorten runways

---

## 7. Multi-Company & Ownership

### 7.1 Shared Airports (from Template Discussion)

**From Community Proposals:**
- 🔄 **Future**: **Multi-company airports**:
  - Airport owner builds infrastructure (runways, taxiways, tower)
  - Other companies rent terminal spaces
  - Each company has designated gate area
  - Landing fees paid to airport owner
- 🔄 **Future**: **Town-owned airports**:
  - Town builds basic airport
  - Companies rent gates/hangars
  - More realistic, complex ownership model
- ⚠️ **Trade-off**: Requires major changes to station ownership system

---

## 8. Performance & Scalability

### 8.1 Pathfinding Optimization

**From Technical Discussions:**
- 🆕 **Improve**: **Replace recursive pathfinding with BFS** (breadth-first search)
  - Current: Potentially recursive A* (can lag)
  - Proposed: Iterative BFS removes lag on large airports
  - Mentioned as practical fix for Dimme's prototype lag
- 🆕 **Add**: **Path caching** (from YAPF/J0anJosep):
  - Cache routes between common points
  - Invalidate cache when airport modified
  - Significant performance boost for large airports
- 🆕 **Add**: **Max iteration limits** (already have 1000):
  - Return "no path found" instead of hanging
  - Show error to user: "Airport too complex for pathfinding"

### 8.2 Large Airport Support

**From "Huge Airports" Thread:**
- 🔄 **Future**: **Support 40+ tile runways**:
  - Realistic for simultaneous operations
  - Requires extended pathfinding range
  - May need increased MAX_PATHFINDER_ITERATIONS
- 🔄 **Future**: **6+ runway airports**:
  - Scale holding patterns appropriately
  - Ensure pathfinding can handle complexity
  - Test with 100+ aircraft (mentioned as goal)

---

## 9. Realism vs Playability Balance

### 9.1 Difficulty Settings

**From Community Consensus:**
- 🆕 **Add**: **Airport realism difficulty setting**:
  - **Casual**: No restrictions, infinite hangar capacity, simple pathfinding
  - **Standard**: Current behavior, some restrictions
  - **Realistic**: Hangar limits, noise restrictions, physics validation
  - **Expert**: Everything + multi-company sharing
- 🆕 **Per-setting toggles**:
  - "Realistic runway length requirements" (ON/OFF)
  - "Limit hangars to 1 aircraft" (ON/OFF)
  - "Require control tower" (ON/OFF)
  - "Enable noise restrictions" (ON/OFF)

### 9.2 Player Guidance

**From Accessibility Discussions:**
- 🆕 **Add**: **Tutorial airport templates**:
  - Ship 5-10 pre-designed airports as examples
  - Include tooltips explaining each piece
  - "This layout supports 10 aircraft"
  - "Copy this design and modify it"
- 🆕 **Add**: **Wizard tool**:
  - Ask: "How many aircraft? Small/Medium/Large?"
  - Auto-generate basic layout
  - Player tweaks from there
  - Reduces learning curve

---

## 10. Implementation Priority Tiers

### Tier 1: Quick Wins (Small Changes, High Value)
1. ✅ **Fence rotation** - DONE!
2. 🆕 **Drag-building for runways/taxiways** - UI improvement
3. 🆕 **Validation warnings** - "No runway" error messages
4. 🆕 **One-way taxiways** - Add direction flag to taxi_dir_mask
5. 🆕 **Hangar overflow queue** - Aircraft wait in hangar for gates

### Tier 2: Medium Features (Moderate Effort)
1. 🆕 **Runway type classification** - Takeoff/Landing/Both flags
2. 🆕 **Save/load airport templates** - File serialization
3. 🆕 **Helper overlay graphics** - Draw pathfinding routes on UI
4. 🆕 **Holding pattern rings** - Auto-calculated circles
5. 🆝 **Speed limits by tile type** - Differentiate taxiway/apron speeds
6. 🆕 **Pre-built template library** - Ship example airports

### Tier 3: Major Features (Significant Work)
1. 🔄 **YAPF migration** - Replace A* with train pathfinder
2. 🆕 **Multi-tile terminals** - 2x2, 3x3 buildings
3. 🆕 **Control tower + waiting points** - Mandatory tower, holding areas
4. 🆕 **Runway physics validation** - Length requirements
5. 🆕 **Path caching** - Performance optimization
6. 🆕 **ICAO runway numbering** - Display heading on runways

### Tier 4: Future Vision (Architectural Changes)
1. 🔄 **AirType system** - Surface type enum like RailType
2. 🔄 **Multi-company airports** - Shared infrastructure
3. 🔄 **NewGRF support** - Custom pieces, surfaces, layouts
4. 🆝 **Diagonal runways** - Requires diagonal pathfinding
5. 🔄 **Climate-aware graphics** - Snow/desert variants

---

## 11. Lessons from Failed/Stalled Projects

### What NOT to Do (from Project Post-Mortems):

1. **Don't start with finite state machines** (richk67's complexity warning)
   - ✅ We avoided this with pathfinding approach

2. **Don't promise perfect deadlock prevention** (DaleStan's caution)
   - ✅ We have escape hatch (teleport to hangar)
   - 🆕 Add explicit warning: "User-built airports may jam under heavy traffic"

3. **Don't require complete rewrite** (Dimme's barrier)
   - ✅ Our implementation is modular, bolt-on to existing system
   - 🔄 J0anJosep did complete rewrite (20k+ lines) - too ambitious for most

4. **Don't ignore savegame compatibility** (mentioned across threads)
   - ✅ We have SaveLoad integration
   - ✅ Version guards prevent breaking old saves

5. **Don't build complex UI first** (phased approach consensus)
   - ✅ We started with basic piece placement
   - 🆕 Enhance UI incrementally (drag-building, overlays, wizards)

### What TO Do (from Successful Patterns):

1. **Release working prototypes early** (Dimme's Java demo)
   - Validates feasibility before committing to implementation
   - Our current system IS a working prototype

2. **Use existing infrastructure** (YAPF suggestion)
   - Don't reinvent pathfinding if YAPF works
   - J0anJosep's approach leverages existing train pathfinder

3. **Keep pieces simple** (consensus on modularity)
   - ✅ Our 26 pieces are intuitive
   - Don't overcomplicate with too many variants initially

4. **Provide templates** (universal request)
   - Players learn by example
   - Reduces "blank canvas" paralysis

5. **Phased implementation** (agreed approach)
   - Core functionality first (pathfinding, pieces)
   - Polish later (graphics, animations, advanced features)

---

## 12. Specific Code Improvements

### 12.1 Pathfinding Algorithm

**Current:**
```cpp
// A* with Manhattan heuristic
int h = abs(dx) + abs(dy);
```

**Proposed Improvement:**
```cpp
// Add BFS option for very large airports
bool use_bfs = (airport_tile_count > 100);
if (use_bfs) {
    // Iterative BFS (no recursion, no lag)
    return FindPathBFS(start, goal);
} else {
    // A* for small airports (faster for simple cases)
    return FindPathAStar(start, goal);
}
```

### 12.2 Deadlock Detection

**Add to ground pathfinder:**
```cpp
// After pathfinding attempt
if (!path_found && v->stuck_counter > 50) {
    // Been stuck for 50 ticks, deadlock likely
    Debug(misc, 1, "Aircraft {} deadlocked, teleporting to hangar", v->index);
    TeleportToHangar(v);
    v->stuck_counter = 0;
}
```

### 12.3 Runway Type Flags

**Extend ModularAirportTileData:**
```cpp
struct ModularAirportTileData {
    // ... existing fields ...
    uint8_t runway_flags;  // NEW: 0x01=takeoff, 0x02=landing, 0x03=both
    uint8_t min_runway_length;  // NEW: for physics validation
};
```

### 12.4 One-Way Taxiway Support

**Already have infrastructure:**
```cpp
// Current: user_taxi_dir_mask allows restricting directions
// Just need UI to set this as "one-way" mode
// Example: taxiway allowing only East direction
tile_data.user_taxi_dir_mask = 0x02;  // East only
tile_data.one_way_taxi = true;  // Display arrow overlay
```

---

## Summary: Top 10 Actionable Improvements

Based on community research, here are the highest-value improvements:

1. **🆕 Drag-building for runways/taxiways** - Biggest UI complaint, easy to add
2. **🆕 One-way taxiways** - Most requested feature, prevents head-on conflicts
3. **🆕 Validation warnings** - "No runway" errors help new players
4. **🆕 Save/load airport templates** - Universal request, enables sharing
5. **🆕 Hangar overflow queue** - Solves deadlock when all gates full
6. **🆕 Pre-built template library** - Teaching tool, reduces learning curve
7. **🆕 Helper overlay graphics** - Show connections during building
8. **🆕 Runway type flags** - Takeoff/landing/both classification
9. **🆕 BFS pathfinding option** - Fixes lag on large airports
10. **🔄 YAPF migration** - Long-term: leverage existing sophisticated pathfinder

---

## Conclusion

The OpenTTD community has spent 15+ years discussing modular airports (2005-2020+). Key insights:

- **Simple piece-based systems work** - Complex state machines fail
- **Deadlock prevention is critical** - Multiple strategies needed
- **Templates are essential** - Players need examples to learn
- **Realism vs playability must be balanced** - Difficulty settings help
- **Validation feedback is crucial** - Show errors, don't just fail silently
- **Incremental implementation wins** - Don't rewrite everything at once

Our current implementation already follows many best practices. The improvements above would bring it closer to the community's 15-year vision while avoiding the pitfalls that stalled previous attempts.
