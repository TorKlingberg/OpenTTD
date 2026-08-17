# Building an AI that designs modular airports

Plan for a NoAI script that designs and builds modular airports rather than picking a stock
type or a saved template.

**Status:** the script API is built (§2) and so is a working AI (`ai/ModularAirportAI/`).
It designs, sites, builds, and grows airports, and flies aircraft between them. M0–M4 are
done; §8 has what is left.

Run it headless with `scripts/run_ai.sh <years> <seed>`. On a 256×256 map from 1970 it
reaches 20 airports and ~55 aircraft carrying ~500 movements a year, every airport large-safe,
using the pier/linear/apron families across all four orientations.

---

## 1. Where this stands

The original blocker was that nothing modular was reachable from NoAI: `AT_MODULAR` existed in
the `AirportType` enum but `IsAirportInformationAvailable()` rejects it by design, and all
seven modular commands were C++ only. That is now fixed — `AIAirport` can build, inspect and,
importantly, *score a layout before building it*.

So the remaining project is the interesting half and the genuinely hard one: emitting a legal,
functional, economically sensible layout for a given patch of terrain, and knowing when to
build, extend or leave alone. Nothing in the API decides any of that.

Two things learned while building the API that shape the AI's design:

- **A modular airport occupies one height level, exactly like a stock one.** Its advantage
  over stock is *shape*, not terrain tolerance (§3).
- **The layout-scoring functions are free and side-effect-free.** An AI can enumerate hundreds
  of candidate layouts per site and cost them without touching the world. That makes a
  generate-and-score planner cheap, which is what §6 leans on.

---

## 2. The API you have

All on `AIAirport`. Full doxygen lives in `src/script/api/script_airport.hpp`.

**Vocabulary.** `ModularPiece` names 26 pieces (`MP_APRON`, `MP_STAND`, `MP_RUNWAY`,
`MP_RUNWAY_END`, `MP_RUNWAY_SMALL_*`, `MP_HANGAR`, `MP_SMALL_HANGAR`, `MP_HELIPAD`, `MP_TOWER`,
`MP_TERMINAL*`, `MP_STAND_TERMINAL`, …). `ModularRunwayFlags`, `ModularSafety` and
`ModularLayoutField` carry the rest.

**Build.**

| Call | Notes |
|---|---|
| `BuildModularAirportTile(tile, piece, rotation, station_id)` | One tile. For growing an airport. |
| `UpgradeModularAirportTile(tile)` | Converts one legacy runway, hangar or grass tile to its modern equivalent. |
| `UpgradeModularAirportArea(start_tile, end_tile)` | Atomically converts all legacy pieces in a rectangle; use this for a whole runway. |
| `PlaceModularAirportLayout(tile, station_id, rotation, w, h, layout)` | Whole layout, all-or-nothing. For new airports. |
| `SetModularRunwayFlags(tile, flags)` | Applies to the whole contiguous runway. |
| `SetModularTaxiwayFlags(tile, dir_mask, one_way)` | Per tile. |

**Inspect.** `IsModularAirportTile`, `GetModularPiece`, `GetModularPieceRotation`,
`GetModularRunwayFlags`, `GetModularAirportSafety`, `IsModularPieceAvailable`,
`GetModularPieceMinYear`.

**Preview — the planner's lever.** Each takes a layout and touches nothing:
`GetModularLayoutNoiseLevel`, `GetModularLayoutCatchmentRadius`,
`GetModularLayoutMonthlyMaintenanceCost` (directly comparable with
`GetMonthlyMaintenanceCost()` for a stock type), `GetModularLayoutAcceptsPlanes`,
`GetModularLayoutHasHelipad`, `GetModularLayoutSafety`.

### The layout format

A flat integer array, `MLF_STRIDE` (= 8) values per tile, in `ModularLayoutField` order:
`dx, dy, piece, rotation, runway_flags, one_way_taxi, taxi_dir_mask, edge_fence_mask`. Flat
rather than nested because the script API's `Param<Array<Titem> &&>` does not support nested
arrays. Write the helpers once:

