# Modular airports: giving them their own type, and deriving noise and maintenance from the layout

Implementation plan. Self-contained: everything needed to execute it is here.

---

## 1. The problem

A modular airport has no `AirportSpec` of its own. It borrows a preset one, and which
preset it borrows depends on how it was built:

| Build path | `airport.type` | `airport.layout` |
|---|---|---|
| Tile by tile (`BuildModularAirportTile_Apply`, `modular_airport_build.cpp:858`) | `AT_SMALL` | 0 |
| Template placement (`modular_airport_template_cmd.cpp:580`, same apply function) | `AT_SMALL` | 0 |
| "Build as modular" from a stock layout (`modular_airport_build.cpp:1176`) | the stock preset | the picked layout |

What actually marks an airport as modular is `AirportBlock::Modular` (bit 61 of
`airport.blocks`). The type is a stand-in, and `Airport::GetSpec()` returns the preset's
data for it — data that describes a layout the player never built.

Every read of that spec is a silent lie. Several of them are live bugs.

### 1.1 Confirmed bugs

**a. Fast jets can never find a modular hangar.**
`FindNearestHangar` (`aircraft_cmd.cpp:223`) skips any airport whose FTA carries the
`ShortStrip` flag when the aircraft is `AIR_FAST`. `AT_SMALL`'s FTA carries it. So *every*
hand-built and template-built modular airport is invisible to "send to nearest depot" and
to automatic-servicing routing for jets — including a 40-tile airport with two paved
8-tile runways. `order_cmd.cpp:1716` is the one short-strip gate that already knows about
modular airports (it tests `!blocks.Test(Modular)` and has a modular sibling branch at
1721-1726). `MaybeCrashAirplane` (`aircraft_cmd.cpp:1519`) has no modular test — it is
simply never reached by modular aircraft, which use
`ModularAircraftHasElevatedOverrunRisk` (`aircraft_cmd.cpp:1534`) instead.

The same function has a **second** spec-derived gate at `aircraft_cmd.cpp:226`: it skips
airports whose FTA lacks the `Airplanes` flag. Both gates need fixing, not just the first.

**b. What can land — and what a hangar can build — is decided by the preset, not the tiles.**
`CanVehicleUseStation` (`vehicle.cpp:3107`) tests the FTA `Airplanes` / `Helicopters`
flags:

- Hand-built (`AT_SMALL`) → both flags set. A single-helipad modular airport accepts
  plane orders; the plane arrives, finds no runway, and cannot land.
- From-stock heliport / helidepot / helistation → `Helicopters` only. Add a runway and
  three stands to that airport and planes still refuse it, forever.

The user-visible symptom: **a hangar in a stock helidepot or helistation built as modular
can only build helicopters, while the identical layout laid down by hand can build any
plane.** Both the build-list filter (`build_vehicle_gui.cpp:1522`) and the command gate
(`aircraft_cmd.cpp:348`) route through `CanVehicleUseStation`, so the borrowed FTA decides
what the hangar sells. `_airportfta_helidepot` and `_airportfta_helistation` are declared
with the `HELIPORT` macro (`airport.cpp:47`), which sets `Helicopters` only; `AT_SMALL`
sets both.

`Aircraft::FindClosestDepot` (`aircraft_cmd.cpp:476-490`) routes entirely through
`CanVehicleUseStation` + `HasHangar`, so it is fixed for free by fixing those.

**c. The hangar accessors are half-converted.** `Airport::HasHangar()` became
layout-derived in commit `4db0bc8cb8`. Its four siblings did not:

| Accessor | Modular status | Reachable on a modular airport? |
|---|---|---|
| `HasHangar` | layout-derived | yes |
| `GetNumHangars` | preset | yes — `script_airport.cpp:102`, `script_depotlist.cpp:34` |
| `GetHangarTile` | preset | yes — `script_airport.cpp:116`, `script_order.cpp:258` |
| `GetHangarNum` | preset | no — `station_cmd.cpp:3903` and `aircraft_cmd.cpp:352` branch on modular first |
| `GetHangarExitDirection` | preset | no — `HandleModularHangar` intercepts before `aircraft_cmd.cpp:1846` |

A script can currently see `HasHangar() == false` next to `GetNumHangars() == 1`, and
`GetHangarTile(0)` hands it a spec-offset tile that may well be a runway.

**d. NewGRF-visible airport type is the preset's.** `newgrf_engine.cpp:514` (aircraft
var 0x44) and `newgrf_station.cpp:421` (station var 0xF1) both report `ttd_airport_type`
from the borrowed spec. `newgrf_airporttiles.h:39` stores `airport_id = st->airport.type`
into the airport-tile scope resolver as well; it appears unread by `GetVariable` today,
but it is a third NewGRF-visible copy of the type.

**e. The land-info window names the preset.** `FillTileDescAirport`
(`station_cmd.cpp:3705`) prints the spec's class and name, so one and the same layout
reads as "Country airfield" when hand-built and "Intercontinental Airport" when built
from stock.

**f. Noise and maintenance are billed against the preset.** A one-tile helipad hand-built
pays the same maintenance as a country airfield; a 99-tile hand-built megaport pays the
same. The same layout built from stock pays the preset's rate instead. Catchment is the
one field already derived from the layout (`GetModularAirportCatchmentRadius`).

Complete list of noise sites, needed by §3.4:

