#!/usr/bin/env python3
"""Parse and visualize modular airport template JSON files.

Usage:
    python3 scripts/parse_airport_template.py <template.json> [options]

Options:
    --grid         Show compact grid view (default)
    --detail       Show detailed per-tile breakdown
    --runways      Show only runway information
    --connections  Show taxiway connectivity analysis (which tiles connect to what)
    --raw          Show raw JSON per tile
"""

import json
import sys
import argparse

# AirportTiles enum (0-indexed, matching src/table/airporttile_ids.h)
PIECE_NAMES = {
    0: "APRON", 1: "APRON_F_NW", 2: "APRON_F_SW", 3: "STAND", 4: "APRON_W",
    5: "APRON_S", 6: "APRON_VX_S", 7: "APRON_HX_W", 8: "APRON_VX_N",
    9: "APRON_HX_E", 10: "APRON_E", 11: "APRON_N", 12: "APRON_HOR",
    13: "APRON_N_F_SW", 14: "RWY1", 15: "RWY2", 16: "RWY3", 17: "RWY4",
    18: "RWY_END_F_SE", 19: "BUILD2", 20: "TWR_F_SW", 21: "ROUND_TERM",
    22: "BUILD3", 23: "BUILD1", 24: "DEPOT_SE", 25: "STAND1",
    26: "STAND_PIER_NE", 27: "PIER_NW_NE", 28: "PIER", 29: "EMPTY",
    30: "EMPTY_F_NE", 31: "RADAR_GR_F_SW", 32: "RADIO_F_NE",
    33: "SM_BUILD3", 34: "SM_BUILD2", 35: "SM_BUILD1", 36: "GRASS_F_SW",
    37: "GRASS2", 38: "GRASS1", 39: "GRASS_F_NE_FL", 40: "RWY_S_NEAR",
    41: "RWY_S_MID", 42: "RWY_S_FAR", 43: "SM_DEPOT_SE", 44: "HELIPORT",
    45: "RWY_END", 46: "RWY5", 47: "TOWER", 48: "APRON_F_NE",
    49: "RWY_END_F_NW", 50: "RWY_F_NW", 51: "RADAR_F_SW", 52: "RADAR_F_NE",
    53: "HELI1", 54: "HELI2_F_NW", 55: "HELI2", 56: "APRON_F_NE_SW",
    57: "RWY_END_F_NW_SW", 58: "RWY_END_F_SE_SW", 59: "RWY_END_F_NE_NW",
    60: "RWY_END_F_NE_SE", 61: "HELI2_F_NE_SE", 62: "APRON_F_SE_SW",
    63: "LOW_BUILD_F_N", 64: "LOW_BUILD_F_NW", 65: "APRON_F_SE",
    66: "HELI3_F_SE_SW", 67: "HELI3_F_NW_SW", 68: "HELI3_F_NW",
    69: "LOW_BUILD", 70: "APRON_F_NE_SE", 71: "APRON_HALF_E",
    72: "APRON_HALF_W", 73: "GRASS_F_NE_FL2", 74: "DEPOT_SW",
    75: "DEPOT_NW", 76: "DEPOT_NE", 77: "SM_DEPOT_SW", 78: "SM_DEPOT_NW",
    79: "SM_DEPOT_NE",
}

RUNWAY_PIECES = {14, 15, 16, 17, 40, 41, 42, 45, 46}
RUNWAY_END_PIECES = {45, 40, 42}  # APT_RUNWAY_END, SMALL_NEAR, SMALL_FAR
HANGAR_PIECES = {24, 43, 74, 75, 76, 77, 78, 79}
HELIPAD_PIECES = {44, 53, 54, 55, 61, 66, 67, 68}
STAND_PIECES = {3, 25}
NON_TAXIABLE = {19, 20, 22, 23, 29, 30, 31, 32, 33, 34, 35, 47, 51, 52}

# Runway flags
RUF_LANDING = 0x01
RUF_TAKEOFF = 0x02
RUF_DIR_LOW = 0x04
RUF_DIR_HIGH = 0x08


