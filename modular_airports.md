# Modular Airports - Investigation and Plan (draft)

Goal: let players build airports tile-by-tile (runways, taxiways, terminals, hangars, helipads) instead of choosing a fixed AirportSpec layout.

## Current Airport Architecture (what exists today)

- Airports are fixed-size layouts defined in `src/table/airport_defaults.h` using `AirportTileLayout` and `AirportTileTable` (tile gfx only).
- Aircraft movement on the ground is not tile-based. It is a hard-coded finite state machine (FTA) defined by:
  - Movement points in `src/table/airport_movement.h` (`AirportMovingData` with x/y coords, flags, direction).
  - A per-airport state graph in `AirportFTAbuildup` arrays, assembled into `AirportFTAClass` in `src/airport.cpp`.
- The FTA is indexed by a position number (0..nofelements-1) with optional branches for different headings.
- Block reservation uses a 64-bit `AirportBlocks` bitmask stored per station (`Station::airport.blocks` in `src/station_base.h`), not per tile.
- Terminals/helipads are managed by terminal groups derived from the FTA tables, limited by constants:
  - `MAX_TERMINALS = 8`, `MAX_HELIPADS = 3`, `MAX_ELEMENTS = 255` in `src/airport.h`.
- Build flow is strictly: choose airport type/layout -> `CmdBuildAirport` -> place pre-defined tiles and set airport type/layout (`src/station_cmd.cpp`, `src/airport_gui.cpp`).
- Airport tile graphics are identified by `StationGfx` and `AirportTileSpec`, but tiles do not encode functional meaning (runway/taxiway/etc). Only the FTA defines behavior.

Implication: a modular airport cannot just place tiles and use the existing FTA; we need a new movement model or a way to generate an FTA from a custom tile layout.

## Core Design Options

### Option A: Generate an FTA per custom airport
- Build a graph of movement points based on the player-placed tiles and their connectivity rules.
- Compile that graph into a per-airport `AirportFTAClass` at build time (or on demand), preserving existing aircraft movement code.
- Pros: reuse most aircraft movement state machine logic and block handling.
- Cons: complex to generate; MAX_ELEMENTS (255) and 64-bit blocks are tight; FTA expects specific headings like TAKEOFF/LANDING/TERM1.

### Option B: Replace ground movement with tile pathfinding
- Treat taxiways/runways like a small road/rail network and use a pathfinder (NPF/YAPF style) for ground routing.
- Keep existing airborne states (approach, landing, takeoff) but switch to pathfinding once on the ground.
- Pros: natural for modular layouts, less reliance on fixed FTA elements.
- Cons: larger code change; must integrate with reservation/anti-collision; need new state transitions.

### Option C: Hybrid (recommended for early steps)
- Keep FTA for airborne approach/landing/takeoff states.
- Replace only ground taxiing with a tile-graph pathfinder (low-speed, reserved edges).
- Keep terminal allocation logic but drive movement via tile graph instead of FTA sequences.

## Design Choices We Need to Make (explicit decisions)

1. **Ground routing model**
   - FTA generation vs tile pathfinding vs hybrid.
   - If FTA generation: how to cap node count and compile directional transitions.
   - If pathfinding: choose reservation granularity (per tile? per edge? per block group?).

2. **Tile metadata for function and connectivity**
   - How to mark a placed tile as runway/taxiway/terminal/hangar/helipad/stand.
   - Whether to allow per-tile directional arrows (player-configured taxi directions).
   - Data storage: embed in tile bits (station tile extra data) or per-airport data structure.

3. **Runway logic**
   - Define runway start/end, takeoff direction(s), and landing direction(s).
   - Decide whether to allow one-way runways or bidirectional.
   - Tie runway endpoints to approach/entry points (currently 4 entry points from `AirportFTAClass`).

4. **Terminal allocation model**
   - Keep max terminals (8) or expand.
   - How to map terminals/stands in the layout to terminal indices or groups.

5. **Blocking / reservation system**
   - Keep current 64-bit `AirportBlocks` or move to dynamic reservation sets.
   - Determine collision model: block segments, tiles, or edge reservations.

6. **User interface**
   - New build UI for assembling modular airports (palette of pieces + orientation).
   - A mode for editing taxi directions (optional but expected by your requirement).
   - Validation feedback (no runway, no terminal, unreachable taxi paths).

7. **Save/load and compatibility**
   - Save custom layout and metadata per airport (new savegame chunk).
   - Backward compatibility: existing airports remain as static AirportSpec.
   - NewGRF airports: leave as-is or allow them to use modular pieces.

8. **Limits and performance**
   - Decide max airport footprint or node count for custom airports.
   - Decide if ground pathfinding is precomputed or computed per aircraft.

## Suggested Implementation Direction (initial recommendation)

Start with **Option C (Hybrid)**:
- Keep existing airborne logic and approach/entry points.
- Build a tile graph for ground movement and reserve tiles/edges for taxiing.
- This allows early playable modular airports without rewriting the entire flight FSM.

## Plan (phased)

Phase 0 - Research/constraints (done)
- Understand current FTA, airport specs, build flow, and limits.

Phase 1 - Data model and savegame
- Define a new `ModularAirport` data structure owned by `Station::airport`.
- Store: tile list, tile type, allowed taxi directions, runways, terminals, hangars, helipads, entry points.
- Add save/load chunk for modular airports; migration path for existing fixed airports (none, keep as legacy).

Phase 2 - Construction + validation
- Add a new build mode in `airport_gui.cpp` to place modular tiles.
- Reuse existing `MakeAirport` tiles but assign tile roles in the new data model.
- Validate: at least one runway, one terminal/helipad, and a connected taxi path between them.

Phase 3 - Ground routing MVP
- Implement a taxiway graph + simple pathfinder for ground movement.
- Add reservation (tile or edge) to avoid collisions; integrate with existing `AirportBlocks` or new reservation map.
- Bind aircraft ground states to path steps; keep FTA for takeoff/landing/airborne.

Phase 4 - Refinement and features
- Directional taxi arrows per tile in the UI (user-specified constraints).
- Support multiple runways, hold-short points, and runway queueing.
- Expand terminal grouping and capacity rules.

Phase 5 - NewGRF + balancing
- Decide how custom tiles map to NewGRF graphics.
- Add cost/maintenance/noise scaling based on built tiles.

## Known Constraints to Track

- `MAX_ELEMENTS` (255) and `AirportBlocks` 64-bit mask if we try to synthesize FTAs.
- `MAX_TERMINALS` 8 and `MAX_HELIPADS` 3 if keeping the current terminal allocation API.
- Many systems assume a fixed `AirportSpec` with size and layout; modular airports will need a parallel path.

