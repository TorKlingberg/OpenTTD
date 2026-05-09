# Unit Test Plan: Modular Airports

This plan outlines the unit tests for the modular airport system in OpenTTD. The goal is to verify the core logic, pathfinding, and reservation systems independently of the full game simulation where possible.

## 1. Helper Function Tests (Pure Logic)
These tests verify stateless utility functions in `src/modular_airport_cmd.h`.

### Piece Classification
- **Goal:** Ensure all modular airport tiles are correctly categorized.
- **Functions:** `IsModularRunwayPiece`, `IsLegacySmallRunwayPiece`, `IsLegacySmallHangarPiece`, `IsModularBuildingPiece`, `IsTaxiwayPiece`, `IsApronOrTaxiwayPiece`, `IsModularHangarPiece`.
- **Test Cases:**
    - Positive and negative tests for each category using `APT_*` constants.
    - Edge cases like NewGRF offsets.

### Rotation Logic
- **Goal:** Verify that pieces correctly swap their graphics/types when rotated.
- **Functions:** `SwapBuildingPieceForRotation`.
- **Test Cases:**
    - Rotations 0-3 for directional hangars (SE, NE, NW, SW).
    - Rotations for `APT_BUILDING_1/2`.
    - Rotations for legacy small runway ends (`NEAR`/`FAR`).

### Runway & Aircraft Metadata
- **Goal:** Verify logic for large aircraft support and runway families.
- **Functions:** `IsLargeRunwayFamily`, `GetCanonicalRunwaySegmentPiece`, `IsModernModularPiece`, `GetModularPieceMinYear`.
- **Test Cases:**
    - Correct piece selection for large vs small runways.
    - Year gating for modern pieces.

## 2. Map-Dependent Logic Tests
These tests require `Map::Allocate` and a basic `Station` setup.

### Runway Analysis
- **Goal:** Verify contiguous runway detection and properties.
- **Functions:** `GetContiguousModularRunwayTiles`, `GetRunwayOtherEnd`, `IsRunwayPieceOnAxis`.
- **Test Cases:**
    - Horizontal vs Vertical runways.
    - Single-tile runways (too short).
    - Multi-tile runways with various pieces.
    - Runway ends and exit points.

### Ground Pathfinding (A*)
- **Goal:** Verify that aircraft find valid, cost-optimal paths.
- **Functions:** `FindAirportGroundPath`.
- **Test Cases:**
    - **Topology:** Simple straight taxiway from Hangar to Stand.
    - **One-way:** Respecting one-way taxiway flags.
    - **Avoidance:** Routing around occupied stands (by mocking an aircraft at a stand).
    - **Penalties:** Ensuring the pathfinder prefers taxiways over cutting through stands if the cost difference is small.
    - **Connectivity:** Correct behavior when no path exists.

### Segment Classification
- **Goal:** Verify that paths are correctly decomposed into reservation segments.
- **Functions:** `BuildTaxiPath`, `FindTaxiSegmentIndex`.
- **Test Cases:**
    - Decomposing a path into `FREE_MOVE`, `ONE_WAY`, and `RUNWAY` segments.
    - Verifying segment start/end indices.

## 3. Reservation System
These tests verify the locking mechanisms for airport tiles.

### Tile Reservation
- **Goal:** Ensure tile ownership is correctly tracked and cleared.
- **Functions:** `SetModularAirportTileReservationOwner`, `GetModularAirportTileReservationOwner`, `ClearModularAirportTileReservation`, `HasModularAirportTileReservation`.
- **Test Cases:**
    - Set/Get/Clear for various tiles and `VehicleID`s.
    - `IsModularAirportTileReservedBy` checks.

### Runway Reservations
- **Goal:** Verify atomic reservation of entire runways.
- **Functions:** `TryReserveContiguousModularRunway`.
- **Test Cases:**
    - Reserving a multi-tile runway.
    - Failure when any tile is already reserved.
    - Reconciling reservations.

## 4. Holding Loop & Movement
- **Goal:** Verify movement helper logic.
- **Functions:** `DirectionsWithin45`, `IsHoldingGateActive`, `GetNearestModularHoldingWaypoint`.
- **Test Cases:**
    - Angle comparisons for 45-degree tolerance.
    - Gate activation logic (wrapping around the loop).

## Implementation Strategy
- Create a new test file: `src/tests/test_modular_airport.cpp`.
- Use `Map::Allocate(64, 64)` for map-based tests.
- Mock `Station` and `Aircraft` objects as needed.
- Focus on `REQUIRE` and `CHECK` macros from Catch2.
