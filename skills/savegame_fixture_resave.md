# Re-saving Savegames in the Current Format

How to migrate `.sav` files — the regression fixtures in `scripts/testdata/` especially — to the current `SAVEGAME_VERSION` without opening the GUI and without advancing the simulation.

## Why

Fixtures loaded from an old savegame version go through the `afterload.cpp` conversion path on every run. Re-saving bakes the conversion in, so later runs load the state directly and the conversion code stops being exercised on every regression run. It also keeps the fixtures loadable if old-version support is ever dropped.

## Usage

```bash
scripts/resave.sh scripts/testdata/*.sav
```

Files are rewritten in place; the script prints the before/after savegame version per file. `OPENTTD_BIN` and `OPENTTD_PERSONAL_DIR` override the binary and personal-dir locations.

Always re-run `scripts/regression_test.sh` afterwards. A format bump that *does* alter state would show up there and nowhere else.

## How it works

`OnStartGame()` (`src/openttd.cpp`) execs `scripts/game_start.scr`. For `-g <save>` that call sits in the `SM_LOAD_GAME` branch of `SwitchToMode()`, which `GameLoop()` runs **before** `StateGameLoop()` — so a script of

```
save <name>
exit
```

writes the loaded state converted to `SAVEGAME_VERSION` with zero ticks elapsed, then quits. The run is headless: `-x -g <save> -s null -m null -v null:ticks=5` (the tick count is only a ceiling; `exit` fires during the first `GameLoop()`).

## Gotchas

- **`save` cannot take a path.** `ConSave` always writes to `<personal dir>/save/<name>.sav`. The script saves under a scratch name there and moves the result into place — it does not save next to the source file.
- **`game_start.scr` is found on the search path**, which includes the current working directory *and* the repo root. The script runs from a private `mktemp -d` so its hook shadows nothing in the repo, the build dir, or `~/Documents/OpenTTD`.
- **Pass `-x`** or the run rewrites `openttd.cfg` from whatever the fixture's settings were.
- **Check the pause state.** Fixtures must stay unpaused (see the Regression Testing notes in `CLAUDE.md`). `_pause_mode` is saved (`misc_sl.cpp`) and is restored from the fixture, so an unpaused save re-saves unpaused — but the `PauseMode::SaveLoad` handling around `SM_LOAD_GAME` is close enough to this path to be worth re-checking if a fixture starts simulating nothing.
- **A re-save keeps `Aircraft::flags`.** ~52 of `helis2.sav`'s 90 helicopters carry a stale
  `VehicleAirFlag::HelicopterDirectDescent` from a build predating the modular touchdown clear,
  and an earlier note here claimed re-saving would destroy that coverage. It does not: the
  field is saved (`SLE_CONDVAR(Aircraft, flags, ...)`, `saveload/vehicle_sl.cpp`) and no
  afterload step clears the bit. A re-save does re-baseline the fixture's floor, like any other.
- **The diff is the whole file.** Savegames are compressed, so a re-save touches every byte. Commit it separately from behavioural work.

## Verifying a re-save changed nothing

Diff the per-year stats between the original and the re-saved copy over the regression window rather than trusting the totals:

```bash
./build/openttd -d misc=1 -x -g SAVE.sav -s null -m null -v null:ticks=140156 2>&1 | grep "\[AirportStats\] Year"
```

`140156` = `(5*365 + 62 + 7) * 74`, matching `n_years_plus2.sh 5`. The year lines should match line for line.
