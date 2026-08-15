# The Borrowed AirportSpec: AT_SMALL and a Possible AT_MODULAR

## The situation

A modular airport has no `AirportSpec` of its own. It borrows a preset one:

- `src/modular_airport_build.cpp:854` — tile-by-tile builds set `st->airport.type = AT_SMALL`
- `src/modular_airport_build.cpp:1172` — template builds set `st->airport.type = airport_type`, the template's preset

`AirportBlock::Modular` (bit 61 of `airport.blocks`) is what actually marks the airport as modular. The type is a stand-in, and `Airport::GetSpec()` happily returns the preset's spec for it.

This is not merely cosmetic. Modular code paths *read* that spec, and every read silently returns preset data that may have nothing to do with the tiles the player laid down.

## What the borrowed spec is supplying today

| Spec field | Read at | Status for modular |
|---|---|---|
| `depots` | `HasHangar`, `GetHangarTile`, `GetNumHangars`, `GetHangarExitDirection` (`station_base.h`) | **`HasHangar` fixed** (see below); the other three still read the preset |
| `fsm` | `GetFTA()`, every handler modular does not intercept | still preset; see the risk note |
| `noise_level` | `station_cmd.cpp:2664`, `station_cmd.cpp:2839`, `modular_airport_build.cpp:486` | still preset |
| `maintenance_cost` | `station.cpp:728` | still preset |
| `catchment` | `station.cpp:325`, `station.cpp:359` | already layout-derived via `GetModularAirportCatchmentRadius` |
| `ttd_airport_type` | `newgrf_engine.cpp:514`, `newgrf_station.cpp:421` | still preset; NewGRF-visible |
| `size_x`/`size_y`, `layouts` | `station_cmd.cpp:2693`, `station_cmd.cpp:3705` | still preset |

## The bug this produced (fixed on `heli-hangarless-modular-airport`)

`AT_SMALL`'s spec carries a depot, so `HasHangar()` returned true for *every* modular airport — including one that is a single helipad tile.

Stock cannot reach this state. A heliport's spec is declared with the `AS_ND` macro ("no depot"), so `HasHangar()` is false and the order is refused at all five gates: order insertion (`order_cmd.cpp:705`), `Aircraft::FindClosestDepot` (`aircraft_cmd.cpp:480`), `FindNearestHangar` (`aircraft_cmd.cpp:218`), automatic servicing (`CheckIfAircraftNeedsService`), and the rebuild purge (`aircraft_cmd.cpp:3006`). The stock heli-endlanding handler even states the assumption out loud: *if an airport has a terminal, it also has a hangar*.

With the lie in place, a helicopter could be sent for service to an airport that could never service it. Wanting a hangar suppresses helipad and stand selection, so on arrival it found nothing it would park on, left by the departure ladder, and scored the same airport best again on the next approach — landing and lifting off forever.

Observed live (station 17, "Flafingway Docks", one tile of `piece_type=55`):

```
V365 hangar-diagnostics reason=terminal_depot_no_hangar tile=69574
V365  no hangar pieces on airport 17
V365 takeoff: FindRunway=INVALID vtile=69574
```

**The fix** made hangar presence layout-derived (`ModularAirportHasHangar`, cached on `MarkLayoutDirty`), required it at the decision point (`ModularAircraftWantsHangar` now takes the station), and added cleanup so the invariant holds: cancel hangar orders when the last hangar tile goes, plus an unconditional repair in `AfterLoadGame`.

Note that layer two was not redundant. An aircraft with `NeedsAutomaticServicing()` true and no depot order at all reaches the same loop, and the five gates do not cover it.

## Research: which ID a new AT_MODULAR should take

### What the NewGRF spec constrains

- GRF-visible airport IDs are **0–9 only** — the built-in types. Property 08 (airport type override) takes one of those as substitute/override; `FF` disables an original airport.
- The cap is **128 airports total**, and the spec says all original types count toward it, so it should not be raised.
- NewGRF airports' runtime IDs are **assigned by OpenTTD, never by the GRF**: `AirportOverrideManager::AddEntityID` (`newgrf_airport.cpp:139`) hands out the first free slot from `NEW_AIRPORT_OFFSET` upward, keyed on (grf local id, grfid). A GRF never sees or references another airport's runtime ID.

