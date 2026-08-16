# Designing an AI that builds modular airports

Investigation + outline plan for a NoAI script that designs and builds modular airports
tile-by-tile, rather than picking a stock type or a saved template.

---

## 1. Verdict

**Feasible, but it is a two-part project, and part one is C++.**

Nothing modular is reachable from the script API today. `ScriptAirport::AirportType` carries
`AT_MODULAR` (so `AIAirport.GetAirportType()` can *report* one), and
`IsAirportInformationAvailable()` explicitly returns false for it — every type-level query
(`GetPrice`, `GetAirportWidth/Height`, `GetAirportCoverageRadius`, `GetNoiseLevelIncrease`,
`GetMaintenanceCostFactor`) refuses modular by design, because those numbers come from the
layout. All seven modular commands (`CMD_BUILD_MODULAR_AIRPORT_TILE`, `CMD_SET_RUNWAY_FLAGS`,
`CMD_SET_TAXIWAY_FLAGS`, `CMD_BUILD_MODULAR_AIRPORT_FROM_STOCK`,
`CMD_SET_MODULAR_AIRPORT_EDGE_FENCE`, `CMD_PLACE_MODULAR_AIRPORT_TEMPLATE`,
`CMD_UPGRADE_MODULAR_AIRPORT_TILE`) exist only C++-side.

The good news is that the hard half is already built. The modular code was written with
"score a layout that does not exist yet" in mind — `GetModularAirportNoiseLevelFromPieces`,
`GetModularAirportCatchmentRadiusFromPieces`, `GetModularAirportMaintenancePointsFromPieces`,
`ModularAirportAcceptsPlanesFromPieces` all take a piece span rather than a `Station *`
(`src/modular_airport_cmd.h:426-459`). Those are exactly the primitives a planner needs, and
they are already exposed to the template GUI for the same reason. The API work is wrapping,
not designing.

What is genuinely new and hard is the *design* problem: emitting a legal, functional,
economically sensible layout for an arbitrary patch of terrain. Sections 5–7 attack that.

---

## 2. What the build commands actually demand

These are the rules the planner has to satisfy. Read them as the spec.

### Per-tile build (`CmdBuildModularAirportTile`, `src/modular_airport_build.cpp:1074`)

Each tile is an independent command with its own check (`BuildModularAirportTile_Check`,
`:765`):

- **Piece year gating** — modern pieces refuse before `GetModularPieceMinYear(piece)`.
- **Town authority** — `CheckIfAuthorityAllowsNewStation` on every *new* tile; replacing a
  piece inside an existing modular airport skips it (no new land taken).
- **Terrain — the airport is flat, like a stock one.** `CheckBuildableTile` runs with a fresh
  `allowed_z = -1` per command, but that is *not* the whole story: a second, airport-wide
  guard at `:878` rejects any tile whose `GetTileMaxZ` differs from the first placed tile's
  (`STR_ERROR_FLAT_LAND_REQUIRED`). A modular airport cannot span two height levels. Sloped
  tiles are fine when their max corner matches the airport level — they get a foundation,
  priced at `Price::BuildFoundation`. Nothing terraforms; the land must already suit.

  This invariant is load-bearing in the movement code, not incidental: ground taxi never
  recomputes `z_pos` (`SetAircraftPosition(v, v->x_pos, v->y_pos, v->z_pos)`,
  `src/modular_airport_cmd.cpp:4077`). z is only corrected at landing
  (`GetTileMaxPixelZ(modular_landing_tile) + 1`), takeoff, and hangar teleport. An airport
  spanning a height step would leave taxiing aircraft visually sunk into or floating over the
  ground. Stock's `AircraftController` states the same assumption outright.
- **Joining** — `FindJoiningStation`; joining a station that already has a *non-modular*
  airport fails with `STR_ERROR_TOO_CLOSE_TO_ANOTHER_AIRPORT`.
- **Noise** — recomputed from the would-be piece set, checked against the town.
- **Replace rules** — only `APT_GRASS_1`/`APT_EMPTY` are freely overwritable; hangar→hangar
  is allowed. Anything else needs a clear first.
