# Modular Airport Routing Options

This document summarizes routing approaches for modular airports, with scope, mechanics, and trade-offs.

## Option A — Compile to AirportSpec/FTA (Vanilla-style)

**Idea**: Convert a modular layout into an AirportSpec-like finite-state machine (FTA), then reuse the existing aircraft movement/state logic.

**How it works**
- Build a layout graph from placed tiles.
- Generate movement nodes/states (terminals, hangars, runway segments, holding points, taxi nodes).
- Compute valid transitions and entry points based on geometry.
- Feed a generated spec/FTA into the existing airport code path.

**Pros**
- Fits OpenTTD’s existing aircraft system best.
- Reuses collision/blocking, takeoff/landing logic, and state machine.
- Fewer changes outside airport logic.

**Cons**
- Hard to generate a correct FTA for arbitrary layouts.
- Debugging invalid graphs can be painful.
- Validation requirements are high (runway length, entry points, blocks).

**Best for**
- Long-term correctness and compatibility with current airport mechanics.

---

## Option B — Taxiway Graph Pathfinding (A*/graph per airport)

**Idea**: Treat taxiways/runways as a graph and pathfind on it. Keep existing airborne logic but replace ground routing with pathfinding.

**How it works**
- Each taxiway tile becomes graph nodes/edges.
- Terminals/stands/hangars are goal nodes.
- Use A* or BFS for ground taxiing routes.
- Runways are special edges/nodes with reservation/locking.

**Pros**
- Easier to support arbitrary shapes.
- Straightforward pathfinding implementation.
- Works well with per-tile direction constraints.

**Cons**
- Requires new ground-routing logic for aircraft.
- Must integrate with existing state machine (ground vs air states).
- Needs a new reservation/locking layer for conflicts.

**Best for**
- Fast MVP with flexible layouts.

---

## Option C — Directed Tile Links (User-specified taxi directions)

**Idea**: Each taxiway tile has explicit allowed in/out directions. Routing follows these directed edges.

**How it works**
- Every taxi tile stores allowed directions (N/E/S/W, one-way or bidirectional).
- Routing is constrained by user-defined adjacency.
- Stand/runway access requires valid directed paths.

**Pros**
- Precise control for players.
- Avoids ambiguous routing in complex layouts.
- Simplifies conflict resolution in some cases.

**Cons**
- High UX overhead (user must configure directions).
- Still needs a pathfinding pass.
- More error cases from bad user configuration.

**Best for**
- Power-user layouts with explicit routing control.

---

## Option D — Hybrid (Auto-graph + user overrides)

**Idea**: Generate a default taxi graph from tiles; allow overrides (one-way, blocked edges, preferred paths).

**How it works**
- Auto-generate a graph from tile type + rotation.
- Provide UI toggles for one-way and blocked edges.
- Use pathfinding on the resulting graph.

**Pros**
- Usable out-of-the-box with optional fine-tuning.
- Fewer invalid layouts.
- Good balance for MVP + power users.

**Cons**
- More UI + validation complexity.
- Needs clear visualization of active edges.

**Best for**
- Practical gameplay with optional advanced control.

---

## Runway Handling (independent of A/B/C/D)

**Choices**
- Treat runway as a special locked edge (exclusive usage).
- Use explicit runway start/end tiles and length validation.
- Maintain current runway takeoff/landing logic and map to modular runway tiles.

**Key concerns**
- Prevent head-on conflicts.
- Ensure sufficient runway length for aircraft type.
- Provide clear entry/exit points for ground routing.

---

## Validation Levels

**Minimal**
- At least one stand/terminal.
- At least one runway or helipad.
- A connected path from stand to runway/helipad.

**Strict**
- Runway length requirements.
- Valid entry/exit/holding points.
- Block reservations do not deadlock.
- No unreachable terminals/hangars.

---

## Recommendation Snapshot

- **Fastest MVP**: Option B (graph pathfinding) with minimal validation.
- **Best long-term fit**: Option A (FTA compilation) once generation is reliable.
- **Best UX compromise**: Option D (auto-graph + overrides).
