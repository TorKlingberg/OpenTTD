# Modular Airports - Current Implementation Guide

This document describes the **implemented** modular-airport system in this branch.

Companion documents:

- `skills/reservations-design.md` — the authoritative detail on segments, safe stops, and the reservation lifecycle. This file summarises; that one decides.
- `CLAUDE.md` — build/test commands, day-to-day invariants, and common pitfalls.
- `coords.md` — the isometric coordinate system every axis-dependent routine here depends on.

## Scope

Modular airports are a first-class airport mode built on station airport tiles plus per-tile modular metadata.

- Players place airport pieces tile-by-tile, or convert a stock airport, or place a saved template.
- Ground movement uses modular routing and reservation logic, not the classic FTA.
- The classic airport FTA is still present and unchanged for non-modular airports.
- Saved templates (JSON) can be created, previewed, rotated, and placed atomically.
- The NoAI/NoGO script API can query and build modular layouts.

## Key Files

Grouped by role. Modular code was split out of `station_cmd.cpp`; the split is the most common source of stale references.

### Core logic

| File | Purpose |
|------|---------|
| `src/modular_airport_cmd.cpp` | Movement, reservation, landing/takeoff selection, ground goals, derived layout properties (noise, catchment, maintenance, safety). |
| `src/modular_airport_cmd.h` | Declarations plus inline predicates (`IsModularRunwayPiece`, `IsTaxiwayPiece`, `IsLargeRunwayFamily`, `SwapBuildingPieceForRotation`, `MGT_*`, `MASR_*`). |
| `src/modular_airport_holding.cpp` | Dubins-curve holding loop, approach geometry, computed helicopter landing/takeoff/service tiles. |
| `src/airport_ground_pathfinder.cpp` / `.h` | A* ground pathfinder, segment classification, crossing-required path cache. |
| `src/airport_pathfinder.cpp` / `.h` | `CalculateAutoTaxiDirectionsForGfx` — per-piece connectivity. |
| `src/aircraft_cmd.cpp` | Classic FTA state machine, shared mechanics, and the modular hooks (`MaybeCrashModularAircraft`, landing commit, helicopter handling). |

### Build and edit commands

| File | Purpose |
|------|---------|
| `src/modular_airport_build.cpp` | `CmdBuildModularAirportTile`, `CmdBuildModularAirportFromStock`, `CmdUpgradeModularAirportTile`, `RemoveModularAirportTile`, noise accounting. |
| `src/modular_airport_template_cmd.cpp` | `CmdSetRunwayFlags`, `CmdSetTaxiwayFlags`, `CmdSetModularAirportEdgeFence`, `CmdPlaceModularAirportTemplate`. |
| `src/station_cmd.h` | Command declarations and `DEF_CMD_TRAIT` registrations; `ModularTemplatePlacementData`. |

### Drawing and UI

| File | Purpose |
|------|---------|
| `src/modular_airport_draw.cpp` / `.h` | Sprite layout overrides, hangar layouts, perimeter fences, direction overlays. |
| `src/modular_airport_gui.cpp` / `.h` | Builder window, sub-pickers, tools, overlays, compound-piece definitions. |
| `src/airport_template_gui.cpp` | Template manager and isometric preview. |
| `src/airport_gui.cpp` | Shared airport toolbar and classic FTA picker. |

### Data, persistence, scripting

| File | Purpose |
|------|---------|
| `src/base_station_base.h` | `ModularAirportTileData`, `RUF_*`. |
| `src/station_base.h` | `Airport`'s modular vector, index, and the layout-derived caches + `MarkLayoutDirty`. |
| `src/airport.h` | `AT_MODULAR`, `AirportBlock::Modular`, `ModularHoldingLoop`, holding constants. |
| `src/aircraft.h` | Per-aircraft modular runtime state. |
| `src/airport_template.cpp` / `.h` | Template JSON load/save, `AirportTemplate`, `AirportTemplateManager`. |
| `src/saveload/station_sl.cpp` | `SlModularAirportTileData`. |
| `src/saveload/vehicle_sl.cpp` | Aircraft modular fields. |
| `src/saveload/airport_sl.cpp` | `MACP` chunk (crossing-required path cache). |
| `src/saveload/extended_version_sl.h` / `.cpp` | Fork feature versioning: `XVER` chunk, `SlxFeature::ModularAirport`, `IsModularAirportSaveFeaturePresent`. |
| `src/script/api/script_airport.cpp` / `.hpp` | Script API surface. |
| `src/tests/test_modular_airport.cpp` | Unit tests. |

## Airport Type and Capability

A modular airport is a real airport type, not a decorated stock one.