def classify_tile(pt):
    """Return a category string for a piece type."""
    if pt in RUNWAY_PIECES:
        return "runway"
    if pt in HANGAR_PIECES:
        return "hangar"
    if pt in HELIPAD_PIECES:
        return "helipad"
    if pt in STAND_PIECES:
        return "stand"
    if pt in NON_TAXIABLE:
        return "building"
    return "apron"


def runway_flags_str(flags):
    """Human-readable runway flags."""
    parts = []
    if flags & RUF_LANDING:
        parts.append("LAND")
    if flags & RUF_TAKEOFF:
        parts.append("TAKE")
    if flags & RUF_DIR_LOW:
        parts.append("LOW")
    if flags & RUF_DIR_HIGH:
        parts.append("HIGH")
    return "|".join(parts) if parts else "none"


def edge_mask_str(mask):
    """Human-readable edge block mask."""
    dirs = []
    if mask & 0x01:
        dirs.append("N")
    if mask & 0x02:
        dirs.append("E")
    if mask & 0x04:
        dirs.append("S")
    if mask & 0x08:
        dirs.append("W")
    return "+".join(dirs) if dirs else ""


def short_name(pt):
    """Short 6-char display name for grid view."""
    name = PIECE_NAMES.get(pt, f"?{pt}")
    # Abbreviate for grid display
    if pt in RUNWAY_END_PIECES:
        return "RWY_E "
    if pt in RUNWAY_PIECES:
        return "RWY   "
    if pt in HANGAR_PIECES:
        return "DEPOT "
    if pt in HELIPAD_PIECES:
        return "HELI  "
    if pt in STAND_PIECES:
        return "STAND "
    if pt in NON_TAXIABLE:
        return "BUILD "
    if "GRASS" in name:
        return "GRASS "
    return "APRON "


def load_template(path):
    with open(path) as f:
        data = json.load(f)
    tiles = data.get("tiles", [])
    meta = {k: v for k, v in data.items() if k != "tiles"}
    return tiles, meta


def build_grid(tiles):
    """Build a 2D dict keyed by (dx, dy)."""
    grid = {}
    for t in tiles:
        grid[(t["dx"], t["dy"])] = t
    max_dx = max(t["dx"] for t in tiles) if tiles else 0
    max_dy = max(t["dy"] for t in tiles) if tiles else 0
    return grid, max_dx, max_dy


def show_grid(tiles, meta):
    """Compact grid visualization."""
    grid, max_dx, max_dy = build_grid(tiles)

    if meta:
        for k, v in meta.items():
            print(f"  {k}: {v}")
        print()

    # Header
    header = "      " + "".join(f"  dx={x:<3}" for x in range(max_dx + 1))
    print(header)
    print("      " + "-" * ((max_dx + 1) * 7))

    for dy in range(max_dy + 1):
        parts = []
        for dx in range(max_dx + 1):
            t = grid.get((dx, dy))
            if t is None:
                parts.append("  ---  ")
                continue
            pt = t["piece_type"]
            flags = t.get("runway_flags", 0)
            ow = t.get("one_way_taxi", False)
            edge = t.get("edge_block_mask", 0)
            name = short_name(pt).rstrip()

            suffix = ""
            if flags:
                if flags & RUF_LANDING:
                    suffix = "L"
                elif flags & RUF_TAKEOFF:
                    suffix = "T"
                if flags & RUF_DIR_LOW:
                    suffix += "v"
                if flags & RUF_DIR_HIGH:
                    suffix += "^"
            elif ow:
                suffix = ">"
            elif edge:
                suffix = "e"

            cell = f"{name}{suffix}"
            parts.append(f" {cell:<6}")

        print(f"dy={dy:<2} |{''.join(parts)}")


