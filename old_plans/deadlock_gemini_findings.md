# Modular Airport Aircraft Deadlock Investigation Findings

## Summary
The investigation into the stuck aircraft at Fondworth Falls Airport has identified a systematic failure in the modular airport reservation system, primarily involving "ghost" reservations that persist after an aircraft is deleted, sold, or performs a save/load cycle.

## Root Causes

### 1. Reservation Leakage on Deletion
Modular airport reservations are stored in tile metadata. When an aircraft is deleted (user sale, company bankruptcy, or crash), these reservations are not cleared.
- **Affected Functions:** `Vehicle::PreDestructor` and `Aircraft::Crash`.
- **Result:** Tiles remain permanently reserved by a non-existent `VehicleID`, blocking all future traffic.

### 2. Save/Load State Loss
Critical modular aircraft fields are not included in the savegame format.
- **Lost Data:** `modular_runway_reservation`, `taxi_reserved_tiles`, `modular_ground_target`, `modular_landing_tile`, `modular_takeoff_tile`, etc.
- **Result:** On load, aircraft "forget" which tiles they own. Reservation cleanup functions (which iterate over these internal vectors) fail to clear the map metadata, creating "untracked" ghost reservations.

### 3. Brittle Cleanup Logic
`ClearModularRunwayReservation` and `ClearTaxiPathReservation` only clear tiles tracked in the aircraft's internal vectors.
- **Failure Mode:** If an aircraft holds a reservation that isn't in its vector (due to a save/load or a logic error), it can never release that tile. V244 in the logs was identified with `owned_rw_not_tracked=1`, confirming this state.

### 4. Logic/Physical Location Mismatch
`AirportGoToNextPosition` uses `v->targetairport` to decide whether to apply modular or traditional FTA logic.
- **Problem:** If an aircraft at a modular airport finishes loading and its next destination is a traditional airport, it immediately switches to FTA logic while still physically on the modular airport ground.
- **Result:** Modular-specific safety gates and cleanup are bypassed, leading to deadlocks or leaked reservations.

### 5. Improper Helipad Filtering
`FindFreeModularHelipad` does not restrict its search to helicopters.
- **Result:** Airplanes can be assigned helipads as ground goals, leading to unexpected behavior in movement and landing logic.

## Evidence from Incident (V244 & V277)
- V277 was stuck waiting for runway tile 57419.
- Tile 57419 was held by V244.
- V244 stopped logging at `10:36:30` after being granted a reservation with an untracked tile.
- V244 was likely sold or deleted, but its reservation persisted, creating the "ghost" blocker for V277.

## Action Plan
1. **Fix Deletion Cleanup:** Ensure `PreDestructor` clears all modular reservations.
2. **Robust Cleanup:** Update cleanup functions to clear map reservations based on `VehicleID` rather than just internal vectors.
3. **Correct Logic Gating:** Base modular logic usage on the aircraft's physical tile location.
4. **Fix Save/Load:** Add modular state fields to the savegame format.
5. **Add Type Filtering:** Ensure airplanes cannot target helipads.