```squirrel
function LayoutTile(dx, dy, piece, rot = 0, rwy = 0, one_way = 0, taxi = 15, fence = 0) {
    return [dx, dy, piece, rot, rwy, one_way, taxi, fence];
}
function Layout(tiles) {
    local out = [];
    foreach (t in tiles) foreach (v in t) out.append(v);
    return out;
}
```

`regression/regression/main.nut` has worked examples of both a small airport and a large-safe
one, and is the fastest way to see the API in use.

### Rules the API enforces

- A runway needs **at least one** of `MRF_LANDING`/`MRF_TAKEOFF` and **exactly one** of
  `MRF_DIR_LOW`/`MRF_DIR_HIGH`. "All flags" is never settable, even though a freshly built
  runway carries `0x0F` internally.
- A one-way taxiway must name exactly one direction, and it must be one the piece already
  allows.
- `PlaceModularAirportLayout` requires every piece to be available in the current year; the
  preview functions deliberately do not, so a script can cost a 1955 airport in 1948.
- Layouts are capped at 128 tiles. Compound small-terminal pieces and legacy small hangars
  cannot rotate; legacy small runway pieces only 0/180.

### Traps found by building against it

Each of these produced an airport that built cleanly and then did not work, and each was
found by measuring rather than by reading the source. They are the reason `run_ai.sh` reports
`[AirportStats]` movement counts: nothing else distinguishes a working airport from a dead one.

- **Rotation is broken.** `PlaceModularAirportLayout` with `rotation != 0` yields an airport
  whose aircraft never leave the hangar — no error, no `[ModAp]` log lines, valid-looking
  orders. The same layout authored vertically by hand at rotation 0 flies. The AI therefore
  rotates layouts itself (`Grid.Rotate`, mirroring `RotateTemplateTile`) and always passes 0.
  Suspected cause: a hangar's facing lives in both the piece type (`APT_DEPOT_*`) and the
  tile's rotation field, and rotation appears to turn both.
- **After rotation the origin tile may not belong to the airport** — for a half-turn, local
  (0,0) maps to (w−1, h−1), which can be an empty cell. `GetStationID(origin)` and
  `GetHangarOfAirport(origin)` then fail on a *successful* build.
- **`AITown.GetAllowedNoise` is not a noise level** when `station_noise_level` is off, which
  is the default: it returns "2 minus this town's airports". Comparing a layout's noise
  against it rejects everything bigger than a single helipad.
- **`AIOrder.AppendOrder` infers station-vs-depot from the tile**, so an order to a hangar
  tile silently becomes go-to-depot. Any layout whose northernmost row holds the hangar
  produces aircraft that fly to a hangar and stop.
- **`AIEngine.GetMaximumOrderDistance` is not in tiles.** It may only be compared against
  `AIOrder.GetOrderDistance`.

### What the API does not answer

- **Whether helicopters can use a finished airport.** `GetModularLayoutHasHelipad` is the
  exact question a layout can answer; the real one depends on a map-dependent fallback tile
  chosen after the airport exists. Read it off the built airport instead.
- **Whether a layout is internally connected** — that every stand is reachable from every
  runway end. Generated layouts from validated families are connected by construction (§6),
  so this only bites if free-form layout search is attempted.
- **Cost before building.** No preview of construction cost; use `AITestMode` around the build
  calls, remembering that `PlaceModularAirportLayout` is `CommandFlag::NoTest`, so its test
  result is not authoritative.

---

## 3. Rules the planner has to satisfy

### Terrain: one height level, arbitrary footprint

A modular airport cannot span two height levels. `BuildModularAirportTile_Check`
(`src/modular_airport_build.cpp:878`) rejects any tile whose `GetTileMaxZ` differs from the
first placed tile's; the layout command applies the same rule across its whole placement.
Sloped tiles are fine when their max corner matches — they get a foundation, priced at
`Price::BuildFoundation`. Nothing terraforms.

