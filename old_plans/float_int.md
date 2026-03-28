# Multiplayer Determinism: Dubins Fixed-Point Refactor (Revised)

## Goal
Replace all floating-point math in the Dubins holding loop generation with bit-identical fixed-point arithmetic to ensure identical results across all CPU architectures and prevent desyncs in multiplayer.

## Why? (Risk Model)
While `ModularHoldingLoop::waypoints` is technically a rebuildable cache, it is **authoritative simulation state**. Vehicle movement advancement depends on the exact pixel coordinates and the total number of waypoints in this list. If two clients (e.g., x86 vs. ARM) generate waypoints that differ by even one pixel or one sample, the aircraft's position and index progress will diverge, causing an immediate and permanent desync.

## Action Plan

### 1. Fixed-Point Specification & Range Safety
- **Representation:** 16.16 fixed-point (scale factor $2^{16} = 65536$).
- **Data Type:** `int64_t` for intermediate storage and arithmetic.
- **Station Spread Invariant:** OpenTTD enforces a hard limit on station spread (max 64 tiles). This ensures that any station's bounding box is at most 1024x1024 pixels.
- **Relative Coordinates:** All calculations in `ComputeDubins` and `SampleDubinsPath` will be performed **relative to the station's center pixel**.
    - Max relative pixel coordinate: $\pm 1024$.
    - In 16.16: $\pm 1024 \times 2^{16} = \pm 2^{26}$.
    - Max intermediate product (e.g., $x^2$ or $x \times y$): $(2^{26})^2 = 2^{52}$.
    - Even with a radius × angle product (max $\approx 2^{25} \times 2^{19} = 2^{44}$), results fit safely within a signed `int64_t` ($2^{63}-1$).
- **Rounding:** Final snap to integer pixel must match `std::lround` (round halfway cases away from zero):
    - `inline int FP16Round(int64_t val) { return (val >= 0) ? (int)((val + 32768) >> 16) : -(int)((-val + 32768) >> 16); }`

### 2. Eliminating Floating-Point Initialization
- **Coordinate Conversion:** Convert integer pixel coordinates directly: `(int64_t)(pixel - center) << 16`.
- **Heading Vectors:** Replace `DirToVec` with a precomputed 16.16 lookup table for the 8 `Direction` constants.
- **Radius:** Convert `radius` directly to 16.16.
- **Sampling Step:** Use `FP16FromInt(MODULAR_HOLDING_SAMPLE_INTERVAL_PX)` (48 pixels).

### 3. Stable Integer Trigonometry
Replace `std` calls with deterministic integer-based alternatives:
- **`FP16Sin` / `FP16Cos`:** Use a hardcoded **4096-entry** `constexpr` lookup table (`table/fixedpoint_sin_table.h`) for $0..2\pi$ to ensure pixel-level accuracy and cross-platform determinism.
- **`FP16Atan2`:** Polynomial approximation (0.1963*r^3 - 0.9817*r + pi/4, max error ~0.28 degrees).
- **`FP16Sqrt(x)`:** Implement using `IntSqrt64(x << 16)` to return a 16.16 result.

### 4. Implementation Details
- **`FP16NormalizeAngle2Pi`:** Implement as `a %= FP16_2PI; if (a < 0) a += FP16_2PI;` to correctly handle negative results from C++ `%`.
- **Epsilon Checks:** Replace `1e-9` with `FP16_EPSILON = 64`.
- **Naming:** All functions/constants use `FP16` prefix to avoid collision with macOS `FixMath.h` macros (`FixedRound`, `FixedToInt`, etc.).
- **Dubins Candidates:** Refactor `eval_lsl`, `eval_rsr`, etc., to perform all arithmetic in 16.16 relative space.

## Benefit
Provides a perfectly deterministic, portable, and overflow-safe foundation for holding patterns, ensuring that players on all supported compilers and hardware remain in sync. Waypoints will be bit-identical on next recompute after the change lands.
