# Working on OpenTTD (macOS)

The fork lives at https://github.com/TorKlingberg/OpenTTD and an authenticated `gh` CLI is available. `gh repo set-default` has pinned it to the fork, so bare `gh run`/`gh release` calls work; the `upstream` remote (`OpenTTD/OpenTTD`) needs an explicit `-R`.

## Build

The build directory is `OpenTTD/build/`. To rebuild after source changes:

```bash
/Users/tor/ttd/OpenTTD/scripts/build_and_sign.sh
```

Equivalent manual command (if needed):
```bash
make -j8 -C /Users/tor/ttd/OpenTTD/build && codesign -s - --deep --force --entitlements /Users/tor/ttd/OpenTTD/scripts/debug.entitlements /Users/tor/ttd/OpenTTD/build/openttd
```

If you need to reconfigure (e.g. after cmake file changes):
```bash
cd /Users/tor/ttd/OpenTTD/build
cmake .. \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_OSX_SYSROOT=/Library/Developer/CommandLineTools/SDKs/MacOSX15.4.sdk \
  -DCMAKE_CXX_FLAGS="-I/Library/Developer/CommandLineTools/SDKs/MacOSX15.4.sdk/usr/include/c++/v1" \
  -DCMAKE_OBJCXX_FLAGS="-I/Library/Developer/CommandLineTools/SDKs/MacOSX15.4.sdk/usr/include/c++/v1"
```

For a debug build (slower, full symbols, unoptimised):
```bash
cmake .. -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_CXX_FLAGS="-I/Library/Developer/CommandLineTools/SDKs/MacOSX15.4.sdk/usr/include/c++/v1" \
  -DCMAKE_OBJCXX_FLAGS="-I/Library/Developer/CommandLineTools/SDKs/MacOSX15.4.sdk/usr/include/c++/v1"
```

**Common build failures:**
- `algorithm file not found` — missing `-DCMAKE_CXX_FLAGS` above

## Before Committing

Do go ahead and commit if you are confident about a change. No need to wait for my approval.

Run `scripts/regression_test.sh` — the bare run, well under a minute; see Regression Testing below for what it checks and when `--full` is needed. Also run `scripts/multiplayer_desync_test.sh` after changes affecting modular path serialization, network joins, or save/load state.

Also ask a subagent to review your change before comitting. Run this in parallel with the regression test.

The official `OpenTTD-git-hooks` are installed in `../openttd_hooks` and linked into `.git/hooks`; the `pre-commit` hook checks the staged diff automatically. (Committing from a worktree needs `HOOKS_DIR` — see Git Worktrees.) After staging the intended changes, run the remaining checks:

```bash
python3 .github/file-descriptions.py <(git diff --cached --name-only) &&
python3 .github/script-missing-mode-enforcement.py &&
cmake --build build --target openttd_test -j8 &&
./build/openttd_test &&
cmake --build build --target regression -j8
```

The last line is the NoAI script regression, **not** covered by `openttd_test`: it replays
`regression/regression/main.nut` and diffs against the committed
`regression/regression/result.txt`. Regenerate that file from
`build/regression_regression_output.txt` rather than hand-editing it, and use the `regression`
build target rather than bare `ctest` — see `skills/regression_testing.md` for both.

## Git Worktrees

Worktrees live under `.claude/worktrees/<name>` and share the main checkout's `.git`.

**Building and running:**
- Each worktree has its own `build/`, and it goes stale independently — check `build/openttd`'s mtime before trusting it; if stale, just run the main checkout's binary instead of rebuilding. A worktree that has never been built needs a full `cmake` configure plus a from-scratch compile (~25 min), so prefer the main checkout for anything that does not need the worktree's own edits compiled.
- A fresh worktree `build/baseset/` has only the bundled files — the **original TTD graphics are missing** (`TRG1.GRF`, `TRGT.GRF`, `TRGC.GRF`, `TRGH.GRF`, `TRGI.GRF`, `TREND.GRF`, `TRHCOM.GRF`, `TRTITLE.GRF`, `GM-TTO.CAT`). Any save using the original base set then blocks at startup on a missing-graphics prompt. It does not look like a failure: the process sits at **0% CPU with no `[AirportStats]` output**, which reads exactly like a slow or paused fixture, and a regression run hangs indefinitely rather than erroring. Copy them in once per worktree:

```bash
cp -n /Users/tor/ttd/OpenTTD/build/baseset/*.GRF /Users/tor/ttd/OpenTTD/build/baseset/*.CAT <worktree>/build/baseset/
```

