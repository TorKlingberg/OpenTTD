/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file airport_ground_pathfinder.cpp Ground pathfinding for modular airports. */

#include "stdafx.h"
#include "debug.h"
#include "airport_ground_pathfinder.h"
#include "airport_pathfinder.h"
#include "modular_airport_cmd.h"
#include "station_base.h"
#include "aircraft.h"
#include "station_map.h"
#include "tile_map.h"
#include "table/airporttile_ids.h"
#include <queue>
#include <unordered_map>
#include <unordered_set>
#include <algorithm>

#include "safeguards.h"

/** Maximum number of iterations for pathfinding (prevent infinite loops) */
static const int MAX_PATHFINDER_ITERATIONS = 1000;
static const size_t MAX_CROSSING_CACHE_SIZE = 4096;
static bool IsModularRunwayPieceLocal(uint8_t gfx);
static bool IsSameContiguousRunway(const Station *st, TileIndex a, TileIndex b);
static std::unordered_set<uint64_t> _crossing_required_path_cache;

static uint64_t BuildCrossingCacheKey(TileIndex start, TileIndex goal)
{
	return (static_cast<uint64_t>(start.base()) << 32) | static_cast<uint64_t>(goal.base());
}

/** Node in the A* search */
struct PathNode {
	TileIndex tile;      ///< Tile position
	int g_cost;          ///< Cost from start to this node
	int f_cost;          ///< Estimated total cost (g_cost + heuristic)
	TileIndex parent;    ///< Parent tile in the path

	PathNode(TileIndex t, int g, int f, TileIndex p) : tile(t), g_cost(g), f_cost(f), parent(p) {}

	/** Comparison for priority queue (lower f_cost = higher priority) */
	bool operator>(const PathNode &other) const { return f_cost > other.f_cost; }
};

/**
 * Calculate Manhattan distance heuristic.
 * @param from Starting tile.
 * @param to Goal tile.
 * @return Estimated distance.
 */
static int CalculateHeuristic(TileIndex from, TileIndex to)
{
	int dx = abs(TileX(from) - TileX(to));
	int dy = abs(TileY(from) - TileY(to));
	return dx + dy;
}

/**
 * Check if a piece type is a non-taxiable building (aircraft cannot taxi through it).
 * @param piece_type The airport piece type.
 * @return True if it's a building that blocks taxiing.
 */
static bool IsNonTaxiableBuilding(uint8_t piece_type)
{
	switch (piece_type) {
		case APT_BUILDING_1:
		case APT_BUILDING_2:
		case APT_BUILDING_3:
		case APT_ROUND_TERMINAL:
			return true;
		default:
			return false;
	}
}


/**
 * Check if a piece type is an aircraft parking tile that should not be used as pass-through route.
 * @param piece_type The airport piece type.
 * @return True if this tile is parking-only (stand variants).
 */
static bool IsParkingOnlyTile(uint8_t piece_type)
{
	if (IsModularHelipadPiece(piece_type)) return true;
	switch (piece_type) {
		case APT_STAND:
		case APT_STAND_1:
		case APT_STAND_PIER_NE:
			return true;
		default:
			return false;
	}
}

/**
 * Check if two runway tiles belong to the same contiguous runway strip.
 */
static bool IsSameContiguousRunway(const Station *st, TileIndex a, TileIndex b)
{
	const ModularAirportTileData *a_data = st->airport.GetModularTileData(a);
	const ModularAirportTileData *b_data = st->airport.GetModularTileData(b);
	if (a_data == nullptr || b_data == nullptr) return false;
	if (!IsModularRunwayPieceLocal(a_data->piece_type) || !IsModularRunwayPieceLocal(b_data->piece_type)) return false;

	const bool horizontal_a = (a_data->rotation % 2) == 0;
	const bool horizontal_b = (b_data->rotation % 2) == 0;
	if (horizontal_a != horizontal_b) return false;

	if (horizontal_a) {
		if (TileY(a) != TileY(b)) return false;
		const int y = TileY(a);
		int x0 = std::min(TileX(a), TileX(b));
		int x1 = std::max(TileX(a), TileX(b));
		for (int x = x0; x <= x1; ++x) {
			TileIndex t = TileXY(x, y);
			const ModularAirportTileData *td = st->airport.GetModularTileData(t);
			if (td == nullptr || !IsModularRunwayPieceLocal(td->piece_type) || (td->rotation % 2) != 0) return false;
		}
		return true;
	}

	if (TileX(a) != TileX(b)) return false;
	const int x = TileX(a);
	int y0 = std::min(TileY(a), TileY(b));
	int y1 = std::max(TileY(a), TileY(b));
	for (int y = y0; y <= y1; ++y) {
		TileIndex t = TileXY(x, y);
		const ModularAirportTileData *td = st->airport.GetModularTileData(t);
		if (td == nullptr || !IsModularRunwayPieceLocal(td->piece_type) || (td->rotation % 2) == 0) return false;
	}
	return true;
}

