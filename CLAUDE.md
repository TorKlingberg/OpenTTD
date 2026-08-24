# Working on OpenTTD (macOS)

The fork lives at https://github.com/TorKlingberg/OpenTTD and an authenticated `gh` CLI is available. `gh repo set-default` has pinned it to the fork, so bare `gh run`/`gh release` calls work; the `upstream` remote (`OpenTTD/OpenTTD`) needs an explicit `-R`.

## Build

The build directory is `OpenTTD/build/`. To rebuild after source changes:

```bash
/Users/tor/ttd/OpenTTD/scripts/build_and_sign.sh
```

Equivalent manual command (if needed):
```bash
make -j8 -C /Users/tor/ttd/OpenTTD/build && codesign -s - --deep --force /Users/tor/ttd/OpenTTD/build/openttd
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
- `cannot find libatomic` — apply fix to `cmake/3rdparty/llvm/CheckAtomic.cmake`: change `if(MSVC)` to `if(MSVC OR APPLE)` at lines 52 and 75

## Before Committing

The official `OpenTTD-git-hooks` are installed in `../openttd_hooks` and linked into `.git/hooks`; the `pre-commit` hook checks the staged diff automatically. The `commit-msg` hook that enforced the `<keyword>: <Details>` subject format is disabled (renamed to `.git/hooks/commit-msg.disabled`); rename it back to re-enable. (Committing from a worktree needs `HOOKS_DIR` — see Git Worktrees.) After staging the intended changes, run the remaining checks:

```bash
python3 .github/file-descriptions.py <(git diff --cached --name-only) &&
python3 .github/script-missing-mode-enforcement.py &&
cmake --build build --target openttd_test -j8 &&
./build/openttd_test
```

Also run `scripts/regression_test.sh` after changes to modular airport reservation, pathfinder, or movement code. That is the bare run — the `T5j2` fixture only, about two minutes, and the right check for almost everything. Save `--full` for changes with a real risk of breaking ground/taxi pathfinding; it is not a per-commit gate.

Also ask a subagent to review your change before comitting. This can run in parallel with the regression test.

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

**Merging back to master:** always fast-forward (`--ff-only`), never a merge commit — this is a standing preference, not just a worktree workaround. A branch checked out in another worktree (e.g. `master`) can't be merged into from here — commit in this worktree, then run the merge from the main checkout:

```bash
cd /Users/tor/ttd/OpenTTD && git merge --ff-only claude/<branch>
```

`--ff-only` fails if master has moved since the branch was cut — rebase the branch onto master first, then fast-forward.

## Debugging

The main runtime log is `/tmp/openttd.log`.

Debugger/logging workflows are documented in the skills list below.

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

The modular airport system lets players build airports tile-by-tile. The reservation design is in `skills/reservations-design.md`.

## Regression Testing

`scripts/regression_test.sh` runs headless 5-year simulations and checks each fixture two
ways: total airport movements against the committed minimum (the `min_movements=` floor in
`scripts/testdata/*.expected`), **and** the run's log against the `FAILURE_PATTERNS` array in
the script — should-never-happen lines like `landing-chain-invariant`, both `[FALLBACK]`
markers, `invalid ground state` and `[AircraftLost]`. Ordinary contention
(`stuck(reserve)`, `runway-rest-invariant`, `retarget failed`, …) is explicitly not gated.
The log check exists because throughput hides small correctness faults: a few aircraft on a
broken path cost a handful of movements out of thousands, well inside the floor's headroom.
Fixtures run at `-d misc=2` so the `[FALLBACK]` markers are visible; that costs ~2.7x the log
volume and does not change any total.

| Invocation | Fixtures |
|---|---|
| `scripts/regression_test.sh` | `T5j2` only, ~2 min — the normal check, including before a commit |
| `scripts/regression_test.sh --full` | all four, ~13 min — only when a change could break taxi pathfinding |

Other flags: `--no-build` reuses whatever `./build/openttd` already is, `--sequential` runs the
fixtures one at a time, and `--log-dir DIR` moves the per-fixture logs off their default `/tmp`
paths.

**`T5j2` is the default because it is the cheapest**, at ~108s against `T7d`'s 784s. It is a
real player layout under sustained contention, so it exercises reservation and queuing hard,
but it is explicitly *not* the broadest fixture. What the default run cannot see:

- **Alternate routing.** `T7d` is the only fixture with genuine route diversity — two or more
  routes between the same endpoints — so route-selection work is invisible here.
- **The wait-don't-downgrade tier.** Also `T7d` only.
- **Helicopters.** `helis2` only.

So reach for `--full` when a change has a real chance of breaking ground/taxi pathfinding, and
always for routing work. It is not a per-commit gate; for ordinary changes the bare run is the
check.

Note also that `T5j2` has the loosest floor of the four on purpose (see the save/load note
below), so it is the least sensitive fixture to a small regression as well as the fastest.

**Fixtures run concurrently.** The sim is single-threaded and the fixtures share no writable
state, so `--full` costs roughly one fixture's wall time on a multi-core machine rather than
four — and since `T7d` is ~7x the others (784s vs 108/110/129s, measured 2026-08-23), `--full`
is essentially the cost of `T7d` alone. Each fixture's output is buffered and replayed in
fixture order after everything finishes, so the report never interleaves.

**Two concurrent *suite* runs still collide**, because the log path is derived from the fixture
name: the second run truncates the first one's log out from under it and both sets of
`[AirportStats]` lines are lost. That surfaces as "no countable years" and reads exactly like a
paused fixture. Pass `--log-dir` to whichever run is the guest. (This is real — it happened on
2026-08-23 with two agents driving the suite in the same tree at once.)

The fixtures:

- `scripts/testdata/T7d.sav` — route diversity: Pladingbury Airport has multiple paths off one landing runway under heavy arrival demand, Sledinghead Cross Airport has multiple paths to one takeoff runway, and other airports mix large and small runways. Busiest fixture (~7.4k movements/year), so with the fixtures running concurrently it is the one that sets the suite's wall time
- `scripts/testdata/T5j2.sav` — **the default fixture.** Real player layout under sustained contention (~26k `stuck(reserve)` reports over 8 years with no permanent stall); 16 modular airports, mixed fleet
- `scripts/testdata/mass7-inair.sav` — mixed fixed-wing throughput; every airport is large-safe
- `scripts/testdata/helis2.sav` — helicopter-heavy stress; every airport is large-safe

**Aircraft crashes are off in all four fixtures** (`vehicle.plane_crashes = 0`, set by
`scripts/disable_crashes.sh`), so which aircraft happen to die no longer perturbs a total. That
setting gates only the general per-brake-tick roll in `RollAirplaneCrashCheck`. The elevated
short-strip overrun for fast jets (fixed prob 3276) **ignores it** and is gated by the
`no_jetcrash` *cheat* instead, which no console command reaches and which would also change
which airports jets may be sent to — so it is left alone. `mass7-inair` and `helis2` are
entirely large-safe, so that path cannot fire there at all; `T5j2` and `T7d` mix runway sizes
and could in principle, but neither logs a crash today. `[AircraftLost]` at `misc=1` stays the
check: a fixture that starts logging one has reached the overrun path, not the general roll.

**`T5j2.sav` is still not flat** with crashes off: throughput declines ~3% across the window
from fleet ageing alone (1306 / 1297 / 1239 / 1235 / 1263 movements per counted year). It is
exact and reproducible, because the sim is deterministic for a fixed save plus tick count and
the window is always the same — but read a `T5j2` drop as "compared with the committed
baseline", not "compared with last year". Its value is contention coverage the other two do not
provide.

**`T7d.sav` is the route-diversity fixture**, and the reference for what a quiet fixture looks
like: it already had `plane_crashes = 0` when it was captured, so it was crash-free before the
other three were, and turning off crashes left its total bit-for-bit unchanged (27033 both
ways). Its four counted years are 6699 / 6796 / 6793 / 6745 — a 1.4% spread, with the dip in
the first counted year, so compare totals rather than reading one year against another. It is
the only fixture with genuine route diversity — two or more routes between the same endpoints —
so it is the one that can detect alternate-exit routing work at all. It also mixes large and
small runways without ever reaching the overrun path, because the strict large-runway
preference keeps fast jets off the short strips, which makes it the only probe of the
wait-don't-downgrade tier (`mass7-inair` and `helis2` are entirely large-safe).

**The fixtures have no AI companies.** `T5j2` and `T7d` carried three each, and which
airports an AI decides to build shifts with any change that consumes the synced `Random()`
differently — worth a couple of hundred movements with no routing cause. `scripts/strip_ai.sh
<save>` deletes every AI company from a save (taking its stations and vehicles with it) and
sets `difficulty.max_no_competitors = 0` so no replacements spawn during the run. It re-saves
through the same `game_start.scr` hook as `resave.sh`, with zero ticks simulated.
`mass7-inair` and `helis2` never had any.

`helis2.sav` was written by a build predating the modular touchdown clearing
`VehicleAirFlag::HelicopterDirectDescent`, so ~52 of its 90 helicopters carry a stale descent
flag — real pre-fix data for the touchdown clear to chew on. An earlier version of this file
warned never to re-save it because that would destroy the coverage. **That was wrong.**
`Aircraft::flags` is ordinary saved vehicle state (`SLE_CONDVAR(Aircraft, flags, ...)` in
`saveload/vehicle_sl.cpp`) and no afterload step clears the bit, so the flags round-trip. The
fixture was re-saved on 2026-08-23 to turn crashes off, which is also what re-baselined it.

The runner excludes the **first reported year** as a warmup (its length depends on the save's start date), so the saves count different calendar windows — that's expected.

**Test saves must be saved unpaused.** A paused save still consumes the tick budget while the game loop does nothing, so the run finishes in seconds having simulated nothing. `airport_stats_history.sh` now fails loudly when a run reports no countable years, because the raw result of a paused save is `movements=0`, which parses fine and would otherwise read as a total throughput collapse (or pass silently against a low floor). Check with `_pause_mode` in a debugger if a new fixture behaves oddly; the committed saves all read 0.

`n_years_plus2.sh` runs `scripts/build_and_sign.sh` before **every** fixture unless
`OPENTTD_SKIP_BUILD=1` is set. `regression_test.sh` builds once up front and sets that, both
because concurrent fixtures would otherwise run concurrent `make` invocations against the same
build directory, and because a per-fixture rebuild lets a mid-run source edit silently split
the result across two builds. That hazard is still live for anything that drives
`n_years_plus2.sh` or `airport_stats_history.sh` in a loop: the early fixtures measure the old
code and the later ones the new, nothing warns, and the totals look perfectly ordinary. Let
such a run finish, or kill it, before touching sources.

Batch runs log to `/tmp/openttd_regression_<save>.log`, one per fixture, overwritten each run (override with `OPENTTD_REGRESSION_LOG`). They deliberately do **not** use `/tmp/openttd.log`: that path belongs to the interactive runners, and a game started by `build_and_run*.sh` holds it open as its stdout for as long as it runs. Truncating it from a batch run does not move the live game's file offset, so the two interleave and the `[AirportStats]` lines get overwritten — which shows up as "no countable years" and reads exactly like a paused fixture. So a regression run is safe to start while a game is open, but read the `log:` path the runner prints rather than `/tmp/openttd.log`.

Run after any change to reservation, pathfinder, or movement code. Bump the committed minimum (in `*.expected`) only when the drop is intentional and justified.

### Comparing two runs

The sim is deterministic for a fixed save + tick count, so a single run is exactly
reproducible — but that does **not** make two runs of *different code* cleanly comparable.
Any change that shifts timing consumes the synced `Random()` differently, which changes:

- **which aircraft crash** — removed by setting `vehicle.plane_crashes = 0` in every fixture.
  While crashes were on, an airport served by three aircraft lost ~22% of its movements when
  one of them died, which is indistinguishable from a routing regression. `[AircraftLost]`
  logs every crash at `misc=1`; all four fixtures log zero, and one appearing means the
  elevated short-strip overrun fired, which that setting does not gate.
- **which airports the AI companies build** — removed as a noise source by stripping the AIs
  from the fixtures, but this is what it looked like while it was there: T7d had zero crashes
  yet eight small airports present in one run were never built in another, worth 238
  movements. If a fixture ever regains an AI, watch for an airport going to 0 while a
  similarly-sized one with a different name appears — that is one airport rebuilt elsewhere,
  not a loss.

With both gone, no fixture has a known noise source left, so read a total delta as a result of
the code under test. The one direct measurement of the re-save itself is `T7d`, which already
had crashes off: rewriting the file changed its total by 0 movements. Keep the floors'
headroom regardless — it costs nothing and covers whatever has not been found yet. To attribute
a delta:

- `[AirportStats] Year N station S "Name"` (at `misc=1`) gives per-airport movements.
- Landing-chain fail/reject diagnostics are emitted at `misc=2`, so they are **absent** from a
  normal regression log. Their absence is not evidence that arrivals are never refused — do a
  one-off `-d misc=2` run to measure that.
- `stuck(reserve) st=S` groups stuck reports by airport.
- Discount airports served by few aircraft first; large airports are far more trustworthy.
- Average **ground time per leg** is robust to how many aircraft exist — but it conflates
  "taxied further" with "landed instead of holding", so rising ground time alongside rising
  movements is the feature working, not a cost.

A sim is deterministic for a fixed save + tick count, so the floors are exact and reproducible: the same code on the same fixture gives the same total every time. Two *different* builds are now comparable too, as long as `[AircraftLost]` is still absent from both runs.

`T5j2`'s floor has more headroom than the others on purpose. Migrating that save to version
375 shifted it by 26 movements (6283 before the re-save, 6309 after) while `mass7-inair` and
`helis2` both round-tripped to identical totals. Whatever state does not survive `T5j2`'s
save/load is worth about half a percent there, so a 30-movement margin of the kind the other
fixtures use would sit inside the noise.

Per-commit attribution: `scripts/airport_stats_history.sh <start_commit> <out_dir> <years>` checks out + rebuilds each commit in `<start>^..HEAD` and records movements to CSV (history mode runs **only** the default save). `--current <years> [save]` runs just the working tree. Underlying runner: `scripts/n_years_plus2.sh <years> [save]` (default save = mass7-inair.sav).

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

- `skills/lldb_debugging.md` — LLDB attach/run workflows and modular runtime log commands.
- `skills/lldb_game_state_inspection.md` — read live game state (pools, towns, stations) from a running game via batch LLDB + Python, no rebuild needed.
- `skills/gui_screenshot_verification.md` — see a GUI change: scratch instance under a chosen base set, windows opened from LLDB, game screenshots itself. Use for any drawing question, especially base-set-specific ones.
- `skills/stuck_plane_debugging.md` — detailed stuck-plane diagnosis playbook.
- `skills/crash_debugging.md` — crash log and stacktrace triage steps.
- `skills/airport_template_analysis.md` — template JSON analysis/visualization workflow.
- `skills/performance_profiling.md` — macOS `sample` profiling + `quick_test.sh`/`regression_test.sh` validation.
- `skills/savegame_fixture_resave.md` — `scripts/resave.sh`: migrate savegames to the current format without advancing the sim.
- `skills/reservations-design.md` — segment types, safe-stop invariant, reservation lifecycle, and entry-contract pitfalls.

## Tile Classification

Every taxiable tile is one of three types used by the segment reservation system:

| Type | Condition | Reservation |
|------|-----------|-------------|
| `RUNWAY` | `IsModularRunwayPiece(piece_type)` — `APT_RUNWAY_1-5`, `APT_RUNWAY_END`, `APT_RUNWAY_SMALL_*` | Crossing: traveled tiles only; explicit landing/takeoff: entire contiguous runway |
| `ONE_WAY` | `IsTaxiwayPiece(piece_type) && one_way_taxi == true` | Safe queue boundary in the forward reservation horizon |
| `FREE_MOVE` | Everything else (aprons, stands, hangars, fenced apron variants) | Traveled tiles through the forward reservation horizon |

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

Modular tile data is saved via `SlModularAirportTileData` in `src/saveload/station_sl.cpp`. Aircraft reservation vectors (`taxi_reserved_tiles`, `modular_runway_reservation`) are saved because map-level reservation bits affect multiplayer game state; the crossing-required ground-path cache is saved via the `MACP` chunk because it changes path choices; `modular_holding_wp_index` is saved because it affects aircraft movement. `taxi_path` and `landing_chain_path` are saved as structured fields in the `VEHS` table chunk by `SlVehicleAircraftPath` in `src/saveload/vehicle_sl.cpp`. The handler serializes the path validity, tiles, and classified segments and reconstructs each `unique_ptr` on load; it does not serialize the pointer value itself. Preserving the exact selected routes is required for multiplayer joins because recomputing them against newer reservation state can choose a different route from the server. Saves predating these fields omit them; the transient `modular_paths_loaded_from_save` and `rollout_restored_from_save` flags identify that legacy case and provide its one-shot mid-landing invariant exemption.

### Fork savegame versioning

**Nothing of this fork's goes into `SaveLoadVersion`.** That enum is upstream's and is merged verbatim; appending to it renumbers on every upstream merge and puts fork savegames on upstream's ordering axis, where they claim to be newer than upstream features they were written without. Fork features are versioned on their own axis instead (`src/saveload/extended_version_sl.h`), following the shape of JGRPP's SLXI chunk so that porting a feature there is mechanical:

- Savegames written here set `SAVEGAME_VERSION_EXT` (`0x8000`) in the header version word on top of an ordinary upstream version. Upstream rejects them with a plain "savegame too new" instead of misreading map bits; the bit is stripped on load.
- The `XVER` chunk holds one `{name, uint16 version, flags}` row per fork feature (`upstream_version`, `modular_airport`). It is registered **first**, so it is written first and known before any chunk that depends on it. An unknown or too-new feature aborts the load unless its saved flags say it may be dropped.
- Gate on the feature, not the version: `IsModularAirportSaveFeaturePresent()` (→ `SlXvIsFeaturePresent(XSLFI_MODULAR_AIRPORT, n)` in a JGRPP port). Bump `MODULAR_AIRPORT_SL_VERSION` and test `min_version` for a format change within the feature.
- Per-field conditions are usually unnecessary: `VEHS` and `STNN` are table chunks, so the savegame lists the fields it holds and a savegame written without ours simply does not load them.
- Savegames stamped 367-375, from before this scheme, are no longer loadable — the temporary shim for them has been removed, so they now hit the ordinary "savegame too new" error.

## Modular Airport Invariants

- Any state that affects aircraft movement, reservations, or path choices must be saved or deterministically rebuilt on load.
- Airport tile `m7` is animation frame storage. Modular reservation ownership belongs in `ModularAirportTileData::reservation_owner`; map `m6` bit 2 is only the reservation-present flag.
- Template placement must preflight the whole final placement before executing. A failure after preflight is an internal bug path, not an acceptable partial build.
- Modular airport creation/removal must keep stock airport town-noise and two-airport local-authority accounting semantics for the selected airport type.
- Changes to movement, reservation, build atomicity, or airport accounting need save/load, multiplayer determinism, and regression coverage checks.

## Common Pitfalls

- `GetModularTileData(tile)` returns `nullptr` if the tile isn't in the modular layout — always null-check.
- Layout-derived answers (catchment, noise, hangar presence, accepted aircraft types, large-safe runways, holding loop, heli tiles) are cached in `mutable` fields on `Airport` and invalidated by `MarkLayoutDirty()` — the only route any layout mutation should use. (The helicopter landing path in `aircraft_cmd.cpp` also forces `modular_heli_tiles_dirty` when the cached landing tile has fallen out of the layout; that is a safety net for a mutation that missed `MarkLayoutDirty()`, not a pattern to copy.) Any code that mutates `ModularAirportTileData` directly instead of going through the commands — tests especially — must call it, or the cached answer silently stays stale. Retyping a tile counts as a layout change: mark dirty *after* the retype, since callers that mark before it leave a window where a read caches a pre-normalization answer.
- `FindAirportGroundPath` with `v=nullptr` ignores stand occupancy (topology only); with `v=aircraft` avoids occupied stands that aren't the goal.
- Path cost has a non-goal stand/parking penalty (`+5`), so routes may prefer slightly longer taxiways over cutting through stands.
- Runway flags (`RUF_LANDING`, `RUF_TAKEOFF`, `RUF_DIR_LOW`, `RUF_DIR_HIGH`) propagate to all tiles in a contiguous runway via `CmdSetRunwayFlags`.
- `AirportTiles` IDs `>= NEW_AIRPORTTILE_OFFSET` (74) are treated as NewGRF airport tiles. Do not store new modular default-tile IDs in map gfx; keep canonical gfx IDs and branch drawing from modular metadata.
- Depot windows can outlive tile deletion. Guard depot UI reads with a valid depot tile check.
- Rotation invariants for modular airports:
  - Hangar directional convention is `0=SE, 1=NE, 2=NW, 3=SW` (clockwise in world space). Keep this mapping consistent across build/rotate/draw/pathfinder code.
  - For preview projection, `iso_x` must use `(dy - dx)` (not `(dx - dy)`), otherwise the entire preview is mirrored.
  - Legacy small runway ends (`NEAR`/`FAR`) swap on odd quarter-turns; after template rotation, preview should normalize each contiguous segment so low-end is `FAR` and high-end is `NEAR`, matching placed tiles.

## GUI Pitfalls

- **`PickerWindowBase::Close()` calls `ResetObjectToPlace()`** — child picker windows (hangar, cosmetic, helipad) must override `Close()` with `this->Window::Close()` to avoid stealing the parent's placement cursor.
- **Picker close behavior**: when child pickers close, parent builder should clear active placement for picker-backed tools (hangar/cosmetic/helipad) so main button state/cursor don't stay latched.
- **`SetObjectToPlace` triggers `OnPlaceObjectAbort` on the current cursor owner** — when changing cursor ownership from within the same window (e.g. fence tool activation), wrap the call in `this->updating_cursor = true/false` to suppress the abort callback.
- **`CloseWindowByClass` can trigger `ResetObjectToPlace` chains** — closing a `PickerWindowBase` sub-window triggers its `Close()` → `ResetObjectToPlace()` → `OnPlaceObjectAbort` on whoever owns the cursor. Guard with `updating_cursor` or override `Close()`.
- **Year-gated picker availability**: if year changes while builder/pickers are open (e.g. Sandbox year change), re-run gating and invalidate picker windows so disabled states update immediately.
- **Sub-tile click position**: use `_tile_fract_coords.x/.y` (0–15 in world X/Y), set by the viewport on every click. Same mechanism as the autoroad tool. Do NOT use `InverseRemapCoords` — it doesn't give tile-relative positions.
- **Widget `SetPIPRatio(left, mid, right)`**: controls how extra space is distributed. `(0,0,1)` = left-aligned, `(1,0,1)` = centered, `(1,0,0)` = right-aligned.
- **Helicopter landing commit**: `AircraftEventHandler_Flying` in `aircraft_cmd.cpp` picks the modular landing target and sets `VehicleAirFlag::HelicopterDirectDescent` when `state == HELILANDING`. Helipad-specific overrides (like skipping the FAF approach) belong here at landing commit, not in the movement code (`AirportMoveModularLanding`).

## Holding Loop Pitfalls

- Don't use `tick_counter` or `running_ticks` for phase timing — both are `uint8_t` and wrap at 256. Use `TimerGameTick::counter` (uint64_t monotonic).
- Movement must be unconditional — don't guard `UpdateAircraftSpeed` inside `if (dist > 0)`.
- Use ghost for movement, nearest-waypoint only for gate checks.
- Reset `modular_holding_wp_index` to `UINT32_MAX` on landing commit.
