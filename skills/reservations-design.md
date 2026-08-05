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

- **Terminal runway** (the tile ground movement ends on lies on this segment): atomically reserve the contiguous runway resource via `TryReserveRunwayResourcesAtomic`. A takeoff *target* does not make every runway on the path terminal — a takeoff path may cross one runway to reach another, and the crossed one is transit.
- **Transit runway** (using runway as a taxiway bridge): all-or-nothing pre-entry reservation of the *full crossing chain* — every runway resource crossed, plus every tile up to and including the **first safe stop** on the far side (a ONE_WAY queue tile, a stand/hangar/helipad, or the path goal). The chain is fully validated first; nothing is committed unless the whole chain is free. If no safe stop is reachable past the runway, or any tile/resource is blocked, entry is denied and the aircraft waits *before* the runway (on its prior safe stop).

This makes runway entry self-sufficient for every aircraft class: stepping onto a transit runway guarantees a reserved path off it to a place where the aircraft can wait indefinitely. It never halts on the runway or on transit grass/apron.

The walk is `BuildRunwayCrossingChain`, and it is the **single** implementation — the entry decision, keep-set retention, and the stuck diagnostics all call it, so they cannot drift apart. `IsModularSafeStopTile` decides which *tiles* may end a chain; the aircraft's own goal is tested separately by the walker.

#### Termination invariant (why the contract is always satisfiable)

**The chain walk always reaches a terminator, because the goal test precedes every piece-type test and the goal is the last tile of the path.** Two consequences:

- A goal that is *itself a runway tile* — a computed helicopter pad or takeoff tile that fell back to a runway end — terminates the chain like any other goal. Its runway resource folds into the atomic acquisition, and the aircraft may stop there because that is exactly where its ground movement ends and it departs.
- `NO_SAFE_STOP` therefore cannot be a traffic state. It means the path does not end at `ground_path_goal`, which is an internal inconsistency, and it is logged as `runway-transit-invariant`, not as a deny.

The walker's loop body deliberately contains **no `continue`**: every tile is classified and then tested for termination. That structure is the guarantee — any classification branch that skips the terminator test reintroduces a permanently unsatisfiable contract, i.e. an aircraft that waits forever on a completely empty airport. That is precisely the bug this shape exists to prevent; see Pitfall 2.

Combined with all-or-nothing acquisition (`TryReserveRunwayResourcesAtomic` validates every resource before mutating any, and continuation tiles are validated before any `SetTaxiReservation`), a denied attempt leaves the aircraft holding exactly what it held before. No hold-and-wait on runway resources, so blocked aircraft cannot form a circular wait over them.

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

### Pitfall 2: Never classify a tile before asking whether it ends the chain

The crossing walk must test "is this the goal / a safe stop?" *before* any
piece-type dispatch, and must never `continue` past a tile it has not tested.

This has now caused the same permanent deadlock twice. `FindRunwayTransitContinuationTile`
(deleted) skipped runway tiles before its goal check; that was latent, because it
only ever produced a hint. `a7346e86c8` copied the ordering into the real
full-chain walk, where it became fatal: a helicopter whose computed pad had
fallen back to a runway end could never satisfy the contract, and waited forever
on an *empty* airport with every blocker reading false. `BuildRunwayCrossingChain`
now has one code path, no `continue`, and the goal tested first.

Two smells to watch for when touching this area:

- A predicate that silently folds the aircraft's goal into a tile-property test
  (the old three-argument `IsModularSafeStopTile(st, tile, goal)`). It hides the
  goal behind a piece-type check that an earlier branch can skip. The goal is a
  property of the *aircraft*, not of the tile — keep it at the call site.
- Two functions that both answer "what is past this runway". They will disagree,
  and the log will confidently print the wrong one's answer while the other denies
  entry. That is exactly how this bug hid in plain sight.

### Pitfall 3: A reservation off a safe stop is not the reconciler's to reclaim

Retention is normally justified by a path — the active `taxi_path` or the stored
`landing_chain_path`. A landing committed through the **no-ground-goal** branch
of `TryReserveLandingChain` has neither: it reserves the runway plus a one-way
buffer to queue on, then deliberately resets the path. Nothing justified the
buffer, so the next reconcile released it, and the aircraft reached the rollout
end owning nothing — standing on a runway with the guarantee that permitted its
landing already thrown away.

`BuildReservationKeepSet` therefore keeps reserved **safe-stop** tiles whenever
the aircraft is standing on a runway. Landing is only allowed against a reserved
route to a safe stop (§5), so until it is on one, that claim is what makes its
position legal.

