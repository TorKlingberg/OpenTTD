  While the logic is sound for single-player, the use of double in simulation-critical code poses a risk of desyncs in multiplayer:
   - Sampling Count: The use of std::floor(len / step_px) on floating-point lengths is dangerous. If two clients calculate the length with even a tiny discrepancy (e.g., due to different CPU architectures or compiler optimizations), they might end up with a different number of waypoints in the waypoints vector. This will cause an immediate desync when
     GetModularHoldingWaypointIndex uses n_wp in its modulo operation.
     - Recommendation: Add a small epsilon (e.g., 1e-7) to the length before floor-calculating the count.
   - Sorting Stability: Sorting gates by atan2 results (sort_angle) is similarly risky if two gates are nearly at the same angle relative to the airport center.
     - Recommendation: Use a tie-breaker (like runway_tile.base()) as already implemented, but consider if sort_angle itself could be replaced by a deterministic integer-based orientation test (e.g., using cross-products) to avoid transcendental functions entirely.