That invariant is load-bearing, not incidental: ground taxi never recomputes `z_pos`
(`src/modular_airport_cmd.cpp:4077`); z is corrected only at landing, takeoff and hangar
teleport. An airport spanning a height step would leave taxiing aircraft sunk into the ground.

**So the terrain advantage over stock is shape, not height.** A stock airport needs its entire
W×H rectangle at one level and clear; a modular one needs only *the tiles it occupies*. A
generated layout can thread around a lake corner, a hill spur, a protected building or a road
— sites where no stock airport fits at all. Site search should look for one-level *regions* of
arbitrary shape, not free rectangles the way ChooChoo's `IsBuildableRectangle` does.

### Large-aircraft safety — the spec for a "real" airport

`GetModularAirportSafety` / `GetModularLayoutSafety` return what is missing:

- `MS_MISSING_TOWER` — needs a control tower.
- `MS_MISSING_BIG_TERMINAL` — needs `MP_TERMINAL`, `MP_TERMINAL_ALT`, `MP_TERMINAL_OTHER`,
  `MP_TERMINAL_ROUND`, `MP_STAND_TERMINAL` or `MP_STAND_PIER`. Note the last two are *also*
  stands, so they satisfy this without spending a non-taxiable tile.
- `MS_MISSING_LANDING_RUNWAY` / `MS_MISSING_TAKEOFF_RUNWAY` — a contiguous **≥6-tile large
  runway** with the matching flag. Five tiles is not enough.

Miss any and fast jets take the elevated overrun crash roll regardless of the "plane crashes"
setting. Any AI flying big planes must treat this as a hard constraint.

### Everything else

- **Town authority** gates every *new* tile; replacing a piece inside an existing modular
  airport does not. A town will not take more than two airports when
  `station_noise_level` is off and tolerance is not permissive — this bit the regression test,
  and it will bite the AI's site search.
- **Noise** is recomputed from the would-be piece set and checked against the town.
- **Joining** a station that already has a non-modular airport fails.
- **Station spread** applies to the modular bounding box.
- **Replacing**: only `MP_GRASS`/`MP_EMPTY` are freely overwritable; hangar→hangar is allowed.
  Anything else needs clearing first, and `EnsureNoVehicleOnGround` applies — which matters
  when upgrading a live airport.
- **Reservation classes drive throughput**: `RUNWAY` (atomic per contiguous runway), `ONE_WAY`
  (per-tile queue), `FREE_MOVE` (atomic per segment). A single wide apron is one atomic
  segment and serialises everything through it. See `skills/reservations-design.md`.
- Hangars work through the normal API (`AIAirport.GetNumHangars`, `GetHangarOfAirport`,
  `AIVehicle.BuildVehicle` at a hangar tile), and `AIStation.GetStationCoverageRadius` works
  post-build.

---

## 4. The strategic thesis

The reason a modular-airport AI is interesting is not prettier airports. It is that **modular
airports grow**.

A stock-airport AI faces a step function: to go from Small to City it must demolish, lose the
station, lose the catchment, and rebuild. Every existing AI therefore over- or under-builds. A
modular AI can:

1. Open a route with a minimal cheap airport (one short runway, two stands, one hangar).
2. Add stands as waiting passengers grow.
3. Extend a busy runway, then add a second when the single runway still saturates.
4. Atomically pave a whole old runway and replace its small hangar, then add the tower and big
   terminal that make the result large-safe.
5. Add one-way taxiways and extra aprons to break up reservation contention as the fleet grows.

Each step is cheap, incremental, and never loses the station. Maintenance is points-derived, so
the AI pays only for what it built — a real economic advantage over the stock ladder, and one
`GetModularLayoutMonthlyMaintenanceCost` lets it quantify before committing.

This should be the AI's identity. Design the layout generator around growth stages, not around
one-shot perfection.

---

## 5. How AAAHogEx and ChooChoo build train lines

Read from `~/Documents/OpenTTD/content_download/ai/`. They are the two ends of the design
space. **Neither does anything comparable to designing an airport** — HogEx's `air.nut` is a
68-line table mapping population to stock airport type, which was the entire state of the art
because until modular there was nothing to decide. Their *rail* patterns are the transferable
part.