- `build/ai/<Name>` is a symlink into the main checkout's `ai/<Name>`, not worktree-relative. To headless-test a worktree's edited AI script, copy it into a scratch dir under a different registered name (edit `GetName`/`GetShortName`/`CreateInstance` in `info.nut` and the class name in `main.nut`) rather than repointing the shared symlink.

**Committing:** the pre-commit hook resolves its helpers from `git rev-parse --git-dir`, which inside a worktree is `.git/worktrees/<name>` — a directory with no hooks in it, so the commit aborts with `check-diff.py: No such file or directory`. Point it at the real hooks instead of skipping them with `--no-verify`:

```bash
HOOKS_DIR=/Users/tor/ttd/OpenTTD/.git/hooks git commit -F <message-file>
```

**Merging back to master:** always fast-forward (`--ff-only`), never a merge commit. A branch checked out in another worktree (e.g. `master`) can't be merged into from here — commit in this worktree, then run the merge from the main checkout:

`--ff-only` fails if master has moved since the branch was cut — rebase the branch onto master first, then fast-forward.

## Debugging

The main runtime log is `/tmp/openttd.log`.

## Coordinate System

OpenTTD uses an isometric view. Tiles are on a rectangular (X, Y) grid; each tile is `TILE_SIZE` = 16 pixel-units wide.

`RemapCoords` (`src/landscape.h`) projects world to screen as `screen_x = (y - x) * 2` and `screen_y = y + x - z`, so **+X goes down-left and +Y goes down-right**:

| Coordinate change | `DiagDirection` | Screen appearance |
|---|---|---|
| X increases | SW | moves left-down diagonally |
| X decreases | NE | moves right-up diagonally |
| Y increases | SE | moves right-down diagonally |
| Y decreases | NW | moves left-up diagonally |

Swapping the two axes here mirrors everything and is easy to miss, because a mirrored model stays self-consistent. Check against `RemapCoords` and `_tileoffs_by_diagdir` (`src/map.cpp`) rather than intuition.

In prose prefer screen-relative terms (up/down/left/right) over compass names, since all axis-aligned moves appear diagonal on screen. See `coords.md` for details.

---

# Modular Airports

The modular airport system lets players build airports tile-by-tile. `modular_airports.md` is
the implementation guide — data model, aircraft flow, commands, routing, save/load, rotation
invariants — and `skills/reservations-design.md` is the authoritative reservation design. This
file carries only what governs day-to-day work.

## Regression Testing

`scripts/regression_test.sh` runs headless 5-year simulations of the fixtures in
`scripts/testdata/` and checks each one two ways: total airport movements against the
`min_movements=` floor in `scripts/testdata/*.expected`, **and** the run's log against the
script's `FAILURE_PATTERNS` — should-never-happen lines such as `landing-chain-invariant` and
`[AircraftLost]`. Ordinary contention (`stuck(reserve)`, `retarget failed`, …) is deliberately
not gated.

- Bare run: `T5j2`, `mass7-inair`, `helis2` concurrently, under a minute apiece — the right
  check for almost everything.
- `--full` adds `T7d` (~13 min, nearly all of it `T7d`). `T7d` is the only fixture with genuine
  route diversity, and the only probe of the wait-don't-downgrade tier, so routing and
  ground/taxi pathfinding work needs it; it is not a per-commit gate.
- Any two suite runs collide over the per-fixture log paths, which are keyed by fixture name
  under `/tmp` regardless of worktree — pass `--log-dir` to whichever is the guest.

Run after any change to reservation, pathfinder, or movement code. Bump a committed floor only
when the drop is intentional and justified. What each fixture covers, how to compare two runs,
the floor history, and the NoAI script regression are in `skills/regression_testing.md`.

## Unit Testing

Modular airport logic is verified by unit tests in `src/tests/test_modular_airport.cpp`. These cover pure logic (classification, rotations), map-dependent helpers, ground pathfinding (including stand avoidance), and reservation invariants.

Run all unit tests:
```bash
/Users/tor/ttd/OpenTTD/build/openttd_test
```

Run only modular airport tests:
```bash
/Users/tor/ttd/OpenTTD/build/openttd_test "ModularAirport*"
```

## Key Source Files

