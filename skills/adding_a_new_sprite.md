# Adding a New GUI Sprite to OpenTTD

How to add a new sprite (e.g. a toolbar button icon) to the OpenTTD base graphics set.

## Prerequisites

- `grfcodec` and `nforenum` must be installed to regenerate the GRF file.
- Without these tools, the build uses a cached `openttd.grf` from git and your new sprite won't appear.

## Steps

### 1. Create the PNG

Either add pixels to the existing sprite sheet (`media/baseset/openttd/openttdgui.png`) or create a new PNG file (cleaner, preferred for distinct icons).

Existing separate PNGs follow the naming convention `openttdgui_<description>.png`, e.g.:
- `openttdgui_convert_road.png`
- `openttdgui_convert_tram.png`
- `openttdgui_build_tram.png`
- `openttdgui_group_livery.png`

Each PNG typically contains two sizes side by side: a 20x20 normal sprite and a 32x32 double-size sprite (for 2x zoom), placed at `(0,0)` and `(24,0)` respectively.

The PNG must use the 8bpp OpenTTD palette.

### 2. Add NFO entries in `openttdgui.nfo`

Add sprite definitions at the **end** of `media/baseset/openttd/openttdgui.nfo` (before any trailing blank lines). Sprites are referenced by ordinal position from `SPR_OPENTTD_BASE`, so new sprites must go at the end to avoid shifting existing indices.

Format:
```nfo
   -1 sprites/openttdgui_mysprite.png 8bpp   0   0  20  20   0   0 normal
   -1 sprites/openttdgui_mysprite.png 8bpp  24   0  32  32   0   0 normal
```

Fields: `sprite_id  path  depth  x  y  width  height  x_offset  y_offset  compression`

Note: the path uses `sprites/` prefix (relative to the GRF build directory).

### 3. Bump `OPENTTD_SPRITE_COUNT`

Update the count in **two** places — it must match the total number of sprite entries in the NFO:

1. **`src/table/sprites.h`** line ~57:
   ```cpp
   static const uint16_t OPENTTD_SPRITE_COUNT = 192;  // increase this
   ```

2. **`media/baseset/openttd/openttdgui.nfo`** line 7:
   ```nfo
   -1 * 3	 05 15 \b 192 // OPENTTD_SPRITE_COUNT  // match the new count
   ```

If you added 2 sprite entries (normal + 2x zoom), increase by 2.

### 4. Add a sprite constant in `sprites.h`

In `src/table/sprites.h`, add a named constant using the old count as the offset:

```cpp
static const SpriteID SPR_IMG_MY_SPRITE = SPR_OPENTTD_BASE + 192;  // normal size
// If you added a 2x zoom variant, it's at +193 (the next index)
```

Place it near related constants (e.g. near other `SPR_IMG_CONVERT_*` definitions around line 1374).

### 5. Register the PNG in CMakeLists.txt

If you created a new PNG file, add it to `media/baseset/openttd/CMakeLists.txt` in the `PNG_SOURCE_FILES` list:

```cmake
                         ${CMAKE_CURRENT_SOURCE_DIR}/openttdgui_mysprite.png
```

### 6. Use the sprite in code

Reference the new constant in your GUI widget definition:

```cpp
SetSpriteTip(SPR_IMG_MY_SPRITE, STR_MY_TOOLTIP),
```

### 7. Build

Rebuild with `grfcodec` available so the GRF is regenerated:

```bash
/Users/tor/ttd/OpenTTD/scripts/build_and_sign.sh
```

If cmake hasn't been reconfigured since adding the new PNG, you may need to re-run cmake first.

## Reference: How existing sprites are laid out

The NFO file defines sprites sequentially starting at line 8 (sprite index 0). Sprite index N corresponds to `SPR_OPENTTD_BASE + N`. Examples:

| Sprite | Index | Constant |
|--------|-------|----------|
| Convert rail | 55 | `SPR_IMG_CONVERT_RAIL` |
| Convert elrail | 59 | `SPR_IMG_CONVERT_ELRAIL` |
| Convert monorail | 65 | `SPR_IMG_CONVERT_MONO` |
| Convert maglev | 71 | `SPR_IMG_CONVERT_MAGLEV` |
| Convert road | 180 | `SPR_IMG_CONVERT_ROAD` |
| Convert tram | 182 | `SPR_IMG_CONVERT_TRAM` |

Note that some sprites have multiple NFO entries (normal + zoom levels), so the index gap between logical sprites can be more than 1.
