/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file modular_airport_build.h Helpers for modular airport build/remove commands. */

#ifndef MODULAR_AIRPORT_BUILD_H
#define MODULAR_AIRPORT_BUILD_H

#include "economy_type.h"
#include "command_type.h"
#include "station_base.h"

void NormalizeRunwaySegmentVisuals(Station *st, TileIndex changed_tile, bool horizontal);
uint8_t GetStockFenceEdgeMask(uint8_t stock_gfx);
uint8_t MapStockGfxToModularPiece(uint8_t stock_gfx);
uint8_t ApplyStockTileOverride(uint8_t airport_type, int dx, int dy, uint8_t piece_type);
Money GetModularAirportPieceBuildCost(uint8_t piece_type);
CommandCost RemoveModularAirportTile(TileIndex tile, DoCommandFlags flags);

#endif /* MODULAR_AIRPORT_BUILD_H */
