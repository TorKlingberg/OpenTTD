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

## Piece availability gating

The pieces drawn from this fork's own stored bitmaps -- the decorations (`APT_MODULAR_*`) and the small hangar's two closed-back views (visual rotations 1 and 2) -- are gated by the `station.new_airport_graphics` setting. `IsNewAirportGraphicsPiece(piece, rotation)` says which piece/rotation pairs it covers and `AreNewAirportGraphicsAvailable()` is the setting. **Runtime mirrors of base-set sprites are deliberately not gated**: the mirrored small terminal and the quarter-turned legacy small runway follow whichever base set is selected, so they stay available with the setting off, and a small-runway template rotates freely on either axis. Both gates on *building* a piece -- that setting and the year gate -- live in `GetModularPieceUnavailableReason(piece, rotation)`, which answers with the year first; `BuildModularAirportTile_Check` is its only command-side caller (`CmdUpgradeModularAirportTile` keeps its own year check, because it skips an ungated tile rather than failing), and the builder's greyed-out buttons (`IsModularPieceLocked`) and the script API (`ScriptAirport::IsModularPieceAvailable`) go through the same helper so they cannot disagree. Rotation is part of the question, so never call it without one. The settings window greys the setting out while `station.modular_airports` is off, but that dependency lives only in `SettingDesc::IsEditable` -- no command consults `modular_airports`. `_settings_game` is all-false in the unit tests, so a test that builds a decoration must set the flag itself.

The exception is `CmdUpgradeModularAirportTile`, which re-derives a year check inline (it skips
an ungated tile rather than failing the command) and so does **not** consult
`station.new_airport_graphics`. Everywhere else: ask `GetModularPieceUnavailableReason(piece,
rotation)`, always with a rotation, and never re-derive the answer locally.