- **Vehicles** — `EnsureNoVehicleOnGround` on replace. Matters when upgrading a live airport.

Runway flags default to `RUF_DEFAULT` (0x0F = landing + takeoff, both directions,
`src/base_station_base.h:40`), so a naive airport works without touching
`CmdSetRunwayFlags`. Directional/role separation is an optimization, not a prerequisite.

### Atomic layout submit (`CmdPlaceModularAirportTemplate`, `src/modular_airport_template_cmd.cpp:327`)

Takes a `ModularTemplatePlacementData` — width, height, rotation, and a vector of
`{dx, dy, piece_type, rotation, runway_flags, one_way_taxi, user_taxi_dir_mask,
edge_block_mask}`. It preflights the whole placement and then executes; a failure after
preflight is an internal bug path, not a partial build.

Constraints it adds over tile-by-tile:

- `MAX_TEMPLATE_TILES` = 128 (`src/airport_template_gui.cpp:93`).
- Rotation restrictions: compound small-terminal pieces and legacy small hangars cannot
  rotate at all; legacy small runway pieces only 0/180.
- `CommandFlag::NoTest` — a test-mode probe is not authoritative for it.

**This command is the key insight for the AI.** "Not from templates" means the AI should
*design* the layout, not pick a canned one — it does not mean it must place tiles one at a
time. Generating a layout in Squirrel and submitting it through the template command gives
all-or-nothing semantics for free, which eliminates the single nastiest failure mode
(a half-built airport when the money runs out or the authority refuses at tile 40).

Recommended split:

| Situation | Mechanism |
|---|---|
| New airport | generate layout → atomic submit |
| Growing an existing airport | tile-by-tile, or a small atomic submit joined to the station |

Both paths enforce the same one-level rule, so there is no "rough terrain" fallback: the
tile-by-tile path buys incrementality and partial layouts, not terrain tolerance.

### The actual terrain advantage: arbitrary footprints

A stock airport needs its entire W×H rectangle at one level and clear. A modular airport needs
only *the tiles it occupies* at one level. That is the real edge, and it is a shape advantage
rather than a height one: a generated layout can thread around a lake corner, a hill spur, a
protected building, or an existing road — sites where no stock airport fits at all. Site
search should exploit this explicitly (§7 `sites.nut`), by scoring one-level *regions* of
arbitrary shape rather than searching for free rectangles the way ChooChoo's
`IsBuildableRectangle` does.

### Large-aircraft safety — the design spec for a "real" airport

`GetModularAirportSafetyStatus` (`src/modular_airport_cmd.cpp:381`) returns the missing set:

- `MASR_TOWER` — needs a control tower piece.
- `MASR_BIG_TERMINAL` — needs one of `APT_ROUND_TERMINAL`, `APT_BUILDING_1/2/3`,
  `APT_STAND_1`, `APT_STAND_PIER_NE`.
- `MASR_LANDING_RUNWAY` / `MASR_TAKEOFF_RUNWAY` — needs a **contiguous ≥6-tile runway of the
  large family** with the matching flag (`IsRunwaySafeForLarge`, `:332`).

Miss any of those and fast jets take the elevated overrun crash roll. Any AI that flies big
planes must treat this as a hard constraint, not a preference.

### Other planner-visible rules

- Station spread applies to the modular bounding box (`src/modular_airport_build.cpp:1274`).
- Tile classification for reservations: `RUNWAY` (atomic per contiguous runway), `ONE_WAY`
  (per-tile queue), `FREE_MOVE` (atomic per segment). Layout shape directly determines
  throughput — a single giant apron is one atomic segment and serialises everything.
  See `skills/reservations-design.md`.
- Hangars already work through the normal API: `Airport::GetNumHangars/GetHangarTile`
  special-case modular (`src/station_base.h:507-551`), so `AIAirport.GetNumHangars` and
  `GetHangarOfAirport` need no changes, and `AIVehicle.BuildVehicle` at a hangar tile works.
- `AIStation.GetStationCoverageRadius(station_id)` works post-build (reads
  `GetCatchmentRadius`). Only the *pre*-build estimate is missing.

---

## 3. How AAAHogEx and ChooChoo build train lines