- `AT_MODULAR = 127` (`src/airport.h`), reserved from NewGRF allocation; `static_assert(AT_MODULAR == NUM_AIRPORTS - 1)`.
- `AirportBlock::Modular` (61) is set in `Airport::blocks` and is the runtime test everywhere: `st->airport.blocks.Test(AirportBlock::Modular)`.
- `AT_MODULAR` carries a generic FTA. What the airport can actually take is layout-derived and overrides the type-level answer:
  - `ModularAirportAcceptsPlanes(st)` / `ModularAirportAcceptsHelicopters(st)` — topological only (no occupancy, no reachability), because `CanVehicleUseStation` calls them.
  - `Airport::HasHangar` reads the layout for modular airports.
  - `GetModularAirportNewGRFType(st)` reports `ATP_TTDP_HELIPORT` for a layout that takes no planes but has a helipad, and otherwise `ATP_TTDP_LARGE` or `ATP_TTDP_SMALL`, to NewGRF.

### Large-aircraft safety

`GetModularAirportSafetyStatus(st)` returns a bitmask of **missing** requirements (`ModularAirportSafetyRequirement`):

| Flag | Requirement |
|------|-------------|
| `MASR_TOWER` | A control tower (`APT_TOWER` or `APT_TOWER_FENCE_SW`). |
| `MASR_BIG_TERMINAL` | A large terminal building (`IsBigTerminalPiece`). |
| `MASR_LANDING_RUNWAY` | A runway of ≥6 tiles, entirely large-runway family, with `RUF_LANDING`. |
| `MASR_TAKEOFF_RUNWAY` | The same, with `RUF_TAKEOFF`. |

`ModularAirportSupportsLargeAircraft(st)` is `GetModularAirportSafetyStatus(st) == MASR_NONE`. Landing and takeoff are checked **separately** — one runway may satisfy both, but a landing-only runway does not make the airport safe.

`GetModularAirportSafetyStatusFromPieces` is the abstract-grid twin, used to measure a template that has not been placed. The two must agree exactly: the elevated jet-overrun crash path is gated on this answer.

### Other derived properties

All layout-derived and cached behind `MarkLayoutDirty`:

- `GetModularAirportNoiseLevel` — derived from the operating surfaces in the layout.
- `GetModularAirportCatchmentRadius` — tiered by layout content; an airport failing the large-aircraft safety check is capped at `CATCH_MIN`.
- `GetModularAirportMaintenancePoints` — in eighths of a stock maintenance-cost point.

Each has a `...FromPieces` twin taking an abstract grid, so the same answer can be computed for a layout that is not on the map yet. The twins are used by the script API, by the template catchment preview, by the noise preflight during building, and by station naming (`ModularAirportAcceptsPlanesFromPieces` decides Airport vs Heliport).

## Data Model

### Per-tile modular metadata

`ModularAirportTileData` (`src/base_station_base.h`):

- `tile`
- `piece_type`
- `rotation` (`0..3`)
- `user_taxi_dir_mask`
- `one_way_taxi`
- `auto_taxi_dir_mask`
- `runway_flags` (`RUF_*`)
- `edge_block_mask`
- `reservation_owner` — the aircraft holding this tile, when the map reservation bit is set

This metadata is authoritative for modular logic; map tile gfx remains canonical airport gfx. Map `m6` bit 2 is only the reservation-*present* flag; ownership lives in `reservation_owner`, never in `m7` (which is animation frame storage).

### Per-airport modular state

In `Airport` (`src/station_base.h`):

- `modular_tile_data` — the tile vector (order is not stable; it is mutated by erase/push_back)
- `modular_tile_index` + `modular_tile_index_dirty` — tile → vector index

Everything else is a `mutable` lazily-computed cache with its own dirty bit:

- `modular_holding_loop`
- `modular_heli_landing_tile`, `modular_heli_takeoff_tile`, `modular_heli_service_tile`, `modular_hangar_reachable_pads`
- `modular_has_hangar`
- `modular_has_large_safe_landing_runway`, `modular_has_large_safe_takeoff_runway`
- `modular_catchment_cache`
- `modular_noise_cache`
- `modular_accepts_planes`, `modular_accepts_helicopters`

**`Airport::MarkLayoutDirty()` invalidates all of them, and every layout mutation is expected to go through it.** Any code that mutates `ModularAirportTileData` directly instead of going through the commands — tests especially — must call it, or a cached answer silently stays stale. Retyping a tile is a layout change: mark dirty *after* the retype, or a read in the window before normalization caches the wrong answer.

One place invalidates without it: the helicopter landing path in `aircraft_cmd.cpp` forces `modular_heli_tiles_dirty` when the cached landing tile turns out to no longer be in the layout. That is a safety net against a mutation that missed `MarkLayoutDirty()`, not a second invalidation route to copy — the build, remove, upgrade and template commands all mark dirty themselves, and a fresh `Airport` starts with every dirty bit set, so nothing else should ever reach it.

