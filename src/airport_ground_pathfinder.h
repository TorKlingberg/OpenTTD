/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file airport_ground_pathfinder.h Ground pathfinding for modular airports. */

#ifndef AIRPORT_GROUND_PATHFINDER_H
#define AIRPORT_GROUND_PATHFINDER_H

#include "tile_type.h"
#include <vector>

struct Station;
struct Aircraft;

/** Result of ground pathfinding */
struct AirportGroundPath {
	std::vector<TileIndex> tiles; ///< Tiles in the path (from start to goal)
	int cost;                      ///< Cost of the path
	bool found;                    ///< Whether a path was found

	AirportGroundPath() : cost(0), found(false) {}
};

/**
 * Find a ground path from start to goal within an airport.
 * Uses A* algorithm to find optimal path respecting taxi directions.
 * @param st The station containing the airport.
 * @param start Starting tile.
 * @param goal Goal tile.
 * @param v The aircraft (optional, for reservation checking).
 * @return The path result.
 */
AirportGroundPath FindAirportGroundPath(const Station *st, TileIndex start, TileIndex goal, const Aircraft *v = nullptr);

#endif /* AIRPORT_GROUND_PATHFINDER_H */
