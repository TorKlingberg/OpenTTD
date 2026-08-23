# Performance Profiling

How to find hotspots in OpenTTD on macOS using the built-in `sample` profiler, plus how to validate that an optimization didn't regress simulation behavior.

## Tools

- **`sample`** — macOS built-in sampling profiler (no install needed). Attaches to a PID, captures stack snapshots at intervals, prints a call tree with sample counts.
- **`scripts/profile_helis.sh`** — wraps headless openttd + `sample` for a given save (defaults to `helis2.sav`, 365 days).
- **`scripts/quick_test.sh`** — build + headless run for N years on one or more saves, prints elapsed wall time and `[AirportStats]` movement counts. Use to confirm speedup without behavior regressions.
- **`scripts/regression_test.sh`** — compares per-save movement counts against `*.expected` baselines (5-year run). A bare run is the `T5j2` fixture only (~2 min), which is the normal check; `--full` adds the other three, run concurrently (~13 min, nearly all of it `T7d`), for changes that could affect ground/taxi pathfinding.

On Linux, swap `sample` for `perf record` / `perf report`. Everything else (headless flags, log parsing) is the same.

## Headless openttd flags

| Flag | Purpose |
|------|---------|
| `-x` | Don't save on exit |
| `-g SAVE.sav` | Load savegame |
| `-s null -m null -v null:ticks=N` | No sound, no music, no video — exit after N ticks |
| `-d misc=0` | Suppress `[AirportStats]` chatter while profiling (less log I/O noise) |
| `-d misc=1` | Emit `[AirportStats] Year N totals` lines — needed by `quick_test.sh` |

`DAY_TICKS = 74`. So `ticks = days * 74`. One in-game year ≈ 27000 ticks.

Always rebuild with `scripts/build_and_sign.sh` first — codesign is required on macOS or the binary won't launch.

## Profiling workflow

```bash
# 1. Identify hotspot (defaults: helis2.sav, 365 game days, 25s sample window)
scripts/profile_helis.sh

# 2. Read the top-of-call-tree output it prints; full sample at:
#    /tmp/openttd_helis_sample.txt
```

`sample` output is a call tree, indented by depth. Each line shows `count function_name (module)`. Keys to reading it:

- **Counts are inclusive** — a frame's count includes everything called from it.
- **Leaves are deep, indented lines** — high count at a leaf = expensive per-call work.
- **Hot caller, cheap leaves** = high call frequency from that caller; the fix is usually "call it less often" rather than "make it faster."
- **Watch for the wrong neighbour**: similarly named functions (`FindModularLandingTarget` vs `FindModularLandingGroundGoal`) can be adjacent in the tree. Re-read the line you suspect before assuming you know which is hot — I burned a debugging cycle on this.

## Validating an optimization

After changing the hot path, two checks before declaring victory:

```bash
# Speed + per-save movement count, ~1 minute per save
scripts/quick_test.sh 1                      # 1 year, default saves
scripts/quick_test.sh 5 scripts/testdata/helis2.sav   # 5 years, single save

# Regression check before commit (T5j2 fixture, ~2 min). Add --full only if the
# change could plausibly affect ground/taxi pathfinding.
bash scripts/regression_test.sh
```

`[AirportStats] Year N totals` shows movement counts per airport. Compare against the prior baseline — a speedup that drops throughput >1–2% is usually a behavior change masquerading as a perf fix. If accepting a small drop, update `scripts/testdata/<save>.expected` (`min_movements=`) with a comment explaining why.

## Lessons from a real session

The hot path in helicopter landing was `FindModularLandingTarget`, which ran a full A* (`FindFreeModularTerminal`) per helipad on every flying tick — purely to compute a distance-to-stand bias. Helicopters don't taxi, so the bias was meaningless to them. Replacing the A* with a Manhattan-distance approximation gave a ~15× speedup (helis 1y: 121s → 8s).

Generalisable signals:

- **A function showing up in `sample` that does heavy work for an unused result** is the highest-value target. Read what the result is *used for*, not just what the function does.
- **Per-tick-per-vehicle work** dominates large saves. If you see N×M scaling in the call counts (helicopters × helipads × A*), look for a cheaper proxy.
- **Per-tile vehicle hash (`HasVehicleOnTile`) beats `Aircraft::Iterate()`** for "is anyone on this tile?" checks. The latter scans the whole pool.
- **A "fix" that improves time but tanks throughput is not a fix** — quick_test.sh exists to catch this before regression_test.sh does.

## When to skip profiling

- One-shot UI/build-time issues — read the code, the bottleneck is usually obvious.
- Anything <50ms per call on a cold path — sampling resolution won't see it.
- Suspected algorithmic complexity bugs — count operations directly with a debug log instead; `sample` won't tell you "this function is O(N²) when it should be O(N)."
