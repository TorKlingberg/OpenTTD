/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file modular_airport_draw.h Drawing helpers for modular airport tiles. */

#ifndef MODULAR_AIRPORT_DRAW_H
#define MODULAR_AIRPORT_DRAW_H

#include "sprite.h"
#include "station_map.h"
#include "viewport_type.h"

void DrawModularAirportPerimeterFences(const TileInfo *ti, PaletteID palette);
void DrawModularAirportDirectionOverlays(const TileInfo *ti);
const DrawTileSprites *GetAirportTileLayoutWithModularOverrides(uint8_t gfx, uint8_t modular_piece_type, uint8_t modular_rotation, uint8_t animation_frame = 0);
const DrawTileSprites *GetModularHangarTileLayoutByPiece(uint8_t piece_type, uint8_t rotation);
const DrawTileSprites *GetModularHangarTileLayout(uint8_t rotation, bool small_hangar);
const DrawTileSprites *GetModularNSRunwayLayout(uint8_t piece_type);
void ApplyModularAirportTileLayoutOverrides(const TileInfo *ti, StationGfx &gfx, const DrawTileSprites *&t);

#endif /* MODULAR_AIRPORT_DRAW_H */
