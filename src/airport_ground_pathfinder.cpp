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
#include <algorithm>

#include "safeguards.h"

/** Maximum number of iterations for pathfinding (prevent infinite loops) */
static const int MAX_PATHFINDER_ITERATIONS = 1000;
static const size_t MAX_CROSSING_CACHE_SIZE = 4096;
static const int PASS_THROUGH_STAND_PENALTY = 5;
static bool IsSameContiguousRunway(const Station *st, TileIndex a, TileIndex b);
std::vector<uint64_t> _modular_airport_crossing_required_path_cache;

/** Bit in a crossing-cache key marking a route planned under the fixed-wing restriction. */
static constexpr uint64_t CROSSING_CACHE_FIXED_WING = 1ULL << 63;

static uint64_t BuildCrossingCacheKey(TileIndex start, TileIndex goal, GroundPathRestriction restriction)
{
	/* The two restrictions are separate routing worlds -- a pair that only a runway
	 * crossing connects for a fixed-wing aircraft may have an ordinary strict route for
	 * a helicopter, which taxis across helipads. Keying them together would teach one
	 * the other's failures. A tile index never reaches bit 31 (the largest map is
	 * 4096x4096), so the top bit of the key is free to carry the distinction. */
	uint64_t key = (static_cast<uint64_t>(start.base()) << 32) | static_cast<uint64_t>(goal.base());
	if (restriction == GroundPathRestriction::FixedWing) key |= CROSSING_CACHE_FIXED_WING;
	return key;
}

void NormalizeModularAirportCrossingPathCache()
{
	std::sort(_modular_airport_crossing_required_path_cache.begin(), _modular_airport_crossing_required_path_cache.end());
	_modular_airport_crossing_required_path_cache.erase(
			std::unique(_modular_airport_crossing_required_path_cache.begin(), _modular_airport_crossing_required_path_cache.end()),
			_modular_airport_crossing_required_path_cache.end());
	if (_modular_airport_crossing_required_path_cache.size() > MAX_CROSSING_CACHE_SIZE) {
		_modular_airport_crossing_required_path_cache.clear();
	}
}

void ClearModularAirportCrossingPathCache()
{
	_modular_airport_crossing_required_path_cache.clear();
}

static bool HasCrossingCacheKey(uint64_t key)
{
	return std::binary_search(_modular_airport_crossing_required_path_cache.begin(), _modular_airport_crossing_required_path_cache.end(), key);
}

static void EraseCrossingCacheKey(uint64_t key)
{
	auto it = std::lower_bound(_modular_airport_crossing_required_path_cache.begin(), _modular_airport_crossing_required_path_cache.end(), key);
	if (it != _modular_airport_crossing_required_path_cache.end() && *it == key) {
		_modular_airport_crossing_required_path_cache.erase(it);
	}
}

static bool InsertCrossingCacheKey(uint64_t key)
{
	auto it = std::lower_bound(_modular_airport_crossing_required_path_cache.begin(), _modular_airport_crossing_required_path_cache.end(), key);
	if (it != _modular_airport_crossing_required_path_cache.end() && *it == key) return false;
	_modular_airport_crossing_required_path_cache.insert(it, key);
	if (_modular_airport_crossing_required_path_cache.size() > MAX_CROSSING_CACHE_SIZE) _modular_airport_crossing_required_path_cache.clear();
	return true;
}

/** Node in the A* search */
struct PathNode {
	TileIndex tile;      ///< Tile position
	int g_cost;          ///< Cost from start to this node
	int f_cost;          ///< Estimated total cost (g_cost + heuristic)
	uint32_t sequence;    ///< Deterministic insertion order for equal-cost ties
	bool passed_safe_stop = false; ///< Route so far has reached a safe stop; see the avoid-set note in FindAirportGroundPath

	PathNode(TileIndex t, int g, int f, uint32_t seq, bool passed = false) : tile(t), g_cost(g), f_cost(f), sequence(seq), passed_safe_stop(passed) {}

	/** Comparison for priority queue (lower f_cost = higher priority) */
	bool operator>(const PathNode &other) const
	{
		if (this->f_cost != other.f_cost) return this->f_cost > other.f_cost;
		return this->sequence < other.sequence;
	}
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
 * Restriction implied by an aircraft.
 * @param v The aircraft, or nullptr.
 * @return FixedWing for a fixed-wing aircraft, None otherwise.
 */
GroundPathRestriction GetGroundPathRestriction(const Aircraft *v)
{
	if (v == nullptr || v->subtype == AIR_HELICOPTER) return GroundPathRestriction::None;
	return GroundPathRestriction::FixedWing;
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
	if (!IsModularRunwayPiece(a_data->piece_type) || !IsModularRunwayPiece(b_data->piece_type)) return false;

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
			if (td == nullptr || !IsModularRunwayPiece(td->piece_type) || (td->rotation % 2) != 0) return false;
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
		if (td == nullptr || !IsModularRunwayPiece(td->piece_type) || (td->rotation % 2) == 0) return false;
	}
	return true;
}

