# Multiplayer Determinism: Dubins Integer-Snap

## Goal
Ensure bit-identical holding loop waypoints across different CPU architectures to prevent desyncs in multiplayer.

## Context
Simulation state (specifically `ModularHoldingLoop::waypoints`) is computed during commands like building modular airport pieces. Floating-point variations between CPUs (e.g., Intel/x86 vs. Apple/ARM) for standard functions like `atan2`, `sin`, and `cos` would cause clients to diverge.

## Action
1.  **Computation:** Keep existing `double`-based Dubins math in `ComputeDubins` and `SampleDubinsPath`. This maintains the precision needed for the iterative geometric calculations.
2.  **Snap to Integer:** In `AddWaypoint` (or the equivalent loop insertion point), round the final `x` and `y` coordinates to the nearest integer pixel using a deterministic rounding function:
    - `(int32_t)std::floor(x + 0.5)`
3.  **Storage:** Store the final coordinates as `int32_t` (or `int`) in the `ModularHoldingLoop::Waypoint` struct.
4.  **Consistency:** Since waypoints are sampled at a relatively large interval (~48px), a ±0.5px rounding error has no impact on aircraft flight behavior but ensures every client sees the exact same pixel positions.

## Benefit
Provides platform-independent simulation state without the significant overhead and risk of a full fixed-point trigonometry rewrite.
