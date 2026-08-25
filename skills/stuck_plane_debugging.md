# Debugging Stuck Planes & Landing Failures

## Setup

Run with debug logging enabled:
```bash
scripts/build_and_run_debug.sh ~/Documents/OpenTTD/save/SAVENAME.sav
```
This starts with `-d misc=3` and logs to `/tmp/openttd.log`.

### Headless test runs

You can run the game headlessly for a fixed number of ticks to test fixes without a GUI:
```bash
scripts/build_and_sign.sh && ./build/openttd -d misc=1 -x -g ~/Documents/OpenTTD/save/SAVENAME.sav -s null -m null -v null:ticks=500 > /tmp/openttd_test.log 2>&1
```
- `-v null:ticks=N` — run N game ticks then exit (no window)
- `-s null -m null` — disable sound/music
- `-x` — don't save on exit
- Adjust `-d misc=N` for verbosity (1=stuck/warnings, 2=reservations, 3=all movement)

## Quick Triage

### Rule out a user-stopped plane first

`AircraftEventHandler` returns early on `VehState::Stopped` (`aircraft_cmd.cpp`), so a plane
the player stopped never runs movement or reservation logic. It keeps its tile reservation and
physically occupies its tile **forever**, and it emits *no* log lines at all. On a modular
airport `TERM1` covers the whole taxi phase, so a plane can legally be stopped mid-taxiway —
park one on a chokepoint and the entire airport wedges with no diagnostic trail.

Check the head of the jam before anything else. In game: open the vehicle and look for the
stopped indicator. From a running process, dump `vehstatus` (see
`skills/lldb_game_state_inspection.md`); bit 1 = `Stopped`, so `0xa` = `Stopped+DefaultPalette`.

Do not treat a null `taxi_path` plus non-zero path state as a single post-load signature.
Current saves persist `taxi_path` through `SlVehicleAircraftPath`, and the fields have distinct
diagnostic meanings:

- `taxi_path == nullptr`, `taxi_path_index == 0`, and a non-zero `taxi_wait_counter` is the
  normal shape of a genuine no-path retry: rebuilding clears the path state, restores the wait
  counter, and increments it.
- `taxi_path == nullptr` with a non-zero `taxi_path_index` is suspicious. It can come from an
  older save whose `VEHS` table omitted the path fields, invalid loaded path data, or stale or
  corrupt state. With `-d sl=1`, invalid path data reports
  `Found Aircraft ... with invalid modular-airport path, ignoring.`

The blocked planes behind it show the normal `stuck(reserve)` chain, and their
`taxi_wait_counter` values are all *identical* — one simultaneous stall, not independent
failures. Divergent counters point at real contention instead.

### Planes circling, not landing
```bash
grep 'landing-chain fail' /tmp/openttd.log | tail -20
```
Look at the `detail=TILE` field — that's the tile blocking the landing chain. Then find who owns it:
```bash
grep 'TILE_NUMBER' /tmp/openttd.log | head -20
```

### One runway is never used, while a parallel one is

Confirm the split, then get the rejection reason for the unused runway:

```bash
grep -o "starting landing on tile [0-9]*" /tmp/openttd.log | sort | uniq -c | sort -rn
grep -E "landing-chain (reject|fail).*runway=29289" /tmp/openttd.log | tail
```

`GatherAndSortGates` colocates parallel same-direction gates (within 5 tiles
lateral / 3 along) onto one shared `wp_index` — two yellow overlay lines meeting
at one waypoint square. Colocated gates are live simultaneously and every filter
answers identically for them, so distance is rarely the discriminator
(`MODULAR_LANDING_GATE_MAX_DIST_TILES` is 25). Gate order plus availability
decides.

`reason=no_goal_path_invalid` with `goal=0` means no free terminal *and* no path
from that runway's rollout end to any stand — the runway is walled off. Check
the rollout end's `edge_block_mask` and whether its neighbours are non-taxiable
decoration or one-way tiles pointing the wrong way. Layout fault, not code
fault.

