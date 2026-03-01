# Debugging Stuck Planes & Landing Failures

## Setup

Run with debug logging enabled:
```bash
scripts/build_and_run_debug.sh ~/Documents/OpenTTD/save/SAVENAME.sav
```
This starts with `-d misc=3` and logs to `/tmp/openttd.log`.

## Quick Triage

### Planes circling, not landing
```bash
grep 'landing-chain fail' /tmp/openttd.log | tail -20
```
Look at the `detail=TILE` field — that's the tile blocking the landing chain. Then find who owns it:
```bash
grep 'TILE_NUMBER' /tmp/openttd.log | head -20
```

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

### Stuck diagnostics
```
[ModAp] V74 unit#33 stuck(no-path) wait=64 state=2 tile=16811 goal=16556 tgt=4 path_found=0 cost=0
```
- `wait` — ticks spent stuck (safety nets trigger at 64 and 512)
- `path_found` — whether pathfinder found any route (0=no, 1=yes but blocked)

```
[ModAp] V74 unit#33 stuck(reserve) wait=32 state=2 tile=16811 next=16812 seg=1 goal=16556 tgt=4 reserved_by_other=1 reserver=V82 occupied_by_other=0 runway_busy=0
```
- `next` — the tile it can't enter
- `reserver` — who holds the reservation on that tile
- `reserved_by_other` / `occupied_by_other` / `runway_busy` — why it can't proceed

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

## Fallback / Safety Net Monitoring

All fallback mechanisms log with `[FALLBACK]` for easy monitoring:
```bash
grep '\[FALLBACK\]' /tmp/openttd.log
```

These should be rare. Frequent occurrences indicate an underlying bug:

| Pattern | Meaning | Concern |
|---------|---------|---------|
| `stale-clear` | Cleared a reservation left behind by a vehicle that moved on | Reservation not cleaned up properly on state transition |
| `orphan-runway-clear` | Runway reservation scan found tiles reserved to wrong vehicle | Runway reservation tracking out of sync |
| `orphan-taxi-clear` | Taxi reservation scan found tiles reserved to wrong vehicle | Taxi reservation tracking out of sync |
| `force-clear-all` | Force-cleared all taxi reservations for a stuck vehicle | Vehicle stuck >64 ticks, aggressive cleanup |
| `stuck-takeoff` | Vehicle stuck >512 ticks, forced to FLYING state | Last resort escape — something badly wrong |
| `takeoff-fallback-runway` | Used fallback runway (wrong direction or small size) | No ideal runway available, using best effort |
| `landing-small-runway` | Large aircraft landing on small runway | No large runway at this airport |
| `pathfind-crossing-fallback` | Pathfinder strict pass failed, fallback allowed runway crossing | Layout requires crossing a runway to reach destination |

### Stale-clear reasons

The `stale-clear` fallback includes a reason code:
- `invalid_vehicle` — reservation points to a non-existent vehicle
- `not_normal_aircraft` — reservation points to something that isn't a normal aircraft
- `not_on_ground` — aircraft is flying/landing, not on the ground at this airport
- `untracked_intent` — aircraft is on the ground but tile isn't in its reservation tracking

## Debugging Workflow

### Step 1: Identify the symptom
- Planes circling → check `landing-chain fail`
- Plane stuck on ground → check `stuck(*)` lines for that vehicle
- Plane at stand not leaving → check `takeoff.*FindRunway=INVALID`

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
If V74 needs a tile held by V82 and V82 needs a tile held by V74, that's a deadlock. The 64-tick force-clear and 512-tick stuck-takeoff safety nets should eventually break it.

## In-Game Tools

- **Query tool (?)**: Click on a tile to see its tile index and properties
- **Console `scrollto TILE`**: Center the viewport on a tile index
- **Save/reload**: Clears all in-memory reservation state (taxi_reserved_tiles, taxi_path, etc. are not saved). If save/reload fixes the problem, it's a reservation state bug.

## Common Root Causes

1. **Stale reservations after takeoff**: Aircraft takes off but preserved landing chain tiles aren't cleared. Fixed by `force_clear_all=true` on takeoff transitions.

2. **Landing chain blocked by stale tile**: `TryReserveLandingChain` now uses `IsTaxiTileReservedByOther` which auto-clears stale reservations.

3. **Large aircraft on small-runway-only airport**: `FindModularRunwayTileForTakeoff` and `FindModularLandingTarget` now fall back to small runways when no large runway exists.

4. **Layout dead ends**: A stand reachable only by crossing another stand or runway. The pathfinder's two-pass system allows runway crossing as a fallback (+8 cost penalty), but stand traversal is blocked if the stand is occupied. Consider adding one-way taxiway routing.