| Site | Role |
|---|---|
| `station_cmd.cpp:2664` | full recompute in `UpdateAirportsNoise` |
| `modular_airport_build.cpp:490` / `553` | tile-removal path: computes, then subtracts, on last tile |
| `modular_airport_build.cpp:786` | tile-by-tile build: authority gate + level for a new facility |
| `modular_airport_build.cpp:1073` / `1079` | **from-stock build: level and authority gate, using the preset spec** |
| `modular_airport_build.cpp:1173` | from-stock build: `noise_reached +=` |
| `modular_airport_template_cmd.cpp:431` | template placement: level for a new facility |

Maintenance is read at `station.cpp:728`, and the `>> 3` scaling convention is duplicated
at `airport_gui.cpp:488` (build-window tooltip) and `script_airport.cpp:173`
(`GetMonthlyMaintenanceCost`).

**g. Flooding uses the preset's `delta_z`.** `water_cmd.cpp:1057-1059` compares
`v->z_pos` against `st->airport.GetFTA()->delta_z + 1`. `AT_SMALL` has `delta_z = 0`;
`AT_HELIPORT` has 60. A modular airport built from the stock heliport therefore never
floods its parked aircraft.

### 1.2 What is *not* affected (verified — do not chase these)

- **`RemoveAirport`'s `GetNumHangars` loop (`station_cmd.cpp:2828`) is unreachable for
  modular.** `RemoveAirport` (`station_cmd.cpp:2807`) has exactly one caller,
  `ClearTile_Station`, which dispatches modular tiles to `RemoveModularAirportTile` at
  `station_cmd.cpp:5003` before reaching the `RemoveAirport` call at `:5004`.
- **`assert(st->airport.HasHangar())` at `aircraft_cmd.cpp:1210` is unreachable for
  modular.** `AirportGoToNextPosition` (`aircraft_cmd.cpp:2443`) returns unconditionally
  out of its modular block (2454-2565) before the sole `AircraftController` call at
  `:2569`. `AircraftEventHandler_Flying` is the one handler dispatched with `apc` for
  modular, and it returns before touching `apc->layout[v->pos]` (`:2242`). Worth
  remembering anyway: now that `HasHangar` tells the truth, that assert *would* fire if a
  future change ever routed modular aircraft through the classic controller.
- **The modular marker cannot be clobbered by FTA block bookkeeping.**
  `AirportBlock::Modular` is bit 61 (`airport.h:155`); the largest bit any FTA table uses
  is `OutWay3 = 31`, and no table entry references `Modular`, `Zeppeliner` or
  `AirportClosed`. So `vehicle.cpp:828`'s
  `blocks.Reset(layout[pos] | layout[previous_pos])` cannot clear it.
- **All `airport.type` equality comparisons in the tree are against `AT_OILRIG`**
  (`aircraft_cmd.cpp:1037`/`1145`, `station_cmd.cpp:461`/`2661`/`2730`/`4843`,
  `town_cmd.cpp:3358`, `script_town.cpp:385`, `modular_airport_build.cpp:801`/`1087`,
  `modular_airport_template_cmd.cpp:460`). None of them is affected by a new type value.

### 1.3 Build-path divergences

Comparing a stock layout built with "Build as modular" against the same layout laid down
by hand, the state that actually differs:

| Field | From stock | Manual / template | Real difference? |
|---|---|---|---|
| `airport.type` | preset | `AT_SMALL` | **yes** — drives §1.1 a–g |
| `airport.layout` | picked layout | 0 | no for stock: every stock spec has exactly one layout |
| `airport.rotation` | spec layout rotation | `DIR_N` | no for stock: all stock layouts are `DIR_N` |
| noise charged | preset's `noise_level` | `AT_SMALL`'s | **yes** |
| maintenance charged | preset's `maintenance_cost` | `AT_SMALL`'s | **yes** |
| `runway_flags` on non-runway tiles | `0` (`modular_airport_build.cpp:1212`) | `RUF_DEFAULT` = `0x0F` (`base_station_base.h:40`/`:50`) | inert today, but it is saved state that differs for identical layouts |
| `edge_block_mask` | derived from stock fence gfx, then mirrored onto neighbours | only what the fence tool sets | not a flag difference — those fences are real layout data the player can reproduce |
| station naming | `STATIONNAMING_AIRPORT`/`HELIPORT` by FTA flag (`modular_airport_build.cpp:1105`) | always `STATIONNAMING_AIRPORT` | naming only, at creation |

Separately: **`CmdBuildModularAirportFromStock` accepts any `airport_type < NUM_AIRPORTS`,
including NewGRF airports**, and the GUI offers the modular toggle for every class
(`airport_gui.cpp:308`). Three things go wrong, in increasing order of how hard they are
to fix:

1. **Runway flags are hardcoded per stock type.** The `runway_configs` switch
   (`modular_airport_build.cpp:1150-1170`) has cases only for the six stock airport types;
   anything else falls through to `default: break`, leaving `runway_configs` empty. Every
   tile keeps `tile_data.runway_flags = 0` (`:1212`) — no landing, no takeoff, no
   direction. The resulting airport cannot be used at all.
