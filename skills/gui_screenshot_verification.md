# Seeing a GUI Change: Scratch Instance + LLDB + In-Game Screenshot

Drawing bugs are settled by looking, not by arithmetic. This launches a scratch game, opens
the windows you want to inspect by calling into it from LLDB, and has the game screenshot
itself — no clicking, no AppleScript, no screen-recording permission, and nothing touched in
the user's own session or settings.

It is how the aBase preview faults (hangar overlap, tile icons sliding off their buttons, the
staircased small terminal) were confirmed and re-checked against the other base sets.

For attach basics see `skills/lldb_debugging.md`; for read-only state dumps see
`skills/lldb_game_state_inspection.md`.

## 1. Launch a scratch instance

```bash
cp ~/Documents/OpenTTD/openttd.cfg /tmp/scratch/test.cfg
./build/openttd -x -c /tmp/scratch/test.cfg -s null -m null -g scripts/testdata/mass7-inair.sav &
```

- `-c <file>` — its own config. The config file's folder also becomes OpenTTD's working and
  personal dir (`DetermineBasePaths` in `src/fileio.cpp`), so screenshots and saves land next
  to the copied config instead of in `~/Documents/OpenTTD`. Content — base sets, NewGRFs, AIs
  — is still found through the ordinary search paths, so the installed sets stay available.
- `-x` — never write the config back, so the user's settings survive untouched.
- `-s null -m null` — no sound, no music.
- `-g <save>` — load a game. On the title screen there is no local company and no build
  toolbars; most build GUIs need `_local_company` to be a real company.
- Allow ~20 s before attaching. Base set loading dominates (aBase's base GRF is 180 MB).

**Use the real video driver.** `-v null` forces the null blitter — the log says
`dbg: [misc:1] Forcing blitter 'null'` — and the null blitter loads the **8bpp** variants of
every sprite. That is precisely the data that hides a 32bpp base set's bug: under `-v null`
aBase's terminal measures a flat 64x31 like openttd.grf does. Headless is useless here.

Pick the base set by editing the copied config before launch:

```bash
sed -i '' 's/^name = .*/name = aBase/; s/^shortname = .*/shortname = 61423332/' /tmp/scratch/test.cfg
```

`aBase = 61423332`, `OpenGFX = 4f474658`, `OpenGFX2_Classic = 6f676678`, `NightGFX = 4e474658`.
Confirm after attaching with `expr -- BaseGraphics::GetUsedSet()->name`.

## 2. Open the windows from LLDB

Call the `Show*` entry point, then drive buttons through the window's virtual `OnClick`:

```
process attach --pid <pid>
expr -- ShowBuildModularAirportWindow()
expr -- FindWindowByClass(WindowClass::BuildToolbar)->OnClick(_cursor.pos, 3, 1)
detach
```

`3` is `WID_MA_PIECE_3` (the cosmetic picker button) — widget ids come from
`src/widgets/*_widget.h`, and the click position argument rarely matters, so any `Point`
lvalue in reach will do. A file-local `Show*` helper that only exists inlined into an
`OnClick` (`image lookup -n ShowModularCosmeticPicker` shows just the inlined copy) cannot be
called directly: click its button instead.

**Detach and wait ~3 s before screenshotting.** Windows are only painted by the main loop,
which is stopped for as long as you are attached, so a screenshot taken in the same session
shows the screen as it was before your calls.

## 3. Make the game screenshot itself

```
process attach --pid <pid>
expr -- MakeScreenshot((ScreenshotType)1, _full_screenshot_path, 0, 0)
expr -- _full_screenshot_path
detach
```

Type 1 is `SC_CRASHLOG`: the raw blitter buffer — the whole game window with every open GUI
window in it, and the only type that runs **synchronously**. All other types queue onto the
main thread and will not have run by the time you detach. `_full_screenshot_path` is empty
going in (so the name is auto-generated) and holds the written path coming out.

Then crop and look at it — `Read` renders a PNG:

```bash
python3 -c "
from PIL import Image
im = Image.open(src)
im.crop((1620, 640, 2060, 830)).resize((880, 380), Image.NEAREST).save(out)"
```

On a Retina display the buffer is twice the logical GUI size (2880x1526 here), so crop
coordinates are doubled and `_gui_zoom` is `In2x` — worth remembering when checking that a
widget's drawing arithmetic scales.

Kill the instance when done: `kill <pid>`.

## Before/after pairs

Capture the "after" first, then `git stash`, rebuild (incremental — usually one or two files
plus the link), capture the "before" the same way, `git stash pop`, rebuild. Two crops of the
same button answer "is this actually wrong, and is it actually fixed" in a way that reading
the sprite maths does not.

## LLDB expression gotchas

- **`Point` is not a type name** in the debug info — it is `OTTD_Point`. Pass `nullptr` for a
  `Point *`, or hand the call a real `Point` global as scratch space and read it back:
  `expr -- GetSpriteSize((SpriteID)2665, &_cursor.pos, ZoomLevel::Normal)` then
  `expr -- _cursor.pos` gives the sprite's offsets (the cursor position is disposable in a
  scratch instance). That pair — `d.height` and `offset.y` — is what tells you how far a base
  set draws a tile sprite above its tile.
- **Plain enum constants are often missing** (`SC_CRASHLOG` is). Cast the number instead:
  `(ScreenshotType)1`.
- **`std::string("literal")` does not compile** in the expression parser. Pass an existing
  `std::string` global (`_full_screenshot_path` is a convenient empty one).
- **Strong types resist casts**: `expr -- _local_company` prints fine, `(int)_local_company`
  does not.
- **Function definitions are not allowed** in an expression; only statements and declarations.
- If even ordinary types (`Dimension`) fail to resolve, the debug map is stale — attach only
  to a binary built from the current `.o` files, and see the retiming note in
  `skills/lldb_debugging.md`.

## Why this is safe next to a live session

Separate process, separate config, `-x` so nothing is written back, screenshots into the
scratch dir. The user's running game is untouched; only the scratch instance is ever stopped
by the debugger, which also makes it the right place for calls that mutate state.