### ChooChoo — blueprints in local coordinates + a task tree

~7.9k lines.

- **A `Task` tree with rollback.** `Task.Run()` walks `subtasks`; `Task.Failed()` recursively
  fails everything completed and demolishes it (`task.nut`). Control flow is exceptions:
  `TaskRetryException`, `NeedMoneyException`, `TooManyVehiclesException`,
  `TaskFailedException`. The main loop pops one task per iteration and dispatches on the
  exception type (`main.nut:110`).
- **Fixed blueprints in local coordinates through a rotation.** `BuildTerminusStation`
  (`builder_stations.nut:305`) is literally a script:
  ```squirrel
  BuildSegment([0, p], [0, p+1]);
  BuildRail([1, p-1], [1, p], [0, p+1]);
  BuildDepot([2,p], [1,p]);
  ```
  `Builder` holds a `rotation`; `GetTile([x,y])` maps template space to the map. `Failed()`
  demolishes the whole rectangle.
- **Site search by valuator chain.** `FindStationSite` (`builder.nut:47`) filters an
  `AITileList`: belongs to the town → accepts passengers → produces cargo → fits a buildable
  rectangle → not across a lake → closest to centre.
- **Topology as a network of standard parts** — `BuildCrossing`, `ConnectStation`,
  `ExtendCrossing`. The pathfinder only fills track between known endpoints.

### AAAHogEx — economics first, geometry declarative

~37.5k lines.

- **An estimator drives everything** (`estimator.nut`, 2.5k lines). Routes are chosen by
  expected value; infrastructure follows.
- **Station factories generate and score candidates.** `RailStationFactory.CreateBest`
  (`station.nut:1313`) sweeps platform lengths downward, generating candidates per length,
  scoring with `GetBestHgStationCosts`, taking the best.
- **Geometry as data, not imperative calls.** `TerminalStation.GetRails()` returns
  `[[x1,y1],[x2,y2],[x3,y3]]` triples parameterised by `platformNum`/`platformLength`
  (`station.nut:4430`). That is what makes layouts parameterisable rather than hand-written
  per size.
- **Test mode everywhere** — `AITestMode()` probes before `AIExecMode()` commits.
- **A persisted world model** — `HgStation.SaveStatics/LoadStatics` serialises every built
  station well enough to rebuild the object graph on load.

### What carries over

| Pattern | Source | Why it matters here |
|---|---|---|
| Parameterised geometry as data | HogEx `GetRails()` | The layout array *is* this format. Families parameterised by stand count / runway length beat hand-written layouts. |
| Candidate generation + scoring | HogEx `CreateBest` | Site selection is the same shape, and the preview API makes scoring free. |
| Task tree with `Failed()` rollback | ChooChoo | Needed for the tile-by-tile growth path; the atomic layout submit makes it unnecessary for new airports. |
| Exception-driven main loop | ChooChoo | Airports are expensive; wait-for-money is directly reusable. |
| Valuator-chain site search | ChooChoo | Same idea, but over one-level regions rather than rectangles (§3). |
| Persisted world model | HogEx | The growth stages (§4) only work if the AI remembers what stage each airport is at. |

---

## 6. Generating a layout

Three options, increasing ambition. **Recommendation: A now, C only if A proves limiting.**

### A. Parametric blueprint families *(recommended)*

A handful of hand-designed *families*, each a function of a few parameters emitting a tile
grid — HogEx's `GetRails()` idea applied to airports.

```
GenerateLayout(family, {runway_length, stands, runways, hangars, large_safe, helipads})
    -> flat layout array
```

Families worth having:

1. **Legacy strip** — three looks sampled evenly: a minimal 4–5-tile strip, a conventional
   6–8-tile strip, or a compact 4×3 strip with stands directly against the runway and the old
   three-tile terminal behind them. Buildable from year zero, which matters: modern pieces are
   gated to 1955.
