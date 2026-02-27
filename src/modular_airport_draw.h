/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file modular_airport_draw.h Drawing helpers for modular airport tiles. */

#ifndef MODULAR_AIRPORT_DRAW_H
#define MODULAR_AIRPORT_DRAW_H

#include "station_map.h"
#include "viewport_type.h"

void DrawModularAirportPerimeterFences(const TileInfo *ti, PaletteID palette);
void DrawModularAirportDirectionOverlays(const TileInfo *ti);

#endif /* MODULAR_AIRPORT_DRAW_H */