### Plane stuck on the ground
```bash
grep 'stuck(' /tmp/openttd.log | tail -20
```
Three variants:
- `stuck(no-path)` — pathfinder found no route (bad layout or genuine dead end)
- `stuck(reserve)` — next tile reserved by another aircraft
- `stuck(occupied)` — next tile physically occupied by another aircraft

### Plane stuck trying to take off
```bash
grep 'takeoff.*FindRunway=INVALID' /tmp/openttd.log | tail -10
```
Means no runway was found for takeoff. Usually a direction flag or large-aircraft-on-small-runway issue.

### Plane stuck at stand, runway found but unreachable (legacy queue-goal path)
```bash
grep 'takeoff.*queue=INVALID' /tmp/openttd.log | tail -10
```
This is mainly useful for older logs. Current code uses runway tiles as takeoff goals instead of queue/free-move goal tiles.

### Plane assigned takeoff target (current behavior)
```bash
grep 'takeoff target runway=' /tmp/openttd.log | tail -20
grep 'found takeoff target goal=' /tmp/openttd.log | tail -20
```
For `MGT_RUNWAY_TAKEOFF`, goal should now be the selected runway tile (real goal), not a free-move queue tile.

### Plane on free-move tile with no forward reservation
```bash
grep 'V{id}.*stuck(reserve)' /tmp/openttd.log | tail -20
grep 'V{id}.*owned-reservations' /tmp/openttd.log | tail -10
```
If a plane repeatedly sits on a free-move tile with only current/self reservation and no forward claim, suspect reservation handoff/churn.

### Runway reservation churn/flapping
```bash
grep 'V{id}.*runway-reserve denied' /tmp/openttd.log | tail -30
grep 'V{id}.*reserve-state reason=.reserve granted.' /tmp/openttd.log | tail -30
```
Repeated deny/grant oscillation for the same vehicle/runway is a strong signal for reservation instability.

### Runway transit vs. runway as destination

An aircraft crossing a runway must be able to reserve the whole chain to the next
safe stop *before* it steps on; otherwise it waits before the runway. That is
ordinary contention and shows up as `stuck(reserve)` with a `deny=` of
`runway_busy` or `reserved_by_other`.

Two deny reasons are contract violations rather than traffic:

- `deny=runway_resource_error` — a crossed runway's contiguous extent could not be
  resolved. Layout/metadata problem.
- `deny=no_safe_stop` — the crossing walk ran off the end of the path without
  reaching the goal or a safe stop. The walk is constructed so traffic cannot cause
  this (see `skills/reservations-design.md` §3); it means `taxi_path` does not end
  at `ground_path_goal`. Look at path construction and retargeting, not contention.

**Note:** a *permanently* stuck aircraft whose `stuck(reserve)` line shows every
blocker false is never contention. Read `deny=` first — an unsatisfiable entry
contract looks exactly like traffic otherwise, and the wait counter wraps at 16
bits so a huge `wait=` can cycle back to a small one.

**Historical note:** this section used to document `runway-transit-deny`,
`runway-transit-debug` and `runway-transit-invariant` log lines. Those were removed
from the source; only the `deny=` reasons above survive. Do not grep for them.

### Takeoff retarget
`TryRetargetModularGroundGoal` re-runs `FindModularRunwayTileForTakeoff` for
`MGT_RUNWAY_TAKEOFF`, so a takeoff end that stops being reachable after it was
picked gets replaced. The selector distinguishes unreachable ends (skipped,
`takeoff-path invalid`) from reachable-but-blocked ones, so ordinary contention
still waits rather than switching runways.

Before this, takeoff goals were never retargeted: `modular_takeoff_tile` is only
re-selected when it is `INVALID_TILE`, so an aircraft that picked a runway end
while standing on that runway, then taxied behind a chokepoint, kept an
unreachable goal forever. A permanent `stuck(no-path)` with `tgt=4` and no
`retarget failed` line alongside it is the signature.

