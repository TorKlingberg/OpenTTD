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

struct Town;

void NormalizeRunwaySegmentVisuals(Station *st, TileIndex changed_tile, bool horizontal);
uint8_t GetStockFenceEdgeMask(uint8_t stock_gfx);
uint8_t MapStockGfxToModularPiece(uint8_t stock_gfx);
uint8_t ApplyStockTileOverride(uint8_t airport_type, int dx, int dy, uint8_t piece_type);
Money GetModularAirportPieceBuildCost(uint8_t piece_type);
CommandCost BuildModularAirportTile_Check(DoCommandFlags flags, TileIndex tile, uint16_t gfx, StationID station_to_join, bool allow_adjacent, Station *&st, Town *&nearest, uint &newnoise_level, bool &new_facility, bool &is_modular_replace, CommandCost &cost);
void BuildModularAirportTile_Apply(TileIndex tile, uint16_t gfx, Station *st, Town *nearest, uint newnoise_level, bool new_facility, bool is_modular_replace, uint8_t rotation, uint8_t taxi_dir_mask, bool one_way_taxi, bool auto_rotate_runway);
CommandCost RemoveModularAirportTile(TileIndex tile, DoCommandFlags flags);

#endif /* MODULAR_AIRPORT_BUILD_H */
