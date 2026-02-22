# File Split: Modular Airport Code Extraction

## Context

`aircraft_cmd.cpp` has grown to 5,554 lines; roughly 2,500 of those are modular airport logic
mixed in with the classic FTA state machine (~1,500 lines) and shared mechanics (~1,500 lines).
`airport_gui.cpp` (1,514 lines) similarly mixes the classic airport picker with the tile-by-tile
modular builder. Both files are split into dedicated companion files.

---

## Split 1: `aircraft_cmd.cpp` → `modular_airport_cmd.{h,cpp}`

### What moves to `modular_airport_cmd.cpp`

All `static` functions whose names contain "Modular", "modular", "Taxi", "Runway", "Helipad",
"Hangar", "Landing" (modular), "Takeoff" (modular), or "Holding" — approximately 2,500 lines.

Full list (remove `static` from all definitions):
- Runway classification: `IsModularRunwayPiece`, `IsRunwayPieceOnAxis`, `IsModularHelipadPiece`, `IsRunwayEndLow`, `GetRunwayFlags`, `GetRunwayOtherEnd`, `GetContiguousModularRunwayTiles`
- Reservation core: `ClearModularRunwayReservation`, `ClearModularAirportReservationsByVehicle`, `TryClearStaleModularReservation`, `ShouldLogModularRateLimited`, `IsModularTileOccupiedByOtherAircraft`, `TryReserveContiguousModularRunway`, `IsContiguousModularRunwayReservedByOther`, `IsContiguousModularRunwayBusyByOther`, `IsContiguousModularRunwayReservedInStateByOther`, `IsContiguousModularRunwayQueuedForTakeoffByOther`
- Taxi: `ClearTaxiPathReservation`, `ClearTaxiPathState`, `SetTaxiReservation`, `IsTaxiTileReservedByOther`, `FindTaxiSegmentIndex`, `TryReserveTaxiSegment`
- Landing chain: `FindModularLandingGroundGoal`, `TryReserveLandingChain`, `FindModularLandingTarget`, `GetModularLandingApproachPoint`
- Holding loop: `NormalizeAngle2Pi`, `DirToVec`, `AddWaypoint`, `AppendFallbackRectLoopWaypoints`, `ComputeDubins`, `SampleDubinsPath`, `GatherAndSortGates`, `GetModularHoldingLoop`, `ComputeModularHoldingLoop`, `GetNearestModularHoldingWaypoint`, `GetModularHoldingWaypointTarget`, `IsHoldingGateActive`, `DirectionsWithin45`, `GetRunwayApproachDirection`
- Runway selection: `FindModularRunwayRolloutPoint`, `FindNearestModularRunwayExitTile`, `FindModularRolloutHoldingTile`, `FindModularRunwayTileForTakeoff`, `FindModularTakeoffQueueTile`
- Parking: `IsModularHangarPiece`, `IsModularHangarTile`, `FindFreeModularTerminal`, `FindFreeModularHelipad`, `FindFreeModularHangar`
- Ground movement: `CanUseModularGroundRouting`, `TryRetargetModularGroundGoal`, `HandleModularGroundArrival`, `LogModularVehicleReservationState`, `LogModularTakeoffRunwayUnavailable`, `AirportMoveModular`
- Movement phases: `AirportMoveModularLanding`, `AirportMoveModularHeliTakeoff`, `AirportMoveModularTakeoff`, `AirportMoveModularFlying`
- Public API (already non-static): `TeleportAircraftOnModularTile`

### What stays in `aircraft_cmd.cpp`

- FTA statics: `AirportSetBlocks`, `AirportHasBlock`, `AirportFindFreeTerminal`, `AirportFindFreeHelipad`, `AirportMove`, `AirportClearBlock`
- All `AircraftEventHandler_*` functions (mixed classic/modular dispatch — they call into modular via the new header)
- `AirportGoToNextPosition` (dispatcher)
- Shared mechanics: `UpdateAircraftSpeed`, `AircraftEntersTerminal`, `CrashAirplane`, `MaybeCrashAirplane`, `HelicopterTickHandler`, `AircraftController`
- Public API: `AircraftLeaveHangar`, `SetAircraftPosition`, `UpdateAircraftCache`, etc.

### Shared helpers that must become non-static

Two static functions in `aircraft_cmd.cpp` are called by modular movement code. Remove `static`
from their definitions so `modular_airport_cmd.cpp` can call them via the header:
- `UpdateAircraftSpeed` — called by all four `AirportMoveModular*` functions
- `AircraftEntersTerminal` — called by `HandleModularGroundArrival`

### `modular_airport_cmd.h` structure