`FindModularRunwayTileForTakeoff` found a runway end but `FindModularTakeoffQueueTile` can't path to it. Common causes:
- **Unreachable fallback runway**: The only runway returned was the Manhattan-distance fallback, which may be across an intervening runway with no ground path. Check `takeoff-fallback-runway` logs.
- **Direction/size filter eliminated reachable runways**: All topologically reachable runway ends were filtered out by direction flags or large-aircraft checks, leaving only the unreachable fallback.
- **All paths temporarily blocked**: Every reachable runway has traffic blocking the first segment.

Debug with:
```bash
grep 'V{id}.*takeoff-skip' /tmp/openttd.log | tail -10   # direction/size filter rejections
grep 'V{id}.*takeoff-path' /tmp/openttd.log | tail -10    # topology/enterability failures
grep 'V{id}.*takeoff-fallback' /tmp/openttd.log | tail -10 # fallback used
```

## Key Log Patterns

### Vehicle identification

Log lines use two IDs:
- `V{id}` — internal vehicle pool index (used in most log lines)
- `unit#{N}` — UI-visible "Aircraft #N" (in `reserve-state`, `stuck(*)`, `retarget-hangar` lines)

To find which V-number corresponds to an aircraft number:
```bash
grep 'unit#34' /tmp/openttd.log | head -1
```

### Landing chain failures
```
[ModAp] V78 landing-chain fail: reason=segment_blocked runway=16556 goal=16809 rollout=16553 detail=16811
```
- `reason` — why the chain failed (`segment_blocked`, `runway_reserved`, etc.)
- `runway` — the runway tile the aircraft wants to land on
- `goal` — the stand/helipad it wants to reach after landing
- `rollout` — where it exits the runway
- `detail` — the specific tile that's blocked (most useful field)

**Common cause:** A stale reservation on the `detail` tile. Check who owns it:
```bash
grep 'owned-reservations.*16811' /tmp/openttd.log | tail -5
```

### Reserve state dumps
```
[ModAp] V76 unit#34 reserve-state reason='reserve granted' state=2 tile=16556 goal=16553 tgt=5 path=0/7 runway_res=8 owned=9 owned_rw=8 tracked_not_owned=0 owned_rw_not_tracked=0
```
- `state` — FTA state (2=TERM1 ground movement, 14=landing approach, etc.)
- `tile` — current tile
- `goal` — path destination
- `tgt` — modular ground target (1=terminal, 2=helipad, 3=hangar, 4=runway_takeoff, 5=rollout)
- `path=current/total` — progress through taxi path
- `owned` — total tiles this aircraft has reserved
- `tracked_not_owned` — tiles in taxi_reserved_tiles that aren't actually reserved to this vehicle (stale tracking)
- `owned_rw_not_tracked` — runway tiles reserved to this vehicle but not in its tracking list (leak)

**Red flags:** `tracked_not_owned > 0` or `owned_rw_not_tracked > 0` indicate reservation tracking bugs.

Following the reserve-state line, look for:
```
[ModAp] V76 owned-reservations [16553,16554,16555,16556,16811]
[ModAp] V76 tracked-runway [16553,16554,16555,16556]
```
These list the exact tiles reserved by and tracked for this vehicle.

**Note:** `owned-reservations` is read from **map state** (`HasAirportTileReservation`/`GetAirportTileReserver`), not from the vehicle's `taxi_reserved_tiles` vector. `tracked-runway` is the vehicle's `modular_runway_reservation`. A divergence between the two — or between either of them and what `taxi_reserved_tiles` "should" hold — is a tracker/map mismatch and a useful diagnostic for reservation overlap or leak bugs.

### Stuck diagnostics
```
[ModAp] V74 unit#33 stuck(no-path) wait=64 state=2 tile=16811 goal=16556 tgt=4 path_found=0 cost=0
```
- `wait` — ticks spent stuck (retarget attempted every 64 ticks)
- `path_found` — whether pathfinder found any route (0=no, 1=yes but blocked)

```
[ModAp] V74 unit#33 stuck(reserve) wait=32 state=2 tile=16811 next=16812 seg=1 goal=16556 tgt=4 deny=reserved_by_other deny_tile=16814 deny_by=V82
```
- `next` — the next tile on the path (**not** necessarily what blocked)
- `deny` — why the reservation was refused: `reserved_by_other`, `occupied_by_other`, `runway_busy`, `runway_resource_error`, `no_path`, `no_safe_stop`
- `deny_tile` — **the tile that actually blocked**, reported by the reservation attempt itself
- `deny_by` — who holds it, where known

