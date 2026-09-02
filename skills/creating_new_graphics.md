# Creating New OpenTTD Graphics

Use this workflow for new sprites that ship with this fork, especially isometric world
objects. It covers art direction, conversion to OpenTTD's native 8bpp format, registration in
`openttd.grf`, and visual checks across base graphics sets. For the narrower GUI-icon process,
also read `skills/adding_a_new_sprite.md`; for automated in-game screenshots, read
`skills/gui_screenshot_verification.md`.

The difficult part is not drawing a recognisable object. It is making the object read at native
size, preserving the indexed palette exactly, anchoring it to the world correctly, and deciding
which part should come from the user's selected base graphics set.

## Agent and tool portability

This is a repository skill, not a ChatGPT-only skill. Its OpenTTD geometry, palette, NFO, build,
and verification instructions apply equally when working with ChatGPT/Codex, Claude, or Gemini
Antigravity. Shell commands, Pillow checks, LLDB inspection, and edits to repository files are
provider-neutral where the environment supplies the relevant tools. Availability, operating
system, and permission requirements can still differ between agent environments.

The following parts are specifically OpenAI/ChatGPT terminology:

- **ImageGen** is the image-generation tool/skill available to ChatGPT/Codex in environments
  that expose it. Claude or Gemini should use whichever image-generation or image-editing tool
  their environment provides. If none is available, skip concept generation and draw or edit the
  sprite directly at native scale.
- **`Use case: stylized-concept`** in the example prompt is a prompt-scaffolding convention used
  by the currently installed Codex ImageGen skill, not a universal image-model parameter. Other
  environments can omit it or translate its intent to an equivalent control.
- Supplying the target sprite sheet as a tool-level reference image depends on the image tool's
  interface. If reference images are unsupported, describe the sheet's projection, lighting,
  palette, and pixel density in the prompt, then rely more heavily on native-scale manual cleanup.

The descriptive part of the prompt is portable. Regardless of provider, generated art is only a
concept source; the indexed-palette conversion, pixel cleanup, registration, and in-game checks
remain required.

## Choose the graphics layer first

Decide who should own the sprite before making the art:

- Use an existing semantic base-set sprite ID when every base graphics set should provide its
  own rendition. Do not assume that different sets give equivalent sprites the same dimensions,
  offsets, order, or even orientation; inspect the loaded sprites when those properties matter.
- Put a new feature-specific fallback sprite in `media/baseset/openttd/` when it should ship once
  with the game and remain available under base sets that do not provide it. These images are
  compiled into OpenTTD's fallback extra GRF, `openttd.grf`. A selected base set or NewGRF can
  replace the corresponding Action 5 slot later, so this is not a guarantee of identical art.
- Use a NewGRF when the art is optional content rather than part of the fork's core behaviour.
- Draw no art at all when what is wanted is an existing base-set sprite the other way round.
  `SPR_MIRRORED_BASE` in `src/table/sprites.h` is a small block of sprite IDs that hold no
  pixels: `SetupMirroredSprites()` points each at the sprite it mirrors once all graphics are
  loaded, and the sprite cache flips the pixels as it decodes them. Add a row to
  `_mirrored_sprites` in `src/spritecache.cpp`, raise `MIRRORED_SPRITE_COUNT`, and the mirrored
  piece follows whatever base set or NewGRF supplied the original instead of clashing with it.
  Its bounding box is reflected about the tile's own axis, so a full-tile sprite lands on
  exactly the columns its unmirrored neighbours leave for it.

Prefer a second stored sprite for the fork's own art, even when the two orientations start out as
an exact mirror of each other. `SPR_MIRRORED_BASE` exists because a base set's sprite is not ours
to copy into the sheet; a sprite already in `media/baseset/openttd/` has no such problem, and
storing both orientations leaves the pixels there for anyone who later wants to tweak one of them.
That matters because a mechanical mirror swaps which of a building's faces points up-left and
which points up-right. Measure rather than assume how much that costs: the decorations on this
sheet turned out near enough symmetric in shading to flip cleanly, and the two car park variants
differ by only thirteen pixels of kerb, but a subject with a strongly lit face or an asymmetric
detail such as a road-facing entrance will want real correction.

