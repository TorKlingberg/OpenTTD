# Modular Airport Implementation Cleanup Plan (Revised)

This document outlines the planned refinements for the modular airport system based on further architectural review and Codex feedback.

## 1. Memory Safety: `TaxiPath` Refactoring
- **Goal:** Replace raw pointers with `std::unique_ptr` for better ownership management.
- **Action:** 
    - Change `TaxiPath *taxi_path` and `TaxiPath *landing_chain_path` to `std::unique_ptr<TaxiPath>` in `src/aircraft.h`.
    - **Comprehensive Audit:** Review ALL assignments to these pointers in `src/modular_airport_cmd.cpp`, including:
        - Path rebuilding in `AirportMoveModular`.
        - Swapping in `AirportMoveModularLanding`.
        - Clearing in `ClearTaxiPathState`, `TeleportAircraftOnModularTile`, and the `Aircraft` destructor.
    - Use `std::move()` for all ownership transfers/swaps.
    - Remove all manual `delete` calls.
- **Benefit:** Cleaner lifecycle management and automatic cleanup.

## 2. UX: Large Aircraft Safety Status
- **Goal:** Provide immediate and actionable feedback on airport infrastructure.
- **Action:**
    - **Station View Window:** Add a dedicated status section in the **Ratings** pane (below throughput info).
    - **Condition:** Show only if airport is modular AND year >= 1955 AND `ModularAirportSupportsLargeAircraft` is false.
    - **Refined Logic:** Implement a new helper `ModularAirportHasSafeRunwayFor(const Station *st, bool landing)` that checks for:
        - A runway end with the appropriate `RUF_LANDING` or `RUF_TAKEOFF` flag.
        - That the contiguous runway is "Safe for Large" (length >= 6 and large-family tiles).
    - **Feedback Details (Red Header + Bullet Points):**
        - `{RED}Unsafe for large aircraft`
        - `Missing Control Tower` (if no `APT_TOWER` / `APT_TOWER_FENCE_SW`)
        - `Missing Large Terminal` (if no `IsBigTerminalPiece`)
        - `No 6-tile Landing Runway` (if `!ModularAirportHasSafeRunwayFor(st, true)`)
        - `No 6-tile Takeoff Runway` (if `!ModularAirportHasSafeRunwayFor(st, false)`)
- **Benefit:** Reduces player frustration and guides infrastructure expansion with precise terminology.

## 3. Implementation Strategy
- **Turn 1:** Refactor `TaxiPath` ownership and perform the comprehensive audit.
- **Turn 2:** Implement the new runway safety helpers in `src/modular_airport_cmd.cpp`.
- **Turn 3:** Implement UI status lines in `src/station_gui.cpp`.
- **Turn 4:** Final verification and cleanup.
