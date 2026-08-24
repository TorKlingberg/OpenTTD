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
#include <span>
#include <vector>

struct Station;
struct Aircraft;

/** Result of ground pathfinding */
struct AirportGroundPath {
	std::vector<TileIndex> tiles{}; ///< Tiles in the path (from start to goal)
	int cost = 0;                   ///< Cost of the path
	bool found = false;             ///< Whether a path was found
};

/**
 * Classification of a taxi path tile's segment type.
 *
 * This describes routing and safe-stop behaviour. It does *not* select a per-class
 * reservation algorithm: reservation scope comes from the aircraft's operation and the
 * single forward horizon built by BuildForwardReservationPlan, not from the segment the
 * aircraft happens to be standing in.
 */
enum class TaxiSegmentType : uint8_t {
	FreeMove, ///< Bidirectional taxiways, aprons, stands, hangars - traveled tiles claimed through the forward horizon
	OneWay,   ///< One-way taxiways - safe to queue on, and a boundary of the forward horizon
	Runway,   ///< Runway tiles - whole runway claimed via TryReserveContiguousModularRunway for an explicit landing/takeoff, traveled tiles only when crossing
};

/** A contiguous segment of same-type tiles within a taxi path. */
struct TaxiSegment {
	TaxiSegmentType type;
	uint16_t start_index;  ///< Index into TaxiPath::tiles (first tile of this segment)
	uint16_t end_index;    ///< Index into TaxiPath::tiles (last tile of this segment, inclusive)
};

/**
 * Aircraft-type routing restriction, kept separate from the @c v parameter.
 *
 * @c v is deliberately nullptr at the reachability probes ("can this aircraft get there
 * at all?"), because those must ignore which stands happen to be occupied right now.
 * They still route on behalf of a concrete aircraft, so the type restriction cannot ride
 * along on @c v -- it travels on its own and defaults to being derived from @c v.
 */
enum class GroundPathRestriction : uint8_t {
	FromAircraft, ///< Derive from the @c v parameter; no restriction when @c v is nullptr.
	None,         ///< No aircraft-type restriction: pure topology.
	FixedWing,    ///< Fixed-wing aircraft: helipads are not taxiable.
};

/**
 * Restriction implied by an aircraft.
 * @param v The aircraft, or nullptr.
 * @return FixedWing for a fixed-wing aircraft, None otherwise.
 */
GroundPathRestriction GetGroundPathRestriction(const Aircraft *v);

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
 * @param restriction Aircraft-type restriction; pass one explicitly when @p v is nullptr.
 * @param avoid_tiles Tiles the route may not pass through, used to generate an alternative
 *                    to a route that could not be reserved. Hard exclusion, not a penalty:
 *                    each run stays a plain topology question, which is what the stored
 *                    paths assume. The goal is *not* exempt -- banning it is how a caller
 *                    says "this goal instance is unusable, try another". A non-empty set
 *                    suppresses the crossing cache entirely (see the .cpp).
 * @return The path result.
 */
AirportGroundPath FindAirportGroundPath(const Station *st, TileIndex start, TileIndex goal, const Aircraft *v = nullptr, bool allow_runway_goal_crossing = false, bool update_cache = true, GroundPathRestriction restriction = GroundPathRestriction::FromAircraft, std::span<const TileIndex> avoid_tiles = {});

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
 * @param restriction Aircraft-type restriction; pass one explicitly when @p v is nullptr.
 * @param avoid_tiles Tiles the route may not pass through; see FindAirportGroundPath.
 * @return A TaxiPath with tiles and segments filled in.
 */
TaxiPath BuildTaxiPath(const Station *st, TileIndex start, TileIndex goal, const Aircraft *v = nullptr, bool allow_runway_goal_crossing = false, GroundPathRestriction restriction = GroundPathRestriction::FromAircraft, std::span<const TileIndex> avoid_tiles = {});

extern std::vector<uint64_t> _modular_airport_crossing_required_path_cache;
void NormalizeModularAirportCrossingPathCache();
void ClearModularAirportCrossingPathCache();

#endif /* AIRPORT_GROUND_PATHFINDER_H */