/**
 * Check if two tiles can be connected based on taxi directions.
 * @param st The station.
 * @param from Source tile.
 * @param to Destination tile.
 * @return True if connection is allowed.
 */
static bool CanTilesConnect(const Station *st, TileIndex from, TileIndex to, const Aircraft *v, TileIndex goal = INVALID_TILE, bool allow_runway_crossing = false)
{
	/* Must be orthogonally adjacent */
	int dx = TileX(to) - TileX(from);
	int dy = TileY(to) - TileY(from);
	if (abs(dx) + abs(dy) != 1) return false;

	/* Determine direction from 'from' to 'to' */
	uint8_t dir_bit = 0;
	if (dy == -1) dir_bit = 0x01; // North
	if (dx == +1) dir_bit = 0x02; // East
	if (dy == +1) dir_bit = 0x04; // South
	if (dx == -1) dir_bit = 0x08; // West

	/* Get tile data for 'from' */
	const ModularAirportTileData *from_data = st->airport.GetModularTileData(from);
	if (from_data == nullptr) return false;

	/* Get effective taxi directions */
	uint8_t from_auto = CalculateAutoTaxiDirectionsForGfx(from_data->piece_type, from_data->rotation);
	uint8_t from_dirs = from_auto;
	if (IsTaxiwayPiece(from_data->piece_type) && from_data->one_way_taxi) {
		from_dirs = GetEffectiveTaxiDirections(from_auto, from_data->user_taxi_dir_mask);
	}
	
	bool from_ok = (from_dirs & dir_bit) != 0;

	/* Get tile data for 'to' */
	const ModularAirportTileData *to_data = st->airport.GetModularTileData(to);
	if (to_data == nullptr) return false;
	const bool from_is_runway = IsModularRunwayPieceLocal(from_data->piece_type);
	const bool to_is_runway = IsModularRunwayPieceLocal(to_data->piece_type);

	/* Runway tiles may only connect along the same runway axis.
	 * In crossing fallback mode, allow perpendicular hops between adjacent
	 * parallel runways so layouts like Metropolitan can cross two runways. */
	if (from_is_runway && to_is_runway) {
		const bool from_horizontal = (from_data->rotation % 2) == 0;
		const bool to_horizontal = (to_data->rotation % 2) == 0;
		if (from_horizontal != to_horizontal) return false;
		if ((from_horizontal && dy != 0) || (!from_horizontal && dx != 0)) {
			if (!allow_runway_crossing) return false;
		}
	}

	/* Prefer to avoid entering runways from apron/taxiway tiles unless the runway
	 * tile is the explicit pathfinder goal (e.g. routing to a takeoff runway end).
	 * A fallback pass may enable constrained crossing when no strict route exists. */
	if (!from_is_runway && to_is_runway && to != goal) {
		/* If routing to a runway goal, allow stepping onto any tile of that same
		 * contiguous runway strip so aircraft can back-taxi to the correct end. */
		if (goal != INVALID_TILE && IsSameContiguousRunway(st, to, goal)) {
			/* allowed */
		} else {
			if (!allow_runway_crossing) return false;

			/* Crossing fallback: only allow perpendicular entry so aircraft do not
			 * route along active runways as a shortcut under heavy traffic. */
			const bool to_horizontal = (to_data->rotation % 2) == 0;
			const bool entering_along_runway_axis = (to_horizontal && dy == 0) || (!to_horizontal && dx == 0);
			if (entering_along_runway_axis) return false;
		}
	}

	/* Don't allow taxiing through buildings */
	if (IsNonTaxiableBuilding(to_data->piece_type)) return false;
	/* Stands are parking endpoints — avoid routing through occupied ones.
	 * Empty stands are allowed so small airports without separate taxiways still work. */
	if (v != nullptr && IsParkingOnlyTile(to_data->piece_type) && to != v->tile && to != v->ground_path_goal) {
		Tile t(to);
		if (IsAirportTile(t) && HasAirportTileReservation(t)) return false;
	}

	/* Determine reverse direction (from 'to' back to 'from') */
	uint8_t reverse_dir_bit = 0;
	if (dir_bit == 0x01) reverse_dir_bit = 0x04; // North -> South
	else if (dir_bit == 0x02) reverse_dir_bit = 0x08; // East -> West
	else if (dir_bit == 0x04) reverse_dir_bit = 0x01; // South -> North
	else if (dir_bit == 0x08) reverse_dir_bit = 0x02; // West -> East

	uint8_t to_dirs = CalculateAutoTaxiDirectionsForGfx(to_data->piece_type, to_data->rotation);
	
	bool to_ok = (to_dirs & reverse_dir_bit) != 0;

	if (from_data->piece_type == APT_DEPOT_SE || from_data->piece_type == APT_DEPOT_SW ||
			from_data->piece_type == APT_DEPOT_NW || from_data->piece_type == APT_DEPOT_NE ||
			from_data->piece_type == APT_SMALL_DEPOT_SE || from_data->piece_type == APT_SMALL_DEPOT_SW ||
			from_data->piece_type == APT_SMALL_DEPOT_NW || from_data->piece_type == APT_SMALL_DEPOT_NE) {
		Debug(misc, 5, "[ModAp] Hangar connect check V2: from={}, to={}, dir={}, from_dirs={:x} (auto={:x}, user={:x}), to_dirs={:x}, from_ok={}, to_ok={}",
			from.base(), to.base(), dir_bit, from_dirs, from_auto, from_data->user_taxi_dir_mask, to_dirs, from_ok, to_ok);
	}

	if (!from_ok) return false; // Direction not allowed from 'from'
	if (!to_ok) return false; // Reverse direction not allowed

	/* Explicit edge fences block movement in both directions. */
	if (from_data->edge_block_mask & dir_bit) return false;
	if (to_data->edge_block_mask & reverse_dir_bit) return false;

	return true;
}