**`deny_tile` is usually not `next`.** A segment claims far more than one tile — a whole
FreeMove run, or a crossing chain spanning several runways — so the aircraft can be
solidly blocked while the tile immediately ahead is free. On a busy save this is the
common case, not the exception (63% of stuck reports in one T5j2 run). The old form of
this line re-derived blockers from `next` and so printed all-clear for genuinely blocked
aircraft; always read `deny_tile`, and treat `next` as route context only.

`deny=no_safe_stop` and `deny=runway_resource_error` are contract violations rather
than traffic — see "Runway transit vs. runway as destination" above.

### Safe-stop invariants after landing

```bash
grep -c 'landing-chain-invariant' /tmp/openttd.log   # expect 0
grep -c 'runway-rest-invariant'   /tmp/openttd.log   # small and self-clearing
```

- `landing-chain-invariant: off a safe stop with no reserved route to one` — the
  aircraft is standing where it may not wait (in practice a rollout end) and owns
  no reserved safe stop. Landing is only permitted against such a route, so this
  means the route was thrown away after commit. **Expect zero**, and
  `scripts/regression_test.sh` fails a fixture that emits any.

  One compatibility case is *not* a violation and is suppressed. Current saves
  persist `landing_chain_path` through `SlVehicleAircraftPath` in the `VEHS`
  table, serializing its data and reconstructing the `unique_ptr` on load. Older
  saves lack those fields, so an aircraft that was already committed to a landing
  can still arrive at its rollout end with no chain to install. `AfterLoadGame`
  uses the transient `Aircraft::modular_paths_loaded_from_save` marker to identify
  that legacy case, sets `Aircraft::rollout_restored_from_save`, and the check
  skips it once. A new save containing the path fields receives no exemption.
- `runway-rest-invariant: waiting on runway` — the aircraft is waiting on a runway
  tile. Unlike the above it still holds a route to a safe stop; it is losing the
  race for the next runway resource (`deny=runway_busy`). Ordinary contention at
  a busy airport, and it self-clears — a rollout end is physically on the runway,
  so some of this is unavoidable. Worth investigating only if a single vehicle
  stays there indefinitely, or if the count climbs sharply after a change.

For FreeMove segments, remember current behavior reserves/checks only the forward part of the segment when already inside it (from `path_idx + 1` onward), plus one boundary tile. Missing "behind us" reservations are expected and not a bug by themselves.

When a runway is involved, read the `deny=` field on the `stuck(reserve)` line:

- A runway reached as the *destination* segment (takeoff/rollout flow) only needs
  the runway itself.
- A runway crossed in *transit* needs the full chain across it to the next safe
  stop (OneWay tile / stand / hangar / helipad / goal) before entry, otherwise the
  aircraft waits before the runway. Repeated `deny=runway_busy` there is contention,
  not a reservation leak.

### Takeoff failures
```
[ModAp] V76 takeoff: FindRunway=INVALID vtile=16811
```
No usable takeoff runway found. Causes:
- No runway has `RUF_TAKEOFF` flag
- Direction flags exclude all ends
- Large aircraft, all runways are small (now falls back to small runway)

```
[ModAp] V76 takeoff-path not enterable: from=16811 to=16556 reason=freemove_blocked
```
Path to runway exists but the first segment is blocked by another aircraft.

### Takeoff runway selection diagnostics
```
[ModAp] V76 takeoff-skip dir: tile=16556 is_low=1 flags=5
```
Runway end skipped due to direction flags. Check `RUF_DIR_LOW`/`RUF_DIR_HIGH` on this runway.

```
[ModAp] V76 takeoff-skip large: tile=16556
```
Runway end skipped because aircraft is large and runway is too small.