Both were read from `~/Documents/OpenTTD/content_download/ai/`. They are the two ends of the
design space, and both are relevant.

### ChooChoo — blueprints in local coordinates + a task tree

~7.9k lines. The architecture is:

- **A `Task` tree with rollback.** `Task.Run()` walks `subtasks`; `Task.Failed()` recursively
  fails everything already completed and demolishes it (`task.nut`). Control flow is
  exceptions: `TaskRetryException`, `NeedMoneyException`, `TooManyVehiclesException`,
  `TaskFailedException`. The main loop pops one task per iteration and dispatches on the
  exception type — sleep and retry, wait for money, or fail and roll back (`main.nut:110`).
- **Fixed blueprints in local coordinates, applied through a rotation.** `BuildTerminusStation`
  (`builder_stations.nut:305`) is literally a script:
  ```squirrel
  BuildSegment([0, p], [0, p+1]);
  BuildRail([1, p-1], [1, p], [0, p+1]);
  BuildDepot([2,p], [1,p]);
  BuildSignal([0, p+1], [0, p+2], AIRail.SIGNALTYPE_PBS_ONEWAY);
  ```
  Every coordinate is `[x, y]` in template space; `Builder` holds a `rotation` and
  `GetTile([x,y])` maps it to the map. `Failed()` demolishes the whole `3 × (p+2)` rectangle.
- **Site search by valuator chain.** `FindStationSite` (`builder.nut:47`) builds an
  `AITileList`, then filters: tiles belonging to the town → accepts passengers → produces
  cargo → fits a buildable rectangle (flat first, then terraformable) → not across a lake →
  closest to town centre.
- **Topology is a network of standard parts.** `BuildCrossing`, `ConnectStation`,
  `ConnectCrossing`, `ExtendCrossing` — the map-level plan is "grow a mainline network of
  identical junctions", and the pathfinder only fills in the track between known endpoints.

### AAAHogEx — economics first, geometry declarative

~37.5k lines. Different centre of gravity:

- **An estimator drives everything** (`estimator.nut`, 2.5k lines). Routes are chosen by
  expected value, and infrastructure is a consequence of that choice, not the starting point.
- **Station *factories* generate and score candidates.** `RailStationFactory.CreateBest`
  (`station.nut:1313`) sweeps platform lengths downward from the max, generating candidate
  placements per length, scoring them with `GetBestHgStationCosts` (direction score, distance,
  build cost), and taking the best. Subclasses (`SmartStation`, `TerminalStation`,
  `TransferStation`, …) differ only in geometry and cost model.
- **Geometry declared as relative-coordinate data, not imperative calls.** `TerminalStation`
  returns its track as a list of triples: `GetRails()` yields `[[x1,y1],[x2,y2],[x3,y3]]`
  entries — in/mid/out — parameterised by `platformNum`/`platformLength`
  (`station.nut:4430`). `At(x,y)` composes the station's own origin, direction and a rotation
  correction. Same idea as ChooChoo, but as *data* — which is what makes the layouts
  parameterisable rather than hand-written per size.
- **Test mode everywhere.** `AITestMode()` scopes probe buildability and cost before an
  `AIExecMode()` block commits (`railbuilder.nut:697,1437`, `pathfinder.nut:278`).
- **A persisted world model.** `HgStation.SaveStatics/LoadStatics` serialises every built
  station with enough state to reconstruct the object graph on load.

### What carries over to modular airports

| Pattern | Source | Why it matters here |
|---|---|---|
| Local-coordinate blueprint + rotation | both | `ModularTemplatePlacementData` *is* this format: `dx/dy` + a 0–3 rotation. The generator's output type is already defined for you. |
| Parameterised geometry as data | HogEx `GetRails()` | Layout families parameterised by stand count / runway length beat hand-written layouts. |
| Candidate generation + scoring | HogEx `CreateBest` | Site selection for airports is the same shape: enumerate (tile, rotation, layout variant), score, take the best. |
| Task tree with `Failed()` rollback | ChooChoo | Needed only for the tile-by-tile path; the atomic template submit makes it unnecessary for the common case. |
| Exception-driven main loop | ChooChoo | Airports are expensive; `NeedMoneyException`/wait-for-money is directly reusable. |
| Test-mode preflight | HogEx | Works for per-tile builds; **not** authoritative for the template command (`NoTest`). |
| Growth by extension | ChooChoo `ExtendCrossing` | The strategic thesis below. |