| File | Purpose |
|------|---------|
| `src/modular_airport_cmd.cpp` | Modular movement, reservation, landing/takeoff selection, and layout-derived properties (noise, catchment, maintenance, safety). ~3900 lines. |
| `src/modular_airport_holding.cpp` | Dubins holding loop, approach geometry, computed helicopter landing/takeoff/service tiles. |
| `src/modular_airport_build.cpp` | `CmdBuildModularAirportTile`, `CmdBuildModularAirportFromStock`, `CmdUpgradeModularAirportTile`, `RemoveModularAirportTile`. |
| `src/modular_airport_template_cmd.cpp` | `CmdSetRunwayFlags`, `CmdSetTaxiwayFlags`, `CmdSetModularAirportEdgeFence`, `CmdPlaceModularAirportTemplate`. |
| `src/modular_airport_draw.cpp` | Sprite layout overrides, hangar layouts, perimeter fences, direction overlays. |
| `src/modular_airport_cmd.h` | Declarations + inline helpers (`IsModularRunwayPiece`, `IsRunwayPieceOnAxis`, MGT_* constants). |
| `src/modular_airport_gui.cpp` | Modular airport builder UI (`BuildModularAirportWindow` + hangar/cosmetic/helipad pickers, fence and upgrade tools, info overlays). |
| `src/modular_airport_gui.h` | `ShowBuildModularAirportWindow` + shared GUI globals. |
| `src/aircraft_cmd.cpp` | Classic FTA state machine, event handlers, shared mechanics (`UpdateAircraftSpeed`, etc.). |
| `src/aircraft.h` | Aircraft struct. Modular fields are under the `Modular airport ground pathfinding` comment block. |
| `src/airport_ground_pathfinder.cpp` | A* ground pathfinder + segment classification |
| `src/airport_ground_pathfinder.h` | `TaxiPath`, `TaxiSegment`, `TaxiSegmentType`, `BuildTaxiPath` |
| `src/base_station_base.h` | `ModularAirportTileData` struct (per-tile metadata) |
| `src/station_map.h`, `src/modular_airport_cmd.cpp` | Reservation flag helpers plus modular reservation owner helpers (`GetModularAirportTileReservationOwner`, etc.) |
| `src/table/airporttile_ids.h` | `AirportTiles` enum: `APT_STAND`, `APT_APRON`, `APT_RUNWAY_*`, `APT_DEPOT_*`, etc. |
| `src/station_cmd.h` | Command declarations + `DEF_CMD_TRAIT` registrations (`Commands::BuildModularAirportTile` etc.), `ModularTemplatePlacementData`. |
| `src/station_base.h` | `Airport`'s modular tile vector, index, and the layout-derived caches + `MarkLayoutDirty`. |
| `src/airport.h` | `AT_MODULAR`, `AirportBlock::Modular`, `ModularHoldingLoop`, holding constants. |
| `src/script/api/script_airport.hpp` | NoAI modular query/build API (`ModularPiece`, `MLF_*` layout arrays, `PlaceModularAirportLayout`). |
| `src/airport_gui.cpp` | Shared airport toolbar + classic FTA airport picker UI |
| `scripts/parse_airport_template.py` | Visualize template JSON files (`--grid`, `--detail`, `--runways`, `--raw`). |

## Skills (Brief)

- `skills/creating_new_graphics.md` — create native 8bpp world art, preserve palette semantics, register it in `openttd.grf`, and verify it across base graphics sets.
- `skills/adding_a_new_sprite.md` — add and register a GUI sprite in the OpenTTD extra base graphics.
- `skills/lldb_debugging.md` — LLDB attach/run workflows and modular runtime log commands.
- `skills/lldb_game_state_inspection.md` — read live game state (pools, towns, stations) from a running game via batch LLDB + Python, no rebuild needed.
- `skills/gui_screenshot_verification.md` — see a GUI change: scratch instance under a chosen base set, windows opened from LLDB, game screenshots itself. Use for any drawing question, especially base-set-specific ones.
- `skills/stuck_plane_debugging.md` — detailed stuck-plane diagnosis playbook.
- `skills/crash_debugging.md` — crash log and stacktrace triage steps.
- `skills/airport_template_analysis.md` — template JSON analysis/visualization workflow.
- `skills/regression_testing.md` — the airport throughput suite in full: fixture coverage, comparing two runs, floor history and what the A* heuristic swap really cost, plus the NoAI script regression and its `ctest` trap.
- `skills/performance_profiling.md` — macOS `sample` profiling + `quick_test.sh`/`regression_test.sh` validation.
- `skills/desync.md` — multiplayer desync checklist for game logic, save/load, caches, and movement changes.
- `skills/modular_editor.md` — builder UI, stock-to-modular conversion, and piece availability gating.
- `skills/savegame_fixture_resave.md` — `scripts/resave.sh`: migrate savegames to the current format without advancing the sim.
- `skills/reservations-design.md` — segment types, safe-stop invariant, reservation lifecycle, and entry-contract pitfalls.

