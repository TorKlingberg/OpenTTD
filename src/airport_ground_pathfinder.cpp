/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file airport_ground_pathfinder.cpp Ground pathfinding for modular airports. */

#include "stdafx.h"
#include "airport_ground_pathfinder.h"
#include "airport_pathfinder.h"
#include "station_base.h"
#include "aircraft.h"
#include "station_map.h"
#include "tile_map.h"
#include <queue>
#include <unordered_map>
#include <algorithm>

#include "safeguards.h"

/** Maximum number of iterations for pathfinding (prevent infinite loops) */
static const int MAX_PATHFINDER_ITERATIONS = 1000;

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
 * Check if two tiles can be connected based on taxi directions.
 * @param st The station.
 * @param from Source tile.
 * @param to Destination tile.
 * @return True if connection is allowed.
 */
static bool CanTilesConnect(const Station *st, TileIndex from, TileIndex to, const Aircraft *v)
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
	uint8_t from_dirs = GetEffectiveTaxiDirections(from_auto, from_data->user_taxi_dir_mask);
	if ((from_dirs & dir_bit) == 0) return false; // Direction not allowed from 'from'

	/* Get tile data for 'to' */
	const ModularAirportTileData *to_data = st->airport.GetModularTileData(to);
	if (to_data == nullptr) return false;

	/* Determine reverse direction (from 'to' back to 'from') */
	uint8_t reverse_dir_bit = 0;
	if (dir_bit == 0x01) reverse_dir_bit = 0x04; // North -> South
	else if (dir_bit == 0x02) reverse_dir_bit = 0x08; // East -> West
	else if (dir_bit == 0x04) reverse_dir_bit = 0x01; // South -> North
	else if (dir_bit == 0x08) reverse_dir_bit = 0x02; // West -> East

	uint8_t to_auto = CalculateAutoTaxiDirectionsForGfx(to_data->piece_type, to_data->rotation);
	uint8_t to_dirs = GetEffectiveTaxiDirections(to_auto, to_data->user_taxi_dir_mask);
	if ((to_dirs & reverse_dir_bit) == 0) return false; // Reverse direction not allowed

	if (v != nullptr) {
		Tile t(to);
		if (HasAirportTileReservation(t) && GetAirportTileReserver(t) != v->index) return false;
	}

	return true;
}

/**
 * Get reachable neighbor tiles from a given tile.
 * @param st The station.
 * @param tile Current tile.
 * @return Vector of reachable neighbor tiles.
 */
static std::vector<TileIndex> GetReachableNeighbors(const Station *st, TileIndex tile, const Aircraft *v)
{
	std::vector<TileIndex> neighbors;

	/* Check all 4 orthogonal directions */
	static const int dx[] = {0, 1, 0, -1};
	static const int dy[] = {-1, 0, 1, 0};

	for (int i = 0; i < 4; i++) {
		TileIndex neighbor = tile + TileDiffXY(dx[i], dy[i]);
		if (!IsValidTile(neighbor)) continue;
		if (!st->TileBelongsToAirport(neighbor)) continue;
		if (CanTilesConnect(st, tile, neighbor, v)) {
			neighbors.push_back(neighbor);
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
AirportGroundPath FindAirportGroundPath(const Station *st, TileIndex start, TileIndex goal, [[maybe_unused]] const Aircraft *v)
{
	AirportGroundPath result;

	/* Validate inputs */
	if (st == nullptr || !IsValidTile(start) || !IsValidTile(goal)) {
		return result;
	}

	if (!st->TileBelongsToAirport(start) || !st->TileBelongsToAirport(goal)) {
		return result;
	}

	/* Check if modular airport */
	if (!st->airport.blocks.Test(AirportBlock::Modular)) {
		return result;
	}

	/* If start == goal, return immediate success */
	if (start == goal) {
		result.tiles.push_back(start);
		result.cost = 0;
		result.found = true;
		return result;
	}

	/* A* algorithm */
	std::priority_queue<PathNode, std::vector<PathNode>, std::greater<PathNode>> open_set;
	std::unordered_map<TileIndex, int> g_costs; // Best known g_cost to reach each tile
	std::unordered_map<TileIndex, TileIndex> parents; // Parent tile in optimal path

	/* Initialize start node */
	int h_start = CalculateHeuristic(start, goal);
	open_set.emplace(start, 0, h_start, INVALID_TILE);
	g_costs[start] = 0;

	int iterations = 0;

	while (!open_set.empty() && iterations < MAX_PATHFINDER_ITERATIONS) {
		iterations++;

		/* Get node with lowest f_cost */
		PathNode current = open_set.top();
		open_set.pop();

		/* Check if we reached the goal */
		if (current.tile == goal) {
			result.tiles = ReconstructPath(parents, start, goal);
			result.cost = current.g_cost;
			result.found = true;
			return result;
		}

		/* Explore neighbors */
		std::vector<TileIndex> neighbors = GetReachableNeighbors(st, current.tile, v);
		for (TileIndex neighbor : neighbors) {
			int tentative_g = current.g_cost + 1; // Cost is 1 per tile

			/* Check if this path to neighbor is better than previously known */
			auto it = g_costs.find(neighbor);
			if (it == g_costs.end() || tentative_g < it->second) {
				/* This is a better path */
				g_costs[neighbor] = tentative_g;
				parents[neighbor] = current.tile;

				int h = CalculateHeuristic(neighbor, goal);
				int f = tentative_g + h;
				open_set.emplace(neighbor, tentative_g, f, current.tile);
			}
		}
	}

	/* No path found */
	return result;
}
