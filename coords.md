# OpenTTD Coordinate Systems and Directions

This document explains the coordinate systems, rotations, and direction conventions used in OpenTTD, particularly for modular airport pathfinding.

## Map Coordinates (X, Y)

OpenTTD uses a 2D tile-based map with integer coordinates (X, Y), both starting at 0.

### Isometric Viewport

The game displays the map in **isometric projection** - the rectangular grid is rotated 45° to appear as a diamond on screen. This means all coordinate-aligned movements appear as **diagonal movements** on your monitor.

### The Projection

`RemapCoords` in `src/landscape.h` is the authority:

```cpp
pt.x = (y - x) * 2;
pt.y = (y + x - z);
```

So **+X moves down-left** (screen x -2, screen y +1) and **+Y moves down-right** (screen x +2, screen y +1). Getting these two the wrong way round mirrors every conclusion that follows, and a mirrored model stays perfectly self-consistent, so it can survive a lot of reasoning before anything looks wrong. Derive from `RemapCoords`, don't guess.

### Map Diamond Shape

For a 100×100 map (coordinates 0-99), the four corners form a diamond:

```
Map corners in (X, Y) coordinates:

                (0, 0)
                  ◆  ← top
                 / \
                /   \
               /     \
      (99, 0) ◆       ◆ (0, 99)
       left            right
              \       /
               \     /
                \   /
                 \ /
                  ◆
              (99, 99)
               bottom
```

(Check: `(99, 0)` projects to screen x `(0 - 99) * 2 = -198`, i.e. far left; `(0, 99)` to `+198`, far right.)

## Orthogonal Movement (Pathfinding Directions)

The airport pathfinder only allows **orthogonal movement** - changing one coordinate at a time.

### The Four Pathfinder Directions

From `src/airport_ground_pathfinder.cpp` lines 86-91:

```cpp
if (dy == -1) dir_bit = 0x01;  // Code label: "North"
if (dx == +1) dir_bit = 0x02;  // Code label: "East"
if (dy == +1) dir_bit = 0x04;  // Code label: "South"
if (dx == -1) dir_bit = 0x08;  // Code label: "West"
```

**Important:** These "North/East/South/West" labels in the code are **NOT viewport directions**. They're just arbitrary names for the four orthogonal map movements. Don't confuse them with compass directions!

### Viewport Appearance (Empirical Test)

A test airport was built with pieces placed all around. Here's how coordinate changes appear on screen and map to overlay sprites:

| Code Label | Bitmask | Coord Change | Viewport Appearance | SPR_ONEWAY_BASE Offset |
|------------|---------|--------------|---------------------|------------------------|
| "North"    | 0x01    | (0, -1)      | Up-left (NW)        | +3                     |
| "East"     | 0x02    | (+1, 0)      | Down-left (SW)      | +0                     |
| "South"    | 0x04    | (0, +1)      | Down-right (SE)     | +4                     |
| "West"     | 0x08    | (-1, 0)      | Up-right (NE)       | +1                     |

**Key Insight:** Coordinate changes move along the diagonal axes of the viewport.

## Combined Direction Bitmasks

Multiple directions can be combined using bitwise OR:

| Bitmask | Directions | Description |
|---------|------------|-------------|
| 0x0F    | All four   | Piece allows movement in all orthogonal directions |
| 0x05    | N + S      | Vertical: dy can change both ways (same X) |
| 0x0A    | E + W      | Horizontal: dx can change both ways (same Y) |
| 0x03    | N + E      | Two adjacent directions |
| 0x06    | E + S      | Two adjacent directions |
| 0x0C    | S + W      | Two adjacent directions |
| 0x09    | W + N      | Two adjacent directions |

## Modular Runway Direction Flags

For modular airports, runway direction flags use **travel-direction semantics**:

- `RUF_DIR_LOW`: operations traveling toward the low-coordinate end (lower X or Y)
- `RUF_DIR_HIGH`: operations traveling toward the high-coordinate end (higher X or Y)

This is different from an "operations starting from this end" interpretation.

### What This Means in Practice

- Landing:
  - Touchdown at low end implies rollout toward high end, so it requires `RUF_DIR_HIGH`.
  - Touchdown at high end implies rollout toward low end, so it requires `RUF_DIR_LOW`.
- Takeoff:
  - Starting from low end means accelerating toward high end, so it requires `RUF_DIR_HIGH`.
  - Starting from high end means accelerating toward low end, so it requires `RUF_DIR_LOW`.

### Visual Overlay Notes

Runway and taxiway overlay arrows use `SPR_ONEWAY_BASE` sprites. Verified in-game mapping:

- **Sprite 0**: SW (Increasing X)
- **Sprite 1**: NE (Decreasing X)
- **Sprite 3**: NW (Decreasing Y)
- **Sprite 4**: SE (Increasing Y)

**Modular Runway Direction Flags:**
- `RUF_DIR_LOW` (Decreasing coordinate) points **NW** (3) or **NE** (1).
- `RUF_DIR_HIGH` (Increasing coordinate) points **SE** (4) or **SW** (0).

**Modular Airport Axis Mapping:**
- **Rotation 0 & 2**: NE-SW axis. Corresponds to `ROAD_X` (Sprite offset 0).
- **Rotation 1 & 3**: NW-SE axis. Corresponds to `ROAD_Y` (Sprite offset 3).

## Rotation Values

Airport pieces can be rotated in 90° increments:

- `rotation = 0`: Base graphic orientation
- `rotation = 1`: Rotated 90° clockwise
- `rotation = 2`: Rotated 180°
- `rotation = 3`: Rotated 270° clockwise (90° counter-clockwise)