Keep the condition narrow. Retaining *everything* an aircraft holds while off a
safe stop costs ~10% throughput on `mass7-inair`, and retaining safe stops from
any non-safe-stop tile still costs ~4%; restricting it to aircraft standing on a
runway fixes the invariant and *gains* throughput, because it targets the landing
case instead of every apron transit. Measure before widening it.

### Pitfall 4: `taxi_reserved_tiles` vs map state

`SetTaxiReservation` blindly overwrites the map-level reserver bit; the caller must have already verified that no other vehicle owns the tile. Likewise, the reconciler edits both the map bits and the vehicle vectors — divergence between vector and map state usually means something wrote map state without going through `SetTaxiReservation`, or vice versa.

The reconciler now *relies* on the vectors being authoritative: its release pass walks `taxi_reserved_tiles` + `modular_runway_reservation` (not the whole airport) to find map bits to clear, so per-step reconcile is O(reserved) not O(tiles). Every map-bit setter (`SetTaxiReservation`, `TryReserveContiguousModularRunway`, `TryReserveRunwayResourcesAtomic`) records into one of these vectors — if you add a new setter, it **must** track here or its bit can leak (the reconciler will never see it).

The `owned-reservations` log line reads map state; `tracked-runway` reads `modular_runway_reservation`. A mismatch is a useful red flag. See `skills/stuck_plane_debugging.md`.

### Pitfall 5: Runway deny is sticky if you already own the resource

`TryReserveContiguousModularRunway` does not clear runway ownership on deny if the aircraft already owns the exact requested contiguous runway. Deny clears happen only when existing runway ownership is stale or mismatched. Don't write recovery code that assumes a deny implies a clean slate.

### Pitfall 6: Reservation clears are rendering invalidation events

The reservation overlay is drawn from map-level reservation bits, but changing those bits does not automatically repaint the viewport. Modular airport code that clears a reservation (`SetAirportTileReservation(t, false)`) must also dirty the affected tile, otherwise the colored overlay line can remain visible until some later redraw happens.

Use the local reservation-clear helper in `src/modular_airport_cmd.cpp` for modular airport cleanup paths instead of calling `SetAirportTileReservation(..., false)` directly. This applies to normal reconciliation, taxi/runway transition cleanup, and stale-reservation fallback cleanup.

### Pitfall 7: Per-tick targeting that reads shared-resource state without claiming it

If a per-tick movement function (`AirportMoveModular*`) sets its target based on "is the shared resource free?" while the actual reservation happens later in a separate handler (e.g. `AircraftEventHandler_Flying` → `TryReserveLandingChain`), then in the window between one aircraft freeing the resource and the next aircraft committing, **every** pre-commit aircraft simultaneously sees "available" and redirects to the same point. Visible as synchronized convergence at state transitions — a cluster of holding helicopters all flying to the landing tile in unison, scattering back to holding once one of them commits.

Cure: don't let movement read the shared-resource bit unless movement also reserves. Keep movement targeting on a private waypoint (holding pattern, current path) until the commit handler claims the resource and changes state — after which a different movement function takes over off the committed state. Helicopters in `AirportMoveModularFlying` now always target the holding waypoint; commit happens in `AircraftEventHandler_Flying`, and post-commit movement runs in `AirportMoveModularLanding` driven by `HELILANDING` state.

### Pitfall 8: Never park an aircraft on a `ONE_WAY` tile

A one-way tile is a queueing corridor, not a parking space. Holding an aircraft
there (computed heli pad, fallback holding spot, giving up mid-corridor) is a
permanent deadlock: it blocks the corridor, and it can only move in the flow
direction — so if that leads somewhere it can't use, it has no legal move at
all. `ComputeModularHeliTiles` hit this and now skips `one_way_taxi` tiles.

Note when touching that heuristic: making its building-adjacency filter a soft
penalty (to get a central pad instead of a runway-end fallback) costs ~500
movements on `helis2.sav`. Tried and reverted; re-measure before retrying.

## 9. Debugging policy

When evaluating proposed fixes for stuck aircraft:

- Prefer strict "can I enter?" rules before movement into `FREE_MOVE` or runway-transit sections.
- Prevent unsafe entry rather than trying to "unstick" later.
- `[FALLBACK]` cleanup paths (stale-clear, orphan-clear, force-clear-all) are safety nets — frequent occurrences mean an upstream contract is wrong, not that the fallbacks need tuning.
- If a plane stops on a `FREE_MOVE` tile, the first question is whether entry should have been denied earlier.
