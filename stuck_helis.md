# Stuck Helicopter Issue: MGT_HELI_TAKEOFF_TILE congestion

## Status: Not fixed

## Problem

Helicopters without helipads use a computed takeoff tile (`MGT_HELI_TAKEOFF_TILE`, tgt=6). On busy airports, a helicopter can leave its stand but get permanently blocked by traffic while trying to reach this tile.

## How to detect from logs

```bash
grep 'stuck(reserve)' /tmp/openttd.log | grep 'tgt=6' | tail -20
```

`tgt=6` is `MGT_HELI_TAKEOFF_TILE`. If the same vehicle appears repeatedly with high `wait=` values (hundreds or thousands), it's stuck.

To confirm it's chronic (not just temporary congestion):
```bash
grep 'V326.*stuck(reserve).*tgt=6' /tmp/openttd.log | tail -5
```
If `wait=` keeps climbing past 1024+ without ever resetting, the helicopter is deadlocked.

## Why it happens on `small2.json`-style layouts

The `small2.json` template has stands at dy=3 and runways at dy=0 and dy=2. Helicopters must cross the intermediate runway (dy=2) to reach the computed takeoff tile at dy=0. Under heavy traffic, the runway crossing is perpetually contested.

## Why retarget doesn't help

`TryRetargetModularGroundGoal` handles `MGT_HELI_TAKEOFF_TILE`, but on airports with only one computed takeoff tile, retarget just re-selects the same tile.

## Observed in

- Save: `stuck-helis3.sav`
- Vehicle: V326, stuck at tile 33063 trying to reach heli takeoff tile 33575
- Stuck for the entire 1-year stress test run

## Rejected fix

Vertical-takeoff fallback from any tile after >1024 ticks was prototyped but rejected (user prefers not to use fallbacks).

## Possible approaches

- Compute a better takeoff tile that's closer to the stands (e.g., on the stand side of the intermediate runway)
- Allow the computed tile algorithm to consider proximity to stands, not just airport center
- On layouts where reaching the takeoff tile requires crossing a runway, pick a tile that doesn't
