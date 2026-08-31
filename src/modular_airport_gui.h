/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file modular_airport_gui.h Declarations for modular airport build UI. */

#ifndef MODULAR_AIRPORT_GUI_H
#define MODULAR_AIRPORT_GUI_H

#include "station_type.h"
#include "viewport_type.h"
#include "gfx_type.h"
#include "table/airporttile_ids.h"

void ShowBuildModularAirportWindow();

/**
 * Every airport graphic the modular builder can place, sorted and deduplicated.
 *
 * This is the definition of what a modular airport may be built from. Anything
 * that places modular tiles without going through the builder -- the script API
 * above all -- must place only graphics from this set.
 *
 * A compound piece appears here once, under the graphic that names it; see
 * GetModularCompoundPieceTiles for what it actually puts on the ground.
 */
std::vector<ModularAirportPieceID> GetModularAirportBuilderPieceGfx();

/** One tile of a compound piece, relative to the tile the player clicked. */
struct ModularCompoundPieceTile {
	int dx;        ///< Offset along X from the anchor tile.
	int dy;        ///< Offset along Y from the anchor tile.
	uint8_t gfx;   ///< The graphic this tile gets.
};

/**
 * The tiles a compound piece places, or an empty span for an ordinary piece.
 *
 * Some airport buildings are drawn across several tiles and only make sense
 * whole, so the builder places them as a unit from one click. The footprint is
 * fixed and unrotatable: each tile has its own graphic, drawn to join up with
 * its neighbours in one orientation only.
 * @param gfx The graphic naming the piece (the one in GetModularAirportBuilderPieceGfx).
 */
std::span<const ModularCompoundPieceTile> GetModularCompoundPieceTiles(ModularAirportPieceID gfx);

/** Footprint of a piece in tiles: the compound's bounding box, or 1x1. */
Dimension GetModularCompoundPieceSize(ModularAirportPieceID gfx);

extern StationID _last_modular_airport_station;
extern bool _show_runway_direction_overlay;
extern bool _show_holding_overlay;
extern bool _show_taxi_reservation_overlay;

void DrawModularHoldingOverlay(const Viewport &vp, DrawPixelInfo *dpi);
void DrawModularTaxiReservationOverlay(const Viewport &vp, DrawPixelInfo *dpi);

#endif /* MODULAR_AIRPORT_GUI_H */