```cpp
#include "aircraft.h"
#include "station_base.h"
#include "airport_ground_pathfinder.h"
#include "airport.h"

// Shared helpers defined in aircraft_cmd.cpp, called by modular code
int  UpdateAircraftSpeed(Aircraft *v, uint speed_limit = UINT_MAX);
void AircraftEntersTerminal(Aircraft *v);

// Public modular API (was already non-static, declared in aircraft.h — keep there)
// bool TeleportAircraftOnModularTile(...);  ← already in aircraft.h, no change needed

// All modular functions (one declaration per function in the move list above)
bool IsModularRunwayPiece(uint8_t piece_type);
// ... (full list at implementation time, matching existing signatures)
```

### Changes to `aircraft_cmd.cpp`

1. Add `#include "modular_airport_cmd.h"` (after existing includes)
2. Remove the ~40 modular forward declarations at lines ~91–131
3. Remove `static` from `UpdateAircraftSpeed` and `AircraftEntersTerminal`
4. Delete all moved function bodies

### CMakeLists.txt addition (after `aircraft_cmd.cpp` entry)

```cmake
modular_airport_cmd.cpp
modular_airport_cmd.h
```

---

## Split 2: `airport_gui.cpp` → `modular_airport_gui.{h,cpp}`

### What moves to `modular_airport_gui.cpp`

- Structs: `ModularAirportPiece`, `CosmeticPiece`
- Static arrays: `_cosmetic_pieces[]`, `_modular_airport_pieces[]`
- Helpers: `GetModularAirportPieceGfx`, `IsModularTaxiwayGfx`
- Globals: `_last_modular_airport_station`, `_modular_hangar_rotation`, `_modular_cosmetic_piece`, `_show_runway_direction_overlay`
- Window classes: `BuildModularAirportWindow`, `BuildModularHangarPickerWindow`, `BuildModularCosmeticPickerWindow`
- Widget descriptors and `WindowDesc`s for all three windows
- Show functions (remove `static`): `ShowBuildModularAirportWindow`, `ShowModularHangarPicker`, `ShowModularCosmeticPicker`

The includes for `modular_airport_gui.cpp` are largely the same as `airport_gui.cpp` (copy the
full include block, then trim any that turn out to be unused).

### What stays in `airport_gui.cpp`

- Globals: `_selected_airport_class`, `_selected_airport_index`, `_selected_airport_layout`
- `BuildAirToolbarWindow` (shared toolbar)
- `BuildAirportWindow` (classic FTA picker) and its widget/desc
- `ShowBuildAirportPicker`, `ShowBuildAirToolbar`
- `PlaceAirport`, `CcBuildAirport`
- `InitializeAirportGui`

### `modular_airport_gui.h` structure

```cpp
struct Window;
void ShowBuildModularAirportWindow(Window *parent);

// Globals also read by airport_gui.cpp or viewport code:
extern bool _show_runway_direction_overlay;
```

Only export what other translation units actually read. Check at implementation time whether
`_last_modular_airport_station` etc. are read outside the modular window — if not, keep them
static in `modular_airport_gui.cpp`.

### Changes to `airport_gui.cpp`

1. Add `#include "modular_airport_gui.h"`
2. Remove the `static void ShowBuildModularAirportWindow(Window *parent)` forward declaration
3. Delete all moved code and globals
4. Verify `InitializeAirportGui` doesn't touch modular state; if it does, extract a
   `InitializeModularAirportGui()` called from there

### CMakeLists.txt addition (after `airport_gui.cpp` entry)

```cmake
modular_airport_gui.cpp
modular_airport_gui.h
```

---

## Critical Files

| File | Action |
|------|--------|
| `src/aircraft_cmd.cpp` | Remove ~2500 lines, make 2 helpers non-static, add include |
| `src/modular_airport_cmd.cpp` | **New** — all modular movement/reservation/pathfinding |
| `src/modular_airport_cmd.h` | **New** — declarations for cross-file calls |
| `src/airport_gui.cpp` | Remove ~950 lines of modular UI, add include |
| `src/modular_airport_gui.cpp` | **New** — all modular builder UI |
| `src/modular_airport_gui.h` | **New** — `ShowBuildModularAirportWindow` + extern globals |
| `src/CMakeLists.txt` | Add 2 new .cpp/.h pairs |

No changes needed to: `aircraft.h`, `airport.h`, `station_base.h`, `airport_ground_pathfinder.*`,
`station_cmd.cpp`, `saveload/`.

---

## Verification

```bash
# Build (pure refactor — zero behaviour change expected)
/Users/tor/ttd/OpenTTD/scripts/build_and_sign.sh

# Smoke test
/Users/tor/ttd/OpenTTD/build/openttd -g ~/Documents/OpenTTD/save/SAVENAME.sav -d misc=3 2>/tmp/openttd.log
grep '\[ModAp\]' /tmp/openttd.log | tail -50
```

Expected: zero compile errors/warnings, identical runtime behaviour.
