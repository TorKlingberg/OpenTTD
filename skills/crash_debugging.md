# Crash Debugging Skill (OpenTTD)

Use this workflow when the game crashes or hits an assertion.

## 1) Find the newest crash artifact
Crash files are written to `~/Documents/OpenTTD/`:
- `crashYYYYMMDDHHMMSS.json.log` (primary signal: reason + stacktrace)
- `crashYYYYMMDDHHMMSS.sav` (repro save)
- `crashYYYYMMDDHHMMSS.png` (screenshot)

Commands:
```bash
ls -lt ~/Documents/OpenTTD/crash*.json.log | head -n 5
latest=$(ls -t ~/Documents/OpenTTD/crash*.json.log | head -n 1)
echo "$latest"
```

## 2) Read crash reason and top of stack
The first pass is to extract:
- `crash.reason`
- `info.openttd.version.hash`
- first engine frames in `stacktrace`

Commands:
```bash
sed -n '1,220p' "$latest"
# optional focused scan:
rg -n '"reason"|"hash"|modular|Draw|Assert|stacktrace' "$latest"
```

## 3) Check recent runtime log tail
If OpenTTD was started with debug logging redirected, inspect the tail of `/tmp/openttd.log`.

Commands:
```bash
tail -n 200 /tmp/openttd.log
# modular airport focus
grep '\[ModAp\]' /tmp/openttd.log | tail -n 120
```

## 4) Correlate crash with code
- Open the crashing function from stacktrace.
- Look for unchecked tile/world coordinate usage, stale pointers, or invalid state transitions.
- Prefer fixing the main logic path; avoid adding broad fallback behavior.

Fast search:
```bash
rg -n 'FunctionNameFromStack|GetSlopePixelZ|IsValidTile|TileVirtXY|reservation|holding' src
```

## 5) Validate fix
1. Build: `make -C build -j8`
2. Re-run the same save/repro steps.
3. Confirm no assertion/crash and no new lockups/regressions.

## 6) Commit discipline
- Commit only the intended fix files.
- Use a clear message: crash symptom + root cause area.
- If docs were updated separately, commit docs in a separate commit.

## Common gotchas in this repo
- Overlay drawing code can run during transient map/edit states.
- `GetSlopePixelZ(x, y)` assumes in-map coords; use the outside-map-safe path when coordinates may be out of bounds.
- Airport reservation/holding visuals should never crash gameplay; rendering must tolerate stale transient data.
