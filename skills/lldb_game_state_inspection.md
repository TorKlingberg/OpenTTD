# Inspecting Live Game State with LLDB

Use this when you need to answer "what is the actual value of X in the running game" — town ratings, noise levels, station facilities, reservation state — without adding debug logging, rebuilding, or parsing savegames. It works directly on the user's live session and is read-only.

For attach basics and log workflows see `skills/lldb_debugging.md`.

## Workflow

1. Find the pid: `ps aux | grep -i "[o]penttd"`
2. Write a Python dump script (template below) that writes results to a file in `/tmp`.
3. Attach in batch mode, run it, detach:

```bash
lldb -p <pid> --batch \
  -o 'command script import /tmp/dump_state.py' \
  -o 'detach' -o 'quit'
```

The game is frozen while attached, so keep the script fast and detach promptly. Never call game functions or mutate state from the debugger on the user's session — field reads via SBValue only. (Attaching to a scratch instance you launched yourself is fine for riskier experiments.)

## Ready-made dumper

`scripts/lldb_dump_state.py` dumps settings, all towns (name, population, noise, per-company ratings) and all stations (town link, decoded facilities, airport type, modular piece histogram) to `/tmp/openttd_state_dump.txt`:

```bash
lldb -p $(pgrep -f 'build/openttd') --batch \
  -o 'command script import scripts/lldb_dump_state.py' \
  -o 'detach' -o 'quit'
```

Start from it when you need other fields — it encodes all the drilling rules below. To inspect a save without a running game, launch a scratch headless instance first (give it a huge tick budget; the null driver burns ticks in seconds):

```bash
./build/openttd -v null:ticks=100000000 -g ~/Documents/OpenTTD/save/SAVE.sav -d misc=0 &
```

## Script template

```python
import lldb

def walk_pool(target, pool_name):
    """Yield (index, item_ptr) for live entries of an OpenTTD pool."""
    pool = target.FindFirstGlobalVariable(pool_name)
    data = pool.GetChildMemberWithName('data')  # std::vector<T*>, lldb gives synthetic children
    for i in range(data.GetNumChildren()):
        ptr = data.GetChildAtIndex(i)
        if ptr.GetValueAsUnsigned(0) == 0: continue  # skip null slots!
        yield i, ptr

def __lldb_init_module(debugger, internal_dict):
    target = debugger.GetSelectedTarget()
    out = open('/tmp/state_dump.txt', 'w')
    for i, ptr in walk_pool(target, '_town_pool'):
        t = ptr.Dereference()
        name = (t.GetChildMemberWithName('cached_name').GetSummary() or '').strip('"')
        noise = t.GetChildMemberWithName('noise_reached').GetValueAsUnsigned(0)
        out.write(f"town[{i}] '{name}' noise={noise}\n")
    out.close()
    print("written /tmp/state_dump.txt")  # shows in lldb output = success marker
```

Pools that matter: `_town_pool`, `_station_pool`, `_vehicle_pool`, `_company_pool`. Globals: `_settings_game`, `_local_company`, `_current_company`.

## Drilling OpenTTD wrapper types

`GetValueAsUnsigned()` on a wrapper aggregate returns 0 — you must drill to the payload child first:

| Type | How to read |
|---|---|
| `TileIndex`, `StationID`, `CompanyID`, other strong IDs | child `value` (or `GetChildAtIndex(0)`) |
| `EnumBitSet` / `CompanyMask` (e.g. `st->facilities`) | child `data` (raw bits) |
| `TypedIndexContainer<std::array<...>>` (e.g. `t->ratings`) | child 0 → `std::array` → `__elems_` → indexed shorts |
| `std::string` | `GetSummary()` (quoted; strip `"`); may be `None` |
| `std::vector`, `std::unique_ptr` | lldb synthetic children: `GetNumChildren()` / `GetChildAtIndex(i)` |

If a debugger-side recursive dump helps, print `GetName()`/`GetType().GetName()`/`GetValue()`/`GetSummary()` per child a few levels deep and eyeball it.

## Pitfalls (all learned the hard way)

- **A rebuild blinds the debugger, silently.** Zero rows from every pool usually
  means LLDB refused the debug map, not an empty game. `scripts/make_dsym.sh`
  (run automatically by the two `build_and_run*` scripts) prevents it; see
  `skills/lldb_debugging.md`.
- **Raw-memory pool walks need `GetNonSyntheticValue()`.** Reading `data.__begin_`
  and striding pointers yourself is far quicker than synthetic children for a
  ~900-entry pool, but `GetChildMemberWithName('data')` returns the libc++
  *synthetic* vector, which has no `__begin_`; the lookup returns an invalid
  value that reads as address 0, so the walk finds nothing. That looks exactly
  like the blinded-debugger case above -- distinguish them by checking
  `pool.first_unused`/`items` first: nonzero there means symbols are fine and the
  bug is yours. Call `.GetNonSyntheticValue()` on `data` before asking for
  `__begin_`.
- **Check every pointer before drilling.** Reading through a null/absent `unique_ptr` or invalid slot yields plausible-looking garbage, not an error — e.g. phantom "3 modular tiles of piece 0" on stations that have no modular data at all. If a value looks suspiciously uniform across many objects, it's probably a misread.
- **Names are lazy and fuzzy.** `Town::cached_name` is only filled once the label has been drawn; and the user's recollection of a generated name may be slightly off ("Grenford" vs actual "Grennford"). Prefer identifying objects by state (tile counts, piece types, town links) and treat name matches as confirmation.
- **Oil rigs look like airports.** They are stations with facilities Airport|Dock (0x18) and `airport.type == 9` (`AT_OILRIG`). Exclude them when counting player airports, as the game itself does.
- **Facility bits** (`StationFacility`, bit positions): Train 0x1, TruckStop 0x2, BusStop 0x4, Airport 0x8, Dock 0x10.
- **Modular `piece_type` values** are `AirportTiles` enum indices (`src/table/airporttile_ids.h`), e.g. `APT_APRON` = 0, `APT_EMPTY` = 29. Count from the enum when decoding a histogram.
- **Avoid `expr` calls into game code.** With RelWithDebInfo, inline functions (`MaxTownNoise`, `Town::GetIfValid`, pool iterators) usually fail to evaluate or are unavailable. Recompute formulas Python-side from raw fields instead.
- **`printf` from expressions goes to the game's stdout,** not to you. Write to a `/tmp` file from Python and print a completion marker.

## Worked example

"Why does the local authority refuse my builds?" was answered by dumping, per town: `cached_name`, `noise_reached`, `cache.population`, `ratings[]`; per station: `town` link, `facilities` bits, `airport.type`, `modular_tile_data` piece histogram; plus `_settings_game.economy.station_noise_level` and `difficulty.town_council_tolerance` to see which authority branch was active. The culprit was a town rating of -360 (below `RATING_VERYPOOR` = -200, `src/town_type.h`) — invisible in any log, obvious in one dump.