### Per-aircraft modular runtime state

In `Aircraft` (`src/aircraft.h`), under the `Modular airport ground pathfinding` comment block:

- `taxi_path` (heap, not saved), `landing_chain_path` (heap, not saved)
- `taxi_path_index`, `taxi_current_segment`, `taxi_wait_counter`
- `taxi_reserved_tiles` — non-runway reservations
- `modular_runway_reservation` — whole-runway claim for a landing or takeoff operation
- `ground_path_goal`
- `modular_ground_target` (`MGT_*`)
- `modular_landing_tile`, `modular_landing_goal`
- `modular_takeoff_tile`, `modular_takeoff_progress`
- `modular_holding_wp_index` (`UINT32_MAX` = uninitialised)

`MGT_*` values (`src/modular_airport_cmd.h`): `MGT_NONE`, `MGT_TERMINAL`, `MGT_HELIPAD`, `MGT_HANGAR`, `MGT_RUNWAY_TAKEOFF`, `MGT_ROLLOUT`, `MGT_HELI_TAKEOFF_TILE`.

## Aircraft Flow (modular)

- `FLYING`: `AirportMoveModularFlying` flies the holding loop; `FindModularLandingTarget` picks a runway or helipad.
- Landing commit (`AircraftEventHandler_Flying` in `aircraft_cmd.cpp`): picks the target, pre-reserves via `TryReserveLandingChain`, and sets `VehicleAirFlag::HelicopterDirectDescent` for `HELILANDING`. Helipad-specific approach overrides belong here, not in `AirportMoveModularLanding`.
- `LANDING` / `ENDLANDING`: `AirportMoveModularLanding` runs the approach and touchdown.
- Ground taxi: `AirportMoveModular` walks the classified taxi path. While the aircraft is still braking on the runway it also rolls `MaybeCrashModularAircraft` on every tick, mirroring the classic FTA.
- Ground targets after touchdown: `MGT_ROLLOUT` first (rollout and runway egress), then `TryRetargetModularGroundGoal` retargets to `MGT_TERMINAL` / `MGT_HELIPAD` / `MGT_HANGAR`.
- Parked: `HandleModularGroundArrival`, then waits on orders.
- Departure: `FindModularRunwayTileForTakeoff` + `FindModularTakeoffQueueTile`, then `AirportMoveModularTakeoff` (or `AirportMoveModularHeliTakeoff`) back to `FLYING`.

`CanUseModularGroundRouting(st, v)` gates ground routing on the aircraft actually standing on a tile of that airport.

Departure statistics are accounted inside `AirportMoveModularTakeoff` / `AirportMoveModularHeliTakeoff`, not in the shared FTA path.

### Holding loop

`ComputeModularHoldingLoop` (`src/modular_airport_holding.cpp`) builds a Dubins-curve loop with a gate per landing-capable runway end (`ModularHoldingLoop::Gate`). Constants live in `src/airport.h` (`MODULAR_HOLDING_TURN_RADIUS_TILES`, `MODULAR_HOLDING_OVERSHOOT_TILES`, `MODULAR_HOLDING_SAMPLE_INTERVAL_PX`, …).

Pitfalls:

- Don't use `tick_counter` or `running_ticks` for phase timing — both are `uint8_t` and wrap at 256. Use `TimerGameTick::counter`.
- Movement must be unconditional; don't guard `UpdateAircraftSpeed` inside `if (dist > 0)`.
- Use the ghost position for movement, nearest-waypoint only for gate checks (`IsHoldingGateActive`).
- Reset `modular_holding_wp_index` to `UINT32_MAX` on landing commit.

### Helicopters

Helicopters use helipads when present. With no helipad, the airport exposes computed tiles: `modular_heli_landing_tile`, `modular_heli_takeoff_tile`, and `modular_heli_service_tile` (touchdown for a depot-bound helicopter when no helipad can reach a hangar). `modular_hangar_reachable_pads` is the set of helipads from which a hangar is reachable by ground — the only pads a depot-bound helicopter may land on. `EnsureModularHeliTilesValid` recomputes them behind `modular_heli_tiles_dirty`.

## Commands and Editing Flow

Registered in `src/station_cmd.h`:

| Command | Implementation |
|---------|----------------|
| `Commands::BuildModularAirportTile` | `modular_airport_build.cpp` |
| `Commands::BuildModularAirportFromStock` | `modular_airport_build.cpp` |
| `Commands::UpgradeModularAirportTile` | `modular_airport_build.cpp` |
| `Commands::SetRunwayFlags` | `modular_airport_template_cmd.cpp` |
| `Commands::SetTaxiwayFlags` | `modular_airport_template_cmd.cpp` |
| `Commands::SetModularAirportEdgeFence` | `modular_airport_template_cmd.cpp` |
| `Commands::PlaceModularAirportTemplate` | `modular_airport_template_cmd.cpp` |