/**
 * Get reachable neighbor tiles from a given tile.
 * @param st The station.
 * @param tile Current tile.
 * @return Vector of reachable neighbor tiles.
 */
static std::vector<TileIndex> GetReachableNeighbors(const Station *st, TileIndex tile, const Aircraft *v, TileIndex goal = INVALID_TILE, bool allow_runway_crossing = false)
{
	std::vector<TileIndex> neighbors;

	/* Check if this is a hangar for extra logging */
	const ModularAirportTileData *tile_data = st->airport.GetModularTileData(tile);
	bool is_hangar = (tile_data &&
			(tile_data->piece_type == APT_DEPOT_SE || tile_data->piece_type == APT_DEPOT_SW ||
			 tile_data->piece_type == APT_DEPOT_NW || tile_data->piece_type == APT_DEPOT_NE ||
			 tile_data->piece_type == APT_SMALL_DEPOT_SE || tile_data->piece_type == APT_SMALL_DEPOT_SW ||
			 tile_data->piece_type == APT_SMALL_DEPOT_NW || tile_data->piece_type == APT_SMALL_DEPOT_NE));

	/* Check all 4 orthogonal directions */
	static const int dx[] = {0, 1, 0, -1};  // N, E, S, W
	static const int dy[] = {-1, 0, 1, 0};

	for (int i = 0; i < 4; i++) {
		TileIndex neighbor = tile + TileDiffXY(dx[i], dy[i]);

		if (is_hangar) {
			Debug(misc, 4, "[ModAp] Hangar {} checking neighbor dir={} (dx={},dy={}), tile={}",
				tile.base(), i, dx[i], dy[i], neighbor.base());
		}

		if (!IsValidTile(neighbor)) {
			if (is_hangar) Debug(misc, 4, "[ModAp]   -> invalid tile");
			continue;
		}

		if (!st->TileBelongsToAirport(neighbor)) {
			if (is_hangar) Debug(misc, 4, "[ModAp]   -> not belong to airport");
			continue;
		}

		/* Must be an actual airport station tile, not just grass within airport bounds */
		Tile t(neighbor);
		if (!IsAirport(t)) {
			if (is_hangar) Debug(misc, 4, "[ModAp]   -> not airport tile (grass)");
			continue;
		}

		if (CanTilesConnect(st, tile, neighbor, v, goal, allow_runway_crossing)) {
			neighbors.push_back(neighbor);
			if (is_hangar) Debug(misc, 4, "[ModAp]   -> CONNECTED!");
		} else {
			if (is_hangar) Debug(misc, 4, "[ModAp]   -> CanTilesConnect failed");
		}
	}

	return neighbors;
}