2. **Rotation is assumed.** `tile_data.rotation = 0` is set unconditionally with the
   comment *"Stock layout 0 = DIR_N; all horizontal runways"* (`:1208`). NewGRF layouts
   declare their own rotation, which the parser masks to one of the four cardinals
   (`newgrf_act0_airports.cpp:89`), and may define several layouts. A rotated layout gets
   wrong per-tile rotation metadata, so its runways are recorded on the wrong axis.
3. **GRF-defined tiles have no modular semantics.** A NewGRF airport layout normally
   references *standard* airport tile IDs — `tile.gfx = buf.ReadByte()`
   (`newgrf_act0_airports.cpp:101`) — and those convert correctly. Only the escape value
   `0xFE` introduces a GRF-defined tile, resolved through `_airporttile_mngr` to a runtime
   ID ≥ `NEW_AIRPORTTILE_OFFSET` (`:103-115`). Those hit
   `MapStockGfxToModularPiece`'s `default:` (`modular_airport_build.cpp:253-254`) and
   become `APT_BUILDING_1`. Nothing in such a tile's properties says "this is a runway" —
   movement is defined by the FSM, which is exactly what the modular system replaces — so
   there is no general way to infer a piece type for them.

None of this is corruption; it wastes the player's money and produces an unusable airport.
Refuse it for now: a guard is one line, whereas making it work means at minimum deriving
runway flags from the tiles instead of a per-type table, plus honouring layout rotation.
That is a plausible future feature for the standard-tile case — most NewGRF airports would
convert — and it is worth writing the rejection so it can be relaxed later rather than
phrased as a permanent impossibility. The command is currently reachable only from
`airport_gui.cpp:105`/`107` (no script API), so hiding the toggle would nearly suffice —
but the command should validate too, so a future caller or a stale GUI state cannot reopen
the trap.

---

## 2. Decisions taken

1. **Noise and maintenance: additive per-piece model** over the layout, calibrated
   against the stock values (§3).

   The alternative considered and rejected was a saved `accounting_type` field carrying
   the preset an airport is "billed as" (`AT_SMALL` for hand-built, the template's preset
   otherwise). It is the safer change — it leaves every running game's numbers untouched —
   but it cannot meet the requirement. A hand-built replica of the intercontinental layout
   would still be billed as a country airfield, so "rebuild a stock airport as modular and
   get the same result" fails for the hand-built case, which is the main case. It also
   leaves the underlying absurdity in place: a 99-tile megaport built tile by tile pays a
   4×3 airfield's upkeep. Layout-derived is the only route that satisfies the requirement,
   and the cost of taking it is that Phase 3 depends on Phase 2 and that running games'
   economics change (§7).
2. **`AT_MODULAR = 127`**, the last slot below `NUM_AIRPORTS` (§4).
3. **Script API gets an `AT_MODULAR` constant** in `script_airport.hpp` rather than
   reporting `AT_INVALID` for a real airport.
4. **Savegame version bump** (`SLV_MODULAR_AIRPORT_TYPE`), even though no type remap is
   needed. It gates the afterload retype instead of running it unconditionally forever,
   and it stops an older build of this fork from loading a save containing type 127 —
   which it would otherwise accept and then dereference a null `fsm` on the first modular
   tick.

---

## 3. Noise and maintenance from the layout

### 3.1 Method

Every stock layout is reduced to a modular piece composition by the existing conversion
(`MapStockGfxToModularPiece` + `ApplyStockTileOverride`), then noise and maintenance are
a weighted sum over that composition, with the weights calibrated against stock.

Stock values, from `table/airport_defaults.h:380-388`:

| | country | commuter | city | metro | intl | intercon | heliport | helidepot | helistation |
|---|---|---|---|---|---|---|---|---|---|
| noise | 3 | 4 | 5 | 8 | 17 | 25 | 1 | 2 | 3 |
| maintenance | 7 | 20 | 24 | 28 | 42 | 72 | 4 | 7 | 14 |

Weight classes follow the modular builder's palette (`modular_airport_gui.cpp:176-192`),
so every class is a piece the player can actually place — otherwise a hand rebuild could
not reproduce a stock airport's number. In particular the palette offers three distinct
helipad pieces (`APT_HELIPAD_2` "helipad", `APT_HELIPAD_3_FENCE_NW` "plain H",
`APT_HELIPORT` "heliport"), and the stock heli airports draw from exactly those three
families, so they are legitimately separable classes.

**Where to get the exact `APT_*` groupings:** copy them from
`GetModularAirportPieceBuildCost` (`modular_airport_build.cpp:290-379`), which already
enumerates every piece type into exactly these families for build cost. It is the
canonical grouping; do not re-derive it by hand.

**But the three models group differently, and that is deliberate — do not factor them into
one shared classifier:**

| | build cost | maintenance (§3.2) | noise (§3.3) |
|---|---|---|---|
| the three helipad families | one class | **three** classes | one class |
| large vs small runway tile | two classes | one class (equal weights) | one class |
| large vs small hangar | two classes | two classes | one class |
| buildings / towers / radar | several classes | several classes | all zero |

The splits exist only where the calibration needs them. Sharing a classifier across the
three would force spurious distinctions or collapse needed ones.

### 3.2 Maintenance — exact on all nine

Additive, in units of ⅛ maintenance point:

| piece class | weight | piece class | weight |
|---|---|---|---|
| apron / taxiway | 3/8 | small hangar | 2/8 |
| stand | 5/8 | helipad (`APT_HELIPAD_2` family) | 22/8 |
| runway tile, large family | 8/8 | plain-H pad (`APT_HELIPAD_3` family) | 24/8 |
| runway tile, small family | 8/8 | heliport pad (`APT_HELIPORT`) | 32/8 |
| large hangar | 27/8 | big terminal / round terminal | 9/8 |
| small terminal building | 2/8 | low building | 4/8 |
| tower | 4/8 | radar | 4/8 |
| radio tower | 4/8 | grass / empty / fence-only | 0 |

All nine stock layouts land exactly on their stock maintenance value.

Plumbing: `AirportMaintenanceCost` (`station.cpp:722-732`) sums
`price * spec->maintenance_cost` over all airports and shifts the total right by 3.
Accumulate in eighths instead — `price * spec->maintenance_cost * 8` for stock airports,
`price * modular_points` for modular ones — and shift by 6. Stock results stay
bit-identical (`×8` then `>>6` ≡ `>>3`); `Money` is `int64_t`, so the extra factor of 8
has ample headroom. Two other copies of the `>> 3` convention must stay consistent:
`airport_gui.cpp:488` and `script_airport.cpp:173`. Neither handles modular airports
today (both are per-*type* displays), so they can keep the stock formula — but factor the
scaling into one shared helper rather than leaving three hand-written `>> 3`s, so a future
change to the convention cannot leave them divergent.

### 3.3 Noise — six of nine

**International cannot be matched, by any additive model with non-negative weights.**
Two stock relations contradict each other:

- intercontinental − international = **+8** noise for +18 runway tiles, +23 aprons,
  +2 stands, +1 terminal, +2 low buildings, with nothing removed
  → runway-tile weight ≤ 8/18 = **0.444**.
- metropolitan − city = **+3** noise for +6 runway tiles and −4 aprons (identical
  terminal, tower, radar and hangar counts, so those cancel)
  → runway-tile weight ≥ (3 + 4·apron)/6 ≥ **0.5**.

0.5 > 0.444. The contradiction survives any regrouping or splitting of the non-runway
classes, since neither relation depends on how they are grouped. The stock table is
simply irregular: international is smaller than intercontinental in every countable
respect yet carries two-thirds of its noise.

**Chosen calibration.** The derived level is an integer (it feeds
`GetAirportNoiseLevelForDistance` as a `uint8_t`), so what has to match is the *rounded*
value. Pushing the fit as far as it will go produces weights that are arithmetically
convenient but unexplainable — one calibration reaches eight of nine only by charging
the low-terminal piece 1.64 noise, more than three runway tiles. A weight table the
player cannot reason about is worse than a slightly looser fit, so the model is
constrained to a shape that states a rule: **noise comes from where aircraft operate.**

| piece class | noise weight |
|---|---|
| apron / taxiway | 1/16 |
| stand | 3/16 |
| runway tile (large or small family) | 9/16 |
| hangar (large or small) | 4/16 |
| helipad, plain-H pad, heliport pad | 16/16 |
| every building, tower, radar, fence, grass, empty tile | 0 |

Result — six of the nine stock values reproduced:

| | country | commuter | city | metro | intl | intercon | heliport | helidepot | helistation |
|---|---|---|---|---|---|---|---|---|---|
| derived | 3.00 | 6.06 | 5.31 | 8.44 | 12.69 | 24.63 | 1.00 | 1.31 | 3.44 |
| rounds to | **3** | 6 | **5** | **8** | 13 | **25** | **1** | 1 | **3** |
| stock | 3 | 4 | 5 | 8 | 17 | 25 | 1 | 2 | 3 |

Deviations: commuter 6 (stock 4), international 13 (stock 17), helidepot 1 (stock 2).
International was never reachable. Helidepot can be bought back only by giving up city
or helistation, so six of nine is the ceiling for a table of this shape.

Both weight tables are one member of a solution polytope, not a unique answer. The unit
test in §6 is the contract; the numbers may be nudged freely as long as it passes.

### 3.4 Noise bookkeeping

Noise is currently added once when the airport facility is created and subtracted once
when its last tile goes, using `GetSpec()` at both moments. Layout-derived noise makes it
a per-mutation quantity, and the update is **not** a simple per-piece addition:

- `GetAirportNoiseLevelForDistance` (`station_cmd.cpp:2552-2571`) applies
  `level − distance/tolerance` with a floor of 1 to the *airport total*, so the town-facing
  number is not linear in the piece weights.
- For modular airports the distance comes from
  `AirportGetNearestTownFromTiles(AirportTileIterator(st), …)` (`station_cmd.cpp:2650`),
  so adding or removing a tile can change **both** the distance **and which town is
  nearest**.

Therefore every mutation must compute *(old town, old reduced level)* before the change
and *(new town, new reduced level)* after, and apply
`old_town->noise_reached -= old_level; new_town->noise_reached += new_level` — the two
towns may differ. Anything that treats the delta as a scalar on one town will drift.

**The unit of update is the command, not the tile.** Three paths mutate many tiles in one
command: template placement, the from-stock build, and `CmdUpgradeModularAirportTile`
(`modular_airport_build.cpp:607`), which walks a `TileArea` and can touch **several
stations at once** — it collects `std::set<StationID> affected_stations` for exactly that
reason. Applying a per-tile delta inside those loops recomputes the nearest town per tile
and pushes meaningless intermediate levels through `noise_reached`. Snapshot before, apply
once after, per affected station.