2. **Linear** — 1 large runway ≥6, a parallel apron spine and a service row containing the
   hangar, stands, tower and terminal.
3. **Pier** — 1 large runway ≥6 and parallel apron, with a perpendicular finger carrying
   stands on one or both sides and the hangar at its tip.
4. **Dual** — parallel landing and takeoff runways, two apron spines and a central service row.
5. **Apron** — 1 large runway ≥6 with one or two open apron rows holding stands; particularly
   tolerant of irregular sites because most of the apron block is optional.
6. **Heliport** — a central apron spine with helipads on one or both sides and a hangar at the
   end; no runway.

Scale-two and scale-three modern families commonly add one or two optional full-size terminal
buildings beyond the terminal required for jet safety. They cost normal upkeep and are the first
things the terrain fitter may drop on cramped ground.

After placing functional and decorative pieces, the generator fills unused cells inside the
layout's bounding rectangle with optional `MP_EMPTY` airport ground. This gives an unobstructed
site a coherent rectangular footprint and reserves space visually, while the terrain fitter may
drop any of those empty tiles around buildings, water or uneven ground. Empty infill is ignored by
layout scoring, so it never outranks a more useful runway, stand or terminal.

Each family is written once, verified by hand in-game, then scales by parameter. Rotation is
free — the layout command rotates the whole thing, and the codebase already handles the
awkward cases (hangar direction convention `0=SE, 1=NE, 2=NW, 3=SW`; legacy small runway
NEAR/FAR swap on odd quarter-turns — see `CLAUDE.md`).

Growth stages fall out naturally: each family declares which parameters can grow in place and
which tiles the additions occupy, so the AI can reserve room at first build.

### B. A plus site-aware patching

When a few tiles of the family's bounding box fall outside the site's one-level region, shift
the runway, shorten the pier or drop cosmetic tiles rather than rejecting the site. This is
where the arbitrary-footprint advantage is actually cashed in, and it needs no search.

### C. Constructive search

Place a runway to maximise usable length within the site, then greedily grow taxiway/apron/
stand structure, scoring with the preview API. Elegant, and much more likely to produce
something subtly non-functional — a layout that scores well but has an unreachable stand.
Needs the connectivity check the API does not provide. Only worth it once A works.

**Design the families with `skills/reservations-design.md` open.** Layout shape *is*
throughput: a wide open apron is one atomic `FREE_MOVE` segment serving one aircraft at a
time, while long thin one-way taxiways queue per tile and pipeline.

---

## 7. Structuring the AI

As built, in `ai/ModularAirportAI/` (copied into `build/ai/` by `scripts/run_ai.sh`):

```
info.nut      AIInfo registration and settings (max_airports, variety, selftest)
main.nut      AIController: the loop, budget policy, town choice, fleet sizing
util.nut      Grid, piece predicates, Grid.Rotate, ValidateGrid (connectivity)
layout.nut    the six families of section 6 - pure functions, no world access
fit.nut       FitGridToMask: trim a layout to the ground that exists; ScoreGrid
sites.nut     terrain scan, candidate generation, tiered family fallback
build.nut     placement, pre-build revalidation, read-back dumps
grow.nut      live growth: legacy upgrades, runway extension, stands/runways on demand
fleet.nut     engine choice gated on airport capability, orders, retirement
selftest.nut  offline dump of every family plus five awkward site masks
```

There is no `task.nut` and no `world.nut`, and both omissions are deliberate. ChooChoo's task
tree exists to roll back a half-built structure; `PlaceModularAirportLayout` is atomic, so new
airports never need it, and growth adds one independently-safe tile at a time. HogEx's
persisted world model is likewise unnecessary while everything the AI reasons about - which
airports exist, what pieces they hold, whether they are large-safe - is re-read from the map
each pass. That trade costs API calls per iteration and buys immunity to the save/load and
suspension bugs a cached model invites.

Two things that bit, and how they are handled:

