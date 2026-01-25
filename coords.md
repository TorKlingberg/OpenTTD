# OpenTTD Coordinate Systems and Directions

This document explains the coordinate systems, rotations, and direction conventions used in OpenTTD, particularly for modular airport pathfinding.

## Map Coordinates (X, Y)

OpenTTD uses a 2D tile-based map with integer coordinates (X, Y), both starting at 0.

### Isometric Viewport

The game displays the map in **isometric projection** - the rectangular grid is rotated 45° to appear as a diamond on screen. This means all coordinate-aligned movements appear as **diagonal movements** on your monitor.

### Map Diamond Shape

For a 100×100 map (coordinates 0-99), the four corners form a diamond:

```
Map corners in (X, Y) coordinates:

                (0, 0)
                  ◆  ← top
                 / \
                /   \
               /     \
      (0, 99) ◆       ◆ (99, 0)
       left            right
              \       /
               \     /
                \   /
                 \ /
                  ◆
              (99, 99)
               bottom
```

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

A test airport was built with a hangar at (36, 60) and pieces placed all around it. Here's how coordinate changes appear on screen:

| Code Label | Bitmask | Coord Change | Viewport Appearance | Example Positions |
|------------|---------|--------------|---------------------|-------------------|
| "East"     | 0x02    | (+1, 0)      | Right-ish diagonal  | (36,60)→(37,60)  |
| "South"    | 0x04    | (0, +1)      | Down-right diagonal | (36,60)→(36,61)  |
| "West"     | 0x08    | (-1, 0)      | Left-ish diagonal   | (36,60)→(35,60)  |
| "North"    | 0x01    | (0, -1)      | Up-left diagonal    | (36,60)→(36,59)  |

**Key Insight:** The hangar door at rotation=0 faces **down-right** on your screen, which corresponds to **increasing Y** (the code's "South" direction, bitmask 0x04).

### Why This Matters

The original code incorrectly set hangars to 0x02 ("East"), which is dx=+1. This would require the apron to be at +X position (appears more horizontal/right).

But the hangar graphic clearly faces down-right, which is dy=+1 ("South", 0x04). That's why your vertical line of aprons (same X, increasing Y) didn't connect until we fixed the code.

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

## Rotation Values

Airport pieces can be rotated in 90° increments:

- `rotation = 0`: Base graphic orientation
- `rotation = 1`: Rotated 90° clockwise
- `rotation = 2`: Rotated 180°
- `rotation = 3`: Rotated 270° clockwise (90° counter-clockwise)

### How Rotation Affects Taxi Directions

For pieces with directional constraints (like hangars), rotation determines which coordinate direction they connect to.

**Example - Hangar taxi directions by rotation:**

At `rotation = 0`:
- Hangar door faces down-right on screen
- Connects via dy=+1 (code's "South", 0x04)
- Apron must be at (X, Y+1)

At `rotation = 1`:
- Hangar rotated 90° clockwise, door faces down-left
- Should connect via dx=-1 (code's "West", 0x08)
- Apron must be at (X-1, Y)

At `rotation = 2`:
- Hangar rotated 180°, door faces up-left
- Should connect via dy=-1 (code's "North", 0x01)
- Apron must be at (X, Y-1)

At `rotation = 3`:
- Hangar rotated 270° clockwise, door faces up-right
- Should connect via dx=+1 (code's "East", 0x02)
- Apron must be at (X+1, Y)

## Common Pitfalls

### 1. "SE" in `APT_DEPOT_SE` Does NOT Mean Southeast

The `_SE` suffix in piece type names like `APT_DEPOT_SE` refers to the **graphic's map orientation**, NOT compass directions or taxi directions.

**Incorrect:** `APT_DEPOT_SE` means the door faces southeast
**Correct:** It's just the graphic ID. At rotation=0, the door faces down-right (dy=+1, code's "South")

### 2. Don't Use Compass Directions for Viewport

Saying "north" or "south" for viewport directions is confusing in isometric view. Instead:
- Describe screen appearance: "up-left", "down-right", "right-ish", "down-ish"
- Or use coordinates: "+Y direction", "-X direction"

### 3. The Code Labels Are Arbitrary

When you see `dir_bit = 0x04; // South` in the code, don't think "southward on a compass". Think "dy=+1 direction" or "down-right on screen". The label is just a convenience.

## Practical Example: Connecting to a Hangar

**Scenario:** You place a hangar at position (40, 50) with rotation=0.

**Question:** Where should you place the connecting apron?

**Answer:**
1. Hangar at rot=0 allows bitmask **0x04** (code's "South")
2. Bitmask 0x04 = dy=+1 (increase Y, same X)
3. Apron must be at position **(40, 51)**

**On screen:** The apron will appear in the down-right direction from the hangar, which is exactly where the hangar door faces.

**If the hangar were rotated 90° (rot=1):**
1. Hangar at rot=1 allows bitmask **0x08** (code's "West")
2. Bitmask 0x08 = dx=-1 (decrease X, same Y)
3. Apron must be at position **(39, 50)**
4. On screen: down-left from the hangar

## Empirical Test Data

Test airport: Hangar at (36, 60) with gfx=24, rot=0

Surrounding tiles (user's compass labels → actual coordinates):

| User Label | Visual Position | Coordinates | Delta from Hangar | Movement Type |
|------------|----------------|-------------|-------------------|---------------|
| SE         | Down-right     | (36, 61)    | (0, +1)          | Orthogonal ✓  |
| E          | Right-ish      | (37, 60)    | (+1, 0)          | Orthogonal ✓  |
| NE         | Up-right       | (37, 59)    | (+1, -1)         | Diagonal      |
| N          | Up-ish         | (36, 59)    | (0, -1)          | Orthogonal ✓  |
| NW         | Up-left        | (35, 60)    | (-1, 0)          | Orthogonal ✓  |
| W          | Left-ish       | (35, 61)    | (-1, +1)         | Diagonal      |
| SW         | Down-left      | (37, 61)    | (+1, +1)         | Diagonal      |
| S          | Down           | (35, 59)    | (-1, -1)         | Diagonal      |

**Result:** The SE position (0, +1) successfully connected to the hangar, confirming that rotation=0 hangars use dy=+1 (bitmask 0x04).

## Quick Reference Table

| What You Want | Coordinate Change | Code Bitmask | Code Label |
|---------------|-------------------|--------------|------------|
| Right-ish on screen | dx = +1, dy = 0 | 0x02 | "East" |
| Down-right on screen | dx = 0, dy = +1 | 0x04 | "South" |
| Left-ish on screen | dx = -1, dy = 0 | 0x08 | "West" |
| Up-left on screen | dx = 0, dy = -1 | 0x01 | "North" |

## References

- `src/direction_type.h` - Direction enums (Note: these are for vehicles, NOT the same as pathfinder bitmasks)
- `src/airport_ground_pathfinder.cpp` lines 86-91 - Direction bitmask definitions
- `src/airport_pathfinder.cpp` - Taxi direction calculation for each piece type

---

**Last Updated**: 2026-01-25
**Author**: Based on empirical testing and bug fix for hangar direction confusion (SE vs South)