Sources: [Action0/Airports](https://newgrf-specs.tt-wiki.net/wiki/Action0/Airports), [AirportTypes](https://newgrf-specs.tt-wiki.net/wiki/AirportTypes).

**So inserting a new built-in type at ID 10 breaks no GRF.**

### The real exposure is savegames

`airport.type` is stored raw as `SLE_UINT8` (`station_sl.cpp:524`) with no remapping on load. NewGRF runtime IDs are stable only because the same GRF set reloads in the same order. Shifting `NEW_AIRPORT_OFFSET` from 10 to 11 makes every NewGRF airport in an existing save read one slot low — a save's type-10 airport would come back as the modular type.

This needs a savegame version bump plus an afterload remap (`type += 1` for `10 <= type < NUM_AIRPORTS`), sequenced **before** `afterload.cpp:2900`, which derives `psa->grfid` from the type.

### Why not a high ID instead

Giving modular something like 253 (below `AT_INVALID` = 254) would dodge the remap entirely, but `AirportSpec::Get` asserts `type < 128` and indexes a fixed `specs[128]`. Several callers index it directly rather than through `GetSpec()`:

- `newgrf_airport.cpp:269`
- `newgrf_airporttiles.cpp:223`
- `afterload.cpp:2900`
- the whole script API (`script_airport.cpp`)

Every one would need a special case, and a missed one is an out-of-bounds read. Rejected.

### Recommendation

`AT_MODULAR = 10`, `NEW_AIRPORT_OFFSET = 11`, with a real eleventh entry in `_origin_airport_specs` (the array is sized by `NEW_AIRPORT_OFFSET`). Costs one NewGRF slot: 118 → 117.

Also worth doing: harden `AirportOverrideManager::Add` to refuse `entity_type == AT_MODULAR`, since raising `max_offset` makes property-08 value 10 valid and would otherwise let a GRF override the modular spec.

The script API needs no constant changes — `script_airport.hpp` mirrors only named built-ins, all below 10, plus `AT_INVALID` = 254.

## Risks to handle before switching the type over

**The FSM is the dangerous one.** Modular intercepts landing, takeoff, flying and ground movement in `AirportGoToNextPosition`, but not everything: aircraft still carry an FTA `pos` (observed values of 15 and 7 in a live game), `AircraftGetEntryPoint` still runs, and the FLYING handler is still dispatched with `apc`. Give `AT_MODULAR` a small FSM of its own and a `pos` of 15 becomes an out-of-bounds layout read. Whatever spec it gets must keep a layout at least as large as the presets it replaces — cloning `AT_SMALL`'s FSM preserves today's behaviour for tile-by-tile airports exactly, but **changes it for template-built ones**, which currently run on their preset's FSM.

**Noise accounting drifts on existing saves.** Noise is added at build time and subtracted at removal using whatever the spec says *at that moment*. A load conversion that retypes an existing template-built modular airport from, say, `AT_INTERCON` to `AT_MODULAR` would subtract a different value than was added, and town noise drifts permanently.

This is the crux of the design decision. `airport.type` currently does double duty: "which movement/spec model" *and* "which preset this airport is billed as". Two ways to split it:

1. **Add a saved `accounting_type` field** holding the preset (`AT_SMALL` for hand-built, the template's preset otherwise), and point the noise and maintenance reads at it. Existing saves keep their current numbers. Safe, and the recommended default.
2. **Derive noise and maintenance from the layout**, the way catchment already is. More principled for a tile-by-tile airport — a 40-tile modular airport would stop being billed as a small one — but it changes numbers in running games and can push towns over their noise limit. A deliberate gameplay change, best kept separate.

## What the enum change is actually worth

It does not fix anything by itself. Its value is that the next `GetSpec()` misuse becomes *visible* instead of silently returning plausible small-airport data — which is precisely how the hangar bug hid for so long. `disaster_vehicle.cpp:744` is a good example of a site that already has to special-case modular alongside `AT_SMALL`/`AT_LARGE` and would read better with an explicit type.

Remaining spec-vs-layout inconsistencies worth auditing whenever this is picked up, all pre-existing:

- `GetHangarTile`, `GetNumHangars`, `GetHangarExitDirection` return preset-derived tiles for modular airports — now inconsistent with the layout-derived `HasHangar`
- `station_cmd.cpp:2828` iterates spec hangars to close depot windows on removal
- `script_airport.cpp:92` and `script_depotlist.cpp:34` expose the preset's hangar count and a spec-offset tile to AI/GS scripts, so a script can see `HasHangar() == false` alongside `GetNumHangars() == 1`. `script_order.cpp:257` already returns `INVALID_TILE` correctly
- `AirportFTAClass::num_helipads` describes the preset, not the layout (already worked around at `modular_airport_cmd.cpp` in the helipad-servicing path)