**Neither AI does anything comparable to designing an airport.** HogEx's `air.nut` is a
68-line static table mapping population thresholds to stock airport types, plus availability
checks. That is the entire state of the art for airport building — because until modular
existed, there was nothing to decide.

---

## 4. The strategic thesis

The reason a modular-airport AI is interesting is not that it can draw a prettier airport.
It is that **modular airports grow**.

A stock-airport AI faces a step function: to go from Small to City it must demolish, lose the
station, lose the catchment, and rebuild. Every existing AI therefore over- or under-builds.
A modular AI can:

1. Open a route with a minimal cheap airport (one short runway, two stands, one hangar).
2. Add stands as waiting passengers grow.
3. Add a second runway and split landing/takeoff via `CmdSetRunwayFlags` when one runway
   saturates.
4. Upgrade to large-safe (tower + big terminal + 6-tile large runway) exactly when the first
   big plane becomes affordable.
5. Add one-way taxiways and extra aprons to break up reservation contention as the fleet grows.

Each step is cheap, incremental, and never loses the station. Maintenance is points-derived
(`GetModularAirportMaintenancePoints`), so the AI pays only for what it built — a genuine
economic advantage over the stock ladder that the estimator can exploit.

This should be the AI's identity. Design the layout generator around growth stages, not
around one-shot perfection.

---

## 5. Phase 0 — the C++ API extension

Add to `src/script/api/script_airport.hpp/.cpp`. The build system auto-generates the Squirrel
bindings from the header (`src/script/api/CMakeLists.txt` runs `SquirrelExport.cmake` over
every `script_*.hpp`), so no registration boilerplate. New methods on a fork need no
`compat_*.nut` entry — those exist for removed/changed APIs.

**Piece vocabulary**
- `enum ModularPiece` mirroring the subset of `AirportTiles` an AI should use (apron, stand,
  runway family, runway ends, hangars ×4 rotations, helipads, tower, terminals, grass/empty).
  Do not expose all 74+ IDs; expose a curated, documented set.
- `GetModularPieceMinYear(piece)` → year gating.
- `IsModularPieceAvailable(piece)` → convenience over the above.

**Build**
- `BuildModularAirportTile(tile, piece, station_id, rotation, taxi_dir_mask, one_way_taxi, auto_rotate_runway)`
  → `CMD_BUILD_MODULAR_AIRPORT_TILE`.
- `SetRunwayFlags(tile, flags)`, `SetTaxiwayFlags(tile, mask, one_way)`,
  `SetModularAirportEdgeFence(tile, edge, set)`.
- `PlaceModularAirportLayout(tile, station_id, layout)` → `CMD_PLACE_MODULAR_AIRPORT_TEMPLATE`.
  **Marshalling decision:** `squirrel_helper.hpp` already supports `Array<Titem> &&`
  parameters (`:122`), so a Squirrel array of arrays is viable and is the cleanest option —
  no cross-call accumulator state to save/load. Verify it handles nested arrays; if not, fall
  back to a flat `Array<int>` of fixed-width records (8 ints per tile) rather than a
  stateful builder object.

**Query / preview** — thin wrappers over existing `*FromPieces` functions, taking the same
Squirrel layout array so the AI can score a layout *before* building it:
- `GetModularLayoutNoiseLevel(layout)`
- `GetModularLayoutCatchmentRadius(layout)`
- `GetModularLayoutMaintenanceCost(layout)`
- `GetModularLayoutAcceptsPlanes(layout)` / `...AcceptsHelicopters(layout)`
- `GetModularLayoutSafetyStatus(layout)` — needs a `FromPieces` variant of
  `GetModularAirportSafetyStatus`; currently `Station *`-only. Small, pure, and useful in
  C++ too (the template preview GUI wants the same thing).

