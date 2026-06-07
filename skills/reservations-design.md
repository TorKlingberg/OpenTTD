# Modular Airport Reservation Design

How taxiing and tile reservations actually work in `src/modular_airport_cmd.cpp`. Source of truth for reservation invariants.

## 1. Segment types

Taxi paths are split into contiguous segments, each with type-specific reservation behavior:

| Type | Where it applies | Reservation model |
|------|------------------|-------------------|
| `RUNWAY` | `IsModularRunwayPiece(piece_type)` | Atomic — entire contiguous runway reserved as one resource |
| `ONE_WAY` | `IsTaxiwayPiece` with `one_way_taxi == true` | Per-tile queue — one tile at a time |
| `FREE_MOVE` | Everything else (aprons, stands, hangars, fenced apron variants) | Atomic per-segment — whole segment reserved at once |

`TaxiSegmentType` is assigned by `ClassifyTile` in `airport_ground_pathfinder.cpp` and consumed by `TryReserveTaxiSegment`.

## 2. Safe stops (key invariant)

**An aircraft on the ground must always have a reserved path to a "safe stop".** A safe stop is a tile where the aircraft can wait indefinitely without holding a shared resource other aircraft need to traverse.

| Tile kind | Safe stop? | Why |
|-----------|-----------|-----|
| Stand | Yes | Parking; per-stand exclusivity, others route around |
| Hangar | Yes | Multi-capacity parking; never hard-blocks |
| Helipad | Yes | Parking for helicopters |
| `ONE_WAY` taxiway tile | Yes | Designed as a queue; per-tile semantics support waiting |
| Runway tile | **No** (in transit) | Atomic resource — stopping pins the whole runway |
| `FREE_MOVE` grass / apron | **No** | Pure transit; stopping blocks anyone else needing to cross |

A runway tile is special: it *is* the destination during takeoff (`MGT_RUNWAY_TAKEOFF` → state `TAKEOFF`), where the aircraft transitions out of ground movement entirely. So a runway-end takeoff goal is acceptable as a path terminus, but **never** as a mid-path resting place.

The fallout: every reservation step must guarantee that the aircraft, after taking it, still owns a chain reaching some safe stop. Stopping on grass/apron is an invariant violation, not a "stuck" symptom — the system is supposed to deny entry rather than allow the stop.

## 3. Reservation rules by segment

### `FREE_MOVE`

`TryReserveTaxiSegment` reserves:
- All forward tiles in the segment atomically (when entering from outside).
- If already inside the same segment, only forward tiles past `taxi_path_index` (re-checking tiles behind can deadlock opposing movers holding disjoint prefixes).
- One exit tile (first tile of the next segment).
- If the next segment is `RUNWAY`, recurses into the runway segment's contract before committing.

Hangar tiles are non-blocking (multi-capacity).

### `ONE_WAY`

- Reserve the single next tile, per-tile.
- If next is a runway tile and target is takeoff (`MGT_RUNWAY_TAKEOFF`), require full contiguous runway reservation before stepping onto a runway tile.
- Hangar next tile: tracked but not map-blocking.

### `RUNWAY`

Two modes, distinguished by `IsRunwaySegmentTerminalGoal`:

- **Terminal runway** (takeoff goal, or path destination is on this runway): atomically reserve the contiguous runway resource via `TryReserveRunwayResourcesAtomic`.
- **Transit runway** (using runway as a taxiway bridge): all-or-nothing pre-entry reservation of the *full crossing chain* — every runway resource crossed, plus every tile up to and including the **first safe stop** on the far side (a ONE_WAY queue tile, a stand/hangar/helipad, or the path goal). The chain is fully validated first; nothing is committed unless the whole chain is free. If no safe stop is reachable past the runway, or any tile/resource is blocked, entry is denied and the aircraft waits *before* the runway (on its prior safe stop).

This makes runway entry self-sufficient for every aircraft class: stepping onto a transit runway guarantees a reserved path off it to a place where the aircraft can wait indefinitely. It never halts on the runway or on transit grass/apron. The walk lives in the `RUNWAY` branch of `TryReserveTaxiSegment`; `IsModularSafeStopTile` decides where the chain may stop.

## 4. Reserve-then-reconcile (Reservation V2)

Per-step movement uses `TryReserveTaxiSegment` to acquire forward, then a deterministic release pass:

1. `BuildReservationKeepSet(v, st, &keep_set)` computes the tiles that should remain reserved.
2. `ReconcileAircraftReservations(v, st, keep_set, "post-step")` releases everything owned that isn't in the keep-set, and prunes `taxi_reserved_tiles` / `modular_runway_reservation` to match.

`keep_set` includes:
- Current tile (`v->tile`).
- Forward segment horizon: tiles from `max(seg.start_index, taxi_path_index)` through `seg.end_index`, plus the first tile of the next segment.
- Landing-chain continuity (every tile in `v->landing_chain_path`, until the rollout transition discards it).
- Runway resources owned along the remaining path (covers takeoff intent, active traversal, and pre-committed transit crossings).
- Full `modular_runway_reservation` when `ShouldRetainRunwayReservation(v, st)` returns true (takeoff intent on the still-valid takeoff runway).

Runway resources are atomic on retention too: if any tile of resource `R` is in the keep-set, the helper expands it to the full contiguous runway. `ClearTaxiPathReservation` is reserved for transitions and force-clear; normal per-step release is reconciler-driven.

## 5. Landing chain (pre-touchdown reservation)