def show_detail(tiles):
    """Detailed per-tile breakdown."""
    grid, max_dx, max_dy = build_grid(tiles)

    for dy in range(max_dy + 1):
        print(f"\n--- Row dy={dy} ---")
        for dx in range(max_dx + 1):
            t = grid.get((dx, dy))
            if t is None:
                continue
            pt = t["piece_type"]
            name = PIECE_NAMES.get(pt, f"unknown({pt})")
            cat = classify_tile(pt)
            rot = t.get("rotation", 0)
            flags = t.get("runway_flags", 0)
            ow = t.get("one_way_taxi", False)
            edge = t.get("edge_block_mask", 0)

            line = f"  ({dx},{dy}): {name} (pt={pt}, {cat})"
            if rot:
                line += f" rot={rot}"
            if flags:
                line += f" flags={runway_flags_str(flags)}"
            if ow:
                line += " ONE_WAY"
            if edge:
                line += f" edge={edge_mask_str(edge)}"
            print(line)


def show_runways(tiles):
    """Show runway analysis."""
    grid, max_dx, max_dy = build_grid(tiles)

    # Find contiguous runway segments
    runway_tiles = [(t["dx"], t["dy"], t) for t in tiles if t["piece_type"] in RUNWAY_PIECES]
    if not runway_tiles:
        print("No runway tiles found.")
        return

    # Group by row (for horizontal) and column (for vertical)
    h_rows = {}  # dy -> list of (dx, tile)
    v_cols = {}  # dx -> list of (dy, tile)
    for dx, dy, t in runway_tiles:
        rot = t.get("rotation", 0)
        if rot % 2 == 0:  # horizontal
            h_rows.setdefault(dy, []).append((dx, t))
        else:  # vertical
            v_cols.setdefault(dx, []).append((dy, t))

    print("Horizontal runways:")
    for dy in sorted(h_rows.keys()):
        row = sorted(h_rows[dy], key=lambda x: x[0])
        x_range = f"dx={row[0][0]}-{row[-1][0]}"
        flags = row[0][1].get("runway_flags", 0)
        ends = [dx for dx, t in row if t["piece_type"] in RUNWAY_END_PIECES]
        print(f"  dy={dy}: {x_range} ({len(row)} tiles) flags={runway_flags_str(flags)} ends@dx={ends}")

    if v_cols:
        print("\nVertical runways:")
        for dx in sorted(v_cols.keys()):
            col = sorted(v_cols[dx], key=lambda x: x[0])
            y_range = f"dy={col[0][0]}-{col[-1][0]}"
            flags = col[0][1].get("runway_flags", 0)
            ends = [dy for dy, t in col if t["piece_type"] in RUNWAY_END_PIECES]
            print(f"  dx={dx}: {y_range} ({len(col)} tiles) flags={runway_flags_str(flags)} ends@dy={ends}")

    # Summary
    print(f"\nTotal runway tiles: {len(runway_tiles)}")
    print(f"Stands: {sum(1 for t in tiles if t['piece_type'] in STAND_PIECES)}")
    print(f"Hangars: {sum(1 for t in tiles if t['piece_type'] in HANGAR_PIECES)}")
    print(f"Helipads: {sum(1 for t in tiles if t['piece_type'] in HELIPAD_PIECES)}")


def main():
    parser = argparse.ArgumentParser(description="Parse OpenTTD modular airport template JSON")
    parser.add_argument("template", help="Path to template JSON file")
    parser.add_argument("--grid", action="store_true", help="Compact grid view (default)")
    parser.add_argument("--detail", action="store_true", help="Detailed per-tile breakdown")
    parser.add_argument("--runways", action="store_true", help="Runway analysis only")
    parser.add_argument("--raw", action="store_true", help="Raw JSON per tile")
    args = parser.parse_args()

    tiles, meta = load_template(args.template)

    if not any([args.grid, args.detail, args.runways, args.raw]):
        args.grid = True

    if args.raw:
        for t in sorted(tiles, key=lambda t: (t["dy"], t["dx"])):
            print(json.dumps(t))
        return

    if args.grid:
        show_grid(tiles, meta)
    if args.detail:
        show_detail(tiles)
    if args.runways:
        show_runways(tiles)


if __name__ == "__main__":
    main()
