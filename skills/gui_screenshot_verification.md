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
  toolbars; most build GUIs need `_local_company` to be a real company. Bare `-g` with no
  argument generates a fresh random map and company, which is usually what you want when the
  check is about a window rather than about particular tiles, and it starts far faster than
  loading a large fixture.
- Year-gated GUI items are drawn disabled, which is easy to mistake for a drawing bug. Set the
  start year in the copied config before launch (`sed -i "" "s/^starting_year = .*/starting_year
  = 2000/"`) so the pieces you came to look at are actually available.
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
lvalue in reach will do. Both arguments have to be spelled that way: the widget **enum
constants are not in the debug info**, so pass the plain number, and `Point p{0,0}` will not
compile, so pass an existing `OTTD_Point` lvalue such as `_cursor.pos`. A file-local `Show*`
helper that only exists inlined into an `OnClick` cannot be called directly: click its button
instead. `nm -a build/openttd | grep <name>` tells you which it is: a file-static
`WindowDesc` keeps its symbol (`__ZL35_build_modular_cosmetic_picker_desc`) while the
`static` function that used it is gone entirely, so the presence of the desc is not a sign
that you can open the window through it.

Each `-o expr` is its own expression: a variable declared in one is **not** visible in the
next. Chain the whole thing into one `-o`, or refer back to the LLDB value (`$0`).

### When the expression parser will not cooperate: synthesise a real click

In an optimised build the types a call needs are only present in compilation units that
actually included them, and `--batch` attaches you to a frame in libsystem where almost
nothing resolves. Rather than hunting for a frame with the right DWARF, drive the input
globals and let the game's own main loop dispatch the click. Only simple globals are
involved, so this works no matter which types resolve, and it exercises real hit-testing and
window focus instead of bypassing them:

```bash
#!/usr/bin/env bash
# click.sh X Y -- synthesise a left click at screen position X,Y in the scratch game.
PID=$(pgrep -f "openttd -x -c /tmp/scratch")
lldb --batch -o "process attach --pid $PID" \
 -o "expr -- _cursor.pos.x = $1" -o "expr -- _cursor.pos.y = $2" \
 -o "expr -- _cursor.in_window = true" \
 -o "expr -- _left_button_clicked = false" -o "expr -- _left_button_down = true" \
 -o "detach" >/dev/null 2>&1
sleep 1   # let the main loop run HandleMouseEvents
lldb --batch -o "process attach --pid $PID" -o "expr -- _left_button_down = false" \
 -o "detach" >/dev/null 2>&1
```

`HandleMouseEvents` (`src/window.cpp`) turns `_left_button_down && !_left_button_clicked`
into a left click at `_cursor.pos`, so those four globals are the whole contract. Release the
button afterwards or the next screenshot catches the game mid-drag. Coordinates are in the
blitter buffer's own pixels, the same ones you read off a previous screenshot.

Confirm the click landed rather than assuming it did:

```
expr -- (int)(FindWindowByClass(WindowClass::BuildDepot) != nullptr)
```

`FindWindowByClass` resolves; `FindWindowById` does not.

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

**It is only empty the first time.** `MakeScreenshot` leaves the path it wrote in
`_full_screenshot_path` and the stem in `_screenshot_name`, so a second call appends to them:
you get `shot.png.png`, then `shot.png.png.png`, and eventually a silent `false` return with
no file written. Nothing in the output says why. Reset both strings before every call. They
are libc++ `std::string`s, and a short string keeps its size in the last byte of the object,
so zeroing byte 23 empties one without needing `std::string` to be constructible in the
expression parser:

```bash
#!/usr/bin/env bash
# shot.sh -- screenshot the scratch game; prints the path written.
PID=$(pgrep -f "openttd -x -c /tmp/scratch")
lldb --batch -o "process attach --pid $PID" \
 -o "expr -- *((unsigned char*)&_screenshot_name + 23) = 0" \
 -o "expr -- *((unsigned char*)&_full_screenshot_path + 23) = 0" \
 -o "expr -- MakeScreenshot((ScreenshotType)1, _full_screenshot_path, 0, 0)" \
 -o "expr -- _full_screenshot_path" -o "detach" 2>&1 |
 grep -oE '"/tmp/scratch[^"]+\.png"' | tail -1 | tr -d '"'
```

Then crop and look at it — `Read` renders a PNG:

```bash
python3 -c "
from PIL import Image
im = Image.open(src)
im.crop((1620, 640, 2060, 830)).resize((880, 380), Image.NEAREST).save(out)"
```

On a Retina display the buffer is twice the logical GUI size, so crop coordinates are double
the `resolution` in the config and `_gui_zoom` is `In2x`, worth remembering when checking
that a widget's drawing arithmetic scales. Ask rather than assume the factor: `expr --
_screen.width` and `_screen.height` are the screenshot's exact pixel dimensions.

To crop one window rather than hunting for it, read its own rectangle:

```
expr -- FindWindowByClass(WindowClass::BuildDepot)->left
expr -- FindWindowByClass(WindowClass::BuildDepot)->top
expr -- FindWindowByClass(WindowClass::BuildDepot)->width
expr -- FindWindowByClass(WindowClass::BuildDepot)->height
```

Those are already in buffer pixels, so they crop the screenshot directly, and the crop stays
right when the window opens somewhere else next time.

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
- **Widget id enums do not resolve either** (`WID_MA_PIECE_0` is as absent as `SC_CRASHLOG`).
  Read the number off `src/widgets/*_widget.h` and pass it.
- Selecting an openttd stack frame does **not** rescue a failed type lookup. The frames an
  idle game offers are its video driver and main loop, whose compilation units never included
  the GUI headers, so `Point` and the widget enums are missing there too. Use the synthetic
  click above instead of hunting for a better frame.
- If even ordinary types (`Dimension`) fail to resolve, the debug map is stale — attach only
  to a binary built from the current `.o` files, and see the retiming note in
  `skills/lldb_debugging.md`.

## Why this is safe next to a live session

Separate process, separate config, `-x` so nothing is written back, screenshots into the
scratch dir. The user's running game is untouched; only the scratch instance is ever stopped
by the debugger, which also makes it the right place for calls that mutate state.
