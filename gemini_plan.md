# Modular Airports for OpenTTD: Investigation and Plan

## 1. Project Goal

To transition OpenTTD from a system of pre-defined, monolithic airports to a modular system where players can construct custom airports piece by piece (e.g., runways, taxiways, terminals, hangars).

## 2. Investigation Summary

A deep-dive into the source code (`airport.cpp`, `aircraft_cmd.cpp`, `station_cmd.cpp`) confirms the initial analysis. The current system is entirely dependent on rigid, pre-defined structures:

-   **Static Finite State Machines (FSMs):** All ground movement is dictated by a hardcoded FSM (`AirportFTAClass`) for each airport type. Aircraft do not pathfind; they follow this script.
-   **No Dynamic Construction:** The `CmdBuildAirport` command simply creates a `Station` and assigns an airport type. The FSM and layout are looked up from global, static tables.
-   **Existing Pathfinder:** A critical discovery is the existence of **YAPF** (Yet Another PathFinder), a generic, template-based A* pathfinder used for trains, road vehicles, and ships. This is a powerful, existing tool that can be adapted for aircraft ground movement, significantly reducing the complexity of the project.

This investigation concludes that the proposed modular system is feasible by replacing the static FSM with a dynamic graph and adapting the existing YAPF pathfinder.

## 3. Core Architectural Decisions

Based on the investigation, the following architectural choices are made:

*   **Airport Structure Representation: Tile-based Graph**
    *   A new data structure, likely a class named `ModularAirport`, will be added to the main `Station` object.
    *   This structure will contain a graph representing the airport layout. Nodes will be `TileIndex` locations, and edges will represent valid taxiing connections between them. This provides maximum flexibility for user creations.

*   **Aircraft Pathfinding: Adapt YAPF**
    *   The existing **YAPF** A* pathfinder will be leveraged.
    *   A new pathfinder class, `AircraftPathfinder`, will be created, specializing the YAPF template for aircraft ground movement.
    *   This will involve defining the node type (`TileIndex`), cost functions (for distance, turns, etc.), and rules for traversing the airport graph.

*   **Collision Detection: Tile Reservation**
    *   The 64-block bitmask system will be replaced with a dynamic, tile-based reservation system.
    *   Before an aircraft moves to the next tile in its calculated path, it will reserve that tile.
    *   This is analogous to the system used for train signals and blocks, a well-understood and scalable concept within the OpenTTD codebase. The pathfinder will need to be aware of reserved tiles to route aircraft around occupied areas.

## 4. Proposed Implementation Plan

This is a high-level, phased approach. Details will be refined as work progresses.

*   **Phase 1: Core Infrastructure**
    *   **Goal:** Establish the foundational data structures for modular airports.
    *   **Tasks:**
        1.  Define a new `ModularAirport` class to hold the airport graph (e.g., using `std::map<TileIndex, std::vector<TileIndex>>` for an adjacency list).
        2.  Add an instance of `ModularAirport` to the `Station` struct (e.g., `std::unique_ptr<ModularAirport> modular_airport`). This will be null for old-style airports.
        3.  Implement save/load hooks for this new data structure. A station with a non-null `modular_airport` will be saved with its graph data.
        4.  Introduce a new `StationType` or flag to distinguish modular airports from classic ones.

*   **Phase 2: Basic Construction & Pathfinding Proof of Concept**
    *   **Goal:** Build a single piece of taxiway and prove that the pathfinder can route an aircraft on it.
    *   **Tasks:**
        1.  Create a new `CmdBuildTaxiway` command.
        2.  When executed, this command will add the specified tile(s) to the `ModularAirport` graph in the corresponding `Station`. It will also need to handle joining with adjacent taxiway tiles.
        3.  Create a new tile type for "taxiway".
        4.  Implement a minimal `AircraftPathfinder` by specializing YAPF. It only needs to understand how to traverse the simple taxiway graph.
        5.  Create a test scenario (e.g., via a cheat or debug command) to call the pathfinder and verify it can find a path between two points on the newly built taxiway.

*   **Phase 3: Complete Airport Loop**
    *   **Goal:** Allow players to build a simple, functional airport with a full operational loop (landing, taxiing to gate, taxiing to runway, takeoff).
    *   **Tasks:**
        1.  Introduce `Runway` and `TerminalGate` as new buildable components/tiles. Add them to the airport graph with special properties.
        2.  Integrate the `AircraftPathfinder` with the `Aircraft` state logic. When an aircraft lands at a modular airport, it will now request a path to a free gate from the pathfinder.
        3.  Implement the tile reservation system. An aircraft following a path must reserve the next tile before moving.
        4.  Modify `aircraft_cmd.cpp` to replace the FSM-following logic with path-following logic when on a modular airport.

*   **Phase 4: UI and Advanced Components**
    *   **Goal:** Create the UI for building modular airports and add more complex components.
    *   **Tasks:**
        1.  Design and implement a new Airport Construction window (`airport_gui.cpp`) with buttons for each modular piece (Taxiway, Runway, Terminal, Hangar).
        2.  Provide visual feedback during construction (e.g., showing connections).
        3.  Implement `Hangar` and `HoldingPoint` components. A hangar will function as a depot.
        4.  Handle different taxiway pieces (straights, curves, intersections) and user-configurable one-way directions.

*   **Phase 5: Integration & Polish**
    *   **Goal:** Fully integrate the system with the wider game.
    *   **Tasks:**
        1.  Integrate with the game economy (construction and maintenance costs for each piece).
        2.  Integrate with town ratings (noise calculations based on where runways and terminals are placed).
        3.  Ensure Game Scripts and the AI can build and use modular airports.
        4.  Ensure graceful fallback for old savegames and NewGRF airports (they will continue to function as monolithic FSM-based airports).