```
[ModAp] V76 takeoff-path invalid: from=16811 to=16556
```
No topology path exists between the aircraft's tile and the runway end. The ground pathfinder found no route — check for intervening runways, missing taxiway connections, or blocked stands in the only path.

## Fallback / Safety Net Monitoring

Fallback mechanisms log with `[FALLBACK]`, at `misc=2`:
```bash
grep '\[FALLBACK\]' /tmp/openttd.log
```

Only two markers exist:

| Pattern | Meaning | Concern |
|---------|---------|---------|
| `stale-clear` | Cleared a reservation left behind by a vehicle that moved on | Reservation not cleaned up properly on state transition |
| `force-clear-all` | Force-cleared all taxi reservations for a stuck vehicle | Vehicle stuck >64 ticks, aggressive cleanup |

Both should be zero, and `scripts/regression_test.sh` fails a fixture that emits
either. Earlier versions of this file also listed `orphan-runway-clear`,
`orphan-taxi-clear`, `takeoff-fallback-runway`, `landing-small-runway` and
`pathfind-crossing-fallback`. **None of those exist any more** — do not grep for
them. (`pathfind-crossing-required` is a live `misc=2` line, but it is a routine
report that the strict pathfinder pass failed and crossing was allowed, not a
`[FALLBACK]`.)

### Stale-clear reasons

- `invalid_vehicle` — reservation points to a non-existent vehicle
- `not_normal_aircraft` — reservation points to something that isn't a normal aircraft
- `not_on_ground` — aircraft is flying/landing, not on the ground at this airport
- `active_untracked` — aircraft is on the ground but the tile isn't in its reservation tracking

## What the Regression Suite Gates On

`scripts/regression_test.sh` runs its fixtures at `-d misc=2` and fails any fixture
whose log contains one of the patterns in that script's `FAILURE_PATTERNS` array —
the should-never-happen lines above, plus both `[FALLBACK]` markers and
`[AircraftLost]`. Throughput alone does not catch these: a few aircraft on a broken
path cost a handful of movements out of thousands, comfortably inside the
`min_movements=` floor's headroom.

`stuck(reserve)` / `stuck(occupied)` / `stuck(no-path)`, `runway-rest-invariant`,
`clamp pre-ground-move`, `retarget failed`, `landing-chain fail`/`reject`,
`takeoff-skip`, `takeoff-path not enterable` and `diverted` are **not** gated —
a healthy busy fixture emits tens of thousands of them.

`FAILURE_PATTERNS` is the authoritative list; this file is prose about it. If you
add a should-never-happen `Debug()` call, add it there too, and log it at `misc<=2`
or the suite will never see it.

## Debugging Workflow

### Step 1: Identify the symptom
- Planes circling → check `landing-chain fail`
- Plane stuck on ground → check `stuck(*)` lines for that vehicle
- Plane at stand not leaving → check `takeoff.*FindRunway=INVALID` and takeoff target logs (`takeoff target runway=` / `found takeoff target`)

### Step 2: Find the blocking tile
- Landing failures: `detail=TILE` in landing-chain fail
- Ground stuck: `next=TILE` in stuck(reserve/occupied)
- Takeoff: `vtile=TILE` is where the aircraft is stranded

### Step 3: Find who owns the blocking tile
```bash
grep 'owned-reservations.*TILE' /tmp/openttd.log | tail -5
```
Or check if it's a stale reservation:
```bash
grep 'FALLBACK.*stale-clear.*TILE' /tmp/openttd.log
```

### Step 4: Trace the blocker's history
Once you know V{id} of the blocker:
```bash
grep 'V{id} ' /tmp/openttd.log | head -30
```
Look for when it reserved the tile, and whether it moved on without clearing.

### Step 5: Check for deadlocks
Two vehicles blocking each other:
```bash
grep -E 'V(74|82) ' /tmp/openttd.log | tail -30
```
If V74 needs a tile held by V82 and V82 needs a tile held by V74, that's a deadlock. The 64-tick retarget and reservation clearing should eventually break it.

