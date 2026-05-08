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
#include <cstdint>
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

/** Classification of a taxi path tile's segment type. */
enum class TaxiSegmentType : uint8_t {
	FREE_MOVE,  ///< Bidirectional taxiways, aprons, stands, hangars - requires atomic reservation
	ONE_WAY,    ///< One-way taxiways - safe for queuing (tile-by-tile reservation)
	RUNWAY,     ///< Runway tiles - atomic reservation via TryReserveContiguousModularRunway
};

/** A contiguous segment of same-type tiles within a taxi path. */
struct TaxiSegment {
	TaxiSegmentType type;
	uint16_t start_index;  ///< Index into TaxiPath::tiles (first tile of this segment)
	uint16_t end_index;    ///< Index into TaxiPath::tiles (last tile of this segment, inclusive)
};

/** A classified taxi path: the raw A* tile list plus segment decomposition. */
struct TaxiPath {
	std::vector<TileIndex> tiles;      ///< Full A* path (start to goal)
	std::vector<TaxiSegment> segments; ///< Classified segments over the tile list
	bool valid = false;                ///< Whether this path is usable
};

/**
 * Find a ground path from start to goal within an airport.
 * Uses A* algorithm to find optimal path respecting taxi directions.
 * @param st The station containing the airport.
 * @param start Starting tile.
 * @param goal Goal tile.
 * @param v The aircraft (optional, for stand avoidance).
 * @param allow_runway_goal_crossing Allow crossing-fallback paths when the goal is a runway.
 * @param update_cache When false, the crossing-required cache is read but never written.
 *                     Diagnostic/debug probes must pass false so that unsaved rate-limit
 *                     gating cannot diverge the saved cache across multiplayer clients.
 * @return The path result.
 */
AirportGroundPath FindAirportGroundPath(const Station *st, TileIndex start, TileIndex goal, const Aircraft *v = nullptr, bool allow_runway_goal_crossing = false, bool update_cache = true);

/**
 * Check if a tile is a one-way taxiway tile.
 * @param st The station.
 * @param tile The tile to check.
 * @return True if the tile is a taxiway piece with one_way_taxi set.
 */
bool IsOneWayTaxiTile(const Station *st, TileIndex tile);

/**
 * Build a classified taxi path from start to goal.
 * Calls A* pathfinder then classifies tiles into segments.
 * @param st The station containing the airport.
 * @param start Starting tile.
 * @param goal Goal tile.
 * @param v The aircraft (optional, for stand avoidance).
 * @return A TaxiPath with tiles and segments filled in.
 */
TaxiPath BuildTaxiPath(const Station *st, TileIndex start, TileIndex goal, const Aircraft *v = nullptr, bool allow_runway_goal_crossing = false);

extern std::vector<uint64_t> _modular_airport_crossing_required_path_cache;
void NormalizeModularAirportCrossingPathCache();
void ClearModularAirportCrossingPathCache();

#endif /* AIRPORT_GROUND_PATHFINDER_H */