/**
 * Check if two tiles can be connected based on taxi directions.
 * @param st The station.
 * @param from Source tile.
 * @param to Destination tile.
 * @param from_data Tile data for @p from.
 * @param to_data Tile data for @p to.
 * @param v The aircraft (optional, for stand avoidance).
 * @param restriction Aircraft-type restriction.
 * @param goal The pathfinder goal, if any.
 * @param allow_runway_crossing Whether the constrained runway-crossing fallback is active.
 * @return True if connection is allowed.
 */
static bool CanTilesConnect(const Station *st, TileIndex from, TileIndex to, const ModularAirportTileData *from_data, const ModularAirportTileData *to_data, const Aircraft *v, GroundPathRestriction restriction, TileIndex goal = INVALID_TILE, bool allow_runway_crossing = false)
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

	/* Tile data for 'from' / 'to' is supplied by the caller (the A* node
	 * expansion already has it), avoiding redundant per-edge index lookups. */
	if (from_data == nullptr) return false;

	/* Get effective taxi directions */
	uint8_t from_auto = CalculateAutoTaxiDirectionsForGfx(from_data->piece_type, from_data->rotation);
	uint8_t from_dirs = from_auto;
	if (IsTaxiwayPiece(from_data->piece_type) && from_data->one_way_taxi) {
		from_dirs = GetEffectiveTaxiDirections(from_auto, from_data->user_taxi_dir_mask);
	}

	bool from_ok = (from_dirs & dir_bit) != 0;

	if (to_data == nullptr) return false;

	/* A one-way tile may not be entered head-on against its arrow. One-way has until now
	 * constrained only the exit direction of the tile being left, so nothing stopped an
	 * aircraft driving into a one-way apron from the tile that apron points at -- it just
	 * had to turn round and come back out the way it came. That is wrong on its face and
	 * it looks wrong on screen.
	 *
	 * Only head-on entry is refused, not entry from the side: a one-way tile flagged east
	 * is still enterable from its north and south neighbours, so taxiways that merge into
	 * a one-way corridor keep working. Requiring entry to be *along* the arrow instead
	 * would strand any stand reachable only through such a merge. */
	if (IsTaxiwayPiece(to_data->piece_type) && to_data->one_way_taxi) {
		const uint8_t to_dirs = GetEffectiveTaxiDirections(
				CalculateAutoTaxiDirectionsForGfx(to_data->piece_type, to_data->rotation),
				to_data->user_taxi_dir_mask);
		uint8_t reverse_bit = 0;
		if (dir_bit == 0x01) reverse_bit = 0x04; // travelling north, arrow pointing south
		if (dir_bit == 0x02) reverse_bit = 0x08; // travelling east,  arrow pointing west
		if (dir_bit == 0x04) reverse_bit = 0x01; // travelling south, arrow pointing north
		if (dir_bit == 0x08) reverse_bit = 0x02; // travelling west,  arrow pointing east
		if ((to_dirs & reverse_bit) != 0) return false;
	}

	const bool from_is_runway = IsModularRunwayPiece(from_data->piece_type);
	const bool to_is_runway = IsModularRunwayPiece(to_data->piece_type);

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

	/* A helipad is helicopter parking, not pavement a fixed-wing aircraft may roll over.
	 * Leaving one is still allowed -- only 'to' is tested -- because a plane can end up
	 * standing on a pad (unstacking, or a pad built under it) and must be able to get off.
	 * The goal exemption is the same escape valve the stand rule below uses: a goal that
	 * cannot be routed to is a permanent stall, and no code path hands a fixed-wing
	 * aircraft a helipad goal in the first place. */
	if (restriction == GroundPathRestriction::FixedWing && to != goal && IsModularHelipadPiece(to_data->piece_type)) {
		return false;
	}

	/* Stands are parking endpoints -- avoid routing through ones another aircraft has
	 * claimed. Unclaimed stands are allowed so small airports without separate
	 * taxiways still work.
	 *
	 * The ownership test matters: a stand this aircraft already holds is not an
	 * obstacle to itself. Reservations outlive the taxi path that created them, so
	 * asking only "is this reserved?" let an aircraft whose sole exit was a stand it
	 * had reserved block its own route -- permanently, since it then never moved and
	 * so never released the tile.
	 *
	 * A reservation whose owner no longer exists is likewise not an obstacle. Elsewhere
	 * that case is handled by IsTaxiTileReservedByOther, which clears it; the pathfinder
	 * may not mutate map state, so it reads the same condition and routes past. Without
	 * this a stand reserved by a since-removed aircraft would block every route through
	 * it for good, with nothing left to release it. */
	if (v != nullptr && IsParkingOnlyTile(to_data->piece_type) && to != v->tile && to != v->ground_path_goal) {
		Tile t(to);
		if (IsAirportTile(t) && HasAirportTileReservation(t) &&
				GetModularAirportTileReservationOwner(to) != v->index &&
				!IsModularReservationOwnerGone(to)) {
			return false;
		}
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

	/* Explicit edge fences block movement in both directions.
	 * Exception: movement within the same contiguous runway ignores edge
	 * fences -- these are decorative perimeter barriers, not internal runway
	 * blockers. Without this, rollout paths between runway ends fail.
	 * Only same-runway is exempt; fences between distinct parallel runways
	 * remain enforced. */
	if (!(from_is_runway && to_is_runway && IsSameContiguousRunway(st, from, to))) {
		if (from_data->edge_block_mask & dir_bit) return false;
		if (to_data->edge_block_mask & reverse_dir_bit) return false;
	}

	return true;
}