`TryReserveLandingChain` reserves before commit:
- The landing runway atomically (terminal mode).
- A post-runway chain to the first safe queueing point:
  - first non-runway segment is `ONE_WAY` → entry tile reserved.
  - first non-runway segment is `FREE_MOVE` → whole segment reserved + one exit tile.

The computed path is stored in `landing_chain_path` and reused after touchdown when possible. If no ground goal exists, landing is only allowed when there is a safe `ONE_WAY` buffer after the runway.

## 6. Path rebuilding and retargeting

- Taxi path is rebuilt when invalid or out of sync with current tile/goal.
- After waiting >64 ticks, `TryRetargetModularGroundGoal` can reroute to a different terminal/helipad/hangar/rollout. Reservations stay until retarget actually succeeds and replaces them. Takeoff goals (`MGT_RUNWAY_TAKEOFF`) are intentionally **not** retargeted — focus debugging effort on contention and segment progression there.

## 7. Hangars

- Multi-capacity in reservation logic: never set map-level reservation bits, never block another aircraft.
- Vehicle-level intent is still tracked in `taxi_reserved_tiles` so path cleanup works.

## 8. Common pitfalls

### Pitfall 1: Transit-runway entry must guarantee a safe stop beyond (resolved)

Entering a transit runway reserves the entire crossing chain to the first safe
stop (see §3 `RUNWAY`), so an aircraft never steps onto a runway — or off it
onto unreserved grass — without owning a chain to a place it can wait. This is
enforced uniformly for fixed-wing and helicopters inside the `RUNWAY` branch of
`TryReserveTaxiSegment`; there is **no** vehicle-class special case at the
`AirportMoveModular` segment boundary anymore.

History: this used to pin only the first continuation tile, leaving the rest of
the downstream `FREE_MOVE` segment unreserved, so fixed-wing could strand on
grass. A helicopter-only revalidation at the segment boundary patched the
symptom; the full-chain transit contract removed the root cause and let the
special case be deleted. Tightening the fixed-wing contract cost ~2% on the
`mass6-inair.sav` baseline (floor lowered 9200 → 9000) — the price of the
provable safe-stop guarantee.

### Pitfall 2: `FindRunwayTransitContinuationTile` is informational only

It returns the first non-runway, non-service-style tile after a transit runway —
usually grass, which is *not* a safe stop. It is now used only for keep-set
hints and debug logging. Reservation safety comes from the full-chain transit
contract (§3), which reserves *past* this tile to a real safe stop. Don't
reintroduce logic that treats "owns continuation tile" as "can safely halt".

### Pitfall 3: `taxi_reserved_tiles` vs map state

`SetTaxiReservation` blindly overwrites the map-level reserver bit; the caller must have already verified that no other vehicle owns the tile. Likewise, the reconciler edits both the map bits and the vehicle vectors — divergence between vector and map state usually means something wrote map state without going through `SetTaxiReservation`, or vice versa.

The `owned-reservations` log line reads map state; `tracked-runway` reads `modular_runway_reservation`. A mismatch is a useful red flag. See `skills/stuck_plane_debugging.md`.

### Pitfall 4: Runway deny is sticky if you already own the resource

`TryReserveContiguousModularRunway` does not clear runway ownership on deny if the aircraft already owns the exact requested contiguous runway. Deny clears happen only when existing runway ownership is stale or mismatched. Don't write recovery code that assumes a deny implies a clean slate.

### Pitfall 5: Reservation clears are rendering invalidation events

The reservation overlay is drawn from map-level reservation bits, but changing those bits does not automatically repaint the viewport. Modular airport code that clears a reservation (`SetAirportTileReservation(t, false)`) must also dirty the affected tile, otherwise the colored overlay line can remain visible until some later redraw happens.

Use the local reservation-clear helper in `src/modular_airport_cmd.cpp` for modular airport cleanup paths instead of calling `SetAirportTileReservation(..., false)` directly. This applies to normal reconciliation, taxi/runway transition cleanup, and stale-reservation fallback cleanup.

### Pitfall 6: Per-tick targeting that reads shared-resource state without claiming it

If a per-tick movement function (`AirportMoveModular*`) sets its target based on "is the shared resource free?" while the actual reservation happens later in a separate handler (e.g. `AircraftEventHandler_Flying` → `TryReserveLandingChain`), then in the window between one aircraft freeing the resource and the next aircraft committing, **every** pre-commit aircraft simultaneously sees "available" and redirects to the same point. Visible as synchronized convergence at state transitions — a cluster of holding helicopters all flying to the landing tile in unison, scattering back to holding once one of them commits.

Cure: don't let movement read the shared-resource bit unless movement also reserves. Keep movement targeting on a private waypoint (holding pattern, current path) until the commit handler claims the resource and changes state — after which a different movement function takes over off the committed state. Helicopters in `AirportMoveModularFlying` now always target the holding waypoint; commit happens in `AircraftEventHandler_Flying`, and post-commit movement runs in `AirportMoveModularLanding` driven by `HELILANDING` state.

## 9. Debugging policy

When evaluating proposed fixes for stuck aircraft:

- Prefer strict "can I enter?" rules before movement into `FREE_MOVE` or runway-transit sections.
- Prevent unsafe entry rather than trying to "unstick" later.
- `[FALLBACK]` cleanup paths (stale-clear, orphan-clear, force-clear-all) are safety nets — frequent occurrences mean an upstream contract is wrong, not that the fallbacks need tuning.
- If a plane stops on a `FREE_MOVE` tile, the first question is whether entry should have been denied earlier.