**The "before" read must happen before the cache is invalidated.**
`BuildModularAirportTile_Apply` calls `MarkLayoutDirty` mid-function
(`modular_airport_build.cpp:982`), so a helper that reads the old level after that point
gets the new one and computes a zero delta. Put both rules in the helper's doc comment,
the way `CancelModularHangarOrdersIfNoneLeft` documents its own "once per command, on the
finished layout" contract — this is the same shape of trap.

Other requirements:

- `UpdateAirportsNoise` (`station_cmd.cpp:2656`) must use the modular derivation, or the
  next full recompute silently reverts every modular airport to spec values.
- The local-authority gate at build time tests the post-build level, not `AT_SMALL`'s —
  at all four gate sites listed in §1.1f.
- The derived value is cached on `Airport` and invalidated from `MarkLayoutDirty`, exactly
  as `modular_catchment_cache` is. `MarkLayoutDirty` is already called from every mutation
  site: `modular_airport_build.cpp:982` (tile apply), `:1253` (from-stock, after the
  runway post-pass), the removal path, `modular_airport_template_cmd.cpp:172`
  (`SetRunwayFlags_Apply`), `:238` (`CmdSetRunwayFlags`), `:307` (`CmdSetTaxiwayFlags`).
- **No load-time migration is needed:** `UpdateAirportsNoise()` already runs on every load
  (`afterload.cpp:306`), and also from `town_cmd.cpp:2110` and `settings_table.cpp:411`.
  That is a hazard as well as a convenience — any disagreement between the incremental
  bookkeeping and the full recompute self-heals at unpredictable moments, which would make
  a delta bug nearly invisible. §6 therefore requires an explicit consistency test.

Two consequences to expect: a large hand-built modular airport starts generating much more
noise than it did, and an existing town may load already over its noise limit. The limit is
only enforced at build time, so nothing breaks; the town simply refuses further expansion.

---

## 4. `AT_MODULAR = 127`

### 4.1 Why the last slot

- `AirportSpec::Get` asserts `type < 128` and indexes a fixed `specs[128]`
  (`newgrf_airport.cpp:59-71`). 127 is in bounds, so no caller needs a special case —
  including those that index the array directly rather than through `Airport::GetSpec()`
  (`newgrf_airport.cpp:269`, `newgrf_airporttiles.cpp:223`, `afterload.cpp:2900`, the
  script API).
- NewGRF airport runtime IDs are assigned by OpenTTD from `NEW_AIRPORT_OFFSET` upward
  (`AirportOverrideManager::AddEntityID`), never named by a GRF. Taking the *last* slot
  leaves IDs 10…126 meaning exactly what they meant before, so **no airport-type remap is
  needed**. (Taking ID 10 and shifting `NEW_AIRPORT_OFFSET` would require one, and would
  misread every NewGRF airport in every existing save until it ran.)
- No property-08 hazard: `max_offset` stays 10, so a GRF cannot name 127 as a substitute
  or an override.
- Cost: the NewGRF airport cap drops from 118 to 117. Only saves already at the limit are
  affected.

### 4.2 How to reserve the slot — *not* by shrinking `max_entities`

The obvious move, `_airport_mngr(NEW_AIRPORT_OFFSET, NUM_AIRPORTS - 1, AT_INVALID)`, is
**wrong and would read out of bounds on every modular `GetSpec()`**:

```
/* AirportSpec::Get, newgrf_airport.cpp:59-71 */
const AirportSpec *as = &AirportSpec::specs[type];
if (type >= NEW_AIRPORT_OFFSET && !as->enabled) {
    if (_airport_mngr.GetGRFID(type) == 0) return as;
```

`127 >= NEW_AIRPORT_OFFSET` and the modular spec is `enabled = false`, so this branch is
taken every time. `OverrideManagerBase::GetGRFID` is an unbounded
`return this->mappings[entity_id].grfid;` (`newgrf_commons.cpp:137-139`), and `mappings`
is sized `max_entities` in the constructor (`newgrf_commons.cpp:46`). With
`max_entities == 127`, `GetGRFID(127)` is a one-past-the-end read — fired several times
per aircraft per tick (`aircraft_cmd.cpp:2478`/`2544`/`2555`), on every open of the
build-airport window (`airport_gui.cpp:384`/`397`/`413` loop to `NUM_AIRPORTS`), and from
`script_airport.cpp:27`.

**Instead:** keep `max_entities == NUM_AIRPORTS` so the index space stays 128 wide, and
give `AirportOverrideManager` a `CheckValidNewID` override so 127 is never handed out:

```cpp
bool CheckValidNewID(uint16_t testid) override { return testid != AT_MODULAR; }
```

`AddEntityID` consults it (`newgrf_commons.cpp:118-121`). This is the established pattern —
`IndustryTileOverrideManager`, `AirportTileOverrideManager` and `ObjectOverrideManager` all
do exactly this (`newgrf_commons.h:253`/`273`/`284`). Same 118→117 cap, no out-of-bounds
read.

### 4.3 The spec itself

`AirportSpec::ResetAirports` (`newgrf_airport.cpp:115-121`) copies `_origin_airport_specs`
and fills the remainder with `AirportSpec{}`; write the modular spec into `specs[127]`
after that fill. It is called from exactly one place (`newgrf.cpp:464`), so it is the
right and only hook.

