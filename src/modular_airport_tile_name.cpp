/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file modular_airport_tile_name.cpp Names for modular airport tile pieces. */

#include "stdafx.h"
#include "modular_airport_tile_name.h"

#include "table/airporttile_ids.h"
#include "table/strings.h"

#include "safeguards.h"

/**
 * Get the player-facing name of a modular airport tile piece.
 *
 * A converted stock airport can contain visual variants which the modular
 * builder does not place directly. Name those as the closest builder concept,
 * while retaining distinctions that are useful when inspecting the layout.
 * @param piece_type AirportTiles value stored in ModularAirportTileData.
 * @return Name of the piece, or STR_NULL when the value is not recognised.
 */
StringID GetModularAirportTileName(uint8_t piece_type)
{
	switch (piece_type) {
		case APT_RUNWAY_1:
		case APT_RUNWAY_2:
		case APT_RUNWAY_3:
		case APT_RUNWAY_4:
		case APT_RUNWAY_5:
		case APT_RUNWAY_FENCE_NW:
			return STR_STATION_BUILD_MODULAR_AIRPORT_PIECE_RUNWAY;

		case APT_RUNWAY_END:
		case APT_RUNWAY_END_FENCE_SE:
		case APT_RUNWAY_END_FENCE_NW:
		case APT_RUNWAY_END_FENCE_NW_SW:
		case APT_RUNWAY_END_FENCE_SE_SW:
		case APT_RUNWAY_END_FENCE_NE_NW:
		case APT_RUNWAY_END_FENCE_NE_SE:
			return STR_STATION_BUILD_MODULAR_AIRPORT_PIECE_RUNWAY_END;

		case APT_RUNWAY_SMALL_NEAR_END:
			return STR_STATION_BUILD_MODULAR_AIRPORT_PIECE_RUNWAY_SMALL_NEAR;

		case APT_RUNWAY_SMALL_MIDDLE:
			return STR_STATION_BUILD_MODULAR_AIRPORT_PIECE_RUNWAY_SMALL_MID;

		case APT_RUNWAY_SMALL_FAR_END:
			return STR_STATION_BUILD_MODULAR_AIRPORT_PIECE_RUNWAY_SMALL_FAR;

		case APT_APRON:
		case APT_APRON_FENCE_NW:
		case APT_APRON_FENCE_SW:
		case APT_APRON_FENCE_NE:
		case APT_APRON_FENCE_NE_SW:
		case APT_APRON_FENCE_SE_SW:
		case APT_APRON_FENCE_SE:
		case APT_APRON_FENCE_NE_SE:
		case APT_PIER_NW_NE:
		case APT_PIER:
			return STR_STATION_BUILD_MODULAR_AIRPORT_PIECE_APRON;

		case APT_APRON_HOR:
		case APT_APRON_VER_CROSSING_N:
			return STR_STATION_BUILD_MODULAR_AIRPORT_PIECE_TAXIWAY;

		case APT_APRON_VER_CROSSING_S:
		case APT_APRON_HOR_CROSSING_W:
		case APT_APRON_HOR_CROSSING_E:
			return STR_STATION_BUILD_MODULAR_AIRPORT_PIECE_TAXIWAY_CROSS;

		case APT_APRON_W:
		case APT_APRON_S:
		case APT_APRON_E:
		case APT_APRON_N:
		case APT_APRON_N_FENCE_SW:
			return STR_LAI_STATION_DESCRIPTION_AIRPORT_APRON_EDGE;

		case APT_APRON_HALF_EAST:
			return STR_STATION_BUILD_MODULAR_AIRPORT_PIECE_APRON_HALF_E;

		case APT_APRON_HALF_WEST:
			return STR_STATION_BUILD_MODULAR_AIRPORT_PIECE_APRON_HALF_W;

		case APT_STAND:
			return STR_STATION_BUILD_MODULAR_AIRPORT_PIECE_STAND;

		case APT_STAND_1:
			return STR_STATION_BUILD_MODULAR_AIRPORT_PIECE_STAND_1;

		case APT_STAND_PIER_NE:
			return STR_STATION_BUILD_MODULAR_AIRPORT_PIECE_STAND_PIER;

		case APT_DEPOT_SE:
		case APT_DEPOT_SW:
		case APT_DEPOT_NW:
		case APT_DEPOT_NE:
			return STR_STATION_BUILD_MODULAR_AIRPORT_PIECE_HANGAR;

		case APT_SMALL_DEPOT_SE:
		case APT_SMALL_DEPOT_SW:
		case APT_SMALL_DEPOT_NW:
		case APT_SMALL_DEPOT_NE:
			return STR_STATION_BUILD_MODULAR_AIRPORT_PIECE_SMALL_HANGAR;

		case APT_HELIPAD_1:
		case APT_HELIPAD_2_FENCE_NW:
		case APT_HELIPAD_2:
		case APT_HELIPAD_2_FENCE_NE_SE:
			return STR_STATION_BUILD_MODULAR_AIRPORT_PIECE_HELIPAD;

		case APT_HELIPAD_3_FENCE_SE_SW:
		case APT_HELIPAD_3_FENCE_NW_SW:
		case APT_HELIPAD_3_FENCE_NW:
			return STR_STATION_BUILD_MODULAR_AIRPORT_PIECE_PLAIN_H;

		case APT_HELIPORT:
			return STR_AIRPORT_HELIPORT;

		case APT_BUILDING_1:
			return STR_STATION_BUILD_MODULAR_AIRPORT_PIECE_TERMINAL;

		case APT_BUILDING_2:
			return STR_STATION_BUILD_MODULAR_AIRPORT_PIECE_TERMINAL_ALT;

		case APT_BUILDING_3:
			return STR_STATION_BUILD_MODULAR_AIRPORT_PIECE_TERMINAL_OTHER;

		case APT_ROUND_TERMINAL:
			return STR_STATION_BUILD_MODULAR_AIRPORT_PIECE_TERMINAL_ROUND;

		case APT_LOW_BUILDING_FENCE_N:
		case APT_LOW_BUILDING_FENCE_NW:
		case APT_LOW_BUILDING:
			return STR_STATION_BUILD_MODULAR_AIRPORT_PIECE_LOW_TERMINAL;

		case APT_SMALL_BUILDING_1:
		case APT_SMALL_BUILDING_2:
		case APT_SMALL_BUILDING_3:
			return STR_STATION_BUILD_MODULAR_AIRPORT_PIECE_SMALL_TERMINAL_3;

		case APT_TOWER_FENCE_SW:
		case APT_TOWER:
			return STR_STATION_BUILD_MODULAR_AIRPORT_PIECE_TOWER;

		case APT_RADIO_TOWER_FENCE_NE:
			return STR_STATION_BUILD_MODULAR_AIRPORT_PIECE_RADIO_TOWER;

		case APT_RADAR_FENCE_SW:
		case APT_RADAR_FENCE_NE:
			return STR_STATION_BUILD_MODULAR_AIRPORT_PIECE_RADAR;

		case APT_RADAR_GRASS_FENCE_SW:
			return STR_STATION_BUILD_MODULAR_AIRPORT_PIECE_RADAR_GRASS;

		case APT_GRASS_FENCE_NE_FLAG_2:
			return STR_STATION_BUILD_MODULAR_AIRPORT_PIECE_FLAG_GRASS;

		case APT_GRASS_FENCE_SW:
		case APT_GRASS_2:
		case APT_GRASS_1:
		case APT_GRASS_FENCE_NE_FLAG:
			return STR_STATION_BUILD_MODULAR_AIRPORT_PIECE_GRASS;

		case APT_EMPTY:
		case APT_EMPTY_FENCE_NE:
			return STR_STATION_BUILD_MODULAR_AIRPORT_PIECE_EMPTY;

		default:
			return STR_NULL;
	}
}