Tile removal goes through the normal clear path into `RemoveModularAirportTile` (`modular_airport_build.cpp`); there is no separate remove command.

### Build tile

`CmdBuildModularAirportTile`:

- checks piece availability by year (`IsModernModularPiece` / `GetModularPieceMinYear`)
- enforces flat-level consistency within an existing modular airport
- allows safe replacement of modular grass/empty tiles
- allows in-place hangar replacement (hangar-on-hangar) without clearing station state first
- stores directional hangar metadata in `piece_type`
- validates one-way taxi settings against auto directions
- inherits runway flags from a contiguous runway if present, otherwise defaults
- runs `NormalizeRunwaySegmentVisuals` on the whole affected segment afterwards
- places compound pieces as a unit (see below)

### Build from stock airport

`CmdBuildModularAirportFromStock` converts a stock layout to modular metadata, applies stock overrides, sets runway flags per airport type, and mirrors fence edges. For `AT_SMALL`, the legacy 3-tile terminal (`APT_SMALL_BUILDING_1/2/3`) is preserved in modular form as a compound piece.

### Upgrade

`CmdUpgradeModularAirportTile` takes an **area** (click or drag) and retypes old pieces to modern ones:

| From | To |
|------|----|
| `APT_RUNWAY_SMALL_NEAR_END` / `APT_RUNWAY_SMALL_FAR_END` | `APT_RUNWAY_END` |
| `APT_RUNWAY_SMALL_MIDDLE` | `APT_RUNWAY_5` |
| `APT_SMALL_DEPOT_*` | `APT_DEPOT_*` |
| `APT_GRASS_1` | `APT_APRON` |

It preflights the whole area (ownership, modular membership, year gate, `EnsureNoVehicleOnGround`) before touching any tile, so a runway is never left half-upgraded with an aircraft standing on a later tile. Cost is removal + rebuild with no discount. Afterwards it re-normalizes runway visuals, marks the layout dirty, cancels hangar orders if the last hangar was retyped away, and applies the noise delta.

### Remove tile

`RemoveModularAirportTile`:

- demolishes multi-tile compound pieces coherently
- clears and rebuilds affected runway visuals around removed segments
- updates modular indices and invalidates layout caches
- tears down the airport facility if no modular tiles remain

### Compound pieces

Some buildings span several tiles and only make sense whole. `GetModularCompoundPieceTiles(gfx)` (`modular_airport_gui.h`) returns the tiles a compound piece places relative to the clicked anchor; `GetModularCompoundPieceSize(gfx)` gives the footprint. Compound footprints are **fixed and unrotatable** — each tile has its own graphic drawn to join up in one orientation only. `GetModularAirportBuilderPieceGfx()` is the definition of what a modular airport may be built from; anything placing modular tiles outside the builder (the script API above all) must place only graphics from that set.

## Ground Routing and Reservation Model

### Pathfinding

`FindAirportGroundPath` (`src/airport_ground_pathfinder.cpp`) is A* across modular airport tiles. Connectivity comes from:

- auto taxi directions (`CalculateAutoTaxiDirectionsForGfx`)
- the optional user one-way restriction mask
- runway axis checks and runway operation restrictions
- explicit edge fences (`edge_block_mask`)

Cost: non-goal stand/parking tiles carry `PASS_THROUGH_STAND_PENALTY = 5`, so routes prefer slightly longer taxiways over cutting through unrelated stands.

Two arguments matter for correctness:

- `v` — with `v == nullptr` the search ignores stand occupancy (topology only); with an aircraft it avoids occupied stands that aren't the goal.
- `update_cache` — the crossing-required path cache (`_modular_airport_crossing_required_path_cache`) is game state. Diagnostic and debug probes must pass `false`, so that unsaved rate-limit gating cannot diverge the saved cache across multiplayer clients.

`BuildTaxiPath` wraps the A* result and classifies it into segments.

### Segment classes

`TaxiSegmentType` (`src/airport_ground_pathfinder.h`), assigned by `ClassifyTile`:

| Type | Condition | Reservation scope |
|------|-----------|-------------------|
| `RUNWAY` | `IsModularRunwayPiece(piece_type)` | Crossing: traveled tiles only. Explicit landing/takeoff operation: the entire contiguous runway. |
| `ONE_WAY` | `IsTaxiwayPiece(piece_type) && one_way_taxi` | Queue tile and forward-horizon boundary. |
| `FREE_MOVE` | Everything else (aprons, stands, hangars, fenced apron variants) | Traveled tiles through the forward horizon. |

