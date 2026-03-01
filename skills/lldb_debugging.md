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

- for stuck taxi/landing/takeoff behavior: `skills/stuck_plane_debugging.md`
- for crash log triage workflow: `skills/crash_debugging.md`