For an object placed on a tile, separate ground from object whenever practical. Draw a canonical
ground sprite supplied by the selected base set, such as apron or grass, then draw the new
`openttd.grf` sprite over it. Baking ground pixels into the new sprite creates visible seams and
makes the tile clash with alternate graphics sets. The modular airport decorations in
`media/baseset/openttd/airports.png` and `src/modular_airport_draw.cpp` are the reference example.

## Establish the native sprite geometry

Work backwards from an existing sprite that is used in the same drawing path. Inspect both its
NFO rectangle and its in-game result before choosing a canvas.

At normal zoom, a flat 16-by-16 world tile projects to a 64-by-31-pixel diamond. In
`airports.nfo` the conventional full-tile ground entry is:

```nfo
-1 sprites/airports.png 8bpp  x  y  64  31  -31   0 normal
```

A full-tile building may use a 64-by-55 rectangle and an offset such as `-31,-24`, which keeps
the bottom aligned with the same tile origin while allowing 24 pixels above it. These values are
examples, not a universal building template: derive the bounding rectangle and offsets from the
actual drawing layout.

Keep the game's projection in mind: `RemapCoords` maps world coordinates as
`screen_x = (y - x) * 2`, `screen_y = y + x - z`. Thus +X points down-left and +Y points
down-right on screen. Prefer screen-relative language when describing an orientation, and make
both required axis variants explicitly. A mechanical mirror is safe for symmetric lights and
markings; buildings usually need their lighting and readable details corrected afterward.

## Generate a concept, then finish it at native scale

Image generation is useful for composition and vocabulary, but its large, antialiased output is
not a finished OpenTTD sprite. When an image-generation tool is available, supply a crop of the
actual target sheet as a style reference when the tool supports reference images, and generate
one asset at a time. A useful prompt shape is shown below. The first `Use case` line is
OpenAI/ChatGPT-specific as explained above; the rest can be adapted to other providers.

```text
Use case: stylized-concept
Asset type: OpenTTD 8bpp one-tile world sprite source
Match the supplied sheet's isometric angle, scale, lighting direction, pixel density,
outline weight, and restricted DOS-palette colour ramps.

[Describe the object, its important readable details, and its screen-facing orientation.]

The footprint is one 16x16 world-unit tile, represented by a 64x31 diamond at normal zoom.
Crisp hand-drawn 1990s transport-sim pixel art; hard pixel clusters; no antialiasing,
gradients, blur, text, logos, watermark, or partial transparency. Transparent background.
```

Ask for the details that make the asset identifiable at a glance: the fire engine outside the
station, loading doors and pallets at a warehouse, tanks and pipework at a fuel farm, or the
open decks, parked cars, and road-facing entrance of a multi-storey car park. Avoid broad scene
descriptions, which encourage background scenery and the wrong scale.

Then redraw or simplify at the final 64-pixel tile width:

- Judge only at 1x nearest-neighbour display. A detail that exists only when enlarged does not
  exist in game.
- Use deliberate one- and two-pixel clusters. Remove antialiasing and isolated near-duplicate
  colours.
- Keep the upper-left-facing planes lighter and the opposite faces darker, matching nearby
  sprites on the target sheet.
- Exaggerate the identifying silhouette and colour blocks slightly. Native-scale legibility is
  more important than realistic small detail.
- Leave the ground transparent for an overlay sprite. Include shadows only when they do not
  fight the base-set ground texture.

## Preserve the indexed DOS palette

The PNGs in `media/baseset/openttd/` are indexed (`P` mode), not ordinary RGB/RGBA images.
Start from a copy of the target sheet or explicitly copy its palette. Do not let an editor save
the finished sheet as true-colour PNG.

Important indices are:

- `0`: transparent colour key. It appears bright blue in an ordinary image viewer but is not a
  drawable blue.
- `198..205`: company-colour remap ramp. Use these only when the drawing code deliberately
  applies a recolour palette.
- `215..226`: unused pink entries. Do not use them.
- `227..254`: animated colours. Avoid them unless animation is explicitly intended.
- `1..197`, `206..214`, and `255`: ordinary static colours.