Segment type describes **routing and safe-stop behaviour**. It does *not* select a per-class reservation algorithm: reservation scope is decided by the aircraft's operation and the forward horizon below, not by which segment it happens to be standing in.

One-way flags only apply to `IsTaxiwayPiece` types; stands, hangars and runways cannot be one-way.

### Unified forward reservation horizon

`TryReserveTaxiSegment` builds **one** reservation horizon from the current `taxi_path_index`, regardless of which segment triggered the call. `BuildForwardReservationPlan` is the single description shared by reservation and retention: starting at `taxi_path_index`, it walks to the aircraft's goal or the first *future* safe stop. The current safe-stop tile cannot immediately end a departure plan — the aircraft must reserve somewhere to advance to.

The plan holds two kinds of claim:

- **Taxi tiles** — every traveled apron, taxiway, parking, and transit-runway tile. A runway *crossing* is ordinary exclusive path space: only the tiles on this path are claimed, so two aircraft may cross the same runway at disjoint places.
- **Operation runway** — only a runway explicitly used for a landing or a runway takeoff, expanded to the whole contiguous runway via `TryReserveContiguousModularRunway`. A takeoff runway is not acquired while the horizon still ends at an upstream one-way queue; it joins the plan when the horizon actually reaches that runway.

Landing supplies its touchdown runway explicitly. Ground movement identifies a takeoff operation from `MGT_RUNWAY_TAKEOFF` + `modular_takeoff_tile`. After touchdown, the aircraft's tracked whole-runway claim identifies the landing operation until it steps off. A runway merely crossed en route to another runway never enters `modular_runway_reservation`.

Landing commit uses `TryReserveLandingChain` to acquire runway plus immediate egress path before the aircraft leaves `FLYING`.

### Safe stops (key invariant)

An aircraft on the ground must always hold a reserved path to a tile where it can wait indefinitely without blocking a shared resource. Stands, hangars, helipads and one-way taxiway tiles are safe stops; runway tiles and `FREE_MOVE` grass/apron are not. A runway-end takeoff goal is an acceptable path *terminus* but never a mid-path resting place.

Stopping on grass or apron is an invariant violation, not a "stuck" symptom — the system is supposed to deny entry rather than allow the stop. See `skills/reservations-design.md` for the full treatment.

### Runway structure invariants

Two structural invariants hold for every contiguous runway, and landing/takeoff eligibility depends on both:

- **Both extremities are end pieces.** `NormalizeRunwaySegmentVisuals` recanonicalizes the whole segment on every placement, removal and upgrade (stock conversion does the same inline), so extending a runway caps the new extremity and demotes the old cap. `GetCanonicalRunwaySegmentPiece` defines the canonical form. A bare `APT_RUNWAY_5` at an extremity is not reachable through the build commands.
- **Exactly one direction bit is set.** `SetRunwayFlags_Check` rejects zero-mode and non-single-direction flags; `NormalizeModularRunwayFlags` canonicalizes template values. Of a runway's two ends, exactly one is a legal landing end.

Runway end fence variants (`APT_RUNWAY_END_FENCE_*`) are decorative and **not** in `IsModularRunwayPiece`; only `APT_RUNWAY_END`, `APT_RUNWAY_SMALL_NEAR_END` and `APT_RUNWAY_SMALL_FAR_END` are landing targets.

### Runway flags

`RUF_*` (`src/base_station_base.h`): `RUF_LANDING`, `RUF_TAKEOFF`, `RUF_DIR_LOW`, `RUF_DIR_HIGH`. `CmdSetRunwayFlags` applies them across the entire contiguous same-axis runway.

The "low" end of an X-axis runway is its lower-X end; of a Y-axis runway, its lower-Y end. (Neither axis is screen-horizontal — both run down-screen, one to the left and one to the right; see `coords.md`.) The direction bits name the direction of *travel*, not an end: `RUF_DIR_LOW` means operations run toward the low end, so a runway landed on at its low end and rolled out toward the high end needs `RUF_DIR_HIGH`.

## Aircraft Crashes

`MaybeCrashModularAircraft(v, st)` (`aircraft_cmd.cpp`) is the modular crash entry. It calls the pure predicate `ModularAircraftHasElevatedOverrunRisk(v, st)`, then `RollAirplaneCrashCheck`. Helicopters never crash via this path.

It is called from `AirportMoveModular` on every brake tick while a plane is still decelerating on the runway after landing, which is what makes the per-landing risk equal a stock airport's. Takeoff never brakes, so — as in stock — there is no takeoff crash.

**Elevated overrun risk** = `AIR_FAST` jet **and** `!_cheats.no_jetcrash.value` **and** `!ModularAirportSupportsLargeAircraft(st)`. It ignores the "Plane crashes" setting, matching the stock short-strip overrun (prob 3276). Otherwise the general roll `(0x4000 << plane_crashes) / 1500` applies, so no crash when `plane_crashes == 0`.