## Tile Classification

Every taxiable tile is one of three types used by the segment reservation system:

| Type | Condition | Reservation |
|------|-----------|-------------|
| `Runway` | `IsModularRunwayPiece(piece_type)` — `APT_RUNWAY_1-5`, `APT_RUNWAY_END`, `APT_RUNWAY_SMALL_*` | Crossing: traveled tiles only; explicit landing/takeoff: entire contiguous runway |
| `OneWay` | `IsTaxiwayPiece(piece_type) && one_way_taxi == true` | Safe queue boundary in the forward reservation horizon |
| `FreeMove` | Everything else (aprons, stands, hangars, fenced apron variants) | Traveled tiles through the forward reservation horizon |

Notes:
- Runway end fence variants (`APT_RUNWAY_END_FENCE_*`) are **not** in `IsModularRunwayPiece` — they're decorative. Only `APT_RUNWAY_END`, `APT_RUNWAY_SMALL_NEAR_END`, `APT_RUNWAY_SMALL_FAR_END` are landing targets.
- Hangars: `APT_DEPOT_SE/SW/NW/NE` (large) and `APT_SMALL_DEPOT_SE/SW/NW/NE` (small) — four rotations each. Hangars are multi-capacity (multiple aircraft can park in one).
- One-way flags only apply to `IsTaxiwayPiece` types. Stands, hangars, and runways cannot be one-way.
- Reservation, retention, and landing admission use the same forward horizon to the aircraft's goal or first future safe stop. Segment boundaries do not define separate acquisition rules.
- Two structural invariants hold for every contiguous runway, and landing/takeoff eligibility code depends on both:
  - **Both extremities are end pieces.** `NormalizeRunwaySegmentVisuals` recanonicalizes the whole segment on every placement, removal and upgrade (stock conversion does the same inline), so extending a runway caps the new extremity and demotes the old cap to a middle piece. A runway with a bare `APT_RUNWAY_5` at an extremity is not reachable through the build commands.
  - **Exactly one direction bit is set.** `SetRunwayFlags_Check` rejects zero-mode and non-single-direction flags; `NormalizeModularRunwayFlags` canonicalizes template values. So of a runway's two ends, exactly one is a legal landing end — landing at the low end rolls toward high and needs `RUF_DIR_HIGH`, and vice versa.

## Aircraft Crashes (modular)

- `MaybeCrashModularAircraft(v, st)` (in `aircraft_cmd.cpp`) is the modular crash entry. It calls the pure predicate `ModularAircraftHasElevatedOverrunRisk(v, st)`, then `RollAirplaneCrashCheck`. Helicopters never crash via this path (early return).
- **Elevated overrun risk** = `AIR_FAST` jet **and** `!_cheats.no_jetcrash.value` **and** airport is not large-safe (`!ModularAirportSupportsLargeAircraft(st)`). It ignores the "Plane crashes" setting, matching the stock short-strip overrun (prob 3276). Otherwise the general roll `(0x4000 << plane_crashes)/1500` applies — no crash when `plane_crashes == 0`.
- The roll consumes the synced game `Random()` (not `_interactive_random`), so the RNG-consumption count must stay client-independent for MP determinism.

## Saveload

Per-tile modular metadata is primary state and is saved as such. Everything derived is saved
too when it can change what the simulation does: the reservation vectors and the `MACP`
crossing cache because map-level reservations are multiplayer game state,
`modular_holding_wp_index` because it moves the aircraft, and both cached paths (`taxi_path`,
`landing_chain_path`) because recomputing a route against newer reservation state can pick a
different one than the server did.

**Nothing of this fork's goes into `SaveLoadVersion`** — that enum is upstream's and is merged
verbatim. Fork features are versioned on their own axis in the `XVER` chunk
(`src/saveload/extended_version_sl.h`) and gated by feature, not by version:
`IsModularAirportSaveFeaturePresent()`. Full detail, including the field-by-field list, is in
`modular_airports.md` (Save/Load).

## Modular Airport Invariants

- Any state that affects aircraft movement, reservations, or path choices must be saved or deterministically rebuilt on load.
- Airport tile `m7` is animation frame storage. Modular reservation ownership belongs in `ModularAirportTileData::reservation_owner`; map `m6` bit 2 is only the reservation-present flag.
- Template placement must preflight the whole final placement before executing. A failure after preflight is an internal bug path, not an acceptable partial build.
- Modular airport creation/removal must keep stock airport town-noise and two-airport local-authority accounting semantics for the selected airport type.
- Changes to movement, reservation, build atomicity, or airport accounting need save/load, multiplayer determinism, and regression coverage checks.

