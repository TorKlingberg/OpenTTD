# OpenGFX2: extra airport hangar sprites are in the wrong order

Draft for https://github.com/OpenTTD/OpenGFX2/issues

## Summary

The six hangar sprites in the extra GRF's airport block (Action 5 type `0x10`, sprites
6–11) are emitted in road-depot order rather than hangar order. Every pair is reversed,
and the last two directions are swapped, so a consumer that draws them per OpenTTD's
documented layout gets two hangars half a tile off their tile and two more wearing each
other's doors.

`baseset/nml/extra/extra-openttd-airport.pnml`, the block commented `//AIRPORTS 6-11
full depot set`:

```
replacenew airports_6(AIRPORTS,  "../graphics/stations/general/64/pygen/modernairdepots_regions_8bpp.png", 6) { template_road_depot(0, 0, 1) }
#32 alternative_sprites(airports_6, ZOOM_LEVEL_NORMAL, BIT_DEPTH_32BPP, "../graphics/stations/general/64/pygen/modernairdepots_regions_bt32bpp.png") { template_road_depot(0, 0, 1) }
```

The `pygen` PNG is generated at build time by `graphics/generate_graphics.py` (region-mask
step, the `"modernairdepots"` entry). The committed artwork is
`graphics/stations/general/64/modernairdepots_shape.png` plus its `_regionmask` /
`_overlayshading` companions (`.pdn` sources alongside).

`template_road_depot` lays out OpenTTD's road depot block (sprites `0x580`–`0x585`,
`src/table/road_land.h`). That is also six sprites in three groups, so it fits — but it
is not the same ordering:

| slot | road depot block | AIRPORTX hangar block |
|---|---|---|
| 6, 7 | SE back, SE front | SE **building**, SE wall |
| 8, 9 | SW back, SW front | SW **building**, SW wall |
| 10, 11 | **NE**, **NW** | **NW** (`_N`), **NE** (`_E`) |

## Measured

Sprite metadata read from `ogfx2e_extra_8.grf` (OpenGFX2 Classic 0.8.1), against
`openttd.grf` — OpenGFX 8.0 matches `openttd.grf` exactly:

| slot | name | openttd.grf / OpenGFX | OpenGFX2 Classic |
|---|---|---|---|
| 6 | `NEWHANGAR_S` | building 64×55 @ (−2,−38) | wall 18×17 @ (15,0) |
| 7 | `NEWHANGAR_S_WALL` | wall 18×17 @ (16,−1) | building 64×56 @ (−1,−39) |
| 8 | `NEWHANGAR_W` | building 64×55 @ (−2,−38) | wall 18×17 @ (−31,0) |
| 9 | `NEWHANGAR_W_WALL` | wall 18×17 @ (−30,1) | building 64×56 @ (−61,−39) |
| 10 | `NEWHANGAR_N` | NW-facing 64×55 @ (−2,−38) | **NE**-facing 64×56 @ (−61,−39) |
| 11 | `NEWHANGAR_E` | NE-facing 64×55 @ (−2,−38) | **NW**-facing 64×56 @ (−1,−39) |

Facing was determined by silhouette: a NW-facing hangar shares its barrel axis with the
SE one, a NE-facing hangar with the SW one. Slots 10/11 match the opposite reference in
OpenGFX2 from the one they match in OpenGFX.

## Why nobody has noticed

Nothing in vanilla OpenTTD draws these six sprites. `_station_display_hangar_{sw,nw,ne}`
in `src/table/station_land.h` exist but are unreferenced by any airport layout, so the
mismatch has no visible symptom in the base game. It surfaced in a fork that builds
airports tile by tile and does draw all four hangar rotations.

## Fix

Emit the block in hangar order instead of road-depot order — a `template_airport_hangar`
alongside `template_road_depot`, or a reordered source sheet:

1. **Swap the two entries of each pair**: building first, then wall, for slots 6/7 and
   8/9; and emit slot 10 as the NW-facing sprite, slot 11 as the NE-facing one.
2. **Give all four buildings the same anchor** — the one slot 7 already has, `(−1,−39)`.
   OpenTTD's layout draws all four from tile origin `(14,0,0)`, so the `(−61,−39)` anchor
   currently on the west/north buildings puts them half a tile left. The two wall sprites
   are already anchored correctly.

The same `.pnml` line drives the 32bpp `alternative_sprites`, so High Def needs the same
change.

## Verifying

Dump the Action 5 type `0x10` block and check slots 6–11 against `openttd.grf`
(sources: `media/baseset/openttd/airports.nfo` in the OpenTTD repo). All four building
sprites should share one anchor, the odd slots should be the small wall pieces, and slot
10 should be the hangar whose silhouette matches slot 6's.