For overload stress tests (intentional over-capacity), add:
```bash
tail -n 10000 /tmp/openttd.log > /tmp/openttd_last10k.log
rg -c 'landing-chain-invariant' /tmp/openttd_last10k.log
rg -c '\[FALLBACK\]' /tmp/openttd_last10k.log
```
- Expect many `stuck(reserve)` lines under overload.
- Treat any `[FALLBACK]` or `landing-chain-invariant` as a correctness regression.

### Step 6: Map the airport layout
When takeoff paths fail, map out the tiles between the stand and runway:
```python
# Convert tile index to (x, y) — map width is typically 256
tile = 11928
x, y = tile % 256, tile // 256
```
Check each tile along the expected path for:
- Intervening runways (pathfinder won't cross unless fallback mode)
- Occupied stands blocking the only route
- Missing taxiway connections
- Free-move boundaries where no forward reservation can be acquired

## In-Game Tools

- **Query tool (?)**: Click on a tile to see its tile index, properties, and reservation owner
- **Reservation overlay**: Toggle in the modular airport builder toolbar to see per-aircraft reservation chains drawn as blue lines
- **Console `scrollto TILE`**: Center the viewport on a tile index
- **Save/reload**: Current saves preserve the reservation vectors plus `taxi_path` and
  `landing_chain_path`; save/reload is no longer a way to clear modular-airport movement state.
  If it changes the problem, investigate a missing or mismatched saveload field, invalid loaded
  path data, or the compatibility reconstruction used for an older save.

## Common Root Causes

0. **A plane stopped by the player** (see Quick Triage above). Not a code fault — but it looks
   exactly like a reservation deadlock, and it produces no log output, so eliminate it first.

1. **Stale reservations after takeoff**: Aircraft takes off but preserved landing chain tiles aren't cleared. Fixed by `force_clear_all=true` on takeoff transitions.

2. **Landing chain blocked by stale tile**: `TryReserveLandingChain` now uses `IsTaxiTileReservedByOther` which auto-clears stale reservations.

3. **Large aircraft on small-runway-only airport**: `FindModularRunwayTileForTakeoff` and `FindModularLandingTarget` now fall back to small runways when no large runway exists.

4. **Layout dead ends**: A stand reachable only by crossing another stand or runway. The pathfinder's two-pass system allows runway crossing as a fallback (+8 cost penalty), but stand traversal is blocked if the stand is occupied. Consider adding one-way taxiway routing.

5. **Unreachable fallback runway**: `FindModularRunwayTileForTakeoff` returns a Manhattan-distance fallback runway that has no ground path (e.g., separated by an intervening runway). Fixed by preferring "blocked but topologically reachable" runway ends over unreachable fallbacks. If all reachable ends are filtered by direction/size, the aircraft gets the unreachable fallback and loops. Fix: check `takeoff-skip dir`/`takeoff-skip large` logs to see which filters are too aggressive.

7. **Rollout fallback ignoring the reserved buffer** (fixed): a landing admitted with
   no free stand reserves the runway plus one one-way queue tile, so the aircraft
   always has somewhere legal to wait. `FindModularRolloutHoldingTile` then searched
   for a stop on the route to the *cheapest* service tile — a different route — and
   gave up when that route's safe stops were held, stranding the aircraft on the
   runway at landing speed with no goal. Signature: `rollout fallback failed`
   immediately followed by `invalid ground state ... at speed`. It now falls back to
   the reserved queue tile the landing was admitted against.

6. **Runway sandwiching stands**: Airports with runways on both sides of the stands can create situations where aircraft can only reach one runway (the one they landed on) but need to take off from the other. Ensure at least one runway end reachable from each stand has `RUF_TAKEOFF` with correct direction flags.

## Debugging Policy Preference

When evaluating or proposing fixes, prefer solid reservation contracts over recovery behavior:

- Prefer strict "can I enter?" rules before movement into `FreeMove`/runway-transit sections.
- Prevent unsafe entry (missing forward ownership chain) rather than trying to "unstick" later.
- Treat fallback cleanup (`[FALLBACK]` stale/orphan clears, force-clear paths) as safety net only, not primary control flow.
- If a plane gets stuck on a free-move tile, first question is whether entry should have been denied earlier.