Fields that must be set explicitly, not left defaulted:

- `enabled = false` — keeps it out of `BindAirportSpecs` and the three
  `for (i < NUM_AIRPORTS)` loops in `airport_gui.cpp`, and makes `IsAvailable()` false so
  a crafted `CMD_BUILD_AIRPORT` cannot build it (`station_cmd.cpp:2694`,
  `modular_airport_build.cpp:1052-1053`).
- `grf_prop = SubstituteGRFFileProps(AT_INVALID)`. A latent trap, not a live one — the
  early `!enabled` return above means the line below is unreachable for type 127 as long
  as `enabled` stays false. Set the field anyway, because the failure mode is silent: the
  default constructor
  is `SubstituteGRFFileProps(uint16_t subst_id = 0) : subst_id(subst_id),
  override_id(subst_id)` (`newgrf_commons.h:463`), i.e. `override_id == 0`. The last line
  of `AirportSpec::Get` is
  `if (as->grf_prop.override_id != AT_INVALID) return &specs[as->grf_prop.override_id];`
  — so a default-constructed `grf_prop` makes `Get(127)` silently return the **country
  airfield**. Today only the early `!enabled` return hides it. The stock specs pass
  `SubstituteGRFFileProps(AT_INVALID)` explicitly (`table/airport_defaults.h:365`); so
  must this one.
- `name = STR_AIRPORT_MODULAR` (new string), so `FillTileDescAirport` reads sensibly.
- `layouts`, `size_x`, `size_y`, `depots` (empty), `catchment`, `noise_level`,
  `maintenance_cost` — all set explicitly. Phase 1 removes the modular path from
  `GetHangarExitDirection` (which reads `layouts[0].rotation` unguarded,
  `station_base.h:511`) and `GetRotatedTileFromOffset` (which reads `size_x`/`size_y`),
  but an empty `layouts` on a live spec is a zero-length index waiting for the next caller.

### 4.4 The FSM, and the `v->pos` repair

Modular aircraft still carry an FTA `pos`: `AircraftGetEntryPoint` runs on them, and the
FLYING / TERM1 / HANGAR / HELIPAD1 handlers are still dispatched with `apc` from
`AirportGoToNextPosition`. Give `AT_MODULAR` a **clone of `AT_SMALL`'s FSM**, which
preserves today's behaviour for hand-built airports exactly.

**But that is not sufficient on its own, and this is the sharpest hazard in the whole
change.** Entry points are per-type: `_airport_entries_country = {16, 15, 18, 17}`
(`table/airport_movement.h:423`) against
`_airport_entries_intercontinental = {44, 43, 46, 45}` (`:653`), whose FTA runs to
position 76 against country's 21.

The airports at risk are **exactly those built by the from-stock path** — it is the only
one that writes a non-`AT_SMALL` type (`modular_airport_build.cpp:1176`). Template
placement goes through `BuildModularAirportTile_Apply` and gets `AT_SMALL` like any
hand-built airport; `modular_airport_template_cmd.cpp` never writes `airport.type` at all.
So a from-stock modular airport typed `AT_INTERCON` has aircraft with `pos ∈ {43..46}`,
saved raw (`vehicle_sl.cpp:977`). Retyping it to `AT_MODULAR` with an `AT_SMALL`-sized FSM
leaves those aircraft indexing a 22-element layout with `pos = 45`:

- `vehicle.cpp:828` — `layout[a->pos]`, out-of-bounds `std::vector` read on every aircraft
  deletion;
- `newgrf_engine.cpp:139` — `afc->MovingData(v->pos)` → `assert(position < nofelements)`
  (`airport.h:212`), for var 0xE2;
- `aircraft_cmd.cpp:1675`/`1686` — `v->state = apc->layout[v->pos].heading`, reached for
  modular aircraft via `AircraftEventHandler_AtTerminal` (`:2005`) and `_InHangar`
  (`:1801`), both dispatched from the modular ground-state fallback at `:2545`.

So the afterload retype **must**, in the same pass, reset `v->pos` and `v->previous_pos`
for every aircraft whose `targetairport` was retyped — via `AircraftGetEntryPoint` on the
new FSM, or simply to 0, matching what the tile-by-tile build path already does
(`aircraft_cmd.cpp:412`). This ordering is load-bearing; the noise resync's position
relative to the retype is not (layout-derived noise keys off `blocks.Test(Modular)`, not
the type).

### 4.5 Script API consequence

`ScriptAirport::IsAirportInformationAvailable` is
`type < NUM_AIRPORTS && AirportSpec::Get(type)->enabled` (`script_airport.cpp:27`). With
`enabled = false`, `GetAirportType()` returns `AT_MODULAR` while
`IsAirportInformationAvailable(AT_MODULAR)` returns false and every per-type getter
(`GetAirportWidth`, `GetAirportCoverageRadius`, `GetMaintenanceCostFactor`,
`GetAirportNumHelipads`) returns -1/0. That is the right answer — a modular airport has no
per-type answer to those questions — but it is a deliberate contract, and
`script_airport.hpp`'s doc comment for `IsAirportInformationAvailable` ("will also return
true when an airport type is no longer buildable") needs updating to match.

---

## 5. Plan

### Phase 1 — layout-derived capability and hangars (no type change)

Fixes §1.1 a–c on its own.

