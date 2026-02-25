# Code Review (Update): Saved Modular Airport Templates

## Overview
Codex has addressed the NewGRF tile resolution and added some normalization for runway flags. However, several critical issues remain, and one regression was introduced regarding station joining.

## Critical Issues

### 1. Command Atomicity (Still Broken)
The `CmdPlaceModularAirportTemplate` still performs map mutations during the first loop if `Execute` is set.
```cpp
/* Pass 1: place all tiles. */
for (size_t i = 0; i < rotated_tiles.size(); i++) {
    CommandCost ret = Command<CMD_BUILD_MODULAR_AIRPORT_TILE>::Do(flags, ...);
    if (ret.Failed()) return ret;
    ...
}
```
If `flags` has `DoCommandFlag::Execute`, the tiles are placed one by one. If the loop fails on tile N, tiles 1 to N-1 remain on the map. This is non-atomic and can lead to broken states or desyncs.

**Fix:** Perform a complete pass with `flags.WithNoExecute()` first. Only if that succeeds, and `flags.Test(DoCommandFlag::Execute)` is true, perform the actual placement pass.

### 2. Station Joining Regression
The logic to update `join_for_tiles` was removed.
If a player builds a non-adjacent template, the first tile creates a station. The second and subsequent tiles *must* join that same station. Without updating `join_for_tiles`, subsequent tiles will either:
- Fail with "Too close to another airport" (because they are near the first tile).
- Create multiple separate stations (if they are far enough apart but within the same template).

**Fix:** Capture the `StationID` from the first tile placement and use it for all subsequent tiles in the same template. In `NoExecute` mode, this is tricky; one approach is to use the `StationID` returned by the first successful `Test` if the command supports returning it, or to rely on the fact that `Execute` will handle it, but then the `Test` pass must be smart enough to know that adjacent tiles *will* join.

### 3. Missing Metadata Costs and Validation
The metadata commands (`CMD_SET_RUNWAY_FLAGS`, etc.) are only called in `Execute` mode and their costs are never added to the total.
```cpp
if (flags.Test(DoCommandFlag::Execute)) {
    /* Pass 2: apply metadata */
    for (...) {
        CommandCost ret = Command<CMD_SET_RUNWAY_FLAGS>::Do(flags, ...);
        // Cost of 'ret' is never added to 'total'!
    }
}
```
- The user is shown an incorrect (too low) cost in the build preview.
- Metadata placement is never "Tested", so failures (e.g., trying to set runway flags on a non-runway piece due to some edge case) only happen during execution.

## Technical Observations

### 4. Code Duplication
`RotateTemplateTile` in `src/station_cmd.cpp` is a verbatim copy of `AirportTemplateTile::Rotate`. These should be unified into a single implementation to avoid maintenance burden.

### 5. Pass 1 vs Pass 2 structure
Pass 2 (metadata) depends on the tiles from Pass 1 already existing on the map. This is why it's currently inside `if (Execute)`. However, for a proper `Test` pass, you should simulate the metadata placement too (with `NoExecute`), but these commands usually check for the existence of the modular piece. This makes testing the full template placement in one go difficult without actual execution. A better pattern in OpenTTD for this is often to have the sub-commands be "internal" or to ensure the `Test` phase of the metadata commands can handle the "will-be-built" state.

## Conclusion
The implementation is still not "OpenTTD-grade" regarding command safety and cost accuracy. The regression in station joining will likely make placing many templates fail in practice.