## Common Pitfalls

- `GetModularTileData(tile)` returns `nullptr` if the tile isn't in the modular layout — always null-check.
- Building a piece is gated twice — the availability year, then `station.new_airport_graphics` for the pieces drawn from this fork's own bitmaps (decorations, the small hangar's closed-back views). Ask `GetModularPieceUnavailableReason(piece, rotation)`, never without a rotation and never by re-deriving it: `BuildModularAirportTile_Check`, the builder's greyed-out buttons and the script API all go through it so they cannot disagree. `CmdUpgradeModularAirportTile` is the one exception — its own inline check is year-only. Runtime mirrors of base-set sprites are deliberately not gated. `_settings_game` is all-false in the unit tests, so a test that builds a decoration must set the flag itself. See `skills/modular_editor.md`.
- Layout-derived answers (catchment, noise, hangar presence, accepted aircraft types, large-safe runways, holding loop, heli tiles) are cached on `Airport`, and `MarkLayoutDirty()` is the only route a layout mutation may use. Any code that mutates `ModularAirportTileData` directly instead of going through the commands — tests especially — must call it, or the cached answer silently stays stale; when retyping a tile, mark dirty *after* the retype, since marking before it leaves a window where a read caches a pre-normalization answer. See `modular_airports.md` (per-airport modular state).
- `FindAirportGroundPath` with `v=nullptr` ignores stand occupancy (topology only); with `v=aircraft` avoids occupied stands that aren't the goal.
- Path cost has a non-goal stand/parking penalty (`+5`), so routes may prefer slightly longer taxiways over cutting through stands.
- Ground routes are riddled with cost ties, and the A* expansion order silently decides which one wins — on at least 43% of routable queries there is an equal-cost, equal-length alternative. That choice is a throughput lever worth ~18% of `mass7-inair` between its best and worst setting, and nothing pulls it deliberately — see `skills/regression_testing.md`.
- Runway flags (`RUF_LANDING`, `RUF_TAKEOFF`, `RUF_DIR_LOW`, `RUF_DIR_HIGH`) propagate to all tiles in a contiguous runway via `CmdSetRunwayFlags`.
- `AirportTiles` IDs `>= NEW_AIRPORTTILE_OFFSET` (74) are treated as NewGRF airport tiles. Do not store new modular default-tile IDs in map gfx; keep canonical gfx IDs and branch drawing from modular metadata.
- Depot windows can outlive tile deletion. Guard depot UI reads with a valid depot tile check.
- Rotation invariants for modular airports:
  - Hangar directional convention is `0=SE, 1=NE, 2=NW, 3=SW` (clockwise in world space). Keep this mapping consistent across build/rotate/draw/pathfinder code.
  - For preview projection, `iso_x` must use `(dy - dx)` (not `(dx - dy)`), otherwise the entire preview is mirrored.
  - Legacy small runway ends (`NEAR`/`FAR`) swap on odd quarter-turns; after template rotation, preview should normalize each contiguous segment so low-end is `FAR` and high-end is `NEAR`, matching placed tiles.

## GUI Pitfalls

The builder's cursor-ownership traps (`PickerWindowBase::Close` resetting the parent's
placement, `SetObjectToPlace` firing `OnPlaceObjectAbort`, `CloseWindowByClass` chains) and
sub-tile click positions via `_tile_fract_coords` are in `modular_airports.md` (GUI pitfalls);
piece availability gating is in `skills/modular_editor.md`.

## Holding Loop Pitfalls

- Don't use `tick_counter` or `running_ticks` for phase timing — both are `uint8_t` and wrap at 256. Use `TimerGameTick::counter` (uint64_t monotonic).
- Movement must be unconditional — don't guard `UpdateAircraftSpeed` inside `if (dist > 0)`.
- Use ghost for movement, nearest-waypoint only for gate checks.
- Reset `modular_holding_wp_index` to `UINT32_MAX` on landing commit.
- `AircraftEventHandler_Flying` (`aircraft_cmd.cpp`) picks the modular landing target and sets `VehicleAirFlag::HelicopterDirectDescent` when `state == HELILANDING`. Helipad-specific overrides (like skipping the FAF approach) belong there at landing commit, not in the movement code (`AirportMoveModularLanding`).
