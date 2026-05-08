# Avoiding Multiplayer Desyncs

Use this checklist whenever changing game logic, command handlers, save/load, caches, pathfinders, vehicle movement, or anything that can affect future simulation.

## Core Model

OpenTTD multiplayer works by sending clients a savegame snapshot, then replaying commands while every peer simulates the deterministic parts locally. A desync happens when any peer's simulated game state diverges.

That gives three main rules:

- Every simulation-affecting state must be saved and loaded, or rebuilt deterministically from saved state.
- Game logic must not depend on undefined, platform-dependent, address-dependent, or timing-dependent order.
- Command test runs and UI-only paths must not mutate game state.

See also:
- `docs/desync.md`
- `docs/debugging_desyncs.md`

## Save/Load Rules

If a value can affect future gameplay, save it.

Examples of state that must be saved or deterministically rebuilt:
- Vehicle movement state, reservations, goals, counters, path progress, and cached decisions that affect path choice.
- Map bits that affect simulation, including reservations and ownership.
- Global/static caches used by live game logic.
- Any "first seen tick", cooldown, retry, timeout, or rate-limiting state if it changes behavior.

Safe to leave unsaved only when:
- It is purely visual or logging/debug output.
- It is always recomputed before it can affect simulation.
- It is a cache whose absence only costs CPU and never changes decisions.
- It is explicitly reset on new game/load and rebuilt deterministically from saved state.

When adding save/load:
- Add a new `SLV_*` version for new fields.
- Use conditional save/load entries for backward compatibility.
- Preserve old cleanup/conversion only for old savegame versions.
- Ensure load initialization does not clear newly loaded state after the chunk has loaded.
- Update docs or local notes when the saved-state contract changes.

## Determinism Rules

Avoid game-state decisions that depend on unspecified order.

Common hazards:
- Iterating `std::unordered_map` / `std::unordered_set` when order affects behavior.
- Sorting with a comparator that leaves equal keys unordered.
- Priority queues where equal-cost nodes can be popped differently across platforms.
- Pointer/address ordering.
- Wall-clock time, real time, thread timing, or local filesystem state.
- Floating-point computations in simulation-sensitive logic.
- Randomness outside the synchronized game RNG.

Preferred patterns:
- Iterate stable containers, IDs, tiles, or sorted vectors when order matters.
- Add explicit tie breakers: tile index, vehicle ID, station ID, insertion sequence, or another deterministic key.
- Keep caches as sorted vectors or maps when their iteration/order affects behavior.
- Make fallback behavior depend only on saved state and deterministic counters.

## Command Rules

Command test runs must be side-effect free.

Before mutating state in a command path, verify the command is executing for real. Check the local command conventions around `DoCommandFlag::Execute` and follow nearby patterns.

Do not change these during test runs:
- Map tiles or ownership bits.
- Vehicle state.
- Company/station/town/industry state.
- Caches that affect later simulation.
- Random seeds or synchronized RNG state.

If a helper may be called from both test and execute paths, either:
- Pass through the execute flag and guard mutations inside the helper.
- Split validation from mutation.

## Cache Rules

Classify each cache before using it in game logic.

Pure cache:
- Recomputed from saved state.
- Missing or stale cache cannot change behavior.
- Safe to clear on load.

Simulation cache:
- Changes a decision, path, reservation, timing, or selection.
- Must be saved, or removed, or made into a deterministic pure derivation.

For simulation caches:
- Save/load them with versioning.
- Normalize on save and load if order or duplicates matter.
- Clear on new game initialization.
- Invalidate deterministically when underlying topology/state changes.
- Never update them from UI rendering, command test runs, or debug-only probes.

## Modular Airport Specifics

Reservations are game state.

If map-level reservation bits affect aircraft movement, the vehicle-side tracking data that explains them must also survive save/load. Do not clear map reservations on load unless the save version predates the saved tracking fields.

Ground pathfinding affects simulation.

Any learned path preference, crossing-required pair, or route-selection cache must either be saved or removed. If using A*, define equal-cost queue order explicitly.

Diagnostic and debug-only callers of pathfinder helpers must not mutate that cache. `FindAirportGroundPath` takes an `update_cache` flag — pass `false` from any caller that is gated by unsaved state (rate-limit maps, debug-suppression counters). Otherwise, an unsaved gate that fires on the host but not on a freshly-joined client diverges the saved cache and changes future path choices.

Stale-reservation cleanup must be deterministic.

Avoid unsaved "seen for N ticks" maps unless they are saved. Prefer decisions based only on saved vehicle state, saved map state, and synchronized game tick.

## Debugging Workflow

Enable desync logging:
```bash
./build/openttd -d desync=1 -x -g SAVE.sav
```

Stronger cache/desync checks:
```bash
./build/openttd -d desync=2 -x -g SAVE.sav
```

Full recording with periodic saves:
```bash
./build/openttd -d desync=3 -x -g SAVE.sav
```

Artifacts to inspect:
- `commands-out.log`
- `dmp_cmds_*.sav`
- autosaves around the first divergence

Useful local docs:
```bash
sed -n '1,220p' docs/desync.md
sed -n '1,180p' docs/debugging_desyncs.md
```

## Review Checklist

Before finishing a simulation-affecting change, ask:

- Does this introduce new state that affects future game behavior?
- Is that state saved, or deterministically rebuilt before use?
- Can a joining multiplayer client reconstruct exactly the server's behavior from the save?
- Does command preview/test execution mutate anything persistent?
- Does any unordered container, equal-key sort, or priority queue tie affect behavior?
- Does any cache affect path choice, reservations, timing, or selection?
- Is old-save conversion scoped to old save versions only?
- Have regression saves or a fixed-tick headless run been executed?

## Validation Commands

Build:
```bash
./scripts/build_and_sign.sh
```

Modular airport regression:
```bash
./scripts/regression_test.sh
```

Fixed-tick single-save run:
```bash
./build/openttd -d misc=1 -x -g SAVE.sav -s null -m null -v null:ticks=5000 > /tmp/openttd_desync_check.log 2>&1
```

Search for risky new code:
```bash
rg -n 'unordered_|sort\\(|priority_queue|TimerGameTick|Random|DoCommandFlag::Execute|static .*map|static .*set' src
```