**Inspection of built airports**
- `GetModularPiece(tile)`, `GetRunwayFlags(tile)`, `IsModularAirport(station_id)`,
  `GetModularAirportSafetyStatus(station_id)`.

**Optional but valuable**
- `IsModularLayoutConnected(layout)` — every stand/hangar reachable from every runway end.
  Only needed if free-form layout *search* is attempted (§6, option C); generated layouts
  from validated families are connected by construction. Would need a pure grid-level
  reimplementation of the taxi topology, since `BuildTaxiPath` operates on a built `Station`.

Estimated size: ~600–900 lines of straightforward wrapper code plus one new pure
`FromPieces` safety function. Also add an `ai_changelog.hpp` entry.

---

## 6. Phase 1 — how to generate a layout

Three options, in increasing ambition. **Recommendation: A now, C later if ever.**

### A. Parametric blueprint families *(recommended)*

A handful of hand-designed *families*, each a function of a few parameters, emitting a tile
grid. This is HogEx's `GetRails()` idea applied to airports.

```
GenerateLayout(family, {runway_length, stands, runways, hangars, large_safe, helipads})
    -> array of {dx, dy, piece, rotation, runway_flags, one_way, taxi_mask, fence}
```

Families worth having:
1. **Minimal strip** — 1 runway (3–4 tiles small family), apron spine, 2 stands, 1 small hangar.
2. **Single-runway pier** — 1 large-family runway ≥6, parallel taxiway, pier of N stands,
   tower + big terminal (large-safe by construction), 1–2 hangars.
3. **Parallel dual-runway** — landing runway + takeoff runway with directional flags, central
   apron/pier of N stands, one-way taxi loop to avoid head-on contention.
4. **Heliport** — helipads + hangar, no runway.

Each family is written once, verified by hand in-game, and then scales by parameter. Rotation
is free — the template command rotates the whole layout, and the codebase already handles the
awkward cases (hangar direction convention `0=SE, 1=NE, 2=NW, 3=SW`; legacy small runway
NEAR/FAR swap on odd quarter-turns — see the rotation invariants in `CLAUDE.md`).

Growth stages fall out naturally: each family declares which parameters can be increased
in-place and which tiles those additions occupy, so the AI can reserve room at build time.

### B. A. plus local repair

Family output plus site-aware patching: when a few tiles of the family's bounding box fall
outside the site's one-level region, shift the runway, shorten the pier, or drop cosmetic
tiles rather than rejecting the site. This is where the arbitrary-footprint advantage is
actually cashed in, and it needs no search. Reasonable second iteration.

### C. Constructive search

Treat it as a layout synthesis problem: place a runway to maximise usable length within the
site, then greedily grow taxiway/apron/stand structure, scoring with the preview API
(catchment, noise, maintenance, safety) plus a connectivity check. Elegant and much more
likely to produce something subtly non-functional. Only worth it once A works and the AI has
a scoring function that has proved itself.

**A note on the reservation system:** layout shape is throughput. A wide open apron is a
single `FREE_MOVE` segment reserved atomically — one aircraft at a time across the whole
thing. Long thin one-way taxiways queue per tile and pipeline. The families should be
designed with `skills/reservations-design.md` open, and validated against the regression
harness's contention behaviour, not just "does a plane land".

---

## 7. Phase 2 — the AI itself

Squirrel, `bin/ai/` or `~/Documents/OpenTTD/ai/`, structured as:

```
info.nut          AIInfo registration, settings (aggressiveness, max airports, family choice)
main.nut          AIController: main loop, event handling, exception dispatch  (ChooChoo shape)
task.nut          Task tree + Failed() rollback                                (ChooChoo, verbatim idea)
world.nut         persisted model: airports built, stage of each, routes       (HogEx SaveStatics idea)
sites.nut         candidate site search: valuator chains over AITileList
layout.nut        the generator of §6 — pure, testable, no API calls
build.nut         execution: atomic submit path + tile-by-tile path + rollback
grow.nut          upgrade decisions: when to add stands / runway / go large-safe
fleet.nut         aircraft purchase, orders, replacement                       (HogEx estimator idea, simplified)
```

Two things to get right early because they are painful later:

- **Save/load.** `Save()`/`Load()` must round-trip the world model. HogEx's approach —
  serialise plain tables keyed by a type name, reconstruct objects on load — is the proven
  pattern. Anything derivable (which tile is which piece) should be re-read from the map
  rather than saved.
- **Suspension.** Every command suspends the script; the world can change between two tiles
  of the same airport. The atomic path sidesteps this. The tile-by-tile path must re-validate
  and be able to roll back, which is exactly what `Task.Failed()` is for.

---

## 8. Milestones

| # | Deliverable | Proves |
|---|---|---|
| M0 ✅ | `AIAirport.BuildModularAirportTile` + `GetModularPiece` exposed; throwaway script builds a hardcoded 3-tile strip | The API path works end to end |
| M1 ✅ | `PlaceModularAirportLayout` + the preview/query wrappers; script submits a hardcoded layout array atomically | Marshalling works; the atomic path works |
| M2 | `layout.nut` family 1 + 2, pure functions, unit-tested by dumping grids and eyeballing with `scripts/parse_airport_template.py` | The generator produces legal layouts |
| M3 | Site search + build + one aircraft flying a route | First working AI |
| M4 | Growth: add stands, add runway, upgrade to large-safe on a live airport | The strategic thesis |
| M5 | Multi-airport network, fleet management, money management | A competitive AI |
| M6 | Non-rectangular footprint fitting + tile-by-tile path with rollback | Fits sites no stock airport can use |

M0–M2 are the risky part and are all C++/plumbing. After M2 the work is ordinary AI writing.

---

## 9. Testing

- **Layout generator**: pure functions. Dump generated grids to JSON in the template format
  and run `scripts/parse_airport_template.py --grid/--detail/--runways` — the visualiser
  already exists and speaks exactly this format. Consider a `openttd_test` case for the new
  `FromPieces` safety function.
- **AI behaviour**: headless, same shape as the existing regression harness —
  `./build/openttd -d script=2 -x -g <save> -s null -m null -v null:ticks=N`, with the AI
  configured in the save. Use the `[AirportStats]` logging that already exists to compare
  throughput of AI-built airports against the committed fixtures' hand-built ones.
- **Do not** regress `scripts/regression_test.sh` — the C++ API additions touch
  `station_cmd`-adjacent code paths only as callers, but the new `FromPieces` safety function
  must agree exactly with the `Station *` version or aircraft crash behaviour diverges.

---

## 10. Open questions

1. ~~**Nested-array marshalling.**~~ **Resolved when M0/M1 were built.** Only
   `Param<Array<Titem> &&>` is specialised, so nested arrays are *not* supported; the API
   takes a flat array with a documented stride (`MLF_STRIDE` = 8 values per tile). Two other
   things surfaced while building it, both now enforced as preconditions: a runway needs at
   least one of landing/takeoff and **exactly one** direction bit (so "all flags" is never a
   settable value, even though a freshly built runway carries 0x0F internally), and a one-way
   taxiway must name exactly one direction. There is also no `*FromPieces` equivalent for
   "accepts helicopters" — the built-airport answer depends on a map-dependent fallback tile
   — so the layout API only offers the exact question, `GetModularLayoutHasHelipad`.
2. **How much does the arbitrary-footprint edge actually buy?** Worth measuring before
   building M6 for it: on a few representative maps, count sites where a non-rectangular
   one-level region admits a viable layout but no stock airport rectangle fits. If the answer
   is "rarely", M6 drops down the list.
3. **Should the AI ever use `CMD_BUILD_MODULAR_AIRPORT_FROM_STOCK`?** It is a legitimate
   bootstrap: build a stock layout as modular, then grow it. Cheap to expose, and it gives
   M3 a working AI before M2's generator is trustworthy.
4. **Noise budget.** Modular noise is layout-derived and the town can refuse mid-build. The
   preview API makes this checkable up front — but the growth path needs to check it *again*
   at every stage, and possibly plan for a maximum eventual size at first build.
5. **Competing with itself.** Two instances of this AI on one map will both want the same
   flat sites. HogEx's `ngStationTiles` blacklist pattern is the cheap fix.