### How Rotation Affects Taxi Directions

For pieces with directional constraints (like hangars), rotation determines which coordinate direction they connect to.

**Example - Hangar taxi directions by rotation:**

Hangar rotations name the compass direction the door faces: `0=SE, 1=NE, 2=NW, 3=SW`. In the bitmask order above that open edge is bit `(rotation + 2) % 4` — see `CalculateAutoTaxiDirectionsForGfx` (`src/airport_pathfinder.cpp`) and `ModularAirportGetHangarExitDirection` (`src/modular_airport_holding.cpp`), which must stay in sync.

At `rotation = 0` (door faces SE):
- Door faces down-right on screen
- Connects via dy=+1 (code's "South", 0x04)
- Apron must be at (X, Y+1)

At `rotation = 1` (door faces NE):
- Door faces up-right on screen
- Connects via dx=-1 (code's "West", 0x08)
- Apron must be at (X-1, Y)

At `rotation = 2` (door faces NW):
- Door faces up-left on screen
- Connects via dy=-1 (code's "North", 0x01)
- Apron must be at (X, Y-1)

At `rotation = 3` (door faces SW):
- Door faces down-left on screen
- Connects via dx=+1 (code's "East", 0x02)
- Apron must be at (X+1, Y)

Note that rotations 1 and 3 are the pair that a mirrored mental model gets 180° wrong while leaving 0 and 2 looking fine — the same trap the pathfinder comment warns about.

## Common Pitfalls

### 1. "SE" in `APT_DEPOT_SE` Names the Door, But Only at rotation=0

The `_SE` suffix does mean the game's SE: at `rotation = 0` an `APT_DEPOT_SE` opens onto (X, Y+1), which is SE in `_tileoffs_by_diagdir` and down-right on screen. The four suffixes match the four hangar rotations exactly (`SE/NE/NW/SW` = rotation `0/1/2/3`).

The trap is that the suffix is not the whole story. `APT_DEPOT_SE` is the *canonical* form, and a stored piece carries a rotation as well: `CalculateAutoTaxiDirectionsForGfx` takes `hangar_rot` from the rotation field for `APT_DEPOT_SE` and only overrides it for the three explicitly-named variants. So a tile whose gfx says `_SE` may still open NE, NW or SW.

**Incorrect:** the suffix is an arbitrary graphic ID with no directional meaning
**Incorrect:** the suffix alone tells you which way the door faces
**Correct:** read the effective direction from piece type *and* rotation, exactly as `CalculateAutoTaxiDirectionsForGfx` does

### 2. Don't Use Compass Directions for Viewport

Saying "north" or "south" for viewport directions is confusing in isometric view. Instead:
- Describe screen appearance: "up-left", "down-right", "right-ish", "down-ish"
- Or use coordinates: "+Y direction", "-X direction"

### 3. The Code Labels Are Arbitrary

When you see `dir_bit = 0x04; // South` in the code, don't think "southward on a compass". Think "dy=+1 direction" or "down-right on screen". The label is just a convenience.

## Empirical Test Data

Test airport: Hangar at (36, 60) with gfx=24, rot=0

The eight tiles around it, with screen positions derived from `RemapCoords`:

| Neighbour | Coordinates | Delta from Hangar | Screen Position | Movement Type |
|-----------|-------------|-------------------|-----------------|---------------|
| SE        | (36, 61)    | (0, +1)          | Down-right      | Orthogonal ✓  |
| SW        | (37, 60)    | (+1, 0)          | Down-left       | Orthogonal ✓  |
| NW        | (36, 59)    | (0, -1)          | Up-left         | Orthogonal ✓  |
| NE        | (35, 60)    | (-1, 0)          | Up-right        | Orthogonal ✓  |
| —         | (37, 59)    | (+1, -1)         | Left            | Diagonal      |
| —         | (35, 61)    | (-1, +1)         | Right           | Diagonal      |
| —         | (37, 61)    | (+1, +1)         | Down            | Diagonal      |
| —         | (35, 59)    | (-1, -1)         | Up              | Diagonal      |

The four diagonal neighbours sit straight up/down/left/right on screen, which is the clearest reminder that screen "up" is not a map direction at all.

**Result:** The SE position (0, +1) successfully connected to the hangar, confirming that rotation=0 hangars use dy=+1 (bitmask 0x04).

## Quick Reference Table

| What You Want | Coordinate Change | Code Bitmask | Code Label |
|---------------|-------------------|--------------|------------|
| Down-left on screen | dx = +1, dy = 0 | 0x02 | "East" |
| Down-right on screen | dx = 0, dy = +1 | 0x04 | "South" |
| Up-right on screen | dx = -1, dy = 0 | 0x08 | "West" |
| Up-left on screen | dx = 0, dy = -1 | 0x01 | "North" |

## References

- `src/landscape.h` - `RemapCoords`, the world-to-screen projection everything here derives from
- `src/map.cpp` - `_tileoffs_by_diagdir`, the compass name for each coordinate step
- `src/direction_type.h` - Direction enums (Note: these are for vehicles, NOT the same as pathfinder bitmasks)
- `src/airport_ground_pathfinder.cpp` lines 86-91 - Direction bitmask definitions
- `src/airport_pathfinder.cpp` - Taxi direction calculation for each piece type

---

**Last Updated**: 2026-08-20
**Author**: Based on empirical testing and finalized ground arrow verified mappings. Screen directions corrected against `RemapCoords` on 2026-08-20; the X and Y axes had been documented the wrong way round.
