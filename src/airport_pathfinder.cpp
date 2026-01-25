/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file airport_pathfinder.cpp Pathfinding for modular airports. */

#include "stdafx.h"
#include "airport_pathfinder.h"
#include "table/airporttile_ids.h"

#include "safeguards.h"

/* Taxi direction bitmasks: 0x01=N, 0x02=E, 0x04=S, 0x08=W */

/**
 * Lookup table for automatic taxi directions based on piece type and rotation.
 * Each row is one piece type (0-25), each column is one rotation (0-3).
 * Values are bitmasks: bit 0=N, 1=E, 2=S, 3=W
 */
static const uint8_t _piece_taxi_directions[26][4] = {
	/* 0: RUNWAY - allows N+S or E+W depending on rotation */
	{ 0x05, 0x0A, 0x05, 0x0A }, // N+S, E+W, N+S, E+W

	/* 1: RUNWAY_END - runway end piece */
	{ 0x05, 0x0A, 0x05, 0x0A },

	/* 2: RUNWAY_SMALL_NEAR - small runway near end */
	{ 0x05, 0x0A, 0x05, 0x0A },

	/* 3: RUNWAY_SMALL_MID - small runway middle */
	{ 0x05, 0x0A, 0x05, 0x0A },

	/* 4: RUNWAY_SMALL_FAR - small runway far end */
	{ 0x05, 0x0A, 0x05, 0x0A },

	/* 5: TAXIWAY - straight taxiway */
	{ 0x05, 0x0A, 0x05, 0x0A }, // N+S, E+W, N+S, E+W

	/* 6: TAXIWAY_CROSS - intersection */
	{ 0x0F, 0x0F, 0x0F, 0x0F }, // All directions

	/* 7: TERMINAL - terminal building */
	{ 0x0F, 0x0F, 0x0F, 0x0F }, // All directions (aircraft can approach from any side)

	/* 8: TERMINAL_ROUND - round terminal */
	{ 0x0F, 0x0F, 0x0F, 0x0F }, // All directions

	/* 9: HANGAR - large hangar */
	{ 0x0F, 0x0F, 0x0F, 0x0F }, // All directions

	/* 10: SMALL_HANGAR - small hangar */
	{ 0x0F, 0x0F, 0x0F, 0x0F }, // All directions

	/* 11: HELIPAD - helicopter pad */
	{ 0x0F, 0x0F, 0x0F, 0x0F }, // All directions

	/* 12: STAND - aircraft stand */
	{ 0x0F, 0x0F, 0x0F, 0x0F }, // All directions

	/* 13: STAND_1 - aircraft stand variant */
	{ 0x0F, 0x0F, 0x0F, 0x0F }, // All directions

	/* 14: STAND_PIER - pier stand */
	{ 0x0F, 0x0F, 0x0F, 0x0F }, // All directions

	/* 15: APRON - apron area */
	{ 0x0F, 0x0F, 0x0F, 0x0F }, // All directions

	/* 16: APRON_EDGE - apron edge */
	{ 0x0F, 0x0F, 0x0F, 0x0F }, // All directions

	/* 17: APRON_HALF_E - half apron east */
	{ 0x0F, 0x0F, 0x0F, 0x0F }, // All directions

	/* 18: APRON_HALF_W - half apron west */
	{ 0x0F, 0x0F, 0x0F, 0x0F }, // All directions

	/* 19: TOWER - control tower */
	{ 0x00, 0x00, 0x00, 0x00 }, // No taxi allowed (decoration only)

	/* 20: RADAR - radar installation */
	{ 0x00, 0x00, 0x00, 0x00 }, // No taxi allowed (decoration only)

	/* 21: RADIO_TOWER - radio tower */
	{ 0x00, 0x00, 0x00, 0x00 }, // No taxi allowed (decoration only)

	/* 22: GRASS - grass area */
	{ 0x00, 0x00, 0x00, 0x00 }, // No taxi allowed (decoration only)

	/* 23: EMPTY - empty tile */
	{ 0x00, 0x00, 0x00, 0x00 }, // No taxi allowed

	/* 24: FENCE - fence */
	{ 0x00, 0x00, 0x00, 0x00 }, // No taxi allowed (decoration only)

	/* 25: ERASE - eraser tool (should never be stored) */
	{ 0x00, 0x00, 0x00, 0x00 }, // No taxi allowed
};

/**
 * Calculate automatic taxi directions based on piece type and rotation.
 * @param piece_type The airport piece type (0-25).
 * @param rotation The rotation of the piece (0-3).
 * @return Bitmask of allowed taxi directions (bit 0=N, 1=E, 2=S, 3=W).
 */
uint8_t CalculateAutoTaxiDirectionsForPiece(uint8_t piece_type, uint8_t rotation)
{
	if (piece_type >= 26) return 0;
	if (rotation > 3) rotation = 0;
	return _piece_taxi_directions[piece_type][rotation];
}

uint8_t CalculateAutoTaxiDirectionsForGfx(uint8_t gfx, uint8_t rotation)
{
	if (rotation > 3) rotation = 0;

	switch (gfx) {
		case APT_RUNWAY_1:
		case APT_RUNWAY_2:
		case APT_RUNWAY_3:
		case APT_RUNWAY_4:
		case APT_RUNWAY_5:
		case APT_RUNWAY_END:
		case APT_RUNWAY_SMALL_NEAR_END:
		case APT_RUNWAY_SMALL_MIDDLE:
		case APT_RUNWAY_SMALL_FAR_END:
		case APT_APRON_HOR:
		case APT_APRON_VER_CROSSING_N:
			return (rotation % 2 == 0) ? 0x05 : 0x0A;
		case APT_APRON_HOR_CROSSING_E:
		case APT_APRON_VER_CROSSING_S:
			return 0x0F;
		case APT_BUILDING_1:
		case APT_ROUND_TERMINAL:
		case APT_STAND:
		case APT_STAND_1:
		case APT_STAND_PIER_NE:
		case APT_DEPOT_SE:
		case APT_SMALL_DEPOT_SE:
		case APT_HELIPAD_1:
		case APT_HELIPAD_2:
		case APT_HELIPAD_2_FENCE_NW:
		case APT_HELIPAD_2_FENCE_NE_SE:
		case APT_HELIPAD_3_FENCE_SE_SW:
		case APT_HELIPAD_3_FENCE_NW_SW:
		case APT_HELIPAD_3_FENCE_NW:
		case APT_APRON:
		case APT_APRON_E:
		case APT_APRON_S:
		case APT_APRON_W:
		case APT_ARPON_N:
		case APT_APRON_HALF_EAST:
		case APT_APRON_HALF_WEST:
			return 0x0F;
		default:
			return 0x00;
	}
}