The roll consumes the synced game `Random()`, not `_interactive_random`, so the RNG-consumption count must stay client-independent for multiplayer determinism.

The same condition feeds two non-crash paths. Automatic hangar selection (`aircraft_cmd.cpp`) skips a modular airport that is not large-safe when looking for somewhere to service a fast jet, and also skips airports whose layout does not accept the aircraft's kind at all — the generic `AT_MODULAR` FTA's `ShortStrip`/`Airplanes` flags cannot answer that, so `ModularAirportAcceptsPlanes` / `ModularAirportAcceptsHelicopters` do. Order validation (`order_cmd.cpp`) raises `STR_NEWS_PLANE_USES_UNSAFE_MODULAR_AIRPORT`, the modular counterpart of the stock short-strip warning; it warns, it does not reject the order.

## Rendering and UI

### Shared tile layout overrides

`GetAirportTileLayoutWithModularOverrides` (`src/modular_airport_draw.cpp`) centralizes modular sprite decisions for:

- directional hangars (`GetModularHangarTileLayoutByPiece`)
- NS runway override sprites (`GetModularNSRunwayLayout`)
- legacy small runway sprites without the baked stock SE fence (modular fences come from `edge_block_mask`)
- modular windsock/helipad variants
- radar/flag animated airport tile layouts

It is used by normal tile drawing (`ApplyModularAirportTileLayoutOverrides`), the builder preview, and the template preview, to keep those paths from diverging. Perimeter fences are drawn by `DrawModularAirportPerimeterFences` from `edge_block_mask` plus `GetModularTileFenceOpenMask`.

`AirportTiles` IDs `>= NEW_AIRPORTTILE_OFFSET` (74) are NewGRF airport tiles. Do not store new modular default-tile IDs in map gfx; keep canonical gfx IDs and branch drawing from modular metadata.

### Builder UI

`src/modular_airport_gui.cpp`:

- piece toolbar with sub-pickers (hangar direction, cosmetic, helipad)
- opens with no preselected piece
- smart runway drag placement (auto end pieces)
- taxi/runway overlay editing mode, rotation and taxi-direction controls, one-way toggle
- an erase piece (last entry in the piece list) that starts a `DDSP_DEMOLISH_AREA` drag
- edge-fence tool (`WID_MA_FENCE_TOOL`) — click near a tile edge, resolved from `_tile_fract_coords`
- upgrade tool (`WID_MA_UPGRADE_TOOL`) — click or drag a `DDSP_UPGRADE_AIRPORT` area
- info-overlay sub-window (`WID_MA_INFO_OVERLAY`) with three independent toggles: taxi arrows (`_show_runway_direction_overlay`), holding loop (`_show_holding_overlay`), and live aircraft taxi reservations (`_show_taxi_reservation_overlay`)
- template manager launch
- year-gated piece availability refresh while the builder is open, including external year jumps (Sandbox options)

Overlay drawing entry points: `DrawModularHoldingOverlay`, `DrawModularTaxiReservationOverlay` (`modular_airport_gui.h`), `DrawModularAirportDirectionOverlays` (`modular_airport_draw.h`).

### GUI pitfalls

- `PickerWindowBase::Close()` calls `ResetObjectToPlace()` — child pickers must override `Close()` with `this->Window::Close()` to avoid stealing the parent's placement cursor.
- When a child picker closes, the parent builder should clear active placement for picker-backed tools so button state and cursor don't stay latched.
- `SetObjectToPlace` triggers `OnPlaceObjectAbort` on the current cursor owner. When changing cursor ownership from within the same window, wrap the call in `this->updating_cursor = true/false`.
- `CloseWindowByClass` can trigger `ResetObjectToPlace` chains via a `PickerWindowBase`'s `Close()`. Guard with `updating_cursor` or override `Close()`.
- Sub-tile click position comes from `_tile_fract_coords.x/.y` (0–15 in world X/Y), set by the viewport on every click — not `InverseRemapCoords`.
- `SetPIPRatio(left, mid, right)`: `(0,0,1)` left-aligned, `(1,0,1)` centered, `(1,0,0)` right-aligned.

## Templates

`AirportTemplate` (`src/airport_template.h`): name, file stem, width/height, tiles, `schema_version`, `is_available`.

`AirportTemplateTile` stores `dx`, `dy`, `piece_type`, `rotation`, `runway_flags`, `one_way_taxi`, `user_taxi_dir_mask`, `edge_block_mask`, and a NewGRF reference (`grfid` = 0 for base-set tiles, else `local_id`). `CheckAvailability()` marks a template unavailable when a referenced NewGRF is missing.