/**
 * Get reachable neighbor tiles from a given tile.
 * @param st The station.
 * @param tile Current tile.
 * @param v The aircraft (optional, for stand avoidance).
 * @param restriction Aircraft-type restriction.
 * @param goal The pathfinder goal, if any.
 * @param allow_runway_crossing Whether the constrained runway-crossing fallback is active.
 * @return Vector of reachable neighbor tiles.
 */
static std::vector<std::pair<TileIndex, const ModularAirportTileData *>> GetReachableNeighbors(const Station *st, TileIndex tile, const Aircraft *v, GroundPathRestriction restriction, TileIndex goal = INVALID_TILE, bool allow_runway_crossing = false)
{
	std::vector<std::pair<TileIndex, const ModularAirportTileData *>> neighbors;

	/* One index lookup for the current tile; reused for every edge check below. */
	const ModularAirportTileData *tile_data = st->airport.GetModularTileData(tile);

	/* is_hangar drives only verbose (level 4) tracing -- don't pay for it otherwise. */
	const bool is_hangar = (_debug_misc_level >= 4) && tile_data != nullptr &&
			(tile_data->piece_type == APT_DEPOT_SE || tile_data->piece_type == APT_DEPOT_SW ||
			 tile_data->piece_type == APT_DEPOT_NW || tile_data->piece_type == APT_DEPOT_NE ||
			 tile_data->piece_type == APT_SMALL_DEPOT_SE || tile_data->piece_type == APT_SMALL_DEPOT_SW ||
			 tile_data->piece_type == APT_SMALL_DEPOT_NW || tile_data->piece_type == APT_SMALL_DEPOT_NE);

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

		const ModularAirportTileData *nb_data = st->airport.GetModularTileData(neighbor);
		if (CanTilesConnect(st, tile, neighbor, tile_data, nb_data, v, restriction, goal, allow_runway_crossing)) {
			neighbors.emplace_back(neighbor, nb_data);
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
static std::vector<TileIndex> ReconstructPath(const std::unordered_map<uint64_t, uint64_t> &parents, uint64_t start_state, uint64_t goal_state)
{
	std::vector<TileIndex> path;
	uint64_t current = goal_state;

	while (current != start_state) {
		path.push_back(TileIndex(static_cast<uint32_t>(current & 0xFFFFFFFFULL)));
		auto it = parents.find(current);
		if (it == parents.end()) break; // Should not happen if path exists
		current = it->second;
	}
	path.push_back(TileIndex(static_cast<uint32_t>(start_state & 0xFFFFFFFFULL)));

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
 * @param allow_runway_goal_crossing Allow crossing-fallback paths when the goal is a runway.
 * @param update_cache Whether the crossing-required cache may be written.
 * @param restriction Aircraft-type restriction; pass one explicitly when @p v is nullptr.
 * @return The path result.
 */
AirportGroundPath FindAirportGroundPath(const Station *st, TileIndex start, TileIndex goal, const Aircraft *v, bool allow_runway_goal_crossing, bool update_cache, GroundPathRestriction restriction, std::span<const TileIndex> avoid_tiles)
{
	if (restriction == GroundPathRestriction::FromAircraft) restriction = GetGroundPathRestriction(v);

	/* A restricted run answers a different question than the cache stores. The cache is
	 * keyed on (start, goal, restriction) only and is *saved*, so letting an avoid-set
	 * failure write it would teach "this pair needs a runway crossing" permanently, on
	 * the strength of a ban that applied for one tick. Neither read nor write here. */
	const bool cacheable = avoid_tiles.empty();
	update_cache = update_cache && cacheable;

	const auto is_avoided = [&avoid_tiles](TileIndex tile) {
		return std::find(avoid_tiles.begin(), avoid_tiles.end(), tile) != avoid_tiles.end();
	};

	/* The avoid-set is scoped to the reservation horizon, not the whole route.
	 * BuildForwardReservationPlan stops claiming at the first safe stop after the start,
	 * so a tile the route only touches beyond that point is never claimed and is not an
	 * obstacle. Banning it for the whole route would discard exactly the alternative the
	 * ban exists to find: where the shortest route is blocked at A, a route that queues
	 * on a one-way tile first and only reaches A later is reservable right now.
	 *
	 * Search state is therefore (tile, has the route reached a safe stop yet). With an
	 * empty avoid-set the flag is never set, so the state space, tie-break order and
	 * results are bit-identical to a plain tile-keyed search. */
	const bool track_horizon = !avoid_tiles.empty();
	const auto state_key = [](TileIndex tile, bool passed) -> uint64_t {
		return static_cast<uint64_t>(tile.base()) | (passed ? (1ULL << 32) : 0ULL);
	};

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
		std::unordered_map<uint64_t, int> g_costs;
		std::unordered_map<uint64_t, uint64_t> parents;
		uint32_t sequence = 0;
		const uint64_t start_state = state_key(start, false);

		/* Prevent revisits on the candidate route itself. The horizon flag makes two
		 * states for one physical tile, so the ordinary g-cost check cannot detect a
		 * route that reaches a tile before a safe stop and returns to it afterwards.
		 * Do not close the opposite state globally: a distinct, non-revisiting route
		 * may legitimately merge there after reaching a safe stop. */
		const auto current_path_contains = [&](uint64_t current_state, TileIndex tile) {
			while (true) {
				if (TileIndex(static_cast<uint32_t>(current_state & 0xFFFFFFFFULL)) == tile) return true;
				if (current_state == start_state) return false;
				auto it = parents.find(current_state);
				if (it == parents.end()) return false;
				current_state = it->second;
			}
		};

		int h_start = CalculateHeuristic(start, goal);
		open_set.emplace(start, 0, h_start, sequence++, false);
		g_costs[start_state] = 0;

		int iterations = 0;
		while (!open_set.empty() && iterations < MAX_PATHFINDER_ITERATIONS) {
			iterations++;

			PathNode current = open_set.top();
			open_set.pop();
			const uint64_t current_state = state_key(current.tile, current.passed_safe_stop);
			if (track_horizon) {
				auto best = g_costs.find(current_state);
				if (best == g_costs.end() || current.g_cost != best->second) continue;
			}

			if (current.tile == goal) {
				result.tiles = ReconstructPath(parents, start_state,
						state_key(goal, current.passed_safe_stop));
				/* Guard the invariant rather than trust it: a revisiting route is never
				 * correct, and callers treat "not found" as "wait on the direct route",
				 * which is the safe answer. */
				if (track_horizon) {
					std::vector<TileIndex> sorted = result.tiles;
					std::sort(sorted.begin(), sorted.end());
					if (std::adjacent_find(sorted.begin(), sorted.end()) != sorted.end()) {
						return AirportGroundPath{};
					}
				}
				result.cost = current.g_cost;
				result.found = true;
				return result;
			}

			auto neighbors = GetReachableNeighbors(st, current.tile, v, restriction, goal, allow_runway_crossing);
			for (const auto &[neighbor, nb_data] : neighbors) {
				/* Inside the horizon the ban applies; past the first safe stop it lifts. */
				if (track_horizon && !current.passed_safe_stop && is_avoided(neighbor)) continue;
				if (track_horizon && current_path_contains(current_state, neighbor)) continue;
				const bool neighbor_passed = track_horizon &&
						(current.passed_safe_stop || IsModularSafeStopTile(st, neighbor, goal));
				int move_cost = 1;
				if (nb_data != nullptr) {
					switch (nb_data->piece_type) {
						case APT_GRASS_1: case APT_GRASS_2: case APT_GRASS_FENCE_SW:
						case APT_GRASS_FENCE_NE_FLAG: case APT_GRASS_FENCE_NE_FLAG_2:
							move_cost = 4;
							break;
						default: break;
					}

					/* Stands are valid transit nodes, but prefer not routing through
					 * unrelated stands when alternatives exist. */
					if (neighbor != goal && IsParkingOnlyTile(nb_data->piece_type)) {
						move_cost += PASS_THROUGH_STAND_PENALTY;
					}

					/* In crossing fallback mode, strongly prefer non-runway alternatives. */
					if (allow_runway_crossing && neighbor != goal && IsModularRunwayPiece(nb_data->piece_type)) {
						move_cost += 8;
					}
				}

				int tentative_g = current.g_cost + move_cost;
				const uint64_t neighbor_state = state_key(neighbor, neighbor_passed);
				auto it = g_costs.find(neighbor_state);
				if (it == g_costs.end() || tentative_g < it->second) {
					g_costs[neighbor_state] = tentative_g;
					parents[neighbor_state] = state_key(current.tile, current.passed_safe_stop);

					int h = CalculateHeuristic(neighbor, goal);
					open_set.emplace(neighbor, tentative_g, tentative_g + h, sequence++, neighbor_passed);
				}
			}
		}

		return result;
	};

	const ModularAirportTileData *goal_data = st->airport.GetModularTileData(goal);
	const bool goal_is_runway = (goal_data != nullptr && IsModularRunwayPiece(goal_data->piece_type));
	const uint64_t crossing_key = BuildCrossingCacheKey(start, goal, restriction);
	const bool prefer_crossing = cacheable && !goal_is_runway && HasCrossingCacheKey(crossing_key);

	/* Learned crossing-required pair: go straight to crossing-capable pass.
	 * This cache is saved because it changes live path choices. Diagnostic
	 * probes (update_cache=false) read the learned preference but never write,
	 * so unsaved rate-limit gating cannot diverge cache state across MP clients. */
	if (prefer_crossing) {
		AirportGroundPath cached_crossing = run_pathfind(true);
		if (cached_crossing.found) return cached_crossing;
		if (update_cache) EraseCrossingCacheKey(crossing_key);
	}

	/* First pass: strict mode blocks non-goal runway entry from taxi/apron tiles. */
	AirportGroundPath strict = run_pathfind(false);
	if (strict.found) {
		if (update_cache) EraseCrossingCacheKey(crossing_key);
		return strict;
	}

	/* For runway goals, only allow crossing fallback when explicitly requested.
	 * Without this gate, crossing paths to runways get selected over temporarily-blocked
	 * non-crossing paths, adding unnecessary runway contention. */
	if (goal_is_runway && !allow_runway_goal_crossing) return strict;

	/* Fallback: allow constrained perpendicular runway crossing. */
	AirportGroundPath crossing = run_pathfind(true);
	if (crossing.found && update_cache) {
		const bool is_new_pair = InsertCrossingCacheKey(crossing_key);
		if (is_new_pair) {
			Debug(misc, 2, "[ModAp] pathfind-crossing-required: from={} to={} cost={} strict_failed",
				start.base(), goal.base(), crossing.cost);
		}
	}
	return crossing;
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
	if (data == nullptr) return TaxiSegmentType::FreeMove;

	if (IsModularRunwayPiece(data->piece_type)) return TaxiSegmentType::Runway;
	if (IsTaxiwayPiece(data->piece_type) && data->one_way_taxi) return TaxiSegmentType::OneWay;
	return TaxiSegmentType::FreeMove;
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
 * @param allow_runway_goal_crossing Allow crossing-fallback paths when the goal is a runway.
 * @param restriction Aircraft-type restriction; pass one explicitly when @p v is nullptr.
 * @return A TaxiPath with tiles and segments filled in.
 */
TaxiPath BuildTaxiPath(const Station *st, TileIndex start, TileIndex goal, const Aircraft *v, bool allow_runway_goal_crossing, GroundPathRestriction restriction, std::span<const TileIndex> avoid_tiles)
{
	TaxiPath result;

	AirportGroundPath path = FindAirportGroundPath(st, start, goal, v, allow_runway_goal_crossing, true, restriction, avoid_tiles);
	if (!path.found) return result;

	result.tiles = std::move(path.tiles);
	result.segments = ClassifyTaxiSegments(st, result.tiles);
	result.valid = true;
	return result;
}
