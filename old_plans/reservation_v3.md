# Reservation V3 Plan: Runway-As-Taxiway Reliability

## Goal
Prevent aircraft from entering runway-transit movement unless they already hold a complete reservation chain to a safe non-runway continuation tile, while preserving correct behavior when runway is the terminal goal (takeoff/rollout).

## 1) Classify runway segments by intent at reservation time

- Add helper in `src/modular_airport_cmd.cpp`:
  - `IsRunwaySegmentTerminalGoal(const Aircraft *v, const TaxiPath *path, const TaxiSegment &seg)`
- Return true only when the runway segment is terminal runway use:
  - `MGT_RUNWAY_TAKEOFF` and takeoff states (`TAKEOFF`, `STARTTAKEOFF`, `ENDTAKEOFF`), or
  - rollout intent/state (`MGT_ROLLOUT` while runway is still terminal movement).
- Otherwise classify as runway-transit (runway used like free-move taxiway crossing).

## 2) Enforce different reservation contracts in `TryReserveTaxiSegment()`

- Terminal-goal runway segment:
  - Keep current behavior: reserve contiguous runway resource atomically.
- Transit runway segment:
  - Require atomic reservation chain before entry (all-or-nothing):
    - runway resource(s),
    - hold tile (current tile),
    - continuation tile (safe non-runway tile after runway transit).
  - If any part is unavailable, reserve nothing and deny entry to runway transit.
  - If continuation tile cannot be derived, deny reservation.
  - If continuation tile is blocked (reserved/occupied by other), deny reservation.
  - Do not allow entering transit runway without owning continuation chain.

## 3) Safe continuation tile derivation

- Add helper in `src/modular_airport_cmd.cpp`:
  - `FindRunwayTransitSafeContinuationTile(const Aircraft *v, const Station *st, const TaxiPath *path, const TaxiSegment &seg)`
- Derivation rules:
  - Walk forward from `seg.end_index + 1` until first non-runway taxiable tile.
  - Exclude stand/hangar/helipad unless it is the explicit path goal.
  - Validate "safe":
    - not reserved by other,
    - not occupied by other,
    - not a known immediate choke tile that creates direct head-on runway exit deadlock.
  - Choke tile rule:
    - prefer taxiway/apron tiles (`IsTaxiwayPiece`/apron-class pieces),
    - reject tiles that are the sole exit connector of a different runway resource.
  - If no safe tile found, return `INVALID_TILE` (reservation denied for runway transit).

## 4) Keep-set/reconcile alignment (`BuildReservationKeepSet`)

- For active transit-runway segment, keep-set must include:
  - current tile,
  - remaining runway resource tiles needed by the active segment,
  - transit continuation tile,
  - hold tile (if used by crossing reservation contract).
- Preserve these until aircraft advances past the continuation boundary.
- Reconcile continues to be single source of release; no fallback cleanup for this flow.
- Path rebuild rule while already on transit runway:
  - on path rebuild, re-derive continuation tile from the new path immediately,
  - if re-derivation fails, keep current physical tile + runway resource, deny further advance until a valid continuation is derivable.

## 5) Invariant logging/asserts

- Add debug invariant check when entering a transit runway segment:
  - log violation if continuation tile is not currently owned by this aircraft.
- Extend stuck diagnostics for transit-runway waits:
  - include computed continuation tile,
  - reservation/occupancy owner for continuation tile,
  - whether segment is classified terminal vs transit.

## 6) Regression scenarios (must pass)

- Harhill-like deadlock:
  - runway occupant exiting via shared connector while takeoff queue exists.
  - expected: connector is reserved by runway-transit aircraft before entry, no circular wait.
- Normal takeoff:
  - path to runway goal behaves as today, no over-reserving past runway.
- Rollout:
  - runway remains terminal behavior; exit to service still succeeds.
- Runway turn-back:
  - runway tiles needed twice are retained correctly by keep-set logic and released after last need.

## 7) Documentation update

- Update `taxi_rules.md`:
  - define two runway reservation modes:
    - terminal runway mode (takeoff/rollout),
    - transit runway mode (free-move style reserve-through to safe continuation).
  - state invariant explicitly:
    - no runway-transit entry without owned continuation reservation chain.