1. Add `ModularAirportAcceptsPlanes(st)` / `ModularAirportAcceptsHelicopters(st)`,
   layout-derived and cached on `Airport` behind `MarkLayoutDirty`.

   **Definition of "accepts planes", since this decides which existing airports stop
   taking plane orders:** the layout holds at least one contiguous runway flagged
   `RUF_LANDING` **and** at least one flagged `RUF_TAKEOFF` — possibly the same runway.
   Both halves are required: an airport a plane can land on but never leave is worse than
   one it refuses outright. This mirrors `has_safe_runway_for(landing)` /
   `has_safe_runway_for(takeoff)` in `GetModularAirportCatchmentRadiusFromPieces`
   (`modular_airport_cmd.cpp:513-521`), which is the existing precedent for this shape of
   question.

   Deliberately **topological only** — no ground-reachability check from a stand to the
   runway, and no length or large-aircraft test. `CanVehicleUseStation` is called from
   order validation and the build-vehicle list, so it must stay cheap and must not depend
   on transient occupancy. Runway length is already handled separately by
   `ModularAirportSupportsLargeAircraft`.

   "Accepts helicopters": at least one helipad piece, or a valid computed landing tile
   (`modular_heli_landing_tile`) — the machinery that already exists.
2. Branch `CanVehicleUseStation` (`vehicle.cpp:3107`) and `FindNearestHangar` on modular.
   **Both** of `FindNearestHangar`'s spec gates must be replaced: the `ShortStrip` skip at
   `aircraft_cmd.cpp:223` (use `ModularAirportSupportsLargeAircraft(st)`) *and* the
   `Airplanes`-flag skip at `:226` (use the Phase 1 capability). Fixing only the first
   leaves a from-heliport modular airport with a runway still invisible to jets.
   Note which way this converges the helidepot symptom from §1.1b: the rule becomes "no
   usable runway → no planes", so it is the *hand-built* side that changes behaviour.
   Today it happily sells a jet that can then never take off.
3. Convert the four hangar siblings for modular:
   - `GetNumHangars()` → number of hangar tiles in the layout;
   - `GetHangarTile(i)` / `GetHangarNum(tile)` → index into the hangar tiles **sorted by
     `TileIndex`**. Not `modular_tile_data` order: that vector is mutated by
     erase/push_back, so its order depends on build history, and index stability matters
     for multiplayer determinism and for scripts.
   - `GetHangarExitDirection(tile)` → `GetModularHangarExitDirection`.
4. Audit the script surface that follows: `script_airport.cpp:92`/`110`,
   `script_depotlist.cpp:34`, `script_order.cpp:257`.
5. **Orders that step 2 invalidates: leave them, do not purge.** Tightening
   `CanVehicleUseStation` means a plane order to a hand-built pad-only modular airport,
   legal when it was issued, stops being legal. Existing saves can hold such orders.

   Decision: no purge. Stock's invalid-entry warning (`order_cmd.cpp:1711`) surfaces it
   and the player fixes the order. Deleting orders on load is the more destructive
   response to a situation that is visible and self-correcting.

   Known consequence, accepted: `AircraftEventHandler_Flying` gates landing on
   `CanVehicleUseStation` (`aircraft_cmd.cpp:2175`), so an aircraft already en route to
   such an airport circles until the order is changed. It holds no reservation and blocks
   nothing on the ground — it is an annoyance and a warning, not a stuck-airport failure.
   Do **not** add a fallback that lets it land anyway; the order is the thing to fix.

*Risk:* aircraft routing. Step 2 changes which airports jets consider for servicing —
run `scripts/regression_test.sh`, particularly the `T5j2.sav` contention fixture.

### Phase 2 — noise and maintenance from the layout

Deliberately ahead of the build-path convergence: making the from-stock path stop writing
the preset type *before* the numbers are layout-derived would bill a from-stock
intercontinental modular airport at `AT_SMALL`'s rates — noise 3 instead of 25,
maintenance 7 instead of 72. That is a large exploitable regression, so the two must land
in this order.

6. `GetModularAirportMaintenancePoints(st)` per §3.2, plumbed through
   `AirportMaintenanceCost` in eighths.
7. `GetModularAirportNoiseLevel(st)` per §3.3, cached and invalidated with
   `MarkLayoutDirty`. Structure it like `GetModularAirportCatchmentRadiusFromPieces`: a
   pure function over an abstract piece list plus a thin cached station-level wrapper, so
   it is unit-testable without a map and reusable for template preview.
8. Two-town delta bookkeeping at every mutation site, and the modular derivation in
   `UpdateAirportsNoise` and at all four build-time authority gates (§3.4).

### Phase 3 — converge the build paths

9. `CmdBuildModularAirportFromStock` stops writing the preset into `airport.type`; it
   writes whatever the tile-by-tile path writes — **`layout = 0` and `rotation = DIR_N`
   too, not just the type**. For stock layouts those already coincide (every stock spec has
   one layout, all `DIR_N`), so this changes nothing today; it stops the from-stock path
   being the only one that can ever produce a non-zero layout, which is what makes the
   afterload layout reset in Phase 4 step 15 a one-off migration rather than a permanent
   repair. This step also fixes §1.1g (the heliport `delta_z` flood bug) as a side
   effect — name it in the commit so the fix is intentional.