Limits: `MAX_TEMPLATE_TILES = 64 * 64` (station spread tops out at 64, so no buildable airport is too large) and `MAX_TEMPLATE_DIM = 255` (placement encodes each offset in one byte). A `static_assert` against `MAX_COMMAND_PAYLOAD_SIZE` in `modular_airport_template_cmd.cpp` checks the whole layout fits one command payload. The caps exist to stop a corrupt or hostile template file from requesting an unbounded allocation.

Rotation is restricted by content:

- `HasNonRotatablePieces()` — compound pieces (e.g. the 3-tile small terminal) lock rotation entirely.
- `HasLegacySmallRunwayPieces()` — legacy small runways are axis-locked, so only 0°/180° are allowed.

`src/airport_template_gui.cpp` provides save-from-selected-airport, load and rotated placement, an in-window isometric preview with zoom-down for large templates, preview runway-end normalization for legacy small runway segments, and a map coverage overlay showing the catchment the placed airport would have (`AirportTemplate::GetCatchmentRadius`, which is rotation-independent).

`CmdPlaceModularAirportTemplate` places the whole layout atomically. It is flagged `CommandFlag::NoTest`: it must preflight the entire final placement itself before executing, and a failure after preflight is an internal bug path, not an acceptable partial build.

Template storage is JSON in the personal dir under `airport_templates/` (`src/airport_template.cpp`). `scripts/parse_airport_template.py` visualizes template files (`--grid`, `--detail`, `--runways`, `--raw`).

## Script API

`ScriptAirport` (`src/script/api/script_airport.hpp`) exposes modular airports to NoAI/NoGO.

Enums: `ModularPiece` (25 pieces plus `MP_INVALID`), `ModularRunwayFlags`, `ModularSafety` (`MS_OK`, `MS_MISSING_TOWER`, `MS_MISSING_BIG_TERMINAL`, `MS_MISSING_LANDING_RUNWAY`, `MS_MISSING_TAKEOFF_RUNWAY`), `ModularLayoutField`.

A layout is a flat integer array of `MLF_STRIDE` values per tile: `MLF_DX`, `MLF_DY`, `MLF_PIECE`, `MLF_ROTATION`, `MLF_RUNWAY_FLAGS`, `MLF_ONE_WAY_TAXI`, `MLF_TAXI_DIR_MASK`, `MLF_EDGE_FENCE_MASK`.

Queries on a built tile: `IsModularAirportTile`, `GetModularPiece`, `GetModularPieceRotation`, `GetModularRunwayFlags`, `GetModularAirportSafety`, plus `IsModularPieceAvailable` / `GetModularPieceMinYear` for year gating.

Building: `PlaceModularAirportLayout` creates a whole airport in one call; `BuildModularAirportTile` grows an existing one. `UpgradeModularAirportTile` / `UpgradeModularAirportArea`, `SetModularRunwayFlags`, `SetModularTaxiwayFlags` edit it.

Pre-placement measurement on an unplaced layout array: `GetModularLayoutNoiseLevel`, `GetModularLayoutCatchmentRadius`, `GetModularLayoutMonthlyMaintenanceCost`, `GetModularLayoutAcceptsPlanes`, `GetModularLayoutHasHelipad`, `GetModularLayoutSafety`.

Note that `MS_OK` means the layout meets the large-aircraft safety requirements — it does not mean the airport is usable (reachability and occupancy are not tested).

## Save/Load

### Fork feature versioning

**Nothing of this fork's goes into `SaveLoadVersion`.** That enum is upstream's and is merged verbatim. Fork features are versioned on their own axis in `src/saveload/extended_version_sl.h`, shaped after JGRPP's SLXI chunk so that porting a feature there is mechanical.

- A savegame written here sets `SAVEGAME_VERSION_EXT` (`0x8000`) in the header version word on top of an ordinary upstream version, so upstream rejects it with a plain "savegame too new" rather than misreading map bits. The bit is stripped on load.
- The `XVER` chunk carries one `{name, uint16 version, flags}` row per `SlxFeature` — currently `UpstreamVersion` and `ModularAirport`. It is registered **first**, so it is written first and known before any chunk that depends on it. An unknown or too-new feature aborts the load unless its saved `SlxFeatureFlag` says it may be dropped.
- Gate on the feature, not on a version: `IsModularAirportSaveFeaturePresent()`. Bump `MODULAR_AIRPORT_SL_VERSION` and pass a `min_version` for a format change within the feature.
- Per-field conditions are usually unnecessary. `VEHS` and `STNN` are table chunks, so a savegame lists the fields it holds and one written without ours simply does not load them — which is why the modular fields in `vehicle_sl.cpp` and `station_sl.cpp` carry no version condition: plain `SLE_VAR`, or `SLE_CONDVECTOR` over the full version range for the two reservation vectors, there being no unconditional `SLE_VECTOR` for struct members.
- Savegames stamped 367-375 — written by the fork before this scheme, when it still appended to `SaveLoadVersion` — are no longer loadable. The temporary shim that translated them was removed; they now fail with the ordinary "savegame too new" error, since those numbers are ahead of the upstream version this build knows.

