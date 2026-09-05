# LLDB and Runtime Debugging

Use this guide for LLDB workflows and runtime debug-log collection on macOS.

## Run with Modular Logging

```bash
# Load a savegame with modular airport debug logging
/Users/tor/ttd/OpenTTD/build/openttd -g ~/Documents/OpenTTD/save/SAVENAME.sav -d misc=3 2>/tmp/openttd.log
```

Useful runtime flags:

- `-g savegame.sav` load save
- `-d misc=3` modular debug logging
- `-f` fullscreen
- `-r 1920x1080` resolution

Log locations:

- runtime debug log: `/tmp/openttd.log`
- crash logs: `~/Documents/OpenTTD/crash*.json.log`
- saves: `~/Documents/OpenTTD/save/`

Common modular filters:

```bash
grep '\[ModAp\]' /tmp/openttd.log | tail -100
grep 'stuck(' /tmp/openttd.log | tail -40
grep 'landing-chain fail' /tmp/openttd.log | tail -40
```

## Symbols That Survive a Rebuild

A RelWithDebInfo link does not embed debug info. It embeds a **debug map**: a
reference to each `.o` file plus that file's mtime. LLDB refuses any `.o` whose
mtime has moved since the link, so rebuilding while a game is running blinds the
debugger against it. The failure is silent -- `error: ... debug map object file
... changed` scrolls past, then pool walks return zero rows and globals read as
`0`, which looks exactly like an empty or paused game rather than a broken
attach. (Type *names* still resolve, so `FindFirstType('Aircraft')` succeeding
proves nothing.)

`scripts/build_and_run.sh` and `scripts/build_and_run_debug.sh` now run
`scripts/make_dsym.sh` after the build, which links the DWARF out of the `.o`
files into a self-contained `.dSYM`. Once that exists LLDB reads types from it
and never consults a `.o` again, so the build directory can move on freely:

```bash
scripts/make_dsym.sh   # ~10s, ~190MB, only needed once per link
```

Bundles are archived under `build/dsyms/<UUID>.dSYM` and retained until manually removed. Delete
unneeded archives only after their games and debugger sessions have closed. The
`build/openttd.dSYM` symlink points to the current one, which is what LLDB finds on its
own. Archiving by UUID is what makes a *mid-session* rebuild survivable: the
running game keeps its own symbols even after `build/openttd` is relinked. If
LLDB ever fails to find them, name the bundle explicitly -- it is matched by UUID,
not by path:

```bash
lldb -p <pid> -o 'target symbols add build/dsyms/<UUID>.dSYM'
```

**`make_dsym.sh` must run before anything touches the `.o` files**, because
`dsymutil` validates the same mtimes. It warns `timestamp mismatch between object
file and debug map` per stale `.o` and silently drops those translation units, so
a bundle built too late is quietly incomplete. A clean run reports no such
warnings.

Verified: with every `.o` in `src/` touched, an attach reported 0 debug-map errors
and read `Map::size_x=512` / 75 stations; without the bundle, the same process at
the same moment gave 225 errors and read `0` / `0`.

The old workaround -- harvesting the `actual:`/`debug map:` mtimes out of LLDB's
errors and `os.utime()`-ing each `.o` back -- still works if you are attaching to
a game that was started before any of this existed. It is only safe when no
layout-defining header changed between the two builds (`git diff --stat <a> <b>
-- src/*_base.h src/map_func.h src/aircraft.h`); restore the mtimes afterwards.

## LLDB Quick Workflow

```bash
# Run debug build under scripted LLDB breakpoints (__assert_rtn/abort/__cxa_throw)
/Users/tor/ttd/OpenTTD/scripts/run_lldb_debug.sh

# Attach to a running game
ps aux | grep openttd
lldb -p <pid>
```

In LLDB, capture all thread stacks:

```lldb
thread backtrace all
```

## When to Use Other Skills

- to read game state (town ratings, station flags, pools) from a running game: `skills/lldb_game_state_inspection.md`
- for stuck taxi/landing/takeoff behavior: `skills/stuck_plane_debugging.md`
- for crash log triage workflow: `skills/crash_debugging.md`