10. Non-runway tiles get the same `runway_flags` in both paths (`RUF_DEFAULT`), and both
    paths derive `STATIONNAMING_*` from the Phase 1 layout capability.
11. Reject NewGRF airport types in `CmdBuildModularAirportFromStock`, and hide the modular
    toggle for NewGRF classes in `airport_gui.cpp`.
12. Add a test that builds each stock layout both ways and diffs the resulting `Airport`
    and `ModularAirportTileData`. That test is the contract for "indistinguishable from
    building it by hand".

### Phase 4 — introduce `AT_MODULAR`

13. `AT_MODULAR = 127`; spec written into `specs[127]` with every field of §4.3 set
    explicitly; `AirportOverrideManager::CheckValidNewID` override per §4.2 (**not** a
    reduced `max_entities`); cloned `AT_SMALL` FSM. Add the `STR_AIRPORT_MODULAR` string
    to `src/lang/english.txt`.
14. Add `SLV_MODULAR_AIRPORT_TYPE` and bump.
15. All three build paths write `AT_MODULAR`. Afterload, gated on the new version:
    retype every modular airport (`blocks.Test(AirportBlock::Modular)` →
    `type = AT_MODULAR`) **and, in the same pass, reset `v->pos`/`v->previous_pos` for
    every aircraft targeting a retyped airport** (§4.4). Reset `airport.layout` to 0 in
    the same pass: `GetAirportCallback` feeds it to the resolver alongside the type
    (`newgrf_airport.cpp:269`), and a from-stock NewGRF airport in an old save can carry a
    non-zero layout that means nothing under the modular spec. Sequence the whole pass
    before `afterload.cpp:2900`, which derives `psa->grfid` from the type.
16. Derive `ttd_airport_type` for modular from the layout — helipad-only →
    `ATP_TTDP_HELIPORT`, otherwise small/large by `ModularAirportSupportsLargeAircraft` —
    rather than reading the spec field (`newgrf_engine.cpp:514`, `newgrf_station.cpp:421`).
17. Add `AT_MODULAR` to `script_airport.hpp`, make `ScriptAirport::GetAirportType` return
    it, and update the `IsAirportInformationAvailable` doc comment per §4.5.
18. Simplify the special cases that exist only because the type was a lie, e.g.
    `disaster_vehicle.cpp:744`.
19. Rewrite the tests that stamp `AT_SMALL` (`test_modular_airport.cpp:1754`, `1804`) —
    their comments explicitly assert borrowed-preset behaviour, so they need rewriting,
    not just retyping.

### Phase 5 — sweep

20. Re-grep `GetSpec()`, `GetFTA()` and `airport.type`; confirm every remaining modular
    read is either layout-derived or deliberately reading the placeholder spec.
21. Save/load round trip and multiplayer determinism review of the new caches.

---

## 6. Verification

- **Unit test, the acceptance criterion:** for all nine stock layouts, derive noise and
  maintenance from the modular piece composition and assert against the tables in §3.
  Maintenance must equal the stock value for all nine. Noise must equal the stock value
  for country, city, metropolitan, intercontinental, heliport and helistation, and must
  equal the documented derived value for commuter, international and helidepot — pinned
  so a drift is caught rather than silently accepted.
- **Noise consistency test:** build up and tear down a modular airport tile by tile,
  comparing incremental `town->noise_reached` against a full `UpdateAirportsNoise()`
  recompute after every step. Without this, a delta bug is masked by the recompute that
  already runs on load and on several settings changes (§3.4).
- **Unit test:** hangar accessors over a hand-built layout with zero, one and several
  hangar tiles, including index stability after a tile in the middle is replaced.
- **Unit test:** plane/helicopter capability for a pad-only layout, a runway-only layout,
  and a mixed one.
- **Build-path equality test** (Phase 3 step 12).
- **`scripts/regression_test.sh` after every phase**, not only the ones that obviously
  touch routing. It takes a few minutes, and the phases that look inert are exactly the
  ones where an unnoticed throughput change would be hard to attribute later. Phases 1 and
  4 are the ones expected to move numbers; a movement change from Phase 2, 3 or 5 is a
  signal to stop and explain it.
- **Save/load:** the fixture must be an airport built by the **"Build as modular" path
  from a large stock preset** (intercontinental or international), because that is the
  only path that produces a non-`AT_SMALL` type and therefore the only one whose aircraft
  carry high `pos` values. A template-built fixture would be `AT_SMALL` and would exercise
  nothing. It must load, retype, repair `pos`, and run without tripping the `MovingData`
  assert — the specific regression §4.4 exists to prevent.

## 7. Risks

- **Phase 1 changes routing.** Jets gain modular airports as servicing candidates, which
  they never had. Throughput effects show up in the regression fixtures.
- **Phase 1 invalidates existing orders.** Plane orders to hand-built pad-only modular
  airports become unsatisfiable in saves that already hold them. By decision (Phase 1
  step 5) they are left in place with stock's invalid-entry warning, so an aircraft
  already en route circles until the player retargets it.
- **Phase 2 changes running games' economics.** Large hand-built modular airports get much
  more expensive to maintain and much noisier. Intended, but a live-game balance change.
- **Phase 4 touches multiplayer-visible state.** `airport.type` is saved raw; the afterload
  retype and `pos` repair must be deterministic.
- **The NewGRF airport cap drops 118 → 117.** Only saves at the very limit are affected.