/**
 * Reconstruct path from goal to start using parent pointers.
 * @param parents Map of tile -> parent tile.
 * @param start Starting tile.
 * @param goal Goal tile.
 * @return Path from start to goal.
 */
static std::vector<TileIndex> ReconstructPath(const std::unordered_map<TileIndex, TileIndex> &parents, TileIndex start, TileIndex goal)
{
	std::vector<TileIndex> path;
	TileIndex current = goal;

	while (current != start) {
		path.push_back(current);
		auto it = parents.find(current);
		if (it == parents.end()) break; // Should not happen if path exists
		current = it->second;
	}
	path.push_back(start);

	/* Reverse to get path from start to goal */
	std::reverse(path.begin(), path.end());
	return path;
}

/**
 * Find a ground path from start to goal within an airport using A* algorithm.
 * @param st The station containing the airport.
 * @param start Starting tile.
 * @param goal Goal tile.
 * @param v The aircraft (optional, for reservation checking).
 * @return The path result.
 */
AirportGroundPath FindAirportGroundPath(const Station *st, TileIndex start, TileIndex goal, const Aircraft *v)
{
	/* Validate inputs */
	if (st == nullptr || !IsValidTile(start) || !IsValidTile(goal)) {
		return AirportGroundPath{};
	}

	if (!st->TileBelongsToAirport(start) || !st->TileBelongsToAirport(goal)) {
		return AirportGroundPath{};
	}

	/* Check if modular airport */
	if (!st->airport.blocks.Test(AirportBlock::Modular)) {
		return AirportGroundPath{};
	}

	/* If start == goal, return immediate success */
	if (start == goal) {
		AirportGroundPath result;
		result.tiles.push_back(start);
		result.cost = 0;
		result.found = true;
		return result;
	}

	auto run_pathfind = [&](bool allow_runway_crossing) {
		AirportGroundPath result;

		std::priority_queue<PathNode, std::vector<PathNode>, std::greater<PathNode>> open_set;
		std::unordered_map<TileIndex, int> g_costs;
		std::unordered_map<TileIndex, TileIndex> parents;

		int h_start = CalculateHeuristic(start, goal);
		open_set.emplace(start, 0, h_start, INVALID_TILE);
		g_costs[start] = 0;

		int iterations = 0;
		while (!open_set.empty() && iterations < MAX_PATHFINDER_ITERATIONS) {
			iterations++;

			PathNode current = open_set.top();
			open_set.pop();

			if (current.tile == goal) {
				result.tiles = ReconstructPath(parents, start, goal);
				result.cost = current.g_cost;
				result.found = true;
				return result;
			}

			std::vector<TileIndex> neighbors = GetReachableNeighbors(st, current.tile, v, goal, allow_runway_crossing);
			for (TileIndex neighbor : neighbors) {
				int move_cost = 1;
				const ModularAirportTileData *nb_data = st->airport.GetModularTileData(neighbor);
				if (nb_data != nullptr) {
					switch (nb_data->piece_type) {
						case APT_GRASS_1: case APT_GRASS_2: case APT_GRASS_FENCE_SW:
						case APT_GRASS_FENCE_NE_FLAG: case APT_GRASS_FENCE_NE_FLAG_2:
							move_cost = 4;
							break;
						default: break;
					}

					/* In crossing fallback mode, strongly prefer non-runway alternatives. */
					if (allow_runway_crossing && neighbor != goal && IsModularRunwayPieceLocal(nb_data->piece_type)) {
						move_cost += 8;
					}
				}

				int tentative_g = current.g_cost + move_cost;
				auto it = g_costs.find(neighbor);
				if (it == g_costs.end() || tentative_g < it->second) {
					g_costs[neighbor] = tentative_g;
					parents[neighbor] = current.tile;

					int h = CalculateHeuristic(neighbor, goal);
					open_set.emplace(neighbor, tentative_g, tentative_g + h, current.tile);
				}
			}
		}

		return result;
	};

	const ModularAirportTileData *goal_data = st->airport.GetModularTileData(goal);
	const bool goal_is_runway = (goal_data != nullptr && IsModularRunwayPieceLocal(goal_data->piece_type));
	const uint64_t crossing_key = BuildCrossingCacheKey(start, goal);
	const bool prefer_crossing = !goal_is_runway && _crossing_required_path_cache.contains(crossing_key);

	/* Learned crossing-required pair: go straight to crossing-capable pass. */
	if (prefer_crossing) {
		AirportGroundPath cached_crossing = run_pathfind(true);
		if (cached_crossing.found) return cached_crossing;
		_crossing_required_path_cache.erase(crossing_key);
	}

	/* First pass: strict mode blocks non-goal runway entry from taxi/apron tiles. */
	AirportGroundPath strict = run_pathfind(false);
	if (strict.found) {
		_crossing_required_path_cache.erase(crossing_key);
		return strict;
	}

	/* If a runway goal itself is unreachable, crossing fallback cannot help. */
	if (goal_is_runway) return strict;

	/* Fallback: allow constrained perpendicular runway crossing. */
	AirportGroundPath crossing = run_pathfind(true);
	if (crossing.found) {
		const bool is_new_pair = _crossing_required_path_cache.insert(crossing_key).second;
		if (_crossing_required_path_cache.size() > MAX_CROSSING_CACHE_SIZE) _crossing_required_path_cache.clear();
		if (is_new_pair) {
			Debug(misc, 2, "[ModAp] pathfind-crossing-required: from={} to={} cost={} strict_failed",
				start.base(), goal.base(), crossing.cost);
		}
	}
	return crossing;
}

