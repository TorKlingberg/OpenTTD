# Airport Template Analysis

## Script: `scripts/parse_airport_template.py`

Parse and visualize modular airport template JSON files. Useful for debugging airport layouts, understanding runway configurations, and verifying taxi routing.

### Usage

```bash
# Compact grid view (default)
python3 scripts/parse_airport_template.py <template.json>

# Detailed per-tile info (piece types, rotations, flags, edges)
python3 scripts/parse_airport_template.py <template.json> --detail

# Runway analysis (segments, directions, ends, capacity summary)
python3 scripts/parse_airport_template.py <template.json> --runways

# Raw JSON output per tile (for piping to jq etc.)
python3 scripts/parse_airport_template.py <template.json> --raw

# Combine modes
python3 scripts/parse_airport_template.py <template.json> --grid --runways
```

### Grid legend

| Suffix | Meaning |
|--------|---------|
| `L`    | Landing runway |
| `T`    | Takeoff runway |
| `v`    | Direction LOW (travel toward low coordinate) |
| `^`    | Direction HIGH (travel toward high coordinate) |
| `>`    | One-way taxiway |
| `e`    | Has edge block mask |

### Template locations

- User templates: `~/Documents/OpenTTD/airport_templates/*.json`
- Test fixtures: `tmp/*.json` (copied for debugging sessions)

### Common tasks

**Check if a layout requires runway crossing:**
```bash
# If terminal area is completely separated from runways by another runway,
# aircraft must cross. Look for runway rows with no apron gaps at the edges.
python3 scripts/parse_airport_template.py <template.json> --grid
```

**Verify runway flag consistency:**
```bash
python3 scripts/parse_airport_template.py <template.json> --runways
# All tiles in a contiguous runway should have the same flags
```

**Compare two layouts:**
```bash
diff <(python3 scripts/parse_airport_template.py a.json --detail) \
     <(python3 scripts/parse_airport_template.py b.json --detail)
```