`docs/palettes/palette_key.png` is the visual key and `docs/palettes/openttd.gpl` is the GIMP
palette. When converting generated RGBA art, make fully transparent pixels index 0 and map each
opaque pixel to its nearest allowed palette colour. Do not map partial alpha: first decide
whether each edge pixel is solid or absent. Excluding the company and animation ranges during
nearest-colour matching prevents accidental flashing or recolouring.

Before building, check the result with Pillow:

```python
from PIL import Image

sheet = Image.open("media/baseset/openttd/airports.png")
assert sheet.mode == "P"

# Replace this with the new sprite's exact source rectangle.
used = set(sheet.crop((100, 170, 164, 225)).getdata())
assert not (used & set(range(198, 206)))  # unless company recolouring is intentional
assert not (used & set(range(215, 227)))  # unused pink entries
assert not (used & set(range(227, 255)))  # unless palette animation is intentional
```

When adding a separate PNG, also compare `getpalette()` with a known-good sheet. When editing an
existing sheet, preserve its palette table and transparent-key convention unchanged.

## Register the sprite

Find the NFO block that owns the relevant Action 5 sprite range. Append new entries within that
block; inserting into the middle changes every later ordinal and can silently redirect existing
sprite constants.

An NFO real-sprite line has this shape:

```nfo
-1 sprites/airports.png 8bpp  x  y  width  height  x_offset  y_offset  normal
```

Update all linked counts and constants together:

1. Increase the Action 5 count in the NFO by the number of real-sprite entries added.
2. Add named `SpriteID` constants at the previous end of the range in
   `src/table/sprites.h`.
3. Increase that range's `*_SPRITE_COUNT` by the same number.
4. If the PNG is new rather than an edit to an existing sheet, add it to
   `media/baseset/openttd/CMakeLists.txt`.

For example, the airport decoration work appended eight entries at offsets `+15..+22` and raised
`AIRPORTX_SPRITE_COUNT` from 15 to 23. Two of them represent the car park's two road-facing
entrance orientations, one the fire station's second appliance-bay facing, and the last two the
small hangar's closed-back views. Read the current end of the range out of `src/table/sprites.h`
rather than trusting this example: appending at a stale offset silently overwrites a sprite that
is already there.

Use a sortable world-sprite layout for a structure with height, and give its bounding box the
real occupied footprint and sufficient Z extent. A flat marking or light array still needs a
bounding box large enough to sort predictably around vehicles and neighbouring structures.
GUI-only images can normally be drawn directly with `DrawSprite` and do not need world sorting.

## Build the generated GRF

`grfcodec`, `nforenum`, and `grfid` must have been found when CMake configured the build. If they
were installed afterward, re-run CMake before building. Then run:

```bash
./scripts/build_and_sign.sh
```

The build regenerates both `media/baseset/openttd.grf` and
`media/baseset/openttd.grf.hash`; both generated files belong in the commit along with the PNG
and NFO sources. If a source sprite changed but the GRF did not, stop and fix tool discovery or
the CMake source list. Do not accept the cached GRF: the source tree and shipped binary would
disagree.

## Verify in game

Visual inspection is required. Use the scratch-instance procedure in
`skills/gui_screenshot_verification.md` so the test does not touch the user's settings or live
game.

Check all of the following:

- the sprite at normal 1x scale, not only enlarged;
- every supported orientation;
- overlap and sorting with aircraft, vehicles, buildings, and adjacent tall objects;
- tile anchoring on all four sides of the footprint;
- the picker or toolbar preview as well as the placed world object;
- at least two contrasting base graphics sets, including the intended default and one alternate;
- transparency edges against both light and dark ground.

For a core overlay, first determine whether the selected base set or an active NewGRF replaces
its Action 5 slot. If not, the fallback object should remain identical while its canonical ground
changes with the selected base set. An unexpected mismatch usually means ground was baked into
the overlay, a pack-specific sprite was assumed to have fixed geometry, or the object was
attached to the wrong sprite range.

Finish with the ordinary build and unit checks appropriate to the code change, plus
`git diff --check`. Graphics-only changes do not need the modular-airport simulation regressions;
run those only when the accompanying code changes movement, reservation, pathfinding, network,
or save/load behaviour as described in `CLAUDE.md`.
