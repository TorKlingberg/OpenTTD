# Modular Airport Editor

Practical notes for the modular airport builder and stock-airport conversion UI.

## Stock-to-modular conversion

Stock "Build as modular" conversion is centralized in `MapStockGfxToModularPiece()` in `src/modular_airport_build.cpp`. If a stock airport tile should keep a distinct modular visual or behavior, preserve or canonicalize it explicitly there.

Helipad stock graphics need separate handling:

- `APT_HELIPORT` should remain `APT_HELIPORT`.
- Classic helipad variants should canonicalize to `APT_HELIPAD_2`.
- Helistation H-pad variants should canonicalize to `APT_HELIPAD_3_FENCE_NW`.

Do not encode stock fence variants as separate modular piece types unless the modular system needs that exact piece identity. Fence blocking is carried separately by `GetStockFenceEdgeMask()`.

## Picker strings

Avoid reusing broad airport class or airport-name strings for modular picker UI when the wording only applies to that picker. Add dedicated modular strings instead of changing shared strings like `STR_AIRPORT_CLASS_HELIPORTS` or `STR_AIRPORT_HELISTATION`, because those are also used in airport lists and airport classes.
