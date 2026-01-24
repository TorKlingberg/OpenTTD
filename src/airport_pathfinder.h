/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file airport_pathfinder.h Pathfinding for modular airports. */

#ifndef AIRPORT_PATHFINDER_H
#define AIRPORT_PATHFINDER_H

#include "tile_type.h"

/**
 * Calculate automatic taxi directions based on piece type and rotation.
 * @param piece_type The airport piece type (0-25).
 * @param rotation The rotation of the piece (0-3).
 * @return Bitmask of allowed taxi directions (bit 0=N, 1=E, 2=S, 3=W).
 */
uint8_t CalculateAutoTaxiDirections(uint8_t piece_type, uint8_t rotation);

/**
 * Get effective taxi directions combining auto-calculated and user overrides.
 * @param auto_mask Auto-calculated directions.
 * @param user_mask User-specified directions.
 * @return Effective directions mask.
 */
inline uint8_t GetEffectiveTaxiDirections(uint8_t auto_mask, uint8_t user_mask)
{
	/* If user has specified any directions, use those; otherwise use auto */
	return user_mask != 0 ? user_mask : auto_mask;
}

#endif /* AIRPORT_PATHFINDER_H */