/**
 * Check if a piece type is a modular runway piece.
 * (Local copy to avoid cross-file dependency on aircraft_cmd.cpp)
 */
static bool IsModularRunwayPieceLocal(uint8_t gfx)
{
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
			return true;
		default:
			return false;
	}
}

/**
 * Check if a tile is a one-way taxiway tile.
 * @param st The station.
 * @param tile The tile to check.
 * @return True if the tile is a taxiway piece with one_way_taxi set.
 */
bool IsOneWayTaxiTile(const Station *st, TileIndex tile)
{
	const ModularAirportTileData *data = st->airport.GetModularTileData(tile);
	if (data == nullptr) return false;
	return IsTaxiwayPiece(data->piece_type) && data->one_way_taxi;
}

/**
 * Classify a tile within an airport path into a segment type.
 * @param st The station.
 * @param tile The tile to classify.
 * @return The segment type for this tile.
 */
static TaxiSegmentType ClassifyTile(const Station *st, TileIndex tile)
{
	const ModularAirportTileData *data = st->airport.GetModularTileData(tile);
	if (data == nullptr) return TaxiSegmentType::FREE_MOVE;

	if (IsModularRunwayPieceLocal(data->piece_type)) return TaxiSegmentType::RUNWAY;
	if (IsTaxiwayPiece(data->piece_type) && data->one_way_taxi) return TaxiSegmentType::ONE_WAY;
	return TaxiSegmentType::FREE_MOVE;
}

/**
 * Walk a path and group consecutive same-type tiles into segments.
 * @param st The station.
 * @param tiles The path tiles.
 * @return Vector of classified segments.
 */
static std::vector<TaxiSegment> ClassifyTaxiSegments(const Station *st, const std::vector<TileIndex> &tiles)
{
	std::vector<TaxiSegment> segments;
	if (tiles.empty()) return segments;

	TaxiSegmentType current_type = ClassifyTile(st, tiles[0]);
	uint16_t seg_start = 0;

	for (uint16_t i = 1; i < static_cast<uint16_t>(tiles.size()); i++) {
		TaxiSegmentType tile_type = ClassifyTile(st, tiles[i]);
		if (tile_type != current_type) {
			segments.push_back({current_type, seg_start, static_cast<uint16_t>(i - 1)});
			current_type = tile_type;
			seg_start = i;
		}
	}
	/* Close the last segment */
	segments.push_back({current_type, seg_start, static_cast<uint16_t>(tiles.size() - 1)});

	return segments;
}

/**
 * Build a classified taxi path from start to goal.
 * Calls A* pathfinder (topology only) then classifies tiles into segments.
 * @param st The station containing the airport.
 * @param start Starting tile.
 * @param goal Goal tile.
 * @param v The aircraft (optional, for stand avoidance).
 * @return A TaxiPath with tiles and segments filled in.
 */
TaxiPath BuildTaxiPath(const Station *st, TileIndex start, TileIndex goal, const Aircraft *v)
{
	TaxiPath result;

	AirportGroundPath path = FindAirportGroundPath(st, start, goal, v);
	if (!path.found) return result;

	result.tiles = std::move(path.tiles);
	result.segments = ClassifyTaxiSegments(st, result.tiles);
	result.valid = true;
	return result;
}