- **Suspension.** Every command suspends the script, so a site search spans *months* of game
  time and the world moves under it. The terrain scan a search runs on is stale by the time it
  picks a winner — a town can build a house on the chosen ground meanwhile — so `BuildSite`
  re-checks every tile immediately before committing. Re-reading thirty tiles is far cheaper
  than a failed build, which costs the attempt and teaches nothing.
- **Cost.** Terrain lookups dominate the search, and done naively they cost tens of thousands
  of suspending API calls per town, which is game *years*. `ScanRegion` reads each town's
  neighbourhood once into a table and the inner loop becomes table lookups. Watch the region's
  radius: it must cover where the *layouts* reach, not just where their origins sit, or every
  tile past the edge reads as unbuildable and nothing ever fits.

---

## 8. Milestones

M0–M4 are **done**, and M6 landed early because the fitter was the natural way to make one
family serve many sites.

| # | Deliverable | State |
|---|---|---|
| M0 | API reachable end to end | done |
| M1 | Atomic layout submit + previews | done |
| M2 | `layout.nut` families as pure functions | done — six families, verified by `selftest` |
| M3 | Site search + build + aircraft flying a route | done |
| M4 | Growth on a live airport | done — legacy upgrades, runway extension/addition, stands on demand |
| M5 | Multi-airport network, fleet and money management | partly — works, but see below |
| M6 | Non-rectangular footprint fitting | done — `FitGridToMask`, reported as `trimmed=` |

What M5 still wants:

- **Save/load.** `Save()` returns an empty table. Everything the AI needs is currently
  re-derived from the map each pass, which is why that is survivable, but the failed-site
  blacklist is lost on load.
- **Contention shaping.** No one-way taxiways are ever set. `skills/reservations-design.md`
  says layout shape *is* throughput — a wide apron is a single atomic segment — and nothing in
  the AI acts on that yet.
- **Competition.** Untested against other AIs on the same map; `PickUnservedTown` has the
  randomisation to avoid fighting over sites but this has never been observed.

---

## 9. Testing

- **Layout generator**: pure functions. Dump generated grids as template JSON and run
  `scripts/parse_airport_template.py --grid/--detail/--runways` — the visualiser already
  speaks that format.
- **API-level behaviour**: the `--AIAirport Modular:*` sections in
  `regression/regression/main.nut` already cover build, inspect, preview, the error paths and
  the one-height-level rule, diffed against `result.txt` by `make regression`. Extend them
  when the API grows.
- **AI behaviour**: headless, like the throughput harness —
  `./build/openttd -d script=2 -x -g <save> -s null -m null -v null:ticks=N`. Use the existing
  `[AirportStats]` logging to compare throughput of AI-built airports against the hand-built
  ones in `scripts/testdata/`.
- **Don't regress** `scripts/regression_test.sh` when touching anything shared.

A known coverage gap: the regression savegame sits in **1954**, so the year gate is exercised
but no test actually *builds* a modern piece — a built airport reporting `MS_OK` is unproven.
Closing it needs a savegame set after 1955 with an AI configured in it, since a savegame
records which AI to run.

---

## 10. Open questions

1. **How much does the arbitrary-footprint edge actually buy?** Worth measuring before
   building M6 for it: on a few representative maps, count sites where a non-rectangular
   one-level region admits a viable layout but no stock rectangle fits. If the answer is
   "rarely", M6 drops down the list.
2. **Should the AI ever use `CMD_BUILD_MODULAR_AIRPORT_FROM_STOCK`?** Not currently exposed.
   It is a legitimate bootstrap — build a stock layout as modular, then grow it — and would
   give M3 a working AI before the generator is trustworthy. Cheap to add if wanted.
3. **Noise budget across growth stages.** Noise is layout-derived and the town can refuse. The
   preview API makes it checkable up front, but the growth path must re-check at every stage,
   and possibly plan the *eventual* maximum size at first build rather than discovering the
   ceiling later.
4. **Competing with itself.** Two instances on one map will want the same sites, and a town
   takes only two airports. HogEx's `ngStationTiles` blacklist is the cheap fix.