### What is saved

| What | Where |
|------|-------|
| Per-tile metadata, including `reservation_owner` | `SlModularAirportTileData`, `src/saveload/station_sl.cpp` |
| Crossing-required ground-path cache | `MACP` chunk, `src/saveload/airport_sl.cpp` |
| Aircraft modular state | `src/saveload/vehicle_sl.cpp` |

Aircraft modular state **is** persisted: `taxi_path_index`, `taxi_current_segment`, `taxi_wait_counter`, `ground_path_goal`, `modular_landing_tile`, `modular_landing_goal`, `modular_ground_target`, `modular_takeoff_tile`, `modular_takeoff_progress`, `taxi_reserved_tiles`, `modular_runway_reservation`, `modular_holding_wp_index`.

`taxi_path` and `landing_chain_path` are **not** saved — they are heap pointers and are recomputed on load.

The governing invariant: any state that affects aircraft movement, reservations, or path choices must be saved or deterministically rebuilt on load. Map-level reservation bits and the crossing cache both affect multiplayer game state, which is why they are persisted rather than recomputed.

## Rotation Invariants (Critical)

These are the most common source of bugs.

- Hangar directional convention is `0=SE, 1=NE, 2=NW, 3=SW` (clockwise in world space). Keep the mapping aligned across:
  - `SwapBuildingPieceForRotation` (`src/modular_airport_cmd.h`)
  - `GetModularHangarTileLayoutByPiece` (`src/modular_airport_draw.cpp`)
  - hangar taxi-direction handling in `CalculateAutoTaxiDirectionsForGfx` (`src/airport_pathfinder.cpp`)
- Preview isometric handedness uses `iso_x = (dy - dx) * half_w`; flipping this mirrors the whole preview.
- Legacy small runway `NEAR`/`FAR` ends swap on odd quarter-turns. After template rotation, the preview must normalize each contiguous segment so the low end is `FAR` and the high end is `NEAR`, matching placed tiles.
- Axis-dependent code must be checked against `RemapCoords` (`src/landscape.h`) and `_tileoffs_by_diagdir` (`src/map.cpp`), not intuition — a mirrored model stays self-consistent and is easy to miss. See `coords.md`.

## Testing

Unit tests: `src/tests/test_modular_airport.cpp` covers classification, rotations, map-dependent helpers, ground pathfinding (including stand avoidance), reservation invariants, and the safety predicates.

```bash
/Users/tor/ttd/OpenTTD/build/openttd_test "ModularAirport*"
```

Regression: `scripts/regression_test.sh` runs headless 5-year simulations and compares total airport movements against the committed floors in `scripts/testdata/*.expected`. A bare run is the `T5j2` fixture only (~2 min), which is the normal check; `--full` runs all four concurrently (~13 min) and is worth it when a change could break ground/taxi pathfinding — always for routing work, since only `T7d` has route diversity. Per-commit attribution is `scripts/airport_stats_history.sh`. See `CLAUDE.md` for the fixture-by-fixture detail and the caveats about paused saves and log paths.

## Debugging

- `src/modular_airport_cmd.cpp` carries dense `[ModAp]` logging around reservation and movement, rate-limited per vehicle and channel via `ShouldLogModularRateLimited`.
- `src/airport_ground_pathfinder.cpp` has detailed neighbour/connectivity traces at higher debug levels.
- `LogModularVehicleReservationState` and `LogModularTakeoffRunwayUnavailable` dump the state you usually want when an aircraft won't move.
- The taxi-reservation overlay in the builder shows live reservations on the map.
- Runtime log is `/tmp/openttd.log`. Playbooks: `skills/stuck_plane_debugging.md`, `skills/lldb_debugging.md`, `skills/lldb_game_state_inspection.md`, `skills/crash_debugging.md`.

## Common Pitfalls

- `GetModularTileData(tile)` returns `nullptr` if the tile isn't in the modular layout — always null-check.
- Layout-derived answers are cached and invalidated **only** by `MarkLayoutDirty()`; see the per-airport state section.
- `modular_tile_data` order is not stable (erase/push_back), so never hold an index across a mutation — hold the tile.
- Depot windows can outlive tile deletion. Guard depot UI reads with a valid depot tile check.
- Modular airport creation/removal must preserve stock town-noise and two-airport local-authority accounting semantics for the selected airport type.
- Changes to movement, reservation, build atomicity, or airport accounting need save/load, multiplayer determinism, and regression coverage checks.
