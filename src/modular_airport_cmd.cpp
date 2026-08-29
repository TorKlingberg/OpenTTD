/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file modular_airport_cmd.cpp Modular airport movement and reservation logic. */

#include "stdafx.h"
#include "aircraft.h"
#include "landscape.h"
#include "news_func.h"
#include "newgrf_engine.h"
#include "newgrf_sound.h"
#include "error_func.h"
#include "strings_func.h"
#include "command_func.h"
#include "window_func.h"
#include "timer/timer_game_calendar.h"
#include "timer/timer_game_economy.h"
#include "vehicle_func.h"
#include "viewport_func.h"
#include "sound_func.h"
#include "cheat_type.h"
#include "company_base.h"
#include "ai/ai.hpp"
#include "game/game.hpp"
#include "company_func.h"
#include "effectvehicle_func.h"
#include "station_base.h"
#include "station_map.h"
#include "engine_base.h"
#include "core/fixedpoint_func.hpp"
#include "core/random_func.hpp"
#include "core/backup_type.hpp"
#include "zoom_func.h"
#include "disaster_vehicle.h"
#include "newgrf_airporttiles.h"
#include "framerate_type.h"
#include "aircraft_cmd.h"
#include "vehicle_cmd.h"
#include "airport_ground_pathfinder.h"
#include "timer/timer_game_tick.h"
#include "modular_airport_cmd.h"

#include "table/strings.h"
#include "table/airporttile_ids.h"

#include <map>
#include <set>
#include <unordered_map>
#include <chrono>
#include <cmath>
#include <algorithm>
#include <array>
#include <limits>

#include "safeguards.h"

/**
 * Special velocities for aircraft.
 */
static constexpr uint16_t SPEED_LIMIT_TAXI = 50; ///< Maximum speed of an aircraft while taxiing
static constexpr uint16_t SPEED_LIMIT_APPROACH = 230; ///< Maximum speed of an aircraft on finals
static constexpr uint16_t SPEED_LIMIT_HOLD = 425; ///< Maximum speed of an aircraft that flies the holding pattern
static constexpr uint16_t SPEED_LIMIT_NONE = UINT16_MAX; ///< No environmental speed limit. Speed limit is type dependent

static std::string_view GetModularAirportDebugName(const Station *st);

static bool IsModularTerminalBuildingPiece(uint8_t piece_type)
{
	switch (piece_type) {
		case APT_BUILDING_1:
		case APT_BUILDING_2:
		case APT_BUILDING_3:
		case APT_ROUND_TERMINAL:
		case APT_LOW_BUILDING:
		case APT_LOW_BUILDING_FENCE_N:
		case APT_LOW_BUILDING_FENCE_NW:
		case APT_SMALL_BUILDING_1:
		case APT_SMALL_BUILDING_2:
		case APT_SMALL_BUILDING_3:
			return true;
		default:
			return false;
	}
}

/**
 * Direction an aircraft should face while parked on a modular airport tile.
 *
 * A one-way taxiway points along one map axis. Vehicle directions are viewport-aligned,
 * so the four mask bits map to the diagonal Direction values used while taxiing between
 * tile centres. A stand instead faces an edge-adjacent terminal building, when present.
 *
 * Round terminals on the two sides supported by the auto-jetway sprites take priority.
 * The remaining scan order is fixed as +Y, -X, -Y, +X so layouts with more than one
 * adjacent terminal remain deterministic.
 */
Direction GetModularAircraftParkedDirection(const Station *st, TileIndex tile)
{
	if (st == nullptr || !IsValidTile(tile)) return Direction::Invalid;

	const ModularAirportTileData *data = st->airport.GetModularTileData(tile);
	if (data == nullptr) return Direction::Invalid;

	if (IsTaxiwayPiece(data->piece_type) && data->one_way_taxi && HasExactlyOneBit(data->user_taxi_dir_mask)) {
		switch (data->user_taxi_dir_mask & 0x0F) {
			case 0x01: return Direction::NW; // -Y
			case 0x02: return Direction::SW; // +X
			case 0x04: return Direction::SE; // +Y
			case 0x08: return Direction::NE; // -X
			default: break;
		}
	}

	if (!IsModularStandPiece(data->piece_type)) return Direction::Invalid;

	struct NeighborDirection {
		int dx;
		int dy;
		Direction direction;
	};
	static constexpr std::array<NeighborDirection, 4> neighbors = {{
		{ 0,  1, Direction::SE},
		{-1,  0, Direction::NE},
		{ 0, -1, Direction::NW},
		{ 1,  0, Direction::SW},
	}};

	const auto NeighborPiece = [&](const NeighborDirection &neighbor) -> uint8_t {
		const TileIndex adjacent = TileAddXY(tile, neighbor.dx, neighbor.dy);
		if (!IsValidTile(adjacent)) return 0xFF;
		const ModularAirportTileData *neighbor_data = st->airport.GetModularTileData(adjacent);
		return neighbor_data != nullptr ? neighbor_data->piece_type : 0xFF;
	};

	/* These are the two round-terminal sides for which the drawing code can add a jetway. */
	for (size_t i = 0; i < 2; ++i) {
		if (NeighborPiece(neighbors[i]) == APT_ROUND_TERMINAL) return neighbors[i].direction;
	}

	for (const NeighborDirection &neighbor : neighbors) {
		if (IsModularTerminalBuildingPiece(NeighborPiece(neighbor))) return neighbor.direction;
	}

	return Direction::Invalid;
}

/** Turn one 45-degree step toward the parked heading, without moving the aircraft. */
bool UpdateModularAircraftParkedDirection(Aircraft *v, const Station *st)
{
	if (v == nullptr) return false;

	const Direction desired = GetModularAircraftParkedDirection(st, v->tile);
	if (desired == Direction::Invalid) return true;
	if (desired == v->direction) return true;

	v->last_direction = v->direction;
	v->direction = ChangeDir(v->direction, LimitDirDiff(DirDifference(desired, v->direction)));
	v->turn_counter = 0;
	v->number_consecutive_turns = 0;
	/* Normal aircraft always have a shadow vehicle. Bare-shell unit-test aircraft do not. */
	if (v->Next() != nullptr) {
		SetAircraftPosition(v, v->x_pos, v->y_pos, v->z_pos);
	} else {
		v->UpdatePositionAndViewport();
	}
	return v->direction == desired;
}

static ModularAirportTileData *GetModularAirportReservationData(TileIndex tile)
{
	if (!IsValidTile(tile)) return nullptr;
	Tile t(tile);
	if (!IsAirportTile(t)) return nullptr;
	Station *st = Station::GetByTile(tile);
	if (st == nullptr || !st->airport.blocks.Test(AirportBlock::Modular)) return nullptr;
	return st->airport.GetModularTileData(tile);
}

bool HasModularAirportTileReservation(TileIndex tile)
{
	const ModularAirportTileData *data = GetModularAirportReservationData(tile);
	if (data == nullptr) return false;
	Tile t(tile);
	return HasAirportTileReservation(t);
}

VehicleID GetModularAirportTileReservationOwner(TileIndex tile)
{
	const ModularAirportTileData *data = GetModularAirportReservationData(tile);
	if (data == nullptr) return VehicleID::Invalid();
	Tile t(tile);
	if (!HasAirportTileReservation(t)) return VehicleID::Invalid();
	return VehicleID{data->reservation_owner};
}

bool IsModularAirportTileReservedBy(TileIndex tile, VehicleID vid)
{
	return GetModularAirportTileReservationOwner(tile) == vid;
}

void SetModularAirportTileReservationOwner(TileIndex tile, VehicleID vid)
{
	ModularAirportTileData *data = GetModularAirportReservationData(tile);
	if (data == nullptr) return;
	Tile t(tile);
	SetAirportTileReservation(t, true);
	data->reservation_owner = vid.base();
	MarkTileDirtyByTile(tile);
}

void ClearModularAirportTileReservation(TileIndex tile)
{
	ModularAirportTileData *data = GetModularAirportReservationData(tile);
	if (data == nullptr) return;
	Tile t(tile);
	const bool was_reserved = HasAirportTileReservation(t);
	const bool had_owner = data->reservation_owner != VehicleID::Invalid().base();
	SetAirportTileReservation(t, false);
	data->reservation_owner = VehicleID::Invalid().base();
	if (was_reserved || had_owner) MarkTileDirtyByTile(tile);
}

struct ModularTakeoffFailLogState {
	uint64_t last_tick = 0;
	uint32_t suppressed_count = 0;
};

/** File-scope static maps that must be cleared on save/load. */
static std::unordered_map<uint32_t, uint64_t> _rate_limit_last_tick;
static std::map<VehicleID, ModularTakeoffFailLogState> _takeoff_fail_state;

/**
 * Aircraft that can affect a modular runway during the current vehicle-tick pass.
 *
 * Aircraft share the global vehicle pool with effects and every other vehicle type,
 * so Aircraft::Iterate() walks the whole pool. Runway entry is revalidated before
 * every taxi step; doing that pool walk for every check dominated busy saves.
 *
 * The cache is deliberately scoped to one CallVehicleTicks pass. It is built lazily
 * from live state on the first runway query, then aircraft that enter a relevant
 * state later in the same pass are inserted after their tick. Candidates that leave
 * a state may remain until the end of the pass, but every query rechecks their live
 * fields, making that harmless. Calls outside CallVehicleTicks use the uncached scan,
 * which keeps commands, save/load recovery, and unit-test state mutation exact.
 *
 * The soundness of all that rests on one invariant: **an aircraft only ever enters a
 * runway-flow state during its own tick**, which CallVehicleTicks follows immediately
 * with UpdateModularAirportRunwayStateCache. A superfluous candidate is harmless, but a
 * missing one is a false negative that clears an aircraft onto a runway another aircraft
 * is already using. Nothing moves another aircraft into one of these states mid-pass
 * today: the cross-aircraft mutators (TeleportAircraftOnModularTile,
 * UpdateAirplanesOnNewStation) only ever leave a state, and both run from commands,
 * outside the pass. A go-around, runway preemption or similar added later would break
 * it silently, so VerifyModularRunwayStateCache() re-derives the set at end of pass in
 * debug builds and asserts nothing was missed.
 */
struct ModularRunwayStateCache {
	bool vehicle_tick_active = false;
	bool built = false;
	std::vector<VehicleID> candidates;
};

static ModularRunwayStateCache _modular_runway_state_cache;

static bool IsModularRunwayFlowState(uint8_t state)
{
	return state == LANDING || state == ENDLANDING ||
			state == HELILANDING || state == HELIENDLANDING ||
			state == TAKEOFF || state == STARTTAKEOFF || state == ENDTAKEOFF;
}

static bool IsModularRunwayStateCandidate(const Aircraft *v)
{
	if (v == nullptr || !v->IsNormalAircraft()) return false;

	return IsModularRunwayFlowState(v->state) || v->modular_ground_target == MGT_ROLLOUT ||
			v->modular_ground_target == MGT_RUNWAY_TAKEOFF;
}

static void InsertModularRunwayStateCandidate(VehicleID id)
{
	auto &candidates = _modular_runway_state_cache.candidates;
	const auto it = std::lower_bound(candidates.begin(), candidates.end(), id);
	if (it == candidates.end() || *it != id) candidates.insert(it, id);
}

static void BuildModularRunwayStateCache()
{
	auto &cache = _modular_runway_state_cache;
	cache.candidates.clear();
	for (const Aircraft *v : Aircraft::Iterate()) {
		if (IsModularRunwayStateCandidate(v)) cache.candidates.push_back(v->index);
	}
	cache.built = true;
}

void BeginModularAirportRunwayStateCache()
{
	auto &cache = _modular_runway_state_cache;
	cache.vehicle_tick_active = true;
	cache.built = false;
	cache.candidates.clear();
}

void UpdateModularAirportRunwayStateCache(const Aircraft *v)
{
	auto &cache = _modular_runway_state_cache;
	if (!cache.vehicle_tick_active || !cache.built || !IsModularRunwayStateCandidate(v)) return;
	InsertModularRunwayStateCandidate(v->index);
}

/**
 * Assert that no aircraft became a runway-state candidate without being recorded.
 *
 * Only meaningful once the set has been built: before that a query would derive it from
 * live state anyway. Costs one pool walk per tick pass, against the per-query walk this
 * cache removes, and only in debug builds.
 */
static void VerifyModularRunwayStateCache()
{
#ifdef _DEBUG
	const auto &cache = _modular_runway_state_cache;
	if (!cache.built) return;

	for (const Aircraft *v : Aircraft::Iterate()) {
		if (!IsModularRunwayStateCandidate(v)) continue;
		assert(std::binary_search(cache.candidates.begin(), cache.candidates.end(), v->index));
	}
#endif
}

void EndModularAirportRunwayStateCache()
{
	auto &cache = _modular_runway_state_cache;
	if (cache.vehicle_tick_active) VerifyModularRunwayStateCache();
	cache.vehicle_tick_active = false;
	cache.built = false;
	cache.candidates.clear();
}

template <typename F>
static bool ForEachModularRunwayStateCandidate(F &&func)
{
	auto &cache = _modular_runway_state_cache;
	if (cache.vehicle_tick_active) {
		if (!cache.built) BuildModularRunwayStateCache();
		for (VehicleID id : cache.candidates) {
			const Aircraft *v = Aircraft::GetIfValid(id);
			if (v != nullptr && func(v)) return true;
		}
		return false;
	}

	for (const Aircraft *v : Aircraft::Iterate()) {
		if (func(v)) return true;
	}
	return false;
}

/** Reset all static state in modular airport code; called after loading a save. */
void ResetModularAirportStaticState()
{
	_rate_limit_last_tick.clear();
	_takeoff_fail_state.clear();
	EndModularAirportRunwayStateCache();
}

/**
 * Determine whether a runway end tile is at the "low" end of its contiguous runway.
 * "Low" means the end with lower X (horizontal) or lower Y (vertical).
 * @param st Station the tile belongs to.
 * @param tile The runway end tile to check.
 * @return true if this is the low-coordinate end, false if high-coordinate end.
 */
bool IsRunwayEndLow(const Station *st, TileIndex tile)
{
	const ModularAirportTileData *data = st->airport.GetModularTileData(tile);
	if (data == nullptr) return true;

	bool horizontal = (data->rotation % 2) == 0;
	TileIndexDiff diff = horizontal ? TileDiffXY(1, 0) : TileDiffXY(0, 1);

	/* Check if runway extends in the positive direction from this tile */
	TileIndex next = tile + diff;
	const ModularAirportTileData *next_data = st->airport.GetModularTileData(next);
	bool extends_positive = IsRunwayPieceOnAxis(next_data, horizontal);

	/* Check if runway extends in the negative direction from this tile */
	TileIndex prev = tile - diff;
	const ModularAirportTileData *prev_data = st->airport.GetModularTileData(prev);
	bool extends_negative = IsRunwayPieceOnAxis(prev_data, horizontal);

	/* If runway only extends positive, we're at the low end.
	 * If runway only extends negative, we're at the high end.
	 * If it extends both ways, we're not an end tile (shouldn't happen for end pieces). */
	if (extends_positive && !extends_negative) return true;  /* Low end */
	if (!extends_positive && extends_negative) return false;  /* High end */

	/* Single tile runway or middle piece: treat as low end */
	return true;
}

/**
 * Get the runway usage flags for a runway containing the given tile.
 * All tiles in a contiguous runway share the same flags.
 * @param st Station the tile belongs to.
 * @param tile Any tile in the runway.
 * @return The runway_flags value, or RUF_DEFAULT if not found.
 */
uint8_t GetRunwayFlags(const Station *st, TileIndex tile)
{
	const ModularAirportTileData *data = st->airport.GetModularTileData(tile);
	if (data == nullptr) return RUF_DEFAULT;
	return data->runway_flags;
}

TileIndex GetRunwayOtherEnd(const Station *st, TileIndex start_tile)
{
	const ModularAirportTileData *data = st->airport.GetModularTileData(start_tile);
	if (data == nullptr) return start_tile;

	bool horizontal = (data->rotation % 2) == 0;
	TileIndexDiff diff = horizontal ? TileDiffXY(1, 0) : TileDiffXY(0, 1);

	/* Determine direction by checking which neighbor is also runway */
	TileIndex check = start_tile + diff;
	const ModularAirportTileData *check_data = st->airport.GetModularTileData(check);
	if (!IsRunwayPieceOnAxis(check_data, horizontal)) {
		diff = -diff; /* Go the other way */
	}

	TileIndex current = start_tile;
	TileIndex next = current + diff;

	/* Walk until we find the end */
	while (true) {
		const ModularAirportTileData *next_data = st->airport.GetModularTileData(next);
		if (!IsRunwayPieceOnAxis(next_data, horizontal)) {
			return current;
		}
		current = next;
		next = current + diff;
	}
}

bool GetContiguousModularRunwayTiles(const Station *st, TileIndex start_tile, std::vector<TileIndex> &tiles)
{
	tiles.clear();

	const ModularAirportTileData *data = st->airport.GetModularTileData(start_tile);
	if (data == nullptr || !IsModularRunwayPiece(data->piece_type)) return false;

	const bool horizontal = (data->rotation % 2) == 0;
	const TileIndexDiff diff = horizontal ? TileDiffXY(1, 0) : TileDiffXY(0, 1);

	TileIndex first = start_tile;
	while (true) {
		TileIndex prev = first - diff;
		const ModularAirportTileData *prev_data = st->airport.GetModularTileData(prev);
		if (!IsRunwayPieceOnAxis(prev_data, horizontal)) break;
		first = prev;
	}

	TileIndex current = first;
	while (true) {
		tiles.push_back(current);
		TileIndex next = current + diff;
		const ModularAirportTileData *next_data = st->airport.GetModularTileData(next);
		if (!IsRunwayPieceOnAxis(next_data, horizontal)) break;
		current = next;
	}

	return !tiles.empty();
}

/**
 * Check whether a contiguous runway segment starting from a given end tile
 * is safe for large (AIR_FAST) aircraft.
 * Requires all tiles to be large runway family and total length >= 6.
 */
bool IsRunwaySafeForLarge(const Station *st, TileIndex runway_end)
{
	std::vector<TileIndex> tiles;
	if (!GetContiguousModularRunwayTiles(st, runway_end, tiles)) return false;
	if ((int)tiles.size() < LARGE_RUNWAY_LENGTH_TILES) return false;
	for (TileIndex t : tiles) {
		const ModularAirportTileData *td = st->airport.GetModularTileData(t);
		if (td == nullptr || !IsLargeRunwayFamily(td->piece_type)) return false;
	}
	return true;
}

/**
 * Check whether an aircraft may use a particular modular runway for landing.
 *
 * Fast jets may use a short runway only as a fallback when the airport has no
 * large-safe landing runway at all. The decision deliberately ignores transient
 * reservations: if the suitable runway is busy, the jet must keep holding rather
 * than divert to a short strip. Small aircraft remain free to use either runway.
 *
 * The fallback leans on an invariant maintained elsewhere: both extremities of a
 * contiguous runway are end pieces, because NormalizeRunwaySegmentVisuals
 * recanonicalizes the whole segment on every placement, removal and upgrade.
 * That is what makes the airport-wide question here equivalent to the
 * directionally-filtered scan its caller runs. Were a runway ever left with a
 * bare middle piece at the extremity its direction flags point landings at,
 * ModularAirportHasSafeRunwayFor would still count the runway's other, unusable
 * end and a jet would be refused the short strip with nowhere else to go.
 */
bool CanAircraftUseModularRunwayForLanding(const Station *st, const Aircraft *v, TileIndex runway_tile)
{
	const ModularAirportTileData *data = st->airport.GetModularTileData(runway_tile);
	if (data == nullptr || !IsModularRunwayEndPiece(data->piece_type)) return false;

	if ((AircraftVehInfo(v->engine_type)->subtype & AIR_FAST) == 0) return true;

	return IsRunwaySafeForLarge(st, runway_tile) || !ModularAirportHasSafeRunwayFor(st, true);
}

void ClearModularRunwayReservation(Aircraft *v)
{
	for (TileIndex tile : v->modular_runway_reservation) {
		if (tile == INVALID_TILE || !IsTileType(tile, TileType::Station)) continue;
		Tile t(tile);
		if (!IsAirportTile(t)) continue;
		if (IsModularAirportTileReservedBy(tile, v->index)) {
			ClearModularAirportTileReservation(tile);
		}
	}
	v->modular_runway_reservation.clear();
}

void ClearModularAirportReservationsByVehicle(const Station *st, VehicleID vid, TileIndex keep_tile)
{
	if (st == nullptr || st->airport.modular_tile_data == nullptr) return;

	for (const ModularAirportTileData &data : *st->airport.modular_tile_data) {
		if (data.tile == keep_tile) continue;
		Tile t(data.tile);
		if (!IsAirportTile(t)) continue;
		if (IsModularAirportTileReservedBy(data.tile, vid)) {
			ClearModularAirportTileReservation(data.tile);
		}
	}
}

/**
 * Teleport all ground aircraft on a modular airport tile to the nearest hangar.
 * Used when a tile is being removed from under an aircraft.
 * @param tile The tile being removed.
 * @param st The station.
 * @param execute If true, actually perform the teleport. If false, just check feasibility.
 * @return True if there are no aircraft to move, or if all can be teleported to a hangar.
 */
bool TeleportAircraftOnModularTile(TileIndex tile, Station *st, bool execute)
{
	/* Collect primary aircraft on this tile (same pattern as IsModularTileOccupiedByOtherAircraft). */
	std::vector<Aircraft *> to_teleport;
	for (Aircraft *a : Aircraft::Iterate()) {
		if (!a->IsNormalAircraft()) continue;
		if (a->tile != tile) continue;
		to_teleport.push_back(a);
	}

	if (to_teleport.empty()) return true;

	/* Find a hangar to teleport to (must not be the tile being removed). */
	TileIndex hangar = INVALID_TILE;
	if (st->airport.modular_tile_data != nullptr) {
		for (const ModularAirportTileData &data : *st->airport.modular_tile_data) {
			if (data.tile == tile) continue;
			if (!IsModularHangarPiece(data.piece_type)) continue;
			hangar = data.tile;
			break;
		}
	}

	if (hangar == INVALID_TILE) return false;

	if (!execute) return true;

	/* Actually teleport each aircraft to the hangar. */
	for (Aircraft *v : to_teleport) {
		ClearTaxiPathState(v);
		ClearModularRunwayReservation(v);
		ClearModularAirportReservationsByVehicle(st, v->index);
		v->landing_chain_path.reset();

		v->ground_path_goal = INVALID_TILE;
		v->modular_ground_target = MGT_NONE;
		v->modular_landing_tile = INVALID_TILE;
		v->modular_landing_goal = INVALID_TILE;
		v->modular_takeoff_tile = INVALID_TILE;
		v->modular_takeoff_progress = 0;
		v->modular_holding_wp_index = UINT32_MAX;

		/* Clear airborne-transient flags: the aircraft is parked in a hangar now.
		 * HelicopterDirectDescent matters most -- CmdStartStopVehicle reads it as a
		 * second "is in flight" condition independent of state, so a helicopter
		 * teleported mid-descent would be left permanently impossible to start.
		 * DestinationTooFar is deliberately left alone: it describes the order
		 * rather than the position, and is re-evaluated with its news item. */
		v->flags.Reset(VehicleAirFlag::HelicopterDirectDescent);
		v->flags.Reset(VehicleAirFlag::InMaximumHeightCorrection);
		v->flags.Reset(VehicleAirFlag::InMinimumHeightCorrection);

		/* Move to hangar tile. */
		int hx = TileX(hangar) * TILE_SIZE + TILE_SIZE / 2;
		int hy = TileY(hangar) * TILE_SIZE + TILE_SIZE / 2;
		int hz = GetTileMaxPixelZ(hangar);

		v->tile = hangar;
		v->targetairport = st->index;
		v->state = HANGAR;
		v->pos = v->previous_pos = 0;
		SetAircraftPosition(v, hx, hy, hz);
		VehicleEnterDepot(v);

		Debug(misc, 1, "[ModAp] Teleported vehicle {} from removed tile {} to hangar {}, state reset to HANGAR",
			v->index, tile.base(), hangar.base());
	}

	return true;
}

bool ShouldLogModularRateLimited(VehicleID vid, uint8_t channel, uint32_t interval_ticks)
{
	const uint32_t key = (uint32_t(vid.base()) << 8) | channel;
	const uint64_t now = TimerGameTick::counter;
	auto it = _rate_limit_last_tick.find(key);
	if (it != _rate_limit_last_tick.end() && now - it->second < interval_ticks) return false;
	_rate_limit_last_tick[key] = now;
	return true;
}

static std::string_view GetModularAirportDebugName(const Station *st)
{
	static const std::string unknown_name = "(unknown)";
	if (st == nullptr) return unknown_name;
	return st->GetCachedName();
}

static void SortAndUniqueTiles(std::vector<TileIndex> &tiles)
{
	std::sort(tiles.begin(), tiles.end(), [](TileIndex a, TileIndex b) { return a.base() < b.base(); });
	tiles.erase(std::unique(tiles.begin(), tiles.end()), tiles.end());
}

static bool ContainsSortedTile(std::span<const TileIndex> tiles, TileIndex tile)
{
	const auto cmp = [](TileIndex a, TileIndex b) { return a.base() < b.base(); };
	return std::binary_search(tiles.begin(), tiles.end(), tile, cmp);
}

static bool GetTakeoffRunwayResourceTiles(const Aircraft *v, const Station *st, std::vector<TileIndex> &tiles)
{
	tiles.clear();
	if (v == nullptr || st == nullptr) return false;
	if (!IsValidTile(v->modular_takeoff_tile)) return false;
	if (!GetContiguousModularRunwayTiles(st, v->modular_takeoff_tile, tiles)) return false;
	if (tiles.empty()) return false;
	SortAndUniqueTiles(tiles);
	return true;
}

bool ShouldRetainRunwayReservation(const Aircraft *v, const Station *st)
{
	if (v == nullptr || st == nullptr) return false;
	if (v->modular_runway_reservation.empty()) return false;
	if (v->modular_ground_target != MGT_RUNWAY_TAKEOFF && v->state != TAKEOFF && v->state != STARTTAKEOFF && v->state != ENDTAKEOFF) return false;

	std::vector<TileIndex> intended_runway;
	if (!GetTakeoffRunwayResourceTiles(v, st, intended_runway)) return false;

	std::vector<TileIndex> tracked = v->modular_runway_reservation;
	SortAndUniqueTiles(tracked);
	return tracked == intended_runway;
}

static bool IsServiceStyleGroundPiece(uint8_t piece_type)
{
	return piece_type == APT_STAND || piece_type == APT_STAND_1 ||
			IsModularHangarPiece(piece_type) || IsModularHelipadPiece(piece_type);
}

/* A "safe stop" is a tile an aircraft may wait on indefinitely without pinning a
 * shared transit resource: a stand/hangar/helipad or a one-way taxiway queue tile.
 * Free-move apron/grass and runways are transit-only and are never safe stops.
 *
 * Parking -- stands and helipads -- is the exception, and it is why @p goal exists. Routes
 * may cross a stand when that is the only way through, but crossing is not the same as
 * stopping: an aircraft that ends its reservation horizon on somebody else's parking tile
 * sits there and takes it out of service for whoever it was meant for. Passing @p goal
 * makes parking count only for the aircraft actually going to it. Hangars are
 * multi-capacity, so they never have this problem and stay unconditional.
 *
 * @param goal The aircraft's destination. INVALID_TILE accepts any parking tile and exists
 *             only for callers that have no goal to offer; route planning always has one
 *             and must pass it. */
bool IsModularSafeStopTile(const Station *st, TileIndex tile, TileIndex goal)
{
	const ModularAirportTileData *td = st->airport.GetModularTileData(tile);
	if (td == nullptr) return false;
	if (IsServiceStyleGroundPiece(td->piece_type)) {
		const bool is_parking = td->piece_type == APT_STAND || td->piece_type == APT_STAND_1 ||
				IsModularHelipadPiece(td->piece_type);
		return !is_parking || !IsValidTile(goal) || tile == goal;
	}
	if (IsTaxiwayPiece(td->piece_type) && td->one_way_taxi) return true;
	return false;
}

static bool IsPathTileRunwayPiece(const Station *st, TileIndex tile)
{
	const ModularAirportTileData *data = st->airport.GetModularTileData(tile);
	return data != nullptr && IsModularRunwayPiece(data->piece_type);
}

/** Outcome of building the forward reservation horizon to the next safe stop. */
enum class ForwardPlanStatus : uint8_t {
	Ok,
	ResourceError,
	NoSafeStop,
};

/**
 * One atomic forward reservation transaction.
 *
 * Taxi tiles include ordinary apron/taxiway tiles and runway tiles crossed in
 * transit. Only the explicitly identified landing/takeoff runway expands to a
 * whole contiguous runway resource. This lets independent ground movements cross
 * different parts of a runway while a real flight operation still excludes every
 * crossing on that runway.
 */
struct ForwardReservationPlan {
	std::vector<TileIndex> taxi_tiles;
	std::vector<TileIndex> operation_runway_tiles;
	TileIndex safe_stop = INVALID_TILE;
};

static TileIndex GetGroundOperationRunwayTile(const Aircraft *v, const Station *st)
{
	if (v == nullptr || st == nullptr) return INVALID_TILE;

	if (v->modular_ground_target == MGT_RUNWAY_TAKEOFF && IsValidTile(v->modular_takeoff_tile)) {
		return v->modular_takeoff_tile;
	}

	/* After touchdown the landing tile is cleared before the aircraft has left its
	 * runway. The tracked whole-runway claim identifies that operation until the
	 * aircraft physically steps onto another resource. Transit crossings are tracked
	 * in taxi_reserved_tiles and therefore never enter this branch. */
	if (IsValidTile(v->tile) && IsPathTileRunwayPiece(st, v->tile) &&
			std::find(v->modular_runway_reservation.begin(), v->modular_runway_reservation.end(), v->tile) !=
					v->modular_runway_reservation.end()) {
		return v->tile;
	}

	return INVALID_TILE;
}

/**
 * Build the claims needed from @p start_index through the first future safe stop.
 * The current tile is part of the plan, but cannot terminate it merely by already
 * being a safe stop. The path goal always terminates, including a runway goal.
 */
static ForwardPlanStatus BuildForwardReservationPlan(const Aircraft *v, const Station *st,
		const TaxiPath *path, uint16_t start_index, TileIndex goal, TileIndex operation_runway,
		ForwardReservationPlan &out)
{
	out = ForwardReservationPlan{};
	if (v == nullptr || st == nullptr || path == nullptr || !path->valid ||
			path->tiles.empty() || start_index >= path->tiles.size()) {
		return ForwardPlanStatus::ResourceError;
	}

	std::vector<TileIndex> operation_resource_tiles;
	if (IsValidTile(operation_runway)) {
		if (!GetContiguousModularRunwayTiles(st, operation_runway, operation_resource_tiles) ||
				operation_resource_tiles.empty()) {
			return ForwardPlanStatus::ResourceError;
		}
		SortAndUniqueTiles(operation_resource_tiles);
	}

	for (size_t i = start_index; i < path->tiles.size(); ++i) {
		const TileIndex tile = path->tiles[i];
		const bool on_operation_runway = ContainsSortedTile(operation_resource_tiles, tile);
		if (on_operation_runway) {
			/* Acquire the operation runway only when this reservation horizon actually
			 * reaches it. An upstream one-way queue remains useful precisely because it
			 * lets the runway serve somebody else until this aircraft advances. */
			if (out.operation_runway_tiles.empty()) out.operation_runway_tiles = operation_resource_tiles;
		} else {
			out.taxi_tiles.push_back(tile);
		}

		/* The goal is an aircraft property, not a tile property, and may legitimately
		 * be the starting tile (direct helicopter touchdown, already-at-goal rebuild). */
		if (IsValidTile(goal) && tile == goal) {
			out.safe_stop = tile;
			SortAndUniqueTiles(out.taxi_tiles);
			return ForwardPlanStatus::Ok;
		}

		/* Do not let the aircraft's present queue/stand tile terminate a departure
		 * plan without advancing. Every later safe stop is a valid horizon. */
		if (i > start_index && IsModularSafeStopTile(st, tile, goal)) {
			out.safe_stop = tile;
			SortAndUniqueTiles(out.taxi_tiles);
			return ForwardPlanStatus::Ok;
		}
	}

	return ForwardPlanStatus::NoSafeStop;
}

static bool ValidateForwardReservationPlan(const Aircraft *v, const Station *st,
		const ForwardReservationPlan &plan, TaxiReserveResult *out);
static bool TryCommitForwardReservationPlan(Aircraft *v, const Station *st,
		const ForwardReservationPlan &plan, TaxiReserveResult *out, bool log_success);

/**
 * How much longer an alternative route may be than the shortest one, in tiles. Only
 * consulted when MODULAR_MAX_ROUTE_ATTEMPTS allows a second route at all.
 *
 * This caps the *whole* route, not just the reservation horizon. The horizon ends at the
 * first safe stop, so capping it only bounds the distance to the next queue tile and lets
 * everything past it grow without limit -- which is not what "go a couple of tiles out of
 * your way" means. Rerouting costs the aircraft the extra taxi distance and costs everyone
 * else the shared tiles it holds while covering it, so the budget stays small: take a
 * genuinely parallel route, never a scenic one.
 *
 * Detours change length in even steps, so this is a budget of three steps rather than six.
 * One step is not enough. Excluding a busy transit runway takes the whole contiguous strip
 * out of the search -- crossing the same runway two tiles along is refused for the same
 * reason -- so the alternative has to reach the far side by another runway entirely, which
 * is rarely within one step. At a budget of one step those routes were found and then
 * discarded unvalidated, and the aircraft waited as though nothing had been tried: T7d
 * measured 0 detours longer than one step, over 3656 in two years. Two steps admits them
 * and they stay rare (103 of 3803, 2.7%), which is what the second step bought.
 *
 * The third step buys reach around a blocked runway exit. A four-exit runway whose nearer
 * exits are occupied can leave the only usable one six tiles out; at two steps that route
 * was built and thrown away unvalidated, so the airport refused every arrival while an
 * empty exit stood open. Reaching it is worth a measurable but small cost, and a player
 * who wants the shorter routing back can fence the long way round.
 *
 * Measured on T7d over five years, 2026-08-29: 27001 movements at a budget of 4, 26974 at
 * 6, 26413 at 8, while diverts rise 8109 -> 8253 -> 8554. So the third step is nearly free
 * (-27, -0.1%) and the fourth is not (-2.2%). The extra length is spent rather than
 * ignored, and past a point throughput falls as it is: no single aircraft's route gets
 * worse, but more of them hold shared tiles for longer, which is the same emergent
 * contention that cost 8% when the ground-pathfinder heuristic changed. Do not read the
 * gentle step from 4 to 6 as a licence to keep going -- 8 is where it turns.
 *
 * Loosening this for landings alone was tried and is worse than loosening it for
 * everything: -554 on T7d against -27, for 4% fewer refusals at the airport that prompted
 * it. The landing case is not special enough to justify its own budget.
 *
 * This is a busy decision, not a rare one -- the same run logs 14136 rate-limited
 * detour-capped fires -- so treat a change here as a throughput change and measure T7d.
 */
static constexpr int MAX_ROUTE_DETOUR_TILES = 6;

/** One route plus the horizon it would claim. @see FindReservableRoute */
struct ReservableRoute {
	TaxiPath path;                ///< Last route considered; usable even when !found.
	ForwardReservationPlan plan;  ///< Horizon for @c path; meaningful only when found.
	bool found = false;           ///< Whether @c plan validated against live state.
	TaxiReserveResult deny;       ///< Why the last attempt failed, when !found.
};

/**
 * Find a route whose reservation horizon can be claimed right now, trying alternatives.
 *
 * The shortest route is tried first. When it is refused by a tile another aircraft holds,
 * that tile is banned and the search runs again -- which is what lets an aircraft take a
 * second exit off a runway instead of waiting for the first. The ban is scoped to the
 * reservation horizon inside the pathfinder (see FindAirportGroundPath), so an alternative
 * that rejoins the blocked tile beyond the first safe stop is still allowed: nothing claims
 * that far, so nothing there can block entry.
 *
 * Retrying helps against a tile somebody else holds, and against a transit runway held for
 * somebody else's operation -- the latter is excluded as one contiguous strip, because
 * crossing the same busy runway two tiles further along is refused for the same reason.
 * The aircraft's own operation runway, a plan that reaches no safe stop, and a blocked goal
 * are all properties of the goal rather than of the route, so they end the search for the
 * caller to answer by picking a different goal.
 *
 * @param v            Aircraft the reservation is for.
 * @param st           Station.
 * @param origin       Where the route starts (aircraft tile, or a landing rollout point).
 * @param goal         Goal tile.
 * @param operation_runway Runway claimed whole for an explicit landing/takeoff, else INVALID_TILE.
 * @param allow_runway_goal_crossing Passed through to the pathfinder.
 * @param restriction  Aircraft-type routing restriction.
 * @param path_v       Aircraft passed to the pathfinder for stand avoidance; deliberately
 *                     nullptr where occupancy must be ignored (pre-touchdown planning).
 * @return The route; check @c found.
 */
static ReservableRoute FindReservableRoute(const Aircraft *v, const Station *st,
		TileIndex origin, TileIndex goal, TileIndex operation_runway,
		bool allow_runway_goal_crossing, GroundPathRestriction restriction,
		const Aircraft *path_v, uint8_t max_attempts = MODULAR_MAX_ROUTE_ATTEMPTS)
{
	ReservableRoute out;
	if (v == nullptr || st == nullptr || !IsValidTile(origin) || !IsValidTile(goal)) {
		out.deny = TaxiReserveResult{TaxiReserveFailure::NoPath, goal};
		return out;
	}

	std::vector<TileIndex> avoid;
	TaxiPath shortest;
	int shortest_length = INT_MAX;
	for (uint8_t attempt = 0; attempt < max_attempts; ++attempt) {
		TaxiPath candidate = BuildTaxiPath(st, origin, goal, path_v, allow_runway_goal_crossing, restriction, avoid);
		if (!candidate.valid || candidate.tiles.empty()) {
			if (attempt == 0) {
				out.deny = TaxiReserveResult{TaxiReserveFailure::NoPath, goal};
				return out;
			}
			break;
		}
		if (attempt == 0) shortest = candidate;
		out.path = std::move(candidate);

		ForwardReservationPlan plan;
		switch (BuildForwardReservationPlan(v, st, &out.path, 0, goal, operation_runway, plan)) {
			case ForwardPlanStatus::Ok:
				break;
			case ForwardPlanStatus::ResourceError:
				out.deny = TaxiReserveResult{TaxiReserveFailure::RunwayResourceError,
						IsValidTile(operation_runway) ? operation_runway : goal};
				return out;
			case ForwardPlanStatus::NoSafeStop:
				out.deny = TaxiReserveResult{TaxiReserveFailure::NoSafeStop, goal};
				return out;
		}

		const int this_length = static_cast<int>(out.path.tiles.size());
		if (attempt == 0) {
			shortest_length = this_length;
		} else if (this_length > shortest_length + MAX_ROUTE_DETOUR_TILES) {
			/* Every further ban only makes the route longer, so stop rather than keep looking.
			 *
			 * Logged because this and the attempt budget are the two ways the alternate-route
			 * search gives up, and without a line here they are indistinguishable from the
			 * outside: the caller reports the same refusal either way, so an airport whose
			 * usable exit sits just past the budget looks exactly like one with no exit at
			 * all. Rate-limited like the landing-chain refusal it usually precedes -- a
			 * refused landing retries every tick it spends in an approach gate. */
			if (ShouldLogModularRateLimited(v->index, 45, 128)) {
				Debug(misc, 2, "[ModAp] V{} detour-capped: attempt={} origin={} goal={} len={} shortest_len={} budget={}",
						v->index, static_cast<int>(attempt), origin.base(), goal.base(),
						this_length, shortest_length, MAX_ROUTE_DETOUR_TILES);
			}
			break;
		}

		TaxiReserveResult deny;
		if (ValidateForwardReservationPlan(v, st, plan, &deny)) {
			if (attempt > 0) {
				Debug(misc, 2, "[ModAp] V{} diverted: attempt={} origin={} goal={} len={} shortest_len={} banned={}",
						v->index, static_cast<int>(attempt), origin.base(), goal.base(),
						this_length, shortest_length, avoid.size());
			}
			out.plan = std::move(plan);
			out.found = true;
			return out;
		}
		out.deny = deny;

		if (attempt + 1 >= max_attempts) break;
		const bool tile_contention = deny.reason == TaxiReserveFailure::ReservedByOther ||
				deny.reason == TaxiReserveFailure::OccupiedByOther;
		const bool busy_transit_runway = deny.reason == TaxiReserveFailure::RunwayBusy &&
				IsValidTile(deny.tile) && !ContainsSortedTile(plan.operation_runway_tiles, deny.tile);
		if (!tile_contention && !busy_transit_runway) {
			break;
		}
		/* Banning the goal, the tile we are standing on, or a tile of the operation runway
		 * cannot produce a usable route -- those are answered by choosing another goal. */
		if (!IsValidTile(deny.tile) || deny.tile == goal || deny.tile == origin) break;
		if (ContainsSortedTile(plan.operation_runway_tiles, deny.tile)) break;

		/* A runway held for an operation is busy as one contiguous resource. Ban all
		 * of it when it is only a transit crossing; banning the reported tile alone
		 * can spend every attempt rediscovering another crossing point on the same
		 * unusable runway instead of trying a genuinely independent route. */
		std::vector<TileIndex> denied_resource{deny.tile};
		if (busy_transit_runway &&
				(!GetContiguousModularRunwayTiles(st, deny.tile, denied_resource) || denied_resource.empty())) {
			break;
		}
		if (std::find(denied_resource.begin(), denied_resource.end(), origin) != denied_resource.end()) break;
		if (std::find(denied_resource.begin(), denied_resource.end(), goal) != denied_resource.end()) break;

		bool added = false;
		for (TileIndex tile : denied_resource) {
			if (std::find(avoid.begin(), avoid.end(), tile) != avoid.end()) continue;
			avoid.push_back(tile);
			added = true;
		}
		/* Nothing new means the ban did not move the route; stop rather than spin. */
		if (!added) break;
	}

	/* Nothing validated. Hand back the shortest route, not whichever detour was tried
	 * last: the caller waits on it, and waiting on the direct route is what the aircraft
	 * did before any of this existed. Leaving it committed to a long way round would be
	 * strictly worse than never having looked. */
	if (!out.found && shortest.valid) out.path = std::move(shortest);
	return out;
}

/** The stored paths of one aircraft, in the order the reservation code treats them. */
static constexpr std::array<std::unique_ptr<TaxiPath> Aircraft::*, 2> _modular_stored_paths = {
	&Aircraft::taxi_path,
	&Aircraft::landing_chain_path,
};

/** Index of the first tile of @p member that @p v has not already traversed. */
static size_t ModularPathStartIndex(const Aircraft *v, std::unique_ptr<TaxiPath> Aircraft::*member)
{
	/* Only taxi_path is walked in place. A landing chain is not traversed at all until it
	 * is handed to taxi_path whole at rollout, so all of it is still ahead. */
	return member == &Aircraft::taxi_path ? v->taxi_path_index : 0;
}

/**
 * The modular airport a stored path is currently running through.
 *
 * Deliberately neither v->targetairport nor simply tiles.front():
 *
 * - Not targetairport, because a landing aborted while airborne -- a zeppeliner wreck over
 *   the airport, say -- leaves landing_chain_path in place and clears only
 *   modular_landing_tile, and order processing then moves targetairport on to the next
 *   destination while the aircraft is still FLYING.
 * - Not tiles.front(), because a path keeps the tiles already behind the aircraft, while
 *   demolition refuses only tiles an aircraft is standing on. A player may therefore remove
 *   the start of a route that is still being taxied.
 *
 * So scan for the first tile that is still part of a modular airport, preferring the part of
 * the route the aircraft has yet to travel.
 * @param path The path, which may be null or invalid.
 * @param from Index of the first tile not yet traversed.
 * @return The airport, or nullptr when no tile of the route is part of one any more.
 */
static const Station *GetModularPathStation(const TaxiPath *path, size_t from)
{
	if (path == nullptr || !path->valid) return nullptr;

	const Station *behind = nullptr;
	for (size_t i = 0; i < path->tiles.size(); ++i) {
		const TileIndex tile = path->tiles[i];
		if (!IsValidTile(tile) || !IsTileType(tile, TileType::Station) || !IsAirport(tile)) continue;

		const Station *st = Station::GetByTile(tile);
		if (st == nullptr || !st->airport.blocks.Test(AirportBlock::Modular)) continue;
		if (st->airport.modular_tile_data == nullptr) continue;

		if (i >= from) return st;
		if (behind == nullptr) behind = st;
	}
	return behind;
}

/**
 * Re-derive the segment classification of every path that runs through @p st.
 *
 * Segment types are a layout-derived cache that happens to live on the aircraft rather
 * than on the Airport, so a layout change that alters what a tile classifies as leaves
 * them stale exactly the way MarkLayoutDirty exists to prevent for the caches on Airport.
 * A stale type reaches real decisions -- notably whether the aircraft is still on a runway
 * it must clear -- so it has to be refreshed at the moment of the edit.
 *
 * The routes themselves are deliberately left alone. Rerouting an aircraft mid-taxi is a
 * reservation decision with its own contract; these tiles are the ones it currently holds.
 * @param st The station whose layout just changed.
 */
void RefreshModularAircraftPathSegments(const Station *st)
{
	if (st == nullptr || st->airport.modular_tile_data == nullptr) return;

	for (Aircraft *v : Aircraft::Iterate()) {
		if (!v->IsNormalAircraft()) continue;
		for (auto member : _modular_stored_paths) {
			const std::unique_ptr<TaxiPath> &path = v->*member;
			if (GetModularPathStation(path.get(), ModularPathStartIndex(v, member)) != st) continue;
			path->segments = ClassifyTaxiSegments(st, path->tiles);
		}
	}
}

/**
 * Rebuild the segment classification of every path restored from a savegame.
 *
 * Only the route is saved (see SlVehicleAircraftPath), so this is what completes the load.
 * Deriving instead of trusting also means a path is classified against the layout as it is
 * now, not as it was when the path was computed, and that every client loading the same
 * save derives the same segments from the same saved layout.
 */
void RestoreModularAircraftPathSegments()
{
	for (Aircraft *v : Aircraft::Iterate()) {
		if (!v->IsNormalAircraft()) continue;

		for (auto member : _modular_stored_paths) {
			std::unique_ptr<TaxiPath> &path = v->*member;
			if (path == nullptr || !path->valid) continue;

			const Station *st = GetModularPathStation(path.get(), ModularPathStartIndex(v, member));
			if (st == nullptr) {
				/* No tile of the route is part of a modular airport any more, so there is
				 * nothing to classify against -- and a valid path must have segments to be
				 * walked at all. Individual tiles that have left the layout need no such
				 * treatment: they classify as FreeMove, here and on the server alike. */
				Debug(sl, 1, "Aircraft {} has a modular path with no airport under it, ignoring.", v->index);
				path.reset();
				continue;
			}
			path->segments = ClassifyTaxiSegments(st, path->tiles);
		}
	}
}

void BuildReservationKeepSet(const Aircraft *v, const Station *st, std::vector<TileIndex> &keep_set)
{
	keep_set.clear();
	if (v == nullptr || st == nullptr || st->airport.modular_tile_data == nullptr) return;

	if (IsValidTile(v->tile) && st->TileBelongsToAirport(v->tile)) keep_set.push_back(v->tile);

	/* An aircraft standing somewhere it may not wait keeps everything it holds.
	 *
	 * Retention is otherwise justified by a path -- the active taxi_path or the stored
	 * landing_chain_path. A landing committed through the no-ground-goal branch has
	 * neither: it reserves a runway plus a one-way buffer to queue on and deliberately
	 * resets the path. Nothing then justified the buffer, so the very next reconcile
	 * released it, and the aircraft arrived at the rollout end owning nothing -- on a
	 * runway, with the guarantee that permitted the landing already thrown away.
	 *
	 * Landing is only allowed against a reserved route to a safe stop, so until the
	 * aircraft is actually standing on one, that route is what makes its position
	 * legal and it is never ours to reclaim. Normal reconciliation resumes the moment
	 * it reaches a safe stop. */
	if (IsValidTile(v->tile) && st->TileBelongsToAirport(v->tile) && IsPathTileRunwayPiece(st, v->tile)) {
		for (TileIndex tile : v->taxi_reserved_tiles) {
			if (IsValidTile(tile) && IsModularSafeStopTile(st, tile, v->ground_path_goal)) keep_set.push_back(tile);
		}
	}

	/* Reservation and retention use the same forward-horizon description. This is
	 * what prevents a later keep pass from either dropping a claim the entry contract
	 * required or resurrecting a runway resource already behind the aircraft. */
	if (v->taxi_path != nullptr && v->taxi_path->valid &&
			v->taxi_path_index < v->taxi_path->tiles.size()) {
		ForwardReservationPlan plan;
		const TileIndex operation_runway = GetGroundOperationRunwayTile(v, st);
		if (BuildForwardReservationPlan(v, st, v->taxi_path.get(), v->taxi_path_index,
				v->ground_path_goal, operation_runway, plan) == ForwardPlanStatus::Ok) {
			keep_set.insert(keep_set.end(), plan.taxi_tiles.begin(), plan.taxi_tiles.end());
			keep_set.insert(keep_set.end(), plan.operation_runway_tiles.begin(), plan.operation_runway_tiles.end());
		}
	}

	if (ShouldRetainRunwayReservation(v, st)) {
		for (TileIndex tile : v->modular_runway_reservation) keep_set.push_back(tile);
	}

	if (v->landing_chain_path != nullptr && v->landing_chain_path->valid &&
			(v->modular_landing_tile != INVALID_TILE || v->modular_ground_target == MGT_ROLLOUT ||
			(IsValidTile(v->tile) && !v->landing_chain_path->tiles.empty() && v->landing_chain_path->tiles.front() == v->tile))) {
		/* Keep landing-chain continuity until taxi_path transitions are complete. */
		for (TileIndex tile : v->landing_chain_path->tiles) keep_set.push_back(tile);
		for (TileIndex tile : v->modular_runway_reservation) keep_set.push_back(tile);
	}

	SortAndUniqueTiles(keep_set);
}

void ReconcileAircraftReservations(Aircraft *v, const Station *st, std::span<const TileIndex> keep_set, const char *reason)
{
	if (v == nullptr || st == nullptr || st->airport.modular_tile_data == nullptr) return;

	std::vector<TileIndex> sorted_keep(keep_set.begin(), keep_set.end());
	SortAndUniqueTiles(sorted_keep);

	/* Release every map reservation bit the aircraft owns that is not in the
	 * keep-set. The aircraft's own reservation vectors are authoritative for
	 * which map bits it owns -- every setter (SetTaxiReservation,
	 * TryReserveContiguousModularRunway, TryCommitForwardReservationPlan) records
	 * the tile here -- so we walk those vectors instead of scanning the whole
	 * airport, which makes per-step reconcile O(reserved) rather than O(tiles). */
	uint16_t released = 0;
	const auto release_if_unwanted = [&](TileIndex tile) {
		if (tile == INVALID_TILE || ContainsSortedTile(sorted_keep, tile)) return;
		Tile t(tile);
		if (!IsAirportTile(t) || !IsModularAirportTileReservedBy(tile, v->index)) return;
		ClearModularAirportTileReservation(tile);
		released++;
	};
	for (TileIndex tile : v->taxi_reserved_tiles) release_if_unwanted(tile);
	for (TileIndex tile : v->modular_runway_reservation) release_if_unwanted(tile);

	std::erase_if(v->taxi_reserved_tiles, [&](TileIndex tile) { return !ContainsSortedTile(sorted_keep, tile); });
	std::erase_if(v->modular_runway_reservation, [&](TileIndex tile) { return !ContainsSortedTile(sorted_keep, tile); });

	if (_debug_misc_level >= 2 && ShouldLogModularRateLimited(v->index, 60, 64)) {
		Debug(misc, 2, "[ModAp] V{} reserve-v2 keep-set size={} release={} reason={}",
			v->index, sorted_keep.size(), released, reason != nullptr ? reason : "n/a");
	}
}

bool TryReserveContiguousModularRunway(Aircraft *v, const Station *st, TileIndex runway_tile, bool append_to_existing)
{
	std::vector<TileIndex> runway_tiles;
	if (!GetContiguousModularRunwayTiles(st, runway_tile, runway_tiles)) return false;
	SortAndUniqueTiles(runway_tiles);

	const auto clear_if_stale = [&]() {
		/* In append mode (e.g. landing chain acquiring rollout + transit runways),
		 * the caller owns rollback so we must not touch pre-existing tracking. */
		if (append_to_existing) return;
		if (v->modular_runway_reservation.empty()) return;
		std::vector<TileIndex> tracked = v->modular_runway_reservation;
		SortAndUniqueTiles(tracked);
		if (tracked != runway_tiles) ClearModularRunwayReservation(v);
	};

	VehicleID state_blocker = VehicleID::Invalid();
	if (IsContiguousModularRunwayReservedInStateByOther(v, st, runway_tiles, &state_blocker)) {
		if (ShouldLogModularRateLimited(v->index, 1, 128)) {
			Debug(misc, 2, "[ModAp] V{} runway-reserve denied: runway held in state by V{}", v->index, state_blocker.base());
		}
		clear_if_stale();
		if (ShouldLogModularRateLimited(v->index, 2, 128)) {
			LogModularVehicleReservationState(st, v, "reserve denied (state-held)");
		}
		return false;
	}

	for (TileIndex tile : runway_tiles) {
		/* Reservation must fail if any other aircraft is physically on the runway,
		 * even when reservation flags are temporarily missing/desynced. */
		if (IsModularTileOccupiedByOtherAircraft(st, tile, v->index)) {
			if (ShouldLogModularRateLimited(v->index, 1, 128)) {
				Debug(misc, 2, "[ModAp] V{} runway-reserve denied: runway tile {} occupied by other aircraft", v->index, tile.base());
			}
			clear_if_stale();
			if (ShouldLogModularRateLimited(v->index, 2, 128)) {
				LogModularVehicleReservationState(st, v, "reserve denied (occupied)");
			}
			return false;
		}

		Tile t(tile);
		if (!IsAirportTile(t)) continue;
		VehicleID reserver = GetModularAirportTileReservationOwner(tile);
		if (HasModularAirportTileReservation(tile) && reserver != v->index) {
			if (ShouldLogModularRateLimited(v->index, 1, 128)) {
				Debug(misc, 2, "[ModAp] V{} runway-reserve denied: runway tile {} reserved by V{}", v->index, tile.base(), reserver.base());
			}
			clear_if_stale();
			if (ShouldLogModularRateLimited(v->index, 2, 128)) {
				LogModularVehicleReservationState(st, v, "reserve denied (reserved)");
			}
			return false;
		}
	}

	const bool reservation_changed = (v->modular_runway_reservation != runway_tiles);
	if (reservation_changed && !append_to_existing) {
		ClearModularRunwayReservation(v);
	}

	for (TileIndex tile : runway_tiles) {
		Tile t(tile);
		if (!IsAirportTile(t)) continue;
		SetModularAirportTileReservationOwner(tile, v->index);
	}
	v->modular_runway_reservation = std::move(runway_tiles);
	if (reservation_changed && ShouldLogModularRateLimited(v->index, 32, 16)) {
		LogModularVehicleReservationState(st, v, "reserve granted");
	}
	return true;
}

bool IsContiguousModularRunwayReservedByOther(const Aircraft *v, const Station *st, TileIndex runway_tile)
{
	std::vector<TileIndex> runway_tiles;
	if (!GetContiguousModularRunwayTiles(st, runway_tile, runway_tiles)) return false;

	for (TileIndex tile : runway_tiles) {
		Tile t(tile);
		if (!IsAirportTile(t)) continue;
		if (HasModularAirportTileReservation(tile) && GetModularAirportTileReservationOwner(tile) != v->index) return true;
	}

	return false;
}

bool IsContiguousModularRunwayBusyByOther(const Aircraft *v, const Station *st, TileIndex runway_tile)
{
	std::vector<TileIndex> runway_tiles;
	if (!GetContiguousModularRunwayTiles(st, runway_tile, runway_tiles)) return false;

	for (TileIndex tile : runway_tiles) {
		if (IsModularTileOccupiedByOtherAircraft(st, tile, v->index)) return true;
		if (IsTaxiTileReservedByOther(st, tile, v->index)) return true;
	}

	return false;
}

bool IsContiguousModularRunwayReservedInStateByOther(const Aircraft *v, const Station *st, std::span<const TileIndex> runway_tiles, VehicleID *blocker)
{
	return ForEachModularRunwayStateCandidate([&](const Aircraft *other) {
		if (other->index == v->index) return false;
		if (!other->IsNormalAircraft()) return false;

		const bool tied_to_station = (other->targetairport == st->index || other->last_station_visited == st->index);
		if (!tied_to_station) return false;

		const ModularAirportTileData *other_tile_data = (IsValidTile(other->tile) ? st->airport.GetModularTileData(other->tile) : nullptr);
		const bool other_on_runway = (other_tile_data != nullptr && IsModularRunwayPiece(other_tile_data->piece_type));

		const bool in_runway_flow = IsModularRunwayFlowState(other->state) ||
				(other->modular_ground_target == MGT_ROLLOUT && other_on_runway);
		if (!in_runway_flow) return false;

		bool overlaps = false;
		for (TileIndex tile : other->modular_runway_reservation) {
			if (std::find(runway_tiles.begin(), runway_tiles.end(), tile) != runway_tiles.end()) {
				overlaps = true;
				break;
			}
		}
		if (!overlaps && (other->state == LANDING || other->state == ENDLANDING ||
				other->state == HELILANDING || other->state == HELIENDLANDING) &&
				IsValidTile(other->modular_landing_tile)) {
			overlaps = std::find(runway_tiles.begin(), runway_tiles.end(), other->modular_landing_tile) != runway_tiles.end();
		}
		if (!overlaps && (other->state == TAKEOFF || other->state == STARTTAKEOFF || other->state == ENDTAKEOFF) &&
				IsValidTile(other->modular_takeoff_tile)) {
			overlaps = std::find(runway_tiles.begin(), runway_tiles.end(), other->modular_takeoff_tile) != runway_tiles.end();
		}

		if (overlaps) {
			if (blocker != nullptr) *blocker = other->index;
			return true;
		}
		return false;
	});
}

bool IsContiguousModularRunwayQueuedForTakeoffByOther(const Aircraft *v, const Station *st, TileIndex runway_tile)
{
	std::vector<TileIndex> runway_tiles;
	if (!GetContiguousModularRunwayTiles(st, runway_tile, runway_tiles)) return false;

	return ForEachModularRunwayStateCandidate([&](const Aircraft *other) {
		if (other->index == v->index) return false;
		if (!other->IsNormalAircraft()) return false;
		if (other->targetairport != st->index && other->last_station_visited != st->index) return false;
		if (other->modular_ground_target != MGT_RUNWAY_TAKEOFF) return false;
		if (!IsValidTile(other->modular_takeoff_tile)) return false;

		if (std::find(runway_tiles.begin(), runway_tiles.end(), other->modular_takeoff_tile) != runway_tiles.end()) {
			return true;
		}
		return false;
	});
}

TileIndex FindModularLandingGroundGoal(const Station *st, const Aircraft *v, uint8_t *target, TileIndex rollout_tile)
{
	TileIndex goal = INVALID_TILE;
	uint8_t tgt = MGT_NONE;

	/* Only look for a hangar if the aircraft actually needs one (depot order / servicing). */
	const bool wants_depot = ModularAircraftWantsHangar(v, st);

	if (wants_depot) {
		goal = FindFreeModularHangar(st, v, rollout_tile);
		if (goal != INVALID_TILE) tgt = MGT_HANGAR;
	}
	if (goal == INVALID_TILE && v->subtype == AIR_HELICOPTER) {
		goal = FindFreeModularHelipad(st, v, rollout_tile);
		if (goal != INVALID_TILE) tgt = MGT_HELIPAD;
	}
	if (goal == INVALID_TILE) {
		goal = FindFreeModularTerminal(st, v, rollout_tile);
		if (goal != INVALID_TILE) tgt = MGT_TERMINAL;
	}

	if (target != nullptr) *target = tgt;
	return goal;
}

bool TryReserveLandingChain(Aircraft *v, const Station *st, TileIndex runway_tile, TileIndex ground_goal)
{
	const ModularAirportTileData *touchdown_data = st->airport.GetModularTileData(runway_tile);
	const bool touchdown_on_runway = touchdown_data != nullptr && IsModularRunwayPiece(touchdown_data->piece_type);
	const bool heli_direct_ground = v->subtype == AIR_HELICOPTER;
	TileIndex rollout = touchdown_on_runway ? FindModularRunwayRolloutPoint(st, v, runway_tile) : INVALID_TILE;
	/* Fixed-wing aircraft reserve from the rollout point because they must stay on the
	 * runway through landing rollout. Helicopters hand off to ground movement directly
	 * from their touchdown tile, so their pre-landing chain must start there or the
	 * touchdown handoff will rebuild the path and reservation set. */
	const TileIndex chain_origin = (touchdown_on_runway && !heli_direct_ground) ? rollout : runway_tile;
	const auto log_chain_fail = [&](std::string_view reason, TileIndex detail = INVALID_TILE) {
		if (ShouldLogModularRateLimited(v->index, 43, 128)) {
			Debug(misc, 2, "[ModAp] V{} landing-chain fail: reason={} runway={} goal={} rollout={} detail={}",
				v->index, reason, runway_tile.base(),
				ground_goal == INVALID_TILE ? 0 : ground_goal.base(),
				rollout == INVALID_TILE ? 0 : rollout.base(),
				detail == INVALID_TILE ? 0 : detail.base());
		}
		return false;
	};

	if (touchdown_on_runway && !CanAircraftUseModularRunwayForLanding(st, v, runway_tile)) {
		return log_chain_fail("large_runway_required", runway_tile);
	}
	if (!IsValidTile(chain_origin)) return log_chain_fail("origin_invalid");

	/* With no concrete parking goal, plan toward any stand but admit the landing
	 * only when the common horizon reaches a one-way queue tile first. */
	TileIndex path_goal = ground_goal;
	if (!IsValidTile(path_goal)) {
		if (st->airport.modular_tile_data != nullptr) {
			for (const ModularAirportTileData &data : *st->airport.modular_tile_data) {
				if (data.piece_type == APT_STAND || data.piece_type == APT_STAND_1) {
					path_goal = data.tile;
					break;
				}
			}
		}
		if (!IsValidTile(path_goal)) return log_chain_fail("no_goal_no_stand");
	}

	/* Where the first exit off the runway is held by somebody else, take another one
	 * rather than refuse the landing and fly a whole extra lap. */
	const TileIndex operation_runway = touchdown_on_runway ? runway_tile : INVALID_TILE;
	ReservableRoute route = FindReservableRoute(v, st, chain_origin, path_goal, operation_runway,
			false, GetGroundPathRestriction(v), nullptr);
	if (!route.found) {
		return log_chain_fail(TaxiReserveFailureName(route.deny.reason), route.deny.tile);
	}

	if (!IsValidTile(ground_goal) && !IsOneWayTaxiTile(st, route.plan.safe_stop)) {
		return log_chain_fail("no_goal_no_one_way_buffer", route.plan.safe_stop);
	}

	TaxiReserveResult reserve_result;
	if (!TryCommitForwardReservationPlan(v, st, route.plan, &reserve_result, false)) {
		return log_chain_fail(TaxiReserveFailureName(reserve_result.reason), reserve_result.tile);
	}

	/* The planned route may open by carrying on along the landing runway to a further exit.
	 * Roll those tiles instead of taxiing them: the aircraft is already on the runway and
	 * moving, and holds the whole runway as one reservation either way, so taxiing them
	 * costs about 41 ticks apiece and looks wrong besides. Absorbing them also puts the
	 * turn-off on the exit the route actually uses rather than on the braking floor, which
	 * is what stops an aircraft overshooting an exit and then taxiing back up its own
	 * runway to reach it.
	 *
	 * A route that heads back toward touchdown is left alone. That is a real back-taxi to
	 * the only reachable exit, and it already starts from the floor -- the earliest the
	 * aircraft could have turned off -- which is the right place for it to start.
	 *
	 * Only the path is trimmed, never route.plan: the reservation covers the whole runway
	 * as a single resource, so the absorbed tiles stay held either way. */
	if (touchdown_on_runway && !heli_direct_ground && route.path.valid && route.path.tiles.size() > 1) {
		std::vector<TileIndex> runway_tiles;
		if (GetContiguousModularRunwayTiles(st, runway_tile, runway_tiles)) {
			const size_t none = runway_tiles.size();
			const auto runway_index = [&](TileIndex t) {
				const auto it = std::find(runway_tiles.begin(), runway_tiles.end(), t);
				return it == runway_tiles.end() ? none : static_cast<size_t>(std::distance(runway_tiles.begin(), it));
			};
			const size_t touchdown_index = runway_index(runway_tile);
			const size_t origin_index = runway_index(route.path.tiles.front());
			size_t absorb = 0;
			if (touchdown_index != none && origin_index != none) {
				const auto rolled_distance = [&](size_t i) {
					return i > touchdown_index ? i - touchdown_index : touchdown_index - i;
				};
				size_t prev_distance = rolled_distance(origin_index);
				for (size_t i = 1; i < route.path.tiles.size(); i++) {
					const size_t index = runway_index(route.path.tiles[i]);
					if (index == none) break;
					const size_t distance = rolled_distance(index);
					/* Strictly further from touchdown, so a route that turns back -- or
					 * crosses to the far side through the touchdown tile -- stops here. */
					if (distance <= prev_distance) break;
					absorb = i;
					prev_distance = distance;
				}
			}
			if (absorb > 0) {
				Debug(misc, 3, "[ModAp] V{} rollout extended {} tile(s) to exit {}",
					v->index, absorb, route.path.tiles[absorb].base());
				route.path.tiles.erase(route.path.tiles.begin(), route.path.tiles.begin() + absorb);
				route.path.segments = ClassifyTaxiSegments(st, route.path.tiles);
			}
		}
	}

	if (IsValidTile(ground_goal)) {
		v->landing_chain_path = std::make_unique<TaxiPath>(std::move(route.path));
		v->rollout_restored_from_save = false;
	} else {
		v->landing_chain_path.reset();
	}

	if (ShouldLogModularRateLimited(v->index, 44, 16)) {
		LogModularVehicleReservationState(st, v, "landing chain reserved");
	}
	return true;
}

TileIndex FindModularLandingTarget(const Station *st, const Aircraft *v)
{
	if (st->airport.modular_tile_data == nullptr) return INVALID_TILE;
	if (st->airport.blocks.Test(AirportBlock::Zeppeliner)) return INVALID_TILE;

	TileIndex best_tile = INVALID_TILE;
	int best_score = INT_MAX;

	bool is_heli = v->subtype == AIR_HELICOPTER;

	/* A helicopter heading for a hangar must touch down where it can taxi to one.
	 * Where no helipad on this airport can reach one -- a rooftop heliport has no
	 * taxiable neighbour at all -- landing on a pad strands it: it reaches neither
	 * the hangar nor a runway, lifts off vertically, and picks the same pad again
	 * on the next approach, forever. Treat the pads as absent for this trip and
	 * use the precomputed service tile instead. */
	const bool heli_wants_hangar = is_heli && ModularAircraftWantsHangar(v, st);
	bool filter_pads_by_hangar_access = false;
	if (heli_wants_hangar) {
		EnsureModularHeliTilesValid(st);
		/* No pad here reaches a hangar at all: land off the pads entirely. Where some
		 * pad does, the scan below keeps to those. */
		if (st->airport.modular_heli_service_tile != INVALID_TILE) {
			return st->airport.modular_heli_service_tile;
		}
		/* Filter only when there is something to filter down to. An empty set at this
		 * point means the service-tile search came up empty as well -- nothing anywhere on
		 * this airport reaches a hangar -- and filtering would then reject every pad and
		 * leave the helicopter circling for good. Landing is strictly better: arriving on
		 * a pad services it, which clears the very condition that sent it looking for a
		 * hangar. */
		filter_pads_by_hangar_access = !st->airport.modular_hangar_reachable_pads.empty();
	}
	int candidates_total = 0;
	int rejected_not_end = 0;
	int rejected_mode = 0;
	int rejected_direction = 0;
	int rejected_reserved = 0;
	int rejected_takeoff_queue = 0;
	int rejected_large_aircraft = 0;

	for (const ModularAirportTileData &data : *st->airport.modular_tile_data) {
		bool is_runway = IsModularRunwayPiece(data.piece_type);
		bool is_helipad = IsModularHelipadPiece(data.piece_type);

		if (!is_runway && !is_helipad) continue;

		if (is_heli) {
			if (is_runway) continue; /* Helicopters use helipads only; computed tile handles no-helipad airports */
			/* A helicopter heading for a hangar may only touch down on a pad it can taxi
			 * off toward one. Landing on a cut-off pad strands it: no route to the hangar,
			 * none to a runway either, so it lifts off vertically and scores the very same
			 * pad best again on the next approach. Filtering here rather than choosing one
			 * precomputed pad keeps several depot-bound helicopters spread across the
			 * usable ones. */
			if (filter_pads_by_hangar_access && !IsModularPadWithHangarAccess(st, data.tile)) continue;
			/* Skip helipads occupied or reserved by another aircraft so multiple helicopters
			 * spread across free helipads instead of all targeting the same one. */
			if (HasModularAirportTileReservation(data.tile) && GetModularAirportTileReservationOwner(data.tile) != v->index) continue;
			if (IsModularTileOccupiedByOtherAircraft(st, data.tile, v->index)) continue;
		} else {
			if (is_helipad) continue; /* Planes can't land on helipads */
		}

		/* Only consider runway ends as valid landing targets */
		if (is_runway) {
			candidates_total++;
			if (!IsModularRunwayEndPiece(data.piece_type)) {
				rejected_not_end++;
				continue;
			}

			/* Skip runways that are too short to be usable. */
			{
				std::vector<TileIndex> rwy;
				if (!GetContiguousModularRunwayTiles(st, data.tile, rwy) || (int)rwy.size() < MIN_RUNWAY_LENGTH_TILES) {
					continue;
				}
			}

			/* Check runway flags: is landing allowed? */
			uint8_t flags = GetRunwayFlags(st, data.tile);
			if (!(flags & RUF_LANDING)) {
				rejected_mode++;
				continue;
			}

			/* Check direction flags with travel-direction semantics.
			 * Landing at low end rolls toward high end, and vice versa. */
			bool is_low = IsRunwayEndLow(st, data.tile);
			if (is_low && !(flags & RUF_DIR_HIGH)) {
				rejected_direction++;
				continue;
			}
			if (!is_low && !(flags & RUF_DIR_LOW)) {
				rejected_direction++;
				continue;
			}

			/* A fast jet must use a large-safe runway whenever the airport has one.
			 * This is topological rather than occupancy-dependent: a busy long runway
			 * makes the jet hold, not fall back to a short strip. */
			if (!CanAircraftUseModularRunwayForLanding(st, v, data.tile)) {
				rejected_large_aircraft++;
				continue;
			}

			/* Avoid converging all arrivals onto one runway:
			 * if this runway is currently reserved by another aircraft,
			 * pick a different eligible runway instead. */
			if (v->subtype == AIR_AIRCRAFT && IsContiguousModularRunwayReservedByOther(v, st, data.tile)) {
				rejected_reserved++;
				continue;
			}

			/* Give priority to queued takeoffs to prevent landing starvation/deadlocks. */
			if (v->subtype == AIR_AIRCRAFT && IsContiguousModularRunwayQueuedForTakeoffByOther(v, st, data.tile)) {
				rejected_takeoff_queue++;
				continue;
			}

		}

		int cx = TileX(data.tile) * TILE_SIZE + TILE_SIZE / 2;
		int cy = TileY(data.tile) * TILE_SIZE + TILE_SIZE / 2;
		int dist_flight = abs(cx - v->x_pos) + abs(cy - v->y_pos);

		int score = dist_flight;

		/* Per-runway terminal scoring: find the nearest stand from this runway's
		 * rollout point, not a single global terminal. */
		if (is_runway) {
			TileIndex rollout = FindModularRunwayRolloutPoint(st, v, data.tile);
			TileIndex term_tile = FindFreeModularTerminal(st, v, rollout);
			if (term_tile != INVALID_TILE) {
				const TileIndex origin = IsValidTile(rollout) ? rollout : data.tile;
				int end_x = TileX(origin) * TILE_SIZE;
				int end_y = TileY(origin) * TILE_SIZE;
				int tx = TileX(term_tile) * TILE_SIZE;
				int ty = TileY(term_tile) * TILE_SIZE;
				int dist_taxi = abs(end_x - tx) + abs(end_y - ty);
				score += dist_taxi * 4;
			}
		} else {
			/* Helipads: prefer ones near a stand (cheap euclidean -- helicopters don't taxi,
			 * so an A*-aware probe like the runway path adds no information but is very
			 * expensive when called per helipad per flying tick). */
			int nearest_stand_dist = INT_MAX;
			for (const ModularAirportTileData &d2 : *st->airport.modular_tile_data) {
				if (d2.piece_type != APT_STAND && d2.piece_type != APT_STAND_1) continue;
				int sx = TileX(d2.tile) * TILE_SIZE;
				int sy = TileY(d2.tile) * TILE_SIZE;
				int d = abs(cx - sx) + abs(cy - sy);
				if (d < nearest_stand_dist) nearest_stand_dist = d;
			}
			if (nearest_stand_dist != INT_MAX) score += nearest_stand_dist * 4;
		}

		if (score < best_score) {
			best_score = score;
			best_tile = data.tile;
		}
	}

	if (best_tile == INVALID_TILE && !is_heli && ShouldLogModularRateLimited(v->index, 18, 128)) {
		Debug(misc, 2,
			"[ModAp] Vehicle {} no landing runway: runway_tiles={} reject_not_end={} reject_mode={} reject_dir={} reject_large={} reject_reserved={} reject_takeoff_queue={}",
			v->index, candidates_total, rejected_not_end, rejected_mode, rejected_direction,
			rejected_large_aircraft, rejected_reserved, rejected_takeoff_queue);
	}

	return best_tile;
}

bool IsModularHeliLandingTileAvailable(const Station *st, const Aircraft *v, TileIndex tile)
{
	if (tile == INVALID_TILE || st->airport.modular_tile_data == nullptr) return false;

	const ModularAirportTileData *data = st->airport.GetModularTileData(tile);
	if (data == nullptr) return false;

	if (IsTaxiTileReservedByOther(st, tile, v->index)) return false;
	if (IsModularTileOccupiedByOtherAircraft(st, tile, v->index)) return false;
	if (IsModularRunwayPiece(data->piece_type) && IsContiguousModularRunwayReservedByOther(v, st, tile)) return false;

	return true;
}

void GetModularLandingApproachPoint(const Station *st, TileIndex runway_tile, int *target_x, int *target_y)
{
	/* Default to runway tile center */
	*target_x = TileX(runway_tile) * TILE_SIZE + TILE_SIZE / 2;
	*target_y = TileY(runway_tile) * TILE_SIZE + TILE_SIZE / 2;

	const ModularAirportTileData *data = st->airport.GetModularTileData(runway_tile);
	if (data == nullptr) return;

	bool horizontal = (data->rotation % 2) == 0; // 0=X-axis (NW-SE), 1=Y-axis (NE-SW)
	int approach_dist = 12 * TILE_SIZE; // 12 tiles out (matches standard airport scale better)

	/* Determine which way is "out" by checking neighbors or rotation */
	/* If horizontal (X-axis), check X+1 and X-1 */
	/* If vertical (Y-axis), check Y+1 and Y-1 */

	bool runway_extends_positive = false;
	bool runway_extends_negative = false;

	if (horizontal) {
		/* Check X axis neighbors */
		TileIndex next = runway_tile + TileDiffXY(1, 0);
		TileIndex prev = runway_tile - TileDiffXY(1, 0);
		const ModularAirportTileData *next_data = st->airport.GetModularTileData(next);
		const ModularAirportTileData *prev_data = st->airport.GetModularTileData(prev);

		if (IsRunwayPieceOnAxis(next_data, horizontal)) runway_extends_positive = true;
		if (IsRunwayPieceOnAxis(prev_data, horizontal)) runway_extends_negative = true;
	} else {
		/* Check Y axis neighbors */
		TileIndex next = runway_tile + TileDiffXY(0, 1);
		TileIndex prev = runway_tile - TileDiffXY(0, 1);
		const ModularAirportTileData *next_data = st->airport.GetModularTileData(next);
		const ModularAirportTileData *prev_data = st->airport.GetModularTileData(prev);

		if (IsRunwayPieceOnAxis(next_data, horizontal)) runway_extends_positive = true;
		if (IsRunwayPieceOnAxis(prev_data, horizontal)) runway_extends_negative = true;
	}

	/* If we are at the negative end (runway extends positive), approach from negative */
	if (runway_extends_positive && !runway_extends_negative) {
		if (horizontal) *target_x -= approach_dist; else *target_y -= approach_dist;
	}
	/* If we are at the positive end (runway extends negative), approach from positive */
	else if (!runway_extends_positive && runway_extends_negative) {
		if (horizontal) *target_x += approach_dist; else *target_y += approach_dist;
	}
	/* Single tile or middle piece? Use rotation to guess typical approach (from "left/top") */
	else {
		if (horizontal) *target_x -= approach_dist; else *target_y -= approach_dist;
	}
}

bool DirectionsWithin45(Direction dir_a, Direction dir_b)
{
	DirDiff diff = DirDifference(dir_a, dir_b);
	return diff == DirDiff::Same || diff == DirDiff::Left45 || diff == DirDiff::Right45;
}

Direction GetRunwayApproachDirection(const Station *st, TileIndex runway_tile)
{
	int approach_x, approach_y;
	GetModularLandingApproachPoint(st, runway_tile, &approach_x, &approach_y);

	const int threshold_x = TileX(runway_tile) * TILE_SIZE + TILE_SIZE / 2;
	const int threshold_y = TileY(runway_tile) * TILE_SIZE + TILE_SIZE / 2;

	const int dx = threshold_x - approach_x;
	const int dy = threshold_y - approach_y;

	if (dx > 0) return Direction::SW;
	if (dx < 0) return Direction::NE;
	if (dy > 0) return Direction::SE;
	if (dy < 0) return Direction::NW;
	return Direction::N;
}

bool AirportMoveModularLanding(Aircraft *v, const Station *st)
{
	if (st->airport.blocks.Test(AirportBlock::Zeppeliner)) {
		/* Match stock behavior: abort modular landing while zeppeliner wreck blocks the airport. */
		ClearTaxiPathReservation(v, INVALID_TILE, true, false);
		ClearModularRunwayReservation(v);
		v->modular_landing_goal = INVALID_TILE;
		v->modular_landing_tile = INVALID_TILE;
		v->state = FLYING;
		return false;
	}

	if (v->modular_landing_tile == INVALID_TILE) {
		v->modular_landing_tile = FindModularLandingTarget(st, v);
		if (v->modular_landing_tile == INVALID_TILE) {
			Debug(misc, 3, "[ModAp] no runway/helipad tile found for landing at station {}", st->index);
			return false;
		}
		Debug(misc, 3, "[ModAp] Vehicle {} starting approach to tile {}, pos=({},{},{})",
			v->index, v->modular_landing_tile.base(), v->x_pos, v->y_pos, v->z_pos);
	}

	/* Older saves omit landing_chain_path, and invalid saved path data is discarded.
	 * If an aircraft already committed to modular landing has neither a path nor
	 * active reservations, reclaim the landing chain before continuing descent;
	 * otherwise a second aircraft can choose the same helipad/touchdown tile. */
	if (v->taxi_reserved_tiles.empty() && v->modular_runway_reservation.empty() && v->landing_chain_path == nullptr) {
		/* Helicopters require a concrete ground goal to land (they'd otherwise circle
		 * forever -- see aircraft_cmd.cpp commit path that rejects helicopter landing
		 * when goal is INVALID_TILE). Re-derive the goal here when the saved value
		 * was lost. Fixed-wing INVALID_TILE is preserved as-is: it represents a
		 * deliberate "queue on a one-way buffer" landing handled by TryReserveLandingChain. */
		if (v->subtype == AIR_HELICOPTER && v->modular_landing_goal == INVALID_TILE) {
			TileIndex rollout = FindModularRunwayRolloutPoint(st, v, v->modular_landing_tile);
			TileIndex goal_from = (rollout != INVALID_TILE) ? rollout : v->modular_landing_tile;
			v->modular_landing_goal = FindModularLandingGroundGoal(st, v, nullptr, goal_from);
		}

		if (!TryReserveLandingChain(v, st, v->modular_landing_tile, v->modular_landing_goal)) {
			Debug(misc, 2, "[ModAp] V{} landing-chain restore failed after load/state rebuild: runway={} goal={}",
				v->index, v->modular_landing_tile.base(),
				v->modular_landing_goal == INVALID_TILE ? 0 : v->modular_landing_goal.base());
			ClearTaxiPathReservation(v, INVALID_TILE, true, false);
			ClearModularRunwayReservation(v);
			v->modular_landing_goal = INVALID_TILE;
			v->modular_landing_tile = INVALID_TILE;
			v->state = FLYING;
			v->tile = TileIndex{};
			return false;
		}
	}

	int airport_z = GetTileMaxPixelZ(v->modular_landing_tile) + 1;
	/* Match stock heliport behavior: rooftop touchdown uses +60 px (afc->delta_z). */
	if (v->subtype == AIR_HELICOPTER) {
		const ModularAirportTileData *landing_data = st->airport.GetModularTileData(v->modular_landing_tile);
		if (landing_data != nullptr && landing_data->piece_type == APT_HELIPORT) airport_z += 60;
	}

	/* Single-stage approach: fly straight to the runway threshold. */
	int target_x = TileX(v->modular_landing_tile) * TILE_SIZE + TILE_SIZE / 2;
	int target_y = TileY(v->modular_landing_tile) * TILE_SIZE + TILE_SIZE / 2;

	int dist = abs(v->x_pos - target_x) + abs(v->y_pos - target_y);

	int count = UpdateAircraftSpeed(v, SPEED_LIMIT_APPROACH, false);
	if (count == 0) return false;

	/* Move 'count' pixels towards target */
	int new_x = v->x_pos;
	int new_y = v->y_pos;
	for (int i = 0; i < count; i++) {
		if (new_x != target_x) new_x += (target_x > new_x) ? 1 : -1;
		if (new_y != target_y) new_y += (target_y > new_y) ? 1 : -1;
	}

	/* Update direction with smooth turning */
	if (new_x != v->x_pos || new_y != v->y_pos) {
		Direction desired_dir = GetDirectionTowards(v, target_x, target_y);
		if (desired_dir != v->direction) {
			v->last_direction = v->direction;
			v->direction = desired_dir;
		}
	}

	/* Aircraft in air has no tile */
	v->tile = TileIndex{};

	/* Altitude logic */
	int z = v->z_pos;
	if (v->subtype == AIR_HELICOPTER) {
		/* Helicopters stay at their current altitude while moving laterally,
		 * then descend straight down once centered over the touchdown tile. */
		if (dist == 0 && z > airport_z) z--;
	} else {
		/* Planes: glide slope to runway */
		if (z > airport_z) {
			int t = std::max(1, dist - 4);
			int delta = z - airport_z;
			if (delta >= t) {
				z -= CeilDiv(z - airport_z, t);
			}
		}
	}

	SetAircraftPosition(v, new_x, new_y, z);

	Debug(misc, 5, "[ModAp] Vehicle {} landing: pos=({},{},{}), target=({},{},{}), dist={}",
		v->index, v->x_pos, v->y_pos, v->z_pos, target_x, target_y, airport_z, dist);

	/* Check if reached target */
	if (v->x_pos == target_x && v->y_pos == target_y) {
		if (v->z_pos > airport_z) return false; // Still descending

		/* Reached threshold, land and start rollout */
		Debug(misc, 3, "[ModAp] Vehicle {} touchdown at ({},{},{})", v->index, target_x, target_y, airport_z);
		RecordAirportMovement(v->targetairport, true);
		v->tile = v->modular_landing_tile;

		/* Down, so no longer descending. CmdStartStopVehicle reads this flag as a second
		 * "is in flight" condition independent of state, so a helicopter that keeps it
		 * while parked refuses every manual start and stop, and fails autoreplace -- which
		 * stops and restarts the vehicle around the swap -- with "Aircraft is in flight".
		 * Its orders still run, which is what makes the symptom so confusing. Stock clears
		 * the flag when the rotors reach full speed in AircraftController's HeliLower;
		 * touchdown is the modular equivalent moment, and modular landing has no other
		 * exit that leaves the aircraft on the ground. */
		v->flags.Reset(VehicleAirFlag::HelicopterDirectDescent);

		v->modular_landing_tile = INVALID_TILE;

		AircraftEventHandler_Landing(v, st->airport.GetFTA());

		if (v->subtype == AIR_HELICOPTER && v->modular_landing_goal != INVALID_TILE) {
			/* Helicopters never do runway rollout -- go straight to the
			 * pre-selected ground destination regardless of landing surface. */
			v->ground_path_goal = v->tile;
			v->modular_ground_target = MGT_ROLLOUT;
			HandleModularGroundArrival(v);
		} else if (v->subtype == AIR_HELICOPTER) {
			Debug(misc, 1, "[ModAp] V{} helicopter touchdown without ground goal -- should not happen", v->index);
			AircraftEventHandler_EndLanding(v, st->airport.GetFTA());
		} else {
			/* Prefer the tile the reserved chain already starts from over asking again.
			 * The two answers are the same function of the same aircraft, but they are
			 * computed a whole approach apart, and the braking distance depends on the
			 * plane-speed setting, which a single-player game can change in between. A
			 * disagreement would land the aircraft somewhere its precomputed chain does
			 * not begin, which HandleModularGroundArrival reports as a broken landing
			 * chain. Only accept it while it is still part of this runway: a layout edit
			 * during the approach can leave it somewhere the aircraft cannot roll to. */
			TileIndex rollout_point = INVALID_TILE;
			if (v->landing_chain_path != nullptr && v->landing_chain_path->valid &&
					!v->landing_chain_path->tiles.empty()) {
				const TileIndex chain_start = v->landing_chain_path->tiles.front();
				std::vector<TileIndex> runway_tiles;
				if (GetContiguousModularRunwayTiles(st, v->tile, runway_tiles) &&
						std::find(runway_tiles.begin(), runway_tiles.end(), chain_start) != runway_tiles.end()) {
					rollout_point = chain_start;
				}
			}
			if (rollout_point == INVALID_TILE) rollout_point = FindModularRunwayRolloutPoint(st, v, v->tile);
			if (rollout_point != INVALID_TILE) {
				Debug(misc, 3, "[ModAp] Vehicle {} starting rollout to tile {}", v->index, rollout_point.base());
				v->ground_path_goal = rollout_point;
				v->modular_ground_target = MGT_ROLLOUT;
				v->state = TERM1;
			} else {
				AircraftEventHandler_EndLanding(v, st->airport.GetFTA());
			}
		}
		return true;
	}

	return false;
}

bool AirportMoveModularHeliTakeoff(Aircraft *v, [[maybe_unused]] const Station *st)
{
	int target_z = GetAircraftFlightLevel(v, true);

	if (v->z_pos < target_z) {
		v->z_pos++;
		SetAircraftPosition(v, v->x_pos, v->y_pos, v->z_pos);
		return true;
	}

	/* Reached altitude, transition to flying -- force-clear all reservations. */
	ClearModularRunwayReservation(v);
	ClearTaxiPathReservation(v, INVALID_TILE, true, false);
	v->state = FLYING;
	RecordAirportMovement(v->targetairport, false);
	v->tile = TileIndex{};
	AircraftNextAirportPos_and_Order(v);
	return true;
}

bool AirportMoveModularTakeoff(Aircraft *v, const Station *st)
{
	auto requeue_takeoff = [&]() {
		TileIndex runway = FindModularRunwayTileForTakeoff(st, v);
		if (runway == INVALID_TILE) {
			v->modular_takeoff_tile = INVALID_TILE;
			v->ground_path_goal = INVALID_TILE;
			v->modular_ground_target = MGT_NONE;
			v->state = TERM1;
			Debug(misc, 1, "[ModAp] V{} takeoff recovery failed: no runway available", v->index);
			return false;
		}

		v->modular_takeoff_tile = runway;
		v->ground_path_goal = runway;
		v->modular_ground_target = MGT_RUNWAY_TAKEOFF;
		v->state = TERM1;
		Debug(misc, 2, "[ModAp] V{} takeoff recovery: requeue runway={}", v->index, runway.base());
		return true;
	};

	if (v->modular_takeoff_tile == INVALID_TILE) return requeue_takeoff();

	const ModularAirportTileData *data = st->airport.GetModularTileData(v->modular_takeoff_tile);
	if (data == nullptr || !IsModularRunwayPiece(data->piece_type)) {
		Debug(misc, 1, "[ModAp] V{} takeoff recovery: invalid takeoff tile {}", v->index, v->modular_takeoff_tile.base());
		return requeue_takeoff();
	}

	if (v->modular_runway_reservation.empty() &&
			!TryReserveContiguousModularRunway(v, st, v->modular_takeoff_tile)) {
		Debug(misc, 2, "[ModAp] V{} takeoff recovery: failed to reserve runway {}, requeueing", v->index, v->modular_takeoff_tile.base());
		return requeue_takeoff();
	}

	const bool horizontal = (data->rotation % 2) == 0;

	/* Liftoff after 2/3 of the ground run this aircraft needs, rather than 2/3 of
	 * whatever runway it happens to be on. The two agree on a runway of exactly the
	 * length the aircraft needs -- which is where every takeoff used to start -- and on a
	 * longer one there is no reason to keep the aircraft down for the extra tiles it was
	 * never going to use. The runway still caps it, so a jet on a short strip it was only
	 * allowed onto because the airport has nothing better rotates where it always did. */
	std::vector<TileIndex> takeoff_runway_tiles;
	int runway_length_tiles = 1;
	if (GetContiguousModularRunwayTiles(st, v->modular_takeoff_tile, takeoff_runway_tiles)) {
		runway_length_tiles = std::max(1, (int)takeoff_runway_tiles.size());
	}
	int liftoff_progress = std::min(runway_length_tiles, (int)ModularTakeoffRunTiles(v)) * TILE_SIZE * 2 / 3;

	if (v->modular_takeoff_progress == 0) {
		/* Determine takeoff direction by finding the other end of the runway */
		TileIndex end_tile = GetRunwayOtherEnd(st, v->modular_takeoff_tile);
		int end_x = TileX(end_tile) * TILE_SIZE + TILE_SIZE / 2;
		int end_y = TileY(end_tile) * TILE_SIZE + TILE_SIZE / 2;

		/* If single tile runway, end_tile == start_tile.
		   Fallback to rotation-based direction if we can't determine direction from length. */
		if (end_tile == v->modular_takeoff_tile) {
			Direction dir = horizontal ? Direction::SE : Direction::SW;
			v->direction = dir;
		} else {
			v->direction = GetDirectionTowards(v, end_x, end_y);
		}

		PlayAircraftSound(v);
	}

	/* Accelerate and move */
	int count = UpdateAircraftSpeed(v, SPEED_LIMIT_NONE);
	for (int i = 0; i < count; i++) {
		/* Move forward along runway */
		GetNewVehiclePosResult gp = GetNewVehiclePos(v);

		/* Calculate altitude - stay on ground until 2/3 of runway, then climb */
		int z = v->z_pos;
		int target_z = GetAircraftFlightLevel(v, true);

		if (v->modular_takeoff_progress > liftoff_progress) {
			int climb_progress = v->modular_takeoff_progress - liftoff_progress;
			int desired_altitude = GetTileMaxPixelZ(v->modular_takeoff_tile) + 1 + (climb_progress * 3 / 2);

			if (z < std::min(desired_altitude, target_z)) {
				z = std::min(desired_altitude, target_z);
			}
		}

		/* Use SetAircraftPosition for proper viewport and shadow updates */
		SetAircraftPosition(v, gp.x, gp.y, z);

		/* Update tile reference */
		TileIndex current_tile = TileVirtXY(v->x_pos, v->y_pos);
		const ModularAirportTileData *tile_data = st->airport.GetModularTileData(current_tile);
		bool on_runway = (tile_data != nullptr && IsModularRunwayPiece(tile_data->piece_type));

		if (on_runway) {
			v->tile = current_tile;
		} else {
			v->tile = TileIndex{};  /* In air */
		}

		v->modular_takeoff_progress++;

		/* Continue takeoff for at least 12 tiles to match stock airport behavior */
		/* Stock airports have planes continue in takeoff direction for some distance */
		if (v->modular_takeoff_progress > TILE_SIZE * 12 && v->z_pos >= target_z) {
			Debug(misc, 3, "[ModAp] Vehicle {} takeoff complete, transitioning to FLYING", v->index);
			ClearModularRunwayReservation(v);
			ClearTaxiPathReservation(v, INVALID_TILE, true, false);
			v->state = FLYING;
			RecordAirportMovement(v->targetairport, false);
			v->modular_takeoff_tile = INVALID_TILE;
			v->modular_takeoff_progress = 0;
			v->tile = TileIndex{};
			AircraftNextAirportPos_and_Order(v);
			return true;
		}
	}

	return false;
}

/**
 * How many tiles of runway an aircraft needs after touchdown to brake from its
 * approach speed down to taxi speed.
 *
 * This replays the deceleration UpdateAircraftSpeed actually performs, rather than
 * approximating it: the soft brake sheds cur_speed^2/16384 per half-tick, which is a
 * fast start and a very long tail, and the tail is most of the distance. The result is
 * where the rollout may end, so being a tile short would have the aircraft turn off a
 * runway above taxi speed and be snapped down by the clamp in AirportMoveModular --
 * which is exactly the thing this is meant to avoid.
 *
 * Touchdown speed is predicted rather than observed because the rollout point has to be
 * known one landing-chain reservation earlier, while the aircraft is still on final. The
 * prediction is exact in practice: approach runs at SPEED_LIMIT_APPROACH capped by the
 * aircraft's own maximum, and the gate sits 12 tiles out -- far enough to shed holding
 * speed, which takes under 3. A broken-down aircraft is capped lower still
 * (SPEED_LIMIT_BROKEN) and so only ever brakes shorter than predicted, which costs it a
 * few tiles of rollout and nothing else.
 *
 * @param v The landing aircraft.
 * @return Rollout distance in tiles, at least 1.
 */
uint ModularRolloutBrakingTiles(const Aircraft *v)
{
	if (v == nullptr) return 1;

	const uint plane_speed = std::max<uint>(1, _settings_game.vehicle.plane_speed);
	const uint taxi_limit = SPEED_LIMIT_TAXI * plane_speed;
	uint speed = std::min<uint>(v->vcache.cached_max_speed, SPEED_LIMIT_APPROACH * plane_speed);

	/* Mirrors the movement loop: one iteration per UpdateAircraftSpeed call, distance
	 * accumulated from the post-brake speed through the same 8-bit progress remainder.
	 * Runway travel is axis-aligned, so GetOldAdvanceSpeed does not scale it down. */
	uint pixels = 0;
	uint progress = 0;
	while (speed > taxi_limit) {
		speed -= std::max<uint>(1, ((speed * speed) / 16384) / plane_speed);
		progress += speed / plane_speed;
		pixels += progress >> 8;
		progress &= 0xFF;
	}

	/* One pixel of slack: the aircraft carries v->progress across the tile boundary, so
	 * an exact multiple of TILE_SIZE -- which is what this comes to at plane_speed 2 --
	 * would place the turn-off one pixel before the aircraft has actually reached taxi
	 * speed, and the ground-move clamp would have to shed the remainder. */
	return std::max<uint>(1, CeilDiv(pixels + 1, TILE_SIZE));
}

/**
 * Find where a landing aircraft's runway rollout ends.
 *
 * An aircraft leaves the runway once it has slowed to taxi speed, not once it has run
 * out of runway: rolling the remaining length of a long runway costs about 41 ticks per
 * tile and holds the whole runway -- as one reservation resource -- for every one of
 * them. Stopping the rollout at the braking distance is what gives a runway longer than
 * that distance any value at all.
 *
 * This is the *earliest* the aircraft may turn off, not where it will: it is the floor
 * TryReserveLandingChain plans its route from, and that route is free to carry on along
 * the runway to a further exit, in which case the rollout is extended to cover it.
 *
 * The floor is capped at LARGE_RUNWAY_LENGTH_TILES tiles of runway even when the aircraft
 * has not finished braking by then. A fast jet needs eight tiles to reach taxi speed at
 * the default plane_speed, but six is the shortest runway it is allowed to land on at
 * all, and on one of those it has always crossed the far end at about 160% of taxi speed
 * and had the remainder clamped off. Rolling further than that on a long runway only
 * overshoots exits the aircraft then has to taxi back to; a jet that can take off from
 * six tiles can turn off within six.
 *
 * A runway shorter than the resulting distance clamps to the far end, which is what every
 * rollout did before this, so nothing changes on a minimum-length runway.
 *
 * @param st The station.
 * @param v The landing aircraft.
 * @param landing_tile Touchdown tile.
 * @return Earliest turn-off tile, or INVALID_TILE if @p landing_tile is not a runway piece.
 */
TileIndex FindModularRunwayRolloutPoint(const Station *st, const Aircraft *v, TileIndex landing_tile)
{
	const ModularAirportTileData *data = st->airport.GetModularTileData(landing_tile);
	if (data == nullptr) return INVALID_TILE;

	bool is_rw = IsModularRunwayPiece(data->piece_type);
	if (is_rw) {
		Debug(misc, 3, "[ModAp] Rollout check: tile={}, gfx={}, is_runway=1", landing_tile.base(), data->piece_type);
	} else {
		return INVALID_TILE;
	}

	std::vector<TileIndex> runway_tiles;
	if (!GetContiguousModularRunwayTiles(st, landing_tile, runway_tiles) || runway_tiles.empty()) {
		return INVALID_TILE;
	}

	const TileIndex far_end = GetRunwayOtherEnd(st, landing_tile);
	/* Helicopters never roll out: they hand off to ground movement from the touchdown tile
	 * itself (see the chain_origin choice in TryReserveLandingChain). The callers that ask
	 * for a rollout point anyway use it as a taxi-distance proxy when scoring landing
	 * targets, so shortening it would quietly re-score every helicopter arriving at a
	 * runway -- a change to helicopter routing, made by a change about aeroplane braking.
	 * Leave them the far end they have always been given. */
	if (v == nullptr || v->subtype == AIR_HELICOPTER) return far_end;

	/* Index both ends rather than assuming the touchdown tile is at one extremity: it
	 * always is today (only end pieces are landing targets), but the roll direction has
	 * to follow GetRunwayOtherEnd either way for the two to agree. */
	const auto touchdown_it = std::find(runway_tiles.begin(), runway_tiles.end(), landing_tile);
	const auto far_it = std::find(runway_tiles.begin(), runway_tiles.end(), far_end);
	if (touchdown_it == runway_tiles.end() || far_it == runway_tiles.end()) return far_end;

	const size_t touchdown_index = static_cast<size_t>(std::distance(runway_tiles.begin(), touchdown_it));
	const size_t far_index = static_cast<size_t>(std::distance(runway_tiles.begin(), far_it));
	const bool toward_high = far_index > touchdown_index;
	const size_t available = toward_high ? far_index - touchdown_index : touchdown_index - far_index;
	/* ModularRolloutBrakingTiles answers the physics question -- how far to taxi speed --
	 * and the cap is the gameplay one. Keep them separate: a slow aircraft that brakes in
	 * less than the cap still turns off where it actually stops. */
	const size_t brake_cap = LARGE_RUNWAY_LENGTH_TILES - 1;
	const size_t rolled = std::min({static_cast<size_t>(ModularRolloutBrakingTiles(v)), brake_cap, available});

	return toward_high ? runway_tiles[touchdown_index + rolled] : runway_tiles[touchdown_index - rolled];
}

TileIndex FindModularRolloutHoldingTile(const Station *st, const Aircraft *v, TileIndex start_tile)
{
	if (!IsValidTile(start_tile) || st->airport.modular_tile_data == nullptr) return INVALID_TILE;

	const GroundPathRestriction restriction = GetGroundPathRestriction(v);

	/* The safe stop the landing chain already reserved, used whenever the search
	 * below comes up empty. Admitting a landing with no free stand requires a
	 * one-way queue tile at the end of the reserved horizon (see
	 * TryReserveLandingChain), precisely so the aircraft always has somewhere legal
	 * to wait once it is down. That tile is not necessarily on the route searched
	 * below: the search aims at the cheapest service tile, while the buffer sits
	 * wherever the admission horizon happened to end, so the two routinely diverge
	 * and the search can fail while the aircraft owns a perfectly good stop.
	 *
	 * Only one-way queue tiles qualify, the same restriction admission applied. A
	 * reservation this aircraft happens to hold on a stand is not somewhere to
	 * park -- that stand belongs to whatever it was routing towards. */
	const auto reserved_queue_tile = [&]() -> TileIndex {
		for (TileIndex tile : v->taxi_reserved_tiles) {
			if (!IsValidTile(tile) || tile == start_tile) continue;
			if (!IsOneWayTaxiTile(st, tile)) continue;
			if (IsModularTileOccupiedByOtherAircraft(st, tile, v->index)) continue;
			TaxiPath own_path = BuildTaxiPath(st, start_tile, tile, nullptr, false, restriction);
			if (!own_path.valid || own_path.tiles.size() < 2 || own_path.segments.empty()) continue;
			return tile;
		}
		return INVALID_TILE;
	};

	/* Keep the route that won, rather than asking for it again once the winner is known.
	 * A second query is not guaranteed to hand back the same tiles: FindAirportGroundPath
	 * both reads and writes the crossing-required cache, so the intervening probes can
	 * change which pass answers, and the aircraft would then walk a route other than the
	 * one whose cost selected this target. */
	TileIndex best_target = INVALID_TILE;
	int best_cost = INT_MAX;
	std::vector<TileIndex> best_route;
	for (const ModularAirportTileData &data : *st->airport.modular_tile_data) {
		/* A helipad is a service tile only for something that may stand on one. */
		const bool is_service = (data.piece_type == APT_STAND || data.piece_type == APT_STAND_1 ||
				IsModularHangarPiece(data.piece_type) ||
				(IsModularHelipadPiece(data.piece_type) && restriction != GroundPathRestriction::FixedWing));
		if (!is_service) continue;
		AirportGroundPath p = FindAirportGroundPath(st, start_tile, data.tile, nullptr, false, true, restriction);
		if (!p.found) continue;
		if (best_target == INVALID_TILE || p.cost < best_cost) {
			best_target = data.tile;
			best_cost = p.cost;
			best_route = std::move(p.tiles);
		}
	}

	/* Return the nearest safe-stop tile along the route (one-way taxiway queue tile or a
	 * stand/hangar/helipad) that is currently clear. An aircraft must never stop on a
	 * free-move apron/grass tile: that pins a shared transit section.
	 *
	 * Parking that is not this aircraft's target does not qualify: an aircraft cannot be
	 * stranded on a runway needing somebody else's stand, because landing is not initiated
	 * unless a way off the runway already exists. When every safe stop on this route is
	 * held by someone else, that reserved way off the runway is what reserved_queue_tile()
	 * returns. */
	if (best_target != INVALID_TILE) {
		for (TileIndex tile : best_route) {
			if (tile == start_tile) continue;
			if (!IsModularSafeStopTile(st, tile, best_target)) continue;
			Tile t(tile);
			if (!IsAirportTile(t)) continue;
			if (HasModularAirportTileReservation(tile) && GetModularAirportTileReservationOwner(tile) != v->index) continue;
			if (IsModularTileOccupiedByOtherAircraft(st, tile, v->index)) continue;
			return tile;
		}
	}

	return reserved_queue_tile();
}

bool IsModularTileOccupiedByOtherAircraft(const Station *st, TileIndex tile, VehicleID self)
{
	/* Hangars can hold multiple aircraft; never treat them as occupied. */
	if (IsModularHangarTile(st, tile)) return false;
	if (!st->TileBelongsToAirport(tile)) return false;

	return HasVehicleOnTile(tile, [self](const Vehicle *v) {
		if (v->type != VehicleType::Aircraft) return false;
		if (v->index == self) return false;
		return Aircraft::From(v)->IsNormalAircraft();
	});
}

bool ModularAirportHasHelipad(const Station *st)
{
	if (st->airport.modular_tile_data == nullptr) return false;
	for (const ModularAirportTileData &data : *st->airport.modular_tile_data) {
		if (IsModularHelipadPiece(data.piece_type)) return true;
	}
	return false;
}

TileIndex FindFreeModularTerminal(const Station *st, const Aircraft *v, TileIndex from_tile, bool allow_helicopter)
{
	if (st->airport.modular_tile_data == nullptr) return INVALID_TILE;

	/* Stock parity: a helicopter uses stands only at airports with no helipads at all
	 * (AirportFindFreeHelipad falls back to terminals exactly when num_helipads == 0).
	 * Where helipads exist but are momentarily taken, the helicopter waits -- circling
	 * if airborne -- rather than occupying a stand a fixed-wing aircraft needs.
	 *
	 * @p allow_helicopter overrides this for callers where the aircraft is already on
	 * the ground and refusing would strand it or leave two aircraft stacked on a tile. */
	if (!allow_helicopter && v != nullptr && v->subtype == AIR_HELICOPTER && ModularAirportHasHelipad(st)) {
		return INVALID_TILE;
	}
	const bool can_ground_route = (from_tile != INVALID_TILE && st->TileBelongsToAirport(from_tile)) || CanUseModularGroundRouting(st, v);
	const TileIndex origin = (from_tile != INVALID_TILE) ? from_tile : (v != nullptr ? v->tile : INVALID_TILE);
	TileIndex best_tile = INVALID_TILE;
	int best_score = INT_MAX;

	/* Terminal piece types: APT_STAND, APT_STAND_1 */
	/* TODO: Also support hangars (9,10) and helipads (11) */
	for (const ModularAirportTileData &data : *st->airport.modular_tile_data) {
		if (data.piece_type == APT_STAND || data.piece_type == APT_STAND_1) {
			/* Check if tile is free */
			if (HasModularAirportTileReservation(data.tile)) {
				/* If reserved by us, it's fine (we might be re-evaluating) */
				if (v != nullptr && GetModularAirportTileReservationOwner(data.tile) == v->index) return data.tile;
				continue;
			}
			if (v != nullptr && IsModularTileOccupiedByOtherAircraft(st, data.tile, v->index)) continue;

			/* Avoid assigning stands that are currently unreachable from our position. */
			int score = 0;
			if (can_ground_route && origin != INVALID_TILE) {
				AirportGroundPath path = FindAirportGroundPath(st, origin, data.tile, nullptr, false, true, GetGroundPathRestriction(v));
				if (!path.found) continue;
				score = path.cost;
			} else if (from_tile != INVALID_TILE) {
				const int cx = TileX(data.tile) * TILE_SIZE + TILE_SIZE / 2;
				const int cy = TileY(data.tile) * TILE_SIZE + TILE_SIZE / 2;
				const int fx = TileX(from_tile) * TILE_SIZE + TILE_SIZE / 2;
				const int fy = TileY(from_tile) * TILE_SIZE + TILE_SIZE / 2;
				score = abs(cx - fx) + abs(cy - fy);
			} else if (v != nullptr) {
				const int cx = TileX(data.tile) * TILE_SIZE + TILE_SIZE / 2;
				const int cy = TileY(data.tile) * TILE_SIZE + TILE_SIZE / 2;
				score = abs(cx - v->x_pos) + abs(cy - v->y_pos);
			}

			if (best_tile == INVALID_TILE || score < best_score) {
				best_score = score;
				best_tile = data.tile;
			}
		}
	}

	return best_tile;
}

TileIndex FindFreeModularHelipad(const Station *st, const Aircraft *v, TileIndex from_tile)
{
	if (st->airport.modular_tile_data == nullptr) return INVALID_TILE;
	if (v != nullptr && v->subtype != AIR_HELICOPTER) return INVALID_TILE;
	const bool can_ground_route = (from_tile != INVALID_TILE && st->TileBelongsToAirport(from_tile)) || CanUseModularGroundRouting(st, v);
	const TileIndex origin = (from_tile != INVALID_TILE) ? from_tile : (v != nullptr ? v->tile : INVALID_TILE);
	TileIndex best_tile = INVALID_TILE;
	int best_score = INT_MAX;

	/* If we are already on a helipad, stay there.
	 * Aircraft can call this while not on a modular airport tile (e.g. airborne),
	 * so guard the tile lookup. */
	if (v != nullptr) {
		const ModularAirportTileData *cur = st->airport.GetModularTileData(v->tile);
		if (cur != nullptr && IsModularHelipadPiece(cur->piece_type)) return v->tile;
	}

	for (const ModularAirportTileData &data : *st->airport.modular_tile_data) {
		if (IsModularHelipadPiece(data.piece_type)) {
			if (HasModularAirportTileReservation(data.tile)) {
				/* If reserved by us, it's fine */
				if (v != nullptr && GetModularAirportTileReservationOwner(data.tile) == v->index) return data.tile;
				continue;
			}

			/* Check for physical occupancy. */
			if (v != nullptr && IsModularTileOccupiedByOtherAircraft(st, data.tile, v->index)) continue;

			int score = 0;
			if (can_ground_route && origin != INVALID_TILE) {
				AirportGroundPath path = FindAirportGroundPath(st, origin, data.tile, nullptr, false, true, GetGroundPathRestriction(v));
				if (!path.found) continue;
				score = path.cost;
			} else if (from_tile != INVALID_TILE) {
				const int cx = TileX(data.tile) * TILE_SIZE + TILE_SIZE / 2;
				const int cy = TileY(data.tile) * TILE_SIZE + TILE_SIZE / 2;
				const int fx = TileX(from_tile) * TILE_SIZE + TILE_SIZE / 2;
				const int fy = TileY(from_tile) * TILE_SIZE + TILE_SIZE / 2;
				score = abs(cx - fx) + abs(cy - fy);
			} else if (v != nullptr) {
				const int cx = TileX(data.tile) * TILE_SIZE + TILE_SIZE / 2;
				const int cy = TileY(data.tile) * TILE_SIZE + TILE_SIZE / 2;
				score = abs(cx - v->x_pos) + abs(cy - v->y_pos);
			}

			if (best_tile == INVALID_TILE || score < best_score) {
				best_score = score;
				best_tile = data.tile;
			}
		}
	}

	return best_tile;
}

TileIndex FindFreeModularHangar(const Station *st, const Aircraft *v, TileIndex from_tile)
{
	if (st->airport.modular_tile_data == nullptr) return INVALID_TILE;
	const bool can_ground_route = (from_tile != INVALID_TILE && st->TileBelongsToAirport(from_tile)) || CanUseModularGroundRouting(st, v);
	const TileIndex origin = (from_tile != INVALID_TILE) ? from_tile : (v != nullptr ? v->tile : INVALID_TILE);

	TileIndex best_path_tile = INVALID_TILE;
	int best_path_score = INT_MAX;
	TileIndex best_fallback_tile = INVALID_TILE;
	int best_fallback_score = INT_MAX;

	for (const ModularAirportTileData &data : *st->airport.modular_tile_data) {
		if (!IsModularHangarPiece(data.piece_type)) continue;

		int fallback_score = 0;
		if (from_tile != INVALID_TILE) {
			const int cx = TileX(data.tile) * TILE_SIZE + TILE_SIZE / 2;
			const int cy = TileY(data.tile) * TILE_SIZE + TILE_SIZE / 2;
			const int fx = TileX(from_tile) * TILE_SIZE + TILE_SIZE / 2;
			const int fy = TileY(from_tile) * TILE_SIZE + TILE_SIZE / 2;
			fallback_score = abs(cx - fx) + abs(cy - fy);
		} else if (v != nullptr) {
			const int cx = TileX(data.tile) * TILE_SIZE + TILE_SIZE / 2;
			const int cy = TileY(data.tile) * TILE_SIZE + TILE_SIZE / 2;
			fallback_score = abs(cx - v->x_pos) + abs(cy - v->y_pos);
		}
		if (best_fallback_tile == INVALID_TILE || fallback_score < best_fallback_score) {
			best_fallback_score = fallback_score;
			best_fallback_tile = data.tile;
		}

		if (!can_ground_route || origin == INVALID_TILE) continue;

		AirportGroundPath path = FindAirportGroundPath(st, origin, data.tile, nullptr, false, true, GetGroundPathRestriction(v));
		if (!path.found) continue;
		if (best_path_tile == INVALID_TILE || path.cost < best_path_score) {
			best_path_score = path.cost;
			best_path_tile = data.tile;
		}
	}

	/* When ground routing is available, never return an unreachable hangar.
	 * Returning a distance fallback here can lock aircraft on disconnected goals. */
	if (can_ground_route) return best_path_tile;
	return best_fallback_tile;
}

TileIndex FindModularUnstackParkingTile(const Station *st, const Aircraft *v, uint8_t *target)
{
	/* A helicopter looks for a pad whichever target it arrived on. It can legitimately
	 * be standing here on MGT_TERMINAL: both HandleModularEndLanding and the helipad
	 * fallback in HandleModularGroundArrival hand a helicopter a stand at an airport
	 * that also has helipads. */
	const bool prefer_helipad = (v->modular_ground_target == MGT_HELIPAD) || (v->subtype == AIR_HELICOPTER);
	TileIndex goal = prefer_helipad ? FindFreeModularHelipad(st, v) : INVALID_TILE;
	uint8_t tgt = MGT_HELIPAD;

	if (goal == INVALID_TILE) {
		/* Unstacking beats parking policy: two aircraft on one tile is worse than a
		 * helicopter on a stand, so the stand is allowed even where helipads exist.
		 * Without that override a helicopter on MGT_TERMINAL would be refused every
		 * stand and the caller would fall through to stacking -- the exact outcome
		 * this whole path exists to prevent. */
		goal = FindFreeModularTerminal(st, v, INVALID_TILE, true);
		tgt = MGT_TERMINAL;
	}

	if (goal == INVALID_TILE) return INVALID_TILE;
	if (target != nullptr) *target = tgt;
	return goal;
}

bool IsModularHangarPiece(uint8_t piece_type)
{
	switch (piece_type) {
		case APT_DEPOT_SE:
		case APT_DEPOT_SW:
		case APT_DEPOT_NW:
		case APT_DEPOT_NE:
		case APT_SMALL_DEPOT_SE:
		case APT_SMALL_DEPOT_SW:
		case APT_SMALL_DEPOT_NW:
		case APT_SMALL_DEPOT_NE:
			return true;
		default:
			return false;
	}
}

/** Check if a tile is a multi-capacity hangar/depot on this airport. */
bool IsModularHangarTile(const Station *st, TileIndex tile)
{
	const ModularAirportTileData *td = st->airport.GetModularTileData(tile);
	return td != nullptr && IsModularHangarPiece(td->piece_type);
}

/**
 * Is this tile's reservation held by a vehicle that no longer exists, or by something
 * that is not a real aircraft?
 *
 * Read-only counterpart to the first two clauses of TryClearStaleModularReservation,
 * for callers that must not mutate map state. The ground pathfinder is the reason it
 * exists: it runs speculatively, per neighbour, and sometimes with no aircraft at all,
 * so clearing reservations from inside A* expansion is not something it may do.
 */
bool IsModularReservationOwnerGone(TileIndex tile)
{
	if (!HasModularAirportTileReservation(tile)) return false;
	const VehicleID owner = GetModularAirportTileReservationOwner(tile);
	if (owner == VehicleID::Invalid()) return true;
	const Vehicle *veh = Vehicle::GetIfValid(owner);
	if (veh == nullptr || veh->type != VehicleType::Aircraft) return true;
	return !Aircraft::From(veh)->IsNormalAircraft();
}

bool TryClearStaleModularReservation(const Station *st, TileIndex tile, VehicleID reserver)
{
	if (st == nullptr || !IsValidTile(tile)) return false;
	Tile t(tile);
	if (!IsAirportTile(t)) return false;
	if (!IsModularAirportTileReservedBy(tile, reserver)) return false;

	Vehicle *veh = Vehicle::GetIfValid(reserver);
	if (veh == nullptr || veh->type != VehicleType::Aircraft) {
		Debug(misc, 2, "[ModAp] [FALLBACK] stale-clear: st={} name='{}' tile={} reserver={} reason=invalid_vehicle",
			st->index, GetModularAirportDebugName(st), tile.base(), reserver.base());
		ClearModularAirportTileReservation(tile);
		return true;
	}

	Aircraft *a = Aircraft::From(veh);
	if (!a->IsNormalAircraft()) {
		Debug(misc, 2, "[ModAp] [FALLBACK] stale-clear: st={} name='{}' tile={} reserver={} reason=not_normal_aircraft",
			st->index, GetModularAirportDebugName(st), tile.base(), reserver.base());
		ClearModularAirportTileReservation(tile);
		return true;
	}

	const bool tied_to_station = (a->targetairport == st->index || a->last_station_visited == st->index);
	const ModularAirportTileData *tile_data = st->airport.GetModularTileData(tile);
	const bool tile_is_runway = (tile_data != nullptr && IsModularRunwayPiece(tile_data->piece_type));
	const ModularAirportTileData *owner_tile_data = (IsValidTile(a->tile) ? st->airport.GetModularTileData(a->tile) : nullptr);
	const bool owner_on_runway = (owner_tile_data != nullptr && IsModularRunwayPiece(owner_tile_data->piece_type));
	const bool in_runway_flow =
			a->state == LANDING || a->state == ENDLANDING ||
			a->state == HELILANDING || a->state == HELIENDLANDING ||
			a->state == TAKEOFF || a->state == STARTTAKEOFF || a->state == ENDTAKEOFF ||
			(a->modular_ground_target == MGT_ROLLOUT && owner_on_runway) || a->modular_ground_target == MGT_RUNWAY_TAKEOFF;

	/* Runway reservations owned by aircraft still in landing/takeoff flow for this station
	 * are never stale, even if transiently untracked by path state. */
	if (tile_is_runway && tied_to_station && in_runway_flow) return false;

	/* If the aircraft still tracks this reservation explicitly, it is not stale
	 * even when the aircraft itself is airborne during landing/takeoff phases. */
	const bool is_current_tile = (a->tile == tile);
	const bool is_goal_tile = (a->modular_ground_target != MGT_NONE && a->ground_path_goal == tile);
	const bool is_tracked_runway = std::find(a->modular_runway_reservation.begin(), a->modular_runway_reservation.end(), tile) != a->modular_runway_reservation.end();
	const bool is_tracked_taxi = std::find(a->taxi_reserved_tiles.begin(), a->taxi_reserved_tiles.end(), tile) != a->taxi_reserved_tiles.end();
	bool is_on_active_path = false;
	if (a->taxi_path != nullptr) {
		const size_t start = std::min<size_t>(a->taxi_path_index, a->taxi_path->tiles.size());
		for (size_t i = start; i < a->taxi_path->tiles.size(); ++i) {
			if (a->taxi_path->tiles[i] == tile) {
				is_on_active_path = true;
				break;
			}
		}
	}
	if (is_current_tile || is_goal_tile || is_tracked_runway || is_tracked_taxi || is_on_active_path) return false;

	/* Reservations must belong to aircraft still tied to this station and physically on its ground. */
	const bool owner_on_ground_here = IsValidTile(a->tile) && st->TileBelongsToAirport(a->tile) && a->state != FLYING;
	if (!tied_to_station || !owner_on_ground_here) {
		Debug(misc, 2, "[ModAp] [FALLBACK] stale-clear: st={} name='{}' tile={} V{} unit#{} reason=not_on_ground state={} vtile={} tied={}",
			st->index, GetModularAirportDebugName(st), tile.base(), a->index, a->unitnumber, a->state,
			IsValidTile(a->tile) ? a->tile.base() : 0, tied_to_station);
		ClearModularAirportTileReservation(tile);
		return true;
	}

	/* Aircraft is active on this station ground but tile is not in any tracked intent.
	 * Clear immediately rather than after an unsaved timeout: the decision is derived
	 * entirely from saved game state, so multiplayer joiners reach the same result. */
	Debug(misc, 2, "[ModAp] [FALLBACK] stale-clear: st={} name='{}' tile={} V{} unit#{} reason=active_untracked state={} vtile={}",
		st->index, GetModularAirportDebugName(st), tile.base(), a->index, a->unitnumber, a->state,
		IsValidTile(a->tile) ? a->tile.base() : 0);
	ClearModularAirportTileReservation(tile);
	return true;
}

/**
 * How much runway a departing aircraft needs ahead of it to get airborne.
 *
 * These are the two lengths the rest of the airport is already built around: a fast jet
 * needs a large-safe runway, which is LARGE_RUNWAY_LENGTH_TILES long, and nothing may use
 * a runway below MIN_RUNWAY_LENGTH_TILES at all. Reusing them means a runway that is long
 * enough to be selected is always long enough to depart from.
 *
 * This only ever *permits* an early start. An aircraft that cannot find that much runway
 * ahead of it taxis to the end and departs from there exactly as before, which is what
 * happens to a jet on the short strip it is allowed onto when an airport has nothing
 * better.
 *
 * @param v The departing aircraft.
 * @return Required ground run in tiles.
 */
uint ModularTakeoffRunTiles(const Aircraft *v)
{
	const bool is_fast = v != nullptr && (AircraftVehInfo(v->engine_type)->subtype & AIR_FAST) != 0;
	return is_fast ? LARGE_RUNWAY_LENGTH_TILES : MIN_RUNWAY_LENGTH_TILES;
}

/**
 * Whether a takeoff roll starting on @p tile has enough of the aircraft's selected runway
 * ahead of it.
 *
 * A departure does not have to reach the far end of its runway, only a point with enough
 * runway left to get airborne. Back-taxiing the rest costs about 41 ticks per tile and
 * holds the whole runway -- one reservation resource -- for every one of them, which is
 * most of what a long runway costs its owner today.
 *
 * The direction is the one the takeoff already rolls in: from the selected end toward the
 * far end. @p tile lies between the two whenever it is on this runway at all, so the
 * distance to the far end is the run remaining ahead of it.
 *
 * @param st   The station.
 * @param v    The departing aircraft, with its takeoff runway already selected.
 * @param tile Candidate tile to start the roll from.
 * @return true when the roll may start here.
 */
bool ModularTakeoffRunFitsFrom(const Station *st, const Aircraft *v, TileIndex tile)
{
	if (st == nullptr || v == nullptr) return false;
	if (!IsValidTile(tile) || !IsValidTile(v->modular_takeoff_tile)) return false;

	std::vector<TileIndex> runway_tiles;
	if (!GetContiguousModularRunwayTiles(st, v->modular_takeoff_tile, runway_tiles)) return false;

	/* Not on this runway: an aircraft crossing some other runway on its way here is
	 * transiting, and has no runway ahead of it at all. */
	const auto here = std::find(runway_tiles.begin(), runway_tiles.end(), tile);
	if (here == runway_tiles.end()) return false;

	const auto far = std::find(runway_tiles.begin(), runway_tiles.end(),
			GetRunwayOtherEnd(st, v->modular_takeoff_tile));
	if (far == runway_tiles.end()) return false;

	const size_t ahead = static_cast<size_t>(std::abs(std::distance(here, far))) + 1;
	return ahead >= ModularTakeoffRunTiles(v);
}

/** Why a runway-end tile is or isn't a usable takeoff end (occupancy/safety aside). */
enum class ModularTakeoffEndStatus : uint8_t {
	Ok,        ///< Usable: a real end, long enough, takeoff-flagged, direction matches.
	NotEnd,    ///< Not a runway-end piece.
	TooShort,  ///< Runway shorter than MIN_RUNWAY_LENGTH_TILES.
	NoTakeoff, ///< RUF_TAKEOFF not set.
	WrongDir,  ///< Direction bits do not permit a takeoff roll toward the far end.
};

/**
 * Classify a runway-end tile as a takeoff candidate, ignoring occupancy,
 * reachability, and large-aircraft safety. Both the up-front "does a good takeoff
 * runway exist" scan and the per-end selection loop in
 * FindModularRunwayTileForTakeoff route through this single predicate so they
 * cannot drift on what counts as a usable end. Direction bits are interpreted as
 * travel direction: a takeoff from the low end travels toward the high end, and
 * vice versa.
 */
static ModularTakeoffEndStatus ClassifyModularTakeoffEnd(const Station *st, TileIndex tile, uint8_t piece_type)
{
	if (!IsModularRunwayEndPiece(piece_type)) return ModularTakeoffEndStatus::NotEnd;

	std::vector<TileIndex> rwy;
	if (!GetContiguousModularRunwayTiles(st, tile, rwy) || (int)rwy.size() < MIN_RUNWAY_LENGTH_TILES) return ModularTakeoffEndStatus::TooShort;

	const uint8_t flags = GetRunwayFlags(st, tile);
	if ((flags & RUF_TAKEOFF) == 0) return ModularTakeoffEndStatus::NoTakeoff;

	const bool is_low = IsRunwayEndLow(st, tile);
	if (is_low && (flags & RUF_DIR_HIGH) == 0) return ModularTakeoffEndStatus::WrongDir;
	if (!is_low && (flags & RUF_DIR_LOW) == 0) return ModularTakeoffEndStatus::WrongDir;
	return ModularTakeoffEndStatus::Ok;
}

/**
 * Find a runway end tile suitable for takeoff, respecting runway usage flags.
 * Returns the end tile where the aircraft should start its takeoff roll.
 */
TileIndex FindModularRunwayTileForTakeoff(const Station *st, const Aircraft *v)
{
	if (st->airport.modular_tile_data == nullptr) return INVALID_TILE;
	if (st->airport.blocks.Test(AirportBlock::Zeppeliner)) return INVALID_TILE;
	const bool can_ground_route = CanUseModularGroundRouting(st, v);
	/* Large aircraft require a large-safe runway. This holds even with the no-jetcrash
	 * cheat on: on modular airports the size preference governs routing, not crash risk. */
	const bool large_takeoff_required = (v != nullptr) &&
			((AircraftVehInfo(v->engine_type)->subtype & AIR_FAST) != 0);
	/* Strict large-runway preference: a large aircraft only uses a short runway when NO
	 * large-safe runway end serves its takeoff direction. Determine this up front,
	 * ignoring transient occupancy -- if a good runway is merely busy, the aircraft waits
	 * (returns a reachable-but-blocked end) rather than downgrading to a short runway.
	 *
	 * The cached answer does not filter by direction, while the per-end loop below does.
	 * The two agree only because of the runway invariants documented on
	 * CanAircraftUseModularRunwayForLanding: flags propagate across a contiguous runway,
	 * exactly one direction bit is set, and both extremities are end pieces, so a runway
	 * has a takeoff-legal end exactly when it has a takeoff-flagged end at all. */
	const bool good_takeoff_runway_exists = large_takeoff_required && ModularAirportHasSafeRunwayFor(st, false);

	/* Try runway ends in two passes: first strict (no intermediate runway crossing),
	 * then with crossing allowed. This prevents crossing paths from being selected
	 * over temporarily-blocked strict paths, which would add runway contention. */
	for (int pass = 0; pass < 2; ++pass) {
		const bool allow_crossing = (pass == 1);

		TileIndex best_path_tile = INVALID_TILE;
		int best_path_score = INT_MAX;
		TileIndex best_non_runway_taxi_tile = INVALID_TILE;
		int best_non_runway_taxi_score = INT_MAX;
		TileIndex best_blocked_tile = INVALID_TILE;  ///< Topologically reachable but currently blocked
		int best_blocked_score = INT_MAX;

		for (const ModularAirportTileData &data : *st->airport.modular_tile_data) {
			const ModularTakeoffEndStatus status = ClassifyModularTakeoffEnd(st, data.tile, data.piece_type);
			if (status != ModularTakeoffEndStatus::Ok) {
				if (status == ModularTakeoffEndStatus::WrongDir && v != nullptr && pass == 0 && ShouldLogModularRateLimited(v->index, 40, 256)) {
					Debug(misc, 2, "[ModAp] V{} takeoff-skip dir: tile={} is_low={} flags={}", v->index, data.tile.base(), IsRunwayEndLow(st, data.tile), GetRunwayFlags(st, data.tile));
				}
				continue;
			}

			/* When a large-safe runway exists for this direction, large aircraft must use
			 * it -- short runways are skipped entirely. Only when no good runway exists at
			 * all do we allow a best-effort short-runway takeoff. */
			if (large_takeoff_required && good_takeoff_runway_exists && !IsRunwaySafeForLarge(st, data.tile)) {
				if (pass == 0 && ShouldLogModularRateLimited(v->index, 42, 256)) {
					Debug(misc, 2, "[ModAp] V{} takeoff-skip short (good runway exists): tile={}", v->index, data.tile.base());
				}
				continue;
			}

			/* Prefer reachable takeoff ends. */
			if (!can_ground_route) continue;
			/* Judge the end by the best route to it, not only by the shortest one: an end
			 * whose direct taxiway is occupied is still usable when a second one is free. */
			ReservableRoute route = FindReservableRoute(v, st, v->tile, data.tile, data.tile,
					allow_crossing, GroundPathRestriction::FromAircraft, v);
			const TaxiPath &taxi_path = route.path;
			if (!taxi_path.valid) {
				if (v != nullptr && pass == 0 && ShouldLogModularRateLimited(v->index, 35, 128)) {
					Debug(misc, 2, "[ModAp] V{} takeoff-path invalid: from={} to={}", v->index, v->tile.base(), data.tile.base());
				}
				continue;
			}
			if (!route.found) {
				if (v != nullptr && pass == 0 && ShouldLogModularRateLimited(v->index, 36, 128)) {
					Debug(misc, 2, "[ModAp] V{} takeoff-path not enterable: from={} to={} reason={} deny_tile={} deny_by=V{}",
						v->index, v->tile.base(), data.tile.base(),
						TaxiReserveFailureName(route.deny.reason),
						IsValidTile(route.deny.tile) ? route.deny.tile.base() : 0,
						route.deny.blocker == VehicleID::Invalid() ? 0 : route.deny.blocker.base());
				}
				/* Track as "reachable but blocked" -- prefer over unreachable Manhattan fallback. */
				const int blocked_cost = static_cast<int>(taxi_path.tiles.size() - 1);
				if (best_blocked_tile == INVALID_TILE || blocked_cost < best_blocked_score) {
					best_blocked_score = blocked_cost;
					best_blocked_tile = data.tile;
				}
				continue;
			}
			const int path_cost = static_cast<int>(taxi_path.tiles.size() - 1);

			bool uses_runway_before_goal = false;
			for (TileIndex t : taxi_path.tiles) {
				if (t == data.tile) break;
				const ModularAirportTileData *td = st->airport.GetModularTileData(t);
				if (td != nullptr && IsModularRunwayPiece(td->piece_type)) {
					uses_runway_before_goal = true;
					break;
				}
			}

			if (!uses_runway_before_goal &&
					(best_non_runway_taxi_tile == INVALID_TILE || path_cost < best_non_runway_taxi_score)) {
				best_non_runway_taxi_score = path_cost;
				best_non_runway_taxi_tile = data.tile;
			}

			if (best_path_tile == INVALID_TILE || path_cost < best_path_score) {
				best_path_score = path_cost;
				best_path_tile = data.tile;
			}
		}

		if (best_non_runway_taxi_tile != INVALID_TILE) return best_non_runway_taxi_tile;
		if (best_path_tile != INVALID_TILE) return best_path_tile;
		/* Topologically reachable but temporarily blocked -- aircraft will wait for traffic to clear. */
		if (best_blocked_tile != INVALID_TILE) return best_blocked_tile;
		/* If strict pass found nothing, try again with crossing allowed. */
	}
	return INVALID_TILE;
}

/**
 * Find a queue tile just before entering the selected takeoff runway end.
 * Returns runway_end when no non-runway queue point exists.
 */
TileIndex FindModularTakeoffQueueTile(const Station *st, const Aircraft *v, TileIndex runway_end)
{
	if (runway_end == INVALID_TILE || v == nullptr) return runway_end;
	if (!CanUseModularGroundRouting(st, v)) return runway_end;

	/* Runway already selected -- allow crossing if needed. */
	AirportGroundPath path = FindAirportGroundPath(st, v->tile, runway_end, v, true);
	if (!path.found || path.tiles.empty()) return INVALID_TILE;

	/* Allow queueing and progress along the selected takeoff runway.
	 * Some layouts can only reach the chosen runway end by entering that runway
	 * at the opposite end and taxiing along it. */
	std::vector<TileIndex> target_runway_tiles;
	if (!GetContiguousModularRunwayTiles(st, runway_end, target_runway_tiles)) return INVALID_TILE;
	auto is_on_target_runway = [&](TileIndex tile) {
		return std::find(target_runway_tiles.begin(), target_runway_tiles.end(), tile) != target_runway_tiles.end();
	};
	TileIndex best_queue_tile = INVALID_TILE;
	for (TileIndex tile : path.tiles) {
		const ModularAirportTileData *td = st->airport.GetModularTileData(tile);
		const bool is_runway_tile = (td != nullptr && IsModularRunwayPiece(td->piece_type));
		if (is_runway_tile && !is_on_target_runway(tile)) {
			/* Don't queue onto unrelated runways while routing to takeoff. */
			break;
		}

		/* Queueing for takeoff should happen on taxi/apron tiles, not stands/hangars/helipads,
		 * and only on currently free tiles to avoid hard blocking by parked aircraft.
		 * Target-runway tiles are explicitly allowed above. */
		const bool service_tile = (td != nullptr) &&
				(td->piece_type == APT_STAND || td->piece_type == APT_STAND_1 ||
				 IsModularHangarPiece(td->piece_type) ||
				 IsModularHelipadPiece(td->piece_type));
		if (service_tile) {
			continue;
		}

		const bool blocked_by_reservation =
				HasModularAirportTileReservation(tile) && GetModularAirportTileReservationOwner(tile) != v->index;
		if (blocked_by_reservation || IsModularTileOccupiedByOtherAircraft(st, tile, v->index)) {
			continue;
		}

		best_queue_tile = tile;
	}

	if (best_queue_tile != INVALID_TILE) return best_queue_tile;

	/* If no safe queue tile exists, only use runway end if it's currently clear. */
	if (!HasModularAirportTileReservation(runway_end) || GetModularAirportTileReservationOwner(runway_end) == v->index) {
		if (!IsModularTileOccupiedByOtherAircraft(st, runway_end, v->index)) return runway_end;
	}

	return INVALID_TILE;

}

bool CanUseModularGroundRouting(const Station *st, const Aircraft *v)
{
	return v != nullptr && IsValidTile(v->tile) && st->TileBelongsToAirport(v->tile);
}

void ClearTaxiPathReservation(Aircraft *v, TileIndex keep_tile, bool force_clear_all, bool as_fallback)
{
	Station *st = Station::GetIfValid(v->targetairport);

	/* Only clear reservations for tiles that are on the current taxi_path.
	 * Tiles reserved by external logic (e.g. landing chain) that aren't on the
	 * current path should be preserved so they survive path rebuilds on touchdown.
	 * When force_clear_all is set (e.g. stuck aircraft), clear everything. */
	std::vector<TileIndex> preserved;
	int force_cleared_count = 0;
	for (TileIndex tile : v->taxi_reserved_tiles) {
		if (tile == keep_tile) continue;
		if (std::find(v->modular_runway_reservation.begin(), v->modular_runway_reservation.end(), tile) != v->modular_runway_reservation.end()) continue;

		if (!force_clear_all) {
			/* If there's a current path, only clear tiles that are on it.
			 * Tiles not on the path were reserved by landing chain or similar -- preserve them. */
			if (v->taxi_path != nullptr) {
				bool on_path = std::find(v->taxi_path->tiles.begin(), v->taxi_path->tiles.end(), tile) != v->taxi_path->tiles.end();
				if (!on_path) {
					preserved.push_back(tile);
					continue;
				}
			} else {
				/* No path -- preserve all non-runway reservations (landing chain tiles survive touchdown). */
				preserved.push_back(tile);
				continue;
			}
		}

		Tile t(tile);
		if (!IsAirportTile(t)) continue;
		if (IsModularAirportTileReservedBy(tile, v->index)) {
			if (force_clear_all) force_cleared_count++;
			ClearModularAirportTileReservation(tile);
		}
	}
	v->taxi_reserved_tiles.clear();

	if (force_clear_all && force_cleared_count > 0) {
		if (as_fallback) {
			Debug(misc, 2, "[ModAp] [FALLBACK] force-clear-all: st={} name='{}' V{} unit#{} cleared={} keep={} state={} vtile={} goal={} tgt={}",
				st != nullptr ? st->index.base() : 0, GetModularAirportDebugName(st), v->index, v->unitnumber, force_cleared_count,
				keep_tile != INVALID_TILE ? keep_tile.base() : 0,
				v->state, IsValidTile(v->tile) ? v->tile.base() : 0,
				IsValidTile(v->ground_path_goal) ? v->ground_path_goal.base() : 0,
				v->modular_ground_target);
		} else {
			Debug(misc, 3, "[ModAp] force-clear-transition: st={} name='{}' V{} unit#{} cleared={} state={} vtile={}",
				st != nullptr ? st->index.base() : 0, GetModularAirportDebugName(st), v->index, v->unitnumber, force_cleared_count,
				v->state, IsValidTile(v->tile) ? v->tile.base() : 0);
		}
	}

	/* Re-add preserved tiles. */
	for (TileIndex tile : preserved) v->taxi_reserved_tiles.push_back(tile);

	if (keep_tile != INVALID_TILE) {
		Tile keep(keep_tile);
		if (IsAirportTile(keep) && IsModularAirportTileReservedBy(keep_tile, v->index)) {
			v->taxi_reserved_tiles.push_back(keep_tile);
		}
	}
}

void ClearTaxiPathState(Aircraft *v, TileIndex keep_tile)
{
	ClearTaxiPathReservation(v, keep_tile);
	v->taxi_path.reset();
	v->taxi_path_index = 0;
	v->taxi_current_segment = 0;
	v->taxi_wait_counter = 0;
}

uint8_t FindTaxiSegmentIndex(const TaxiPath *path, uint16_t tile_index)
{
	if (path == nullptr) return 0;
	for (size_t i = 0; i < path->segments.size(); ++i) {
		const TaxiSegment &seg = path->segments[i];
		if (tile_index >= seg.start_index && tile_index <= seg.end_index) return static_cast<uint8_t>(i);
	}
	return static_cast<uint8_t>(path->segments.size());
}

bool IsTaxiTileReservedByOther(const Station *st, TileIndex tile, VehicleID vid)
{
	Tile t(tile);
	if (!IsAirportTile(t)) return false;
	/* Hangars are multi-capacity -- never treat as reserved. */
	if (IsModularHangarTile(st, tile)) return false;
	if (!HasModularAirportTileReservation(tile)) return false;
	const VehicleID reserver = GetModularAirportTileReservationOwner(tile);
	if (reserver == vid) return false;
	if (TryClearStaleModularReservation(st, tile, reserver)) return false;
	return HasModularAirportTileReservation(tile) && GetModularAirportTileReservationOwner(tile) != vid;
}

void SetTaxiReservation(Aircraft *v, TileIndex tile)
{
	Tile t(tile);
	if (!IsAirportTile(t)) return;
	/* Hangars are multi-capacity -- never set map-level reservation bits.
	 * Still track in the vehicle's vector so path cleanup works. */
	Station *st = Station::GetIfValid(v->targetairport);
	if (st != nullptr && IsModularHangarTile(st, tile)) {
		if (std::find(v->taxi_reserved_tiles.begin(), v->taxi_reserved_tiles.end(), tile) == v->taxi_reserved_tiles.end()) {
			v->taxi_reserved_tiles.push_back(tile);
		}
		return;
	}
	SetModularAirportTileReservationOwner(tile, v->index);
	if (std::find(v->taxi_reserved_tiles.begin(), v->taxi_reserved_tiles.end(), tile) == v->taxi_reserved_tiles.end()) {
		v->taxi_reserved_tiles.push_back(tile);
	}
}

static void SetTaxiReservationUnlessOperationRunway(Aircraft *v, TileIndex tile)
{
	if (std::find(v->modular_runway_reservation.begin(), v->modular_runway_reservation.end(), tile) !=
			v->modular_runway_reservation.end()) {
		return;
	}
	SetTaxiReservation(v, tile);
}

/**
 * Test whether one forward plan can be claimed right now, without acquiring anything.
 *
 * This is the single authority on "what must I own before advancing". Candidate
 * selection dry-runs it and the commit path below runs it for real; keeping one
 * implementation ensures both paths stay consistent.
 *
 * Not pure: IsTaxiTileReservedByOther clears a reservation whose owner no longer
 * exists. That is deliberate and belongs here rather than at the call sites -- a
 * dead vehicle's claim is not an obstacle to anybody, and a selection pass that
 * treats it as one rejects a route nothing is actually using.
 */
static bool ValidateForwardReservationPlan(const Aircraft *v, const Station *st,
		const ForwardReservationPlan &plan, TaxiReserveResult *out)
{
	const auto fail = [&](TaxiReserveFailure reason, TileIndex tile, VehicleID blocker = VehicleID::Invalid()) {
		if (out != nullptr) *out = TaxiReserveResult{reason, tile, blocker};
		return false;
	};
	if (out != nullptr) *out = TaxiReserveResult{};

	/* A landing/takeoff runway is one atomic resource. */
	if (!plan.operation_runway_tiles.empty()) {
		VehicleID state_blocker = VehicleID::Invalid();
		if (IsContiguousModularRunwayReservedInStateByOther(v, st, plan.operation_runway_tiles, &state_blocker)) {
			return fail(TaxiReserveFailure::RunwayBusy, plan.operation_runway_tiles.front(), state_blocker);
		}
		for (TileIndex tile : plan.operation_runway_tiles) {
			if (IsModularTileOccupiedByOtherAircraft(st, tile, v->index)) {
				return fail(TaxiReserveFailure::RunwayBusy, tile);
			}
			if (IsTaxiTileReservedByOther(st, tile, v->index)) {
				return fail(TaxiReserveFailure::RunwayBusy, tile,
						HasModularAirportTileReservation(tile) ? GetModularAirportTileReservationOwner(tile) : VehicleID::Invalid());
			}
		}
	}

	/* Runway crossings are ordinary exclusive path tiles, so a crossing claims only
	 * what it drives over. The state-held check is what keeps that safe, and it is
	 * load-bearing in normal play rather than a post-load safety net: a landing or
	 * departing aircraft does not always have the whole runway in its map claims,
	 * which is why IsContiguousModularRunwayReservedInStateByOther falls back to
	 * modular_landing_tile / modular_takeoff_tile when the reservation vector does
	 * not overlap. Drop this and a crossing can step in front of a rolling aircraft
	 * in exactly the cases those fallbacks exist to cover. */
	for (TileIndex tile : plan.taxi_tiles) {
		if (IsModularHangarTile(st, tile)) continue;
		if (IsPathTileRunwayPiece(st, tile)) {
			std::vector<TileIndex> runway;
			if (!GetContiguousModularRunwayTiles(st, tile, runway) || runway.empty()) {
				return fail(TaxiReserveFailure::RunwayResourceError, tile);
			}
			VehicleID state_blocker = VehicleID::Invalid();
			if (IsContiguousModularRunwayReservedInStateByOther(v, st, runway, &state_blocker)) {
				return fail(TaxiReserveFailure::RunwayBusy, tile, state_blocker);
			}
		}
		if (IsTaxiTileReservedByOther(st, tile, v->index)) {
			return fail(TaxiReserveFailure::ReservedByOther, tile,
					HasModularAirportTileReservation(tile) ? GetModularAirportTileReservationOwner(tile) : VehicleID::Invalid());
		}
		if (tile != v->tile && IsModularTileOccupiedByOtherAircraft(st, tile, v->index)) {
			return fail(TaxiReserveFailure::OccupiedByOther, tile);
		}
	}

	return true;
}

/** Validate and commit one forward plan without partial acquisition. */
static bool TryCommitForwardReservationPlan(Aircraft *v, const Station *st,
		const ForwardReservationPlan &plan, TaxiReserveResult *out, bool log_success)
{
	if (!ValidateForwardReservationPlan(v, st, plan, out)) return false;

	const std::vector<TileIndex> old_runway_tracking = v->modular_runway_reservation;
	/* Validation is complete, so replace the operation claim as one transaction.
	 * This also releases a takeoff runway when the aircraft is staged behind an
	 * upstream safe stop, rather than carrying that runway through the queue. */
	for (TileIndex tile : old_runway_tracking) {
		if (ContainsSortedTile(plan.operation_runway_tiles, tile)) continue;
		Tile t(tile);
		if (IsAirportTile(t) && IsModularAirportTileReservedBy(tile, v->index)) {
			ClearModularAirportTileReservation(tile);
		}
	}
	v->modular_runway_reservation = plan.operation_runway_tiles;
	for (TileIndex tile : plan.operation_runway_tiles) {
		SetModularAirportTileReservationOwner(tile, v->index);
	}

	/* A tile promoted from taxi crossing to the aircraft's operation runway belongs
	 * in exactly one tracking vector. */
	std::erase_if(v->taxi_reserved_tiles, [&](TileIndex tile) {
		return ContainsSortedTile(plan.operation_runway_tiles, tile);
	});

	for (TileIndex tile : plan.taxi_tiles) SetTaxiReservation(v, tile);

	if (log_success && old_runway_tracking != v->modular_runway_reservation &&
			ShouldLogModularRateLimited(v->index, 32, 16)) {
		LogModularVehicleReservationState(st, v, "reserve granted");
	}
	return true;
}

std::string_view TaxiReserveFailureName(TaxiReserveFailure reason)
{
	switch (reason) {
		case TaxiReserveFailure::None: return "none";
		case TaxiReserveFailure::NoPath: return "no_path";
		case TaxiReserveFailure::ReservedByOther: return "reserved_by_other";
		case TaxiReserveFailure::OccupiedByOther: return "occupied_by_other";
		case TaxiReserveFailure::RunwayBusy: return "runway_busy";
		case TaxiReserveFailure::RunwayResourceError: return "runway_resource_error";
		case TaxiReserveFailure::NoSafeStop: return "no_safe_stop";
		default: return "?";
	}
}

bool TryReserveTaxiSegment(Aircraft *v, const Station *st, uint8_t segment_idx, TaxiReserveResult *out)
{
	const auto fail = [&](TaxiReserveFailure reason, TileIndex tile = INVALID_TILE) {
		if (out != nullptr) *out = TaxiReserveResult{reason, tile, VehicleID::Invalid()};
		return false;
	};
	if (out != nullptr) *out = TaxiReserveResult{};

	if (v == nullptr || st == nullptr || v->taxi_path == nullptr ||
			segment_idx >= v->taxi_path->segments.size() ||
			v->taxi_path_index >= v->taxi_path->tiles.size()) {
		return fail(TaxiReserveFailure::NoPath);
	}

	ForwardReservationPlan plan;
	const TileIndex operation_runway = GetGroundOperationRunwayTile(v, st);
	switch (BuildForwardReservationPlan(v, st, v->taxi_path.get(), v->taxi_path_index,
			v->ground_path_goal, operation_runway, plan)) {
		case ForwardPlanStatus::Ok:
			break;
		case ForwardPlanStatus::ResourceError:
			return fail(TaxiReserveFailure::RunwayResourceError,
					IsValidTile(operation_runway) ? operation_runway : v->tile);
		case ForwardPlanStatus::NoSafeStop:
			return fail(TaxiReserveFailure::NoSafeStop, v->ground_path_goal);
	}

	return TryCommitForwardReservationPlan(v, st, plan, out, true);
}

bool TryRetargetModularGroundGoal(Aircraft *v, const Station *st)
{
	TileIndex alt_goal = INVALID_TILE;
	uint8_t alt_target = v->modular_ground_target;
	/* Applied only once the retarget is committed, so a refused one leaves nothing behind. */
	TileIndex alt_takeoff_tile = INVALID_TILE;

	switch (v->modular_ground_target) {
		case MGT_TERMINAL:
			alt_goal = FindFreeModularTerminal(st, v);
			alt_target = MGT_TERMINAL;
			break;
		case MGT_HELIPAD:
			alt_goal = FindFreeModularHelipad(st, v);
			alt_target = MGT_HELIPAD;
			break;
		case MGT_HANGAR:
			alt_goal = FindFreeModularHangar(st, v);
			alt_target = MGT_HANGAR;
			break;
		case MGT_RUNWAY_TAKEOFF:
			/* The chosen end can stop being reachable after it was selected. */
			alt_goal = FindModularRunwayTileForTakeoff(st, v);
			alt_takeoff_tile = alt_goal;
			alt_target = MGT_RUNWAY_TAKEOFF;
			break;
		case MGT_ROLLOUT:
			alt_goal = FindModularLandingGroundGoal(st, v, &alt_target);
			break;
		case MGT_HELI_TAKEOFF_TILE:
			EnsureModularHeliTilesValid(st);
			alt_goal = st->airport.modular_heli_takeoff_tile;
			alt_target = MGT_HELI_TAKEOFF_TILE;
			if (alt_goal == INVALID_TILE) {
				/* The computed takeoff tile is gone -- the airport gained a helipad (which
				 * makes ComputeModularHeliTiles yield nothing), or the tile it pointed at
				 * stopped qualifying. The goal is saved on the vehicle, so without this it
				 * would keep a destination that can never be reached, forever and silently.
				 *
				 * Re-run the departure ladder instead of just re-reading the cache: a
				 * takeoff runway if one is usable, otherwise lift off vertically from where
				 * we stand. A helicopter is never obliged to reach a particular tile in
				 * order to leave, so this ladder always terminates. */
				alt_goal = FindModularRunwayTileForTakeoff(st, v);
				if (alt_goal != INVALID_TILE) {
					/* Staged rather than written: the shared failure check below can still
					 * reject this goal, and a takeoff tile left behind by a retarget that
					 * did not happen is state no longer backed by a matching target. */
					alt_takeoff_tile = alt_goal;
					alt_target = MGT_RUNWAY_TAKEOFF;
				} else if (v->subtype == AIR_HELICOPTER && !st->airport.blocks.Test(AirportBlock::Zeppeliner)) {
					Debug(misc, 2, "[ModAp] V{} unit#{} retarget-heli-takeoff: no computed tile or runway, departing vertically from tile={}",
						v->index, v->unitnumber, IsValidTile(v->tile) ? v->tile.base() : 0);
					ClearTaxiPathReservation(v, v->tile, true, false);
					ClearTaxiPathState(v, v->tile);
					v->ground_path_goal = INVALID_TILE;
					v->modular_ground_target = MGT_NONE;
					v->taxi_wait_counter = 0;
					v->state = HELITAKEOFF;
					return true;
				}
			}
			break;
		default:
			return false;
	}

	if (alt_goal == INVALID_TILE || alt_goal == v->ground_path_goal) {
		/* Common and benign under contention ("every stand is busy right now"), so this
		 * is rate-limited chatter rather than a fault. What it must never do is repeat
		 * forever for the same vehicle: that means the goal is unreachable rather than
		 * merely taken, and no amount of waiting will fix it. */
		if (ShouldLogModularRateLimited(v->index, 47, 128)) {
			Debug(misc, 2, "[ModAp] V{} unit#{} retarget failed: tgt={} tile={} goal={} alt={} wait={}",
				v->index, v->unitnumber, v->modular_ground_target,
				IsValidTile(v->tile) ? v->tile.base() : 0,
				IsValidTile(v->ground_path_goal) ? v->ground_path_goal.base() : 0,
				IsValidTile(alt_goal) ? alt_goal.base() : 0,
				v->taxi_wait_counter);
		}
		return false;
	}

	v->ground_path_goal = alt_goal;
	v->modular_ground_target = alt_target;
	if (alt_takeoff_tile != INVALID_TILE) v->modular_takeoff_tile = alt_takeoff_tile;
	ClearTaxiPathState(v, v->tile);
	v->taxi_wait_counter = 0;
	return true;
}

void HandleModularGroundArrival(Aircraft *v)
{
	const Station *st = Station::Get(v->targetairport);

	switch (v->modular_ground_target) {
		case MGT_ROLLOUT:
			/* Completed rollout along runway, now find a terminal */
			Debug(misc, 3, "[ModAp] Vehicle {} completed rollout, finding terminal", v->index);
			{
				const bool wants_depot = ModularAircraftWantsHangar(v, st);
				TileIndex goal = INVALID_TILE;
				uint8_t target = MGT_NONE;

				/* Prefer the preselected landing goal used during landing-chain reservation. */
				if (v->modular_landing_goal != INVALID_TILE) {
					const ModularAirportTileData *goal_data = st->airport.GetModularTileData(v->modular_landing_goal);
					if (goal_data != nullptr) {
						goal = v->modular_landing_goal;
						if (IsModularHangarPiece(goal_data->piece_type)) {
							target = MGT_HANGAR;
						} else if (IsModularHelipadPiece(goal_data->piece_type)) {
							target = MGT_HELIPAD;
						} else {
							target = MGT_TERMINAL;
						}
					}
				}

				if (goal == INVALID_TILE) {
					if (v->subtype == AIR_HELICOPTER && !wants_depot) {
						goal = FindFreeModularHelipad(st, v);
						target = MGT_HELIPAD;
					}

					if (goal == INVALID_TILE && wants_depot) {
						goal = FindFreeModularHangar(st, v);
						target = MGT_HANGAR;
					}

					if (goal == INVALID_TILE) {
						goal = FindFreeModularTerminal(st, v);
						target = MGT_TERMINAL;
					}
				}

				v->modular_landing_goal = INVALID_TILE;
				if (goal != INVALID_TILE) {
					/* One-shot compatibility for saves written before modular paths were
					 * persisted. Every later landing by this aircraft is judged normally. */
					const bool restored_from_save = v->rollout_restored_from_save;
					v->rollout_restored_from_save = false;

					v->ground_path_goal = goal;
					v->modular_ground_target = target;
					v->state = (target == MGT_HANGAR) ? HANGAR : TERM1;

					/* Install pre-computed landing chain path if it matches current position and goal. */
					if (v->landing_chain_path != nullptr &&
							v->landing_chain_path->valid &&
							!v->landing_chain_path->tiles.empty() &&
							v->landing_chain_path->tiles.front() == v->tile &&
							v->landing_chain_path->tiles.back() == goal) {
						v->taxi_path = std::move(v->landing_chain_path);
						v->taxi_path_index = 0;
						v->taxi_current_segment = FindTaxiSegmentIndex(v->taxi_path.get(), 0);
						v->taxi_wait_counter = 0;
						SetTaxiReservationUnlessOperationRunway(v, v->tile);
					} else if (!IsModularSafeStopTile(st, v->tile, goal)) {
						/* The precomputed path could not be installed and the aircraft is
						 * standing where it may not wait -- in practice the rollout end, on the
						 * runway. That is only a contract violation if it also no longer owns
						 * a reserved route to somewhere it *can* wait. A missing path object
						 * is not itself the test: the no-ground-goal landing branch reserves
						 * a runway plus a one-way buffer and resets the path deliberately, and
						 * that aircraft is perfectly safe -- it owns its queueing tile. */
						const bool owns_safe_stop = std::any_of(v->taxi_reserved_tiles.begin(), v->taxi_reserved_tiles.end(),
							[&](TileIndex t) { return IsValidTile(t) && (t == goal || IsModularSafeStopTile(st, t, goal)); });
						if (!owns_safe_stop && !restored_from_save) {
							Debug(misc, 1,
								"[ModAp] V{} unit#{} landing-chain-invariant: off a safe stop with no reserved route to one tile={} goal={} owned={}",
								v->index, v->unitnumber,
								IsValidTile(v->tile) ? v->tile.base() : 0,
								IsValidTile(goal) ? goal.base() : 0,
								v->taxi_reserved_tiles.size());
						}
					}
				} else if (!IsModularSafeStopTile(st, v->tile, goal)) {
					/* No service tile free yet and we are not on a safe stop (still on
					 * the runway): vacate to the nearest reachable safe stop. The landing
					 * chain guaranteed one exists by reserving an adjacent one-way buffer
					 * before touchdown. Once parked there the keepalive re-polls for a
					 * stand. */
					TileIndex holding_tile = FindModularRolloutHoldingTile(st, v, v->tile);
					if (holding_tile != INVALID_TILE && holding_tile != v->tile) {
						v->ground_path_goal = holding_tile;
						v->modular_ground_target = MGT_ROLLOUT;
						v->state = TERM1;
						if (ShouldLogModularRateLimited(v->index, 33, 64)) {
							Debug(misc, 2, "[ModAp] Vehicle {} rollout fallback: vacate runway via tile {}", v->index, holding_tile.base());
						}
					} else {
						if (ShouldLogModularRateLimited(v->index, 33, 64)) {
							const ModularAirportTileData *cd = st->airport.GetModularTileData(v->tile);
							Debug(misc, 1, "[ModAp] Vehicle {} rollout fallback failed: tile={} piece={} one_way={}", v->index,
								IsValidTile(v->tile) ? v->tile.base() : 0,
								cd != nullptr ? cd->piece_type : 255,
								cd != nullptr ? cd->one_way_taxi : 0);
						}
					}
				}
				/* else: already parked on a safe stop with no service tile free -- stay
				 * idle in MGT_ROLLOUT and let the keepalive re-poll for a stand. */

				/* Discard any remaining landing chain path -- either installed above or no longer needed. */
				v->landing_chain_path.reset();

			}
			break;

		case MGT_TERMINAL:
		case MGT_HELIPAD:
			if ((v->modular_ground_target == MGT_TERMINAL || v->modular_ground_target == MGT_HELIPAD) &&
					IsModularTileOccupiedByOtherAircraft(st, v->tile, v->index)) {
				/* Reservation desync safety: if another aircraft is already on this stand or pad,
				 * re-target to a different one instead of stacking aircraft on one tile. */
				uint8_t goal_target = MGT_NONE;
				TileIndex goal = FindModularUnstackParkingTile(st, v, &goal_target);
				if (goal != INVALID_TILE && goal != v->tile) {
					v->ground_path_goal = goal;
					v->modular_ground_target = goal_target;
					v->state = TERM1;
					return;
				}
			}
			if (IsAirportTile(v->tile)) {
				SetTaxiReservationUnlessOperationRunway(v, v->tile);
			}
			AircraftEntersTerminal(v);
			{
				/* Stock parity for the "service helicopters at helipads" setting. The stock
				 * check reads AirportFTAClass::num_helipads, which on a modular airport
				 * comes from the generic movement FSM and not the player-built layout, so
				 * read the tile we parked on instead. This
				 * is also the arrival that matters: for modular airports v->pos never
				 * moves, so the FTA's own "just arrived at a terminal" branch never runs. */
				const ModularAirportTileData *parked_on = st->airport.GetModularTileData(v->tile);
				MaybeServiceAircraftAtHelipad(v, parked_on != nullptr && IsModularHelipadPiece(parked_on->piece_type));
			}
			v->state = (v->subtype == AIR_HELICOPTER) ? HELIPAD1 : TERM1;
			v->modular_ground_target = MGT_NONE;
			break;

		case MGT_HANGAR:
			{
				const ModularAirportTileData *tile_data = st->airport.GetModularTileData(v->tile);
				if (tile_data == nullptr || !IsModularHangarPiece(tile_data->piece_type)) {
					/* Airport layout changed while this aircraft was taxiing to a depot target. */
					TileIndex alt = FindFreeModularHangar(st, v);
					if (ShouldLogModularRateLimited(v->index, 26, 64)) {
						Debug(misc, 1, "[ModAp] Vehicle {} reached non-hangar tile {} for hangar target; alt={}",
							v->index, IsValidTile(v->tile) ? v->tile.base() : 0,
							IsValidTile(alt) ? alt.base() : 0);
					}
					if (alt != INVALID_TILE && alt != v->tile) {
						v->ground_path_goal = alt;
						v->modular_ground_target = MGT_HANGAR;
						v->state = TERM1;
					} else {
						/* No usable hangar anymore: release the intent and continue normal flow. */
						v->modular_ground_target = MGT_NONE;
						v->state = TERM1;
					}
					return;
				}
			}
			if (IsAirportTile(v->tile)) {
				SetTaxiReservationUnlessOperationRunway(v, v->tile);
			}
			Debug(misc, 3, "[ModAp] Vehicle {} entering hangar at tile {}", v->index, v->tile.base());
			VehicleEnterDepot(v);
			v->state = HANGAR;
			v->modular_ground_target = MGT_NONE;
			break;

			case MGT_RUNWAY_TAKEOFF:
				if (st->airport.blocks.Test(AirportBlock::Zeppeliner)) {
					/* Airport-wide zeppeliner block: hold departures until the wreck is cleared. */
					ClearTaxiPathReservation(v, INVALID_TILE, true, false);
					ClearModularRunwayReservation(v);
					v->modular_takeoff_tile = INVALID_TILE;
					v->modular_takeoff_progress = 0;
					v->ground_path_goal = v->tile;
					v->modular_ground_target = MGT_NONE;
					v->state = TERM1;
					return;
				}

				/* Keep progressing through one-way queue tiles toward runway entry.
				 * Runway reservation is enforced only when actually entering runway tiles. */
				{
					const ModularAirportTileData *tile_data = st->airport.GetModularTileData(v->tile);
					const bool on_runway = (tile_data != nullptr && IsModularRunwayPiece(tile_data->piece_type));

					if (!on_runway) {
						if (!ShouldRetainRunwayReservation(v, st)) ClearModularRunwayReservation(v);
						if (IsAirportTile(v->tile)) {
							SetTaxiReservationUnlessOperationRunway(v, v->tile);
						}

						if (v->modular_takeoff_tile == INVALID_TILE) {
							v->modular_takeoff_tile = FindModularRunwayTileForTakeoff(st, v);
						}
						if (v->modular_takeoff_tile == INVALID_TILE) {
							v->ground_path_goal = v->tile;
							v->state = TERM1;
							return;
						}

						v->ground_path_goal = v->modular_takeoff_tile;
						v->state = TERM1;
						return;
					}
				}

			/* On runway entry tile: only start takeoff if full runway reservation is held.
			 * Without this guard, aircraft can enter TAKEOFF and deadlock forever. */
			if (v->modular_takeoff_tile == INVALID_TILE) v->modular_takeoff_tile = v->tile;
			if (!TryReserveContiguousModularRunway(v, st, v->modular_takeoff_tile)) {
				if (ShouldLogModularRateLimited(v->index, 30, 64)) {
					Debug(misc, 2, "[ModAp] V{} runway-entry wait: full-runway reserve not available at tile {}", v->index, v->tile.base());
				}
				v->ground_path_goal = v->tile;
				v->state = TERM1;
				return;
			}

			/* On runway entry tile with reservation: start takeoff roll. */
			if (IsAirportTile(v->tile)) {
				SetTaxiReservationUnlessOperationRunway(v, v->tile);
			}
			v->state = TAKEOFF;
			/* Keep the selected runway end rather than overwriting it with wherever the
			 * roll begins. GetRunwayOtherEnd is only meaningful from an end tile -- from a
			 * middle one it always answers "the high end" -- and it is what tells
			 * AirportMoveModularTakeoff which way to roll. */
			v->modular_takeoff_progress = 0;
			v->modular_ground_target = MGT_NONE;
			break;

		case MGT_HELI_TAKEOFF_TILE:
			if (st->airport.blocks.Test(AirportBlock::Zeppeliner)) {
				v->ground_path_goal = v->tile;
				v->modular_ground_target = MGT_NONE;
				v->state = TERM1;
				return;
			}
			v->state = HELITAKEOFF;
			v->modular_ground_target = MGT_NONE;
			break;

		default:
			v->modular_ground_target = MGT_NONE;
			break;
	}
}

void LogModularVehicleReservationState(const Station *st, const Aircraft *v, std::string_view reason)
{
	if (st == nullptr || v == nullptr || st->airport.modular_tile_data == nullptr) return;
	if (_debug_misc_level < 2) return;

	std::vector<TileIndex> owned_tiles;
	std::vector<TileIndex> owned_runway_tiles;
	for (const ModularAirportTileData &data : *st->airport.modular_tile_data) {
		Tile t(data.tile);
		if (!IsAirportTile(t)) continue;
		if (!IsModularAirportTileReservedBy(data.tile, v->index)) continue;
		owned_tiles.push_back(data.tile);
		if (IsModularRunwayPiece(data.piece_type)) owned_runway_tiles.push_back(data.tile);
	}

	std::sort(owned_tiles.begin(), owned_tiles.end(), [](TileIndex a, TileIndex b) { return a.base() < b.base(); });
	std::sort(owned_runway_tiles.begin(), owned_runway_tiles.end(), [](TileIndex a, TileIndex b) { return a.base() < b.base(); });
	std::vector<TileIndex> keep_set;
	BuildReservationKeepSet(v, st, keep_set);

	uint32_t tracked_but_unowned = 0;
	for (TileIndex tile : v->modular_runway_reservation) {
		if (std::find(owned_tiles.begin(), owned_tiles.end(), tile) == owned_tiles.end()) tracked_but_unowned++;
	}

	uint32_t owned_runway_untracked = 0;
	for (TileIndex tile : owned_runway_tiles) {
		if (std::find(v->modular_runway_reservation.begin(), v->modular_runway_reservation.end(), tile) == v->modular_runway_reservation.end()) owned_runway_untracked++;
	}

	uint32_t owned_minus_keep = 0;
	for (TileIndex tile : owned_tiles) {
		if (std::find(keep_set.begin(), keep_set.end(), tile) == keep_set.end()) owned_minus_keep++;
	}

	size_t path_len = (v->taxi_path != nullptr) ? v->taxi_path->tiles.size() : 0;
	Debug(misc, 2,
		"[ModAp] V{} unit#{} reserve-state reason='{}' state={} tile={} goal={} tgt={} path={}/{} runway_res={} owned={} owned_rw={} tracked_not_owned={} owned_rw_not_tracked={} keep_set={} owned_minus_keep={}",
		v->index, v->unitnumber, reason, v->state,
		IsValidTile(v->tile) ? v->tile.base() : 0,
		IsValidTile(v->ground_path_goal) ? v->ground_path_goal.base() : 0,
		v->modular_ground_target,
		v->taxi_path_index, path_len,
		v->modular_runway_reservation.size(), owned_tiles.size(), owned_runway_tiles.size(),
		tracked_but_unowned, owned_runway_untracked, keep_set.size(), owned_minus_keep);

	if (_debug_misc_level >= 3 && !owned_tiles.empty()) {
		std::string owned;
		for (TileIndex tile : owned_tiles) {
			if (!owned.empty()) owned += ",";
			owned += fmt::format("{}", tile.base());
		}
		Debug(misc, 2, "[ModAp] V{} owned-reservations [{}]", v->index, owned);
	}
	if (_debug_misc_level >= 3 && !v->modular_runway_reservation.empty()) {
		std::string tracked;
		for (TileIndex tile : v->modular_runway_reservation) {
			if (!tracked.empty()) tracked += ",";
			tracked += fmt::format("{}", tile.base());
		}
		Debug(misc, 2, "[ModAp] V{} tracked-runway [{}]", v->index, tracked);
	}
}

void LogModularTakeoffRunwayUnavailable(const Station *st, const Aircraft *v)
{
	auto &fail_state = _takeoff_fail_state;
	static constexpr uint64_t LOG_INTERVAL_TICKS = 74;

	ModularTakeoffFailLogState &state = fail_state[v->index];
	const uint64_t now = TimerGameTick::counter;

	if (state.last_tick != 0 && (now - state.last_tick) < LOG_INTERVAL_TICKS) {
		state.suppressed_count++;
		return;
	}

	if (state.suppressed_count > 0) {
		Debug(misc, 3, "[ModAp] Vehicle {} failed to find takeoff runway ({} suppressed)", v->index, state.suppressed_count);
	} else {
		Debug(misc, 3, "[ModAp] Vehicle {} failed to find takeoff runway", v->index);
	}

	if (st != nullptr && st->airport.modular_tile_data != nullptr) {
		const bool can_ground_route = CanUseModularGroundRouting(st, v);
		for (const ModularAirportTileData &data : *st->airport.modular_tile_data) {
			if (!IsModularRunwayEndPiece(data.piece_type)) continue;

			const uint8_t flags = GetRunwayFlags(st, data.tile);
			const bool mode_ok = (flags & RUF_TAKEOFF) != 0;
			const bool is_low = IsRunwayEndLow(st, data.tile);
			const bool dir_ok = is_low ? ((flags & RUF_DIR_HIGH) != 0) : ((flags & RUF_DIR_LOW) != 0);

			bool path_ok = false;
			int path_cost = -1;
			if (can_ground_route) {
				AirportGroundPath path = FindAirportGroundPath(st, v->tile, data.tile, nullptr, false, false, GetGroundPathRestriction(v));
				path_ok = path.found;
				path_cost = path.found ? path.cost : -1;
			}

			Debug(misc, 3, "[ModAp]  takeoff end {} flags={:x} mode_ok={} dir_ok={} is_low={} path_ok={} cost={}",
				data.tile.base(), flags, mode_ok, dir_ok, is_low, path_ok, path_cost);
		}
	}

	state.last_tick = now;
	state.suppressed_count = 0;
}

/**
 * Move aircraft on modular airport ground path.
 * @param v The aircraft.
 * @param st The station.
 * @return True if reached destination.
 */
bool AirportMoveModular(Aircraft *v, const Station *st)
{
	if (v->ground_path_goal == INVALID_TILE) return true;

	/* Stock-parity rollout braking applies only while still physically on runway. */
	bool rollout_on_runway = false;
	if (v->modular_ground_target == MGT_ROLLOUT &&
			IsValidTile(v->tile) &&
			st->TileBelongsToAirport(v->tile)) {
		const ModularAirportTileData *tile_data = st->airport.GetModularTileData(v->tile);
		rollout_on_runway = (tile_data != nullptr && IsModularRunwayPiece(tile_data->piece_type));
	}

	/* Ground-path movement must never run at flight/takeoff speeds.
	 * If landing/takeoff transitions leave residual high speed, clamp before any
	 * pathing/reservation decisions to avoid long-tail runway deadlocks. */
	const uint scaled_taxi_limit = SPEED_LIMIT_TAXI * _settings_game.vehicle.plane_speed;
	if (!rollout_on_runway && v->cur_speed > scaled_taxi_limit) {
		if (ShouldLogModularRateLimited(v->index, 61, 128)) {
			Debug(misc, 1, "[ModAp] V{} clamp pre-ground-move speed {}->{} state={} tile={} goal={} tgt={}",
				v->index, v->cur_speed, scaled_taxi_limit, v->state,
				IsValidTile(v->tile) ? v->tile.base() : 0,
				IsValidTile(v->ground_path_goal) ? v->ground_path_goal.base() : 0,
				v->modular_ground_target);
		}
		v->cur_speed = scaled_taxi_limit;
		v->subspeed = 0;
		v->modular_takeoff_progress = 0;
	}

	/* Stock-parity crash roll: the classic FTA path rolls MaybeCrashAirplane on
	 * every brake tick while decelerating on the runway after landing. Mirror
	 * that here during runway rollout so the per-landing crash risk equals a
	 * stock airport. A fast jet landing at an airport that lacks the
	 * large-aircraft safety requirements gets the elevated short-strip overrun
	 * chance; every other plane gets the general "Plane crashes" chance. (Takeoff
	 * never brakes, so -- like stock -- there is no takeoff crash.) */
	if (rollout_on_runway && v->cur_speed > scaled_taxi_limit &&
			MaybeCrashModularAircraft(v, st)) {
		return false;
	}

	if (!IsValidTile(v->tile) || !st->TileBelongsToAirport(v->tile)) {
		ClearTaxiPathState(v);
		return false;
	}

	const ModularAirportTileData *goal_data = st->airport.GetModularTileData(v->ground_path_goal);
	if (goal_data == nullptr) {
		if (ShouldLogModularRateLimited(v->index, 62, 128)) {
			Debug(misc, 1, "[ModAp] V{} goal_data null for goal={} tgt={} st={} vtile={}",
				v->index, v->ground_path_goal.base(), v->modular_ground_target,
				st->index, IsValidTile(v->tile) ? v->tile.base() : 0);
		}
		ClearTaxiPathState(v);
		v->ground_path_goal = INVALID_TILE;
		v->modular_ground_target = MGT_NONE;
		return true;
	}

	if (v->tile == v->ground_path_goal) {
		UpdateModularAircraftParkedDirection(v, st);
		ClearTaxiPathState(v, v->tile);
		v->ground_path_goal = INVALID_TILE;
		HandleModularGroundArrival(v);
		return true;
	}

	/* A departure standing on its runway with enough of it left to get airborne is where
	 * it needs to be, whether or not that is the end the route was aiming at. The arrival
	 * handler starts the roll from the tile the aircraft is on, so this is only a matter
	 * of stopping the taxi early; the rest of the route is released with the taxi path.
	 * The whole-runway claim is not held yet -- taxiing out only ever takes the per-tile
	 * crossing reservations -- so the arrival handler makes it here exactly as it does at
	 * the runway end, and waits on this tile, retrying every tick, if it is refused. That
	 * cannot strand the aircraft behind another user of the same runway: from the moment
	 * modular_takeoff_tile is set, a pending departure excludes every other operation on
	 * that runway (IsContiguousModularRunwayQueuedForTakeoffByOther). */
	if (v->modular_ground_target == MGT_RUNWAY_TAKEOFF && ModularTakeoffRunFitsFrom(st, v, v->tile)) {
		UpdateModularAircraftParkedDirection(v, st);
		ClearTaxiPathState(v, v->tile);
		v->ground_path_goal = INVALID_TILE;
		HandleModularGroundArrival(v);
		return true;
	}

	const bool needs_rebuild =
			v->taxi_path == nullptr ||
			!v->taxi_path->valid ||
			v->taxi_path->tiles.empty() ||
			v->taxi_path_index >= v->taxi_path->tiles.size() ||
			v->taxi_path->tiles[v->taxi_path_index] != v->tile ||
			v->taxi_path->tiles.back() != v->ground_path_goal;
	if (needs_rebuild) {
		const uint16_t saved_wait = v->taxi_wait_counter;
		ClearTaxiPathState(v, v->tile);
		v->taxi_wait_counter = saved_wait;
		/* Allow runway crossing when already committed to a goal -- the two-pass
		 * selection in FindModularRunwayTileForTakeoff ensures crossing goals
		 * are only assigned when no strict path exists. */
		/* Departures pick their runway in FindModularRunwayTileForTakeoff but their *route*
		 * here, so this is where an alternative exit actually gets taken. Falling back to
		 * the last attempt preserves the old behaviour when no reservable route exists:
		 * the aircraft still has a path to walk and waits on the reservation instead. */
		ReservableRoute rebuild = FindReservableRoute(v, st, v->tile, v->ground_path_goal,
				GetGroundOperationRunwayTile(v, st), true, GroundPathRestriction::FromAircraft, v);
		TaxiPath new_path = std::move(rebuild.path);
		if (!new_path.valid || new_path.tiles.size() < 2 || new_path.segments.empty()) {
			v->taxi_wait_counter++;
			if (_debug_misc_level >= 1 && v->taxi_wait_counter >= 128 && (v->taxi_wait_counter % 128) == 0) {
				/* Diagnostic A* only when someone is listening -- gate on debug level. */
				AirportGroundPath dbg_path = FindAirportGroundPath(st, v->tile, v->ground_path_goal, v, false, false);
				Debug(misc, 1,
					"[ModAp] V{} unit#{} stuck(no-path) wait={} state={} tile={} goal={} tgt={} path_found={} cost={}",
					v->index, v->unitnumber, v->taxi_wait_counter, v->state,
					IsValidTile(v->tile) ? v->tile.base() : 0,
					IsValidTile(v->ground_path_goal) ? v->ground_path_goal.base() : 0,
					v->modular_ground_target, dbg_path.found, dbg_path.cost);
			}
			if (v->taxi_wait_counter > 64 && (v->taxi_wait_counter % 64) == 0) {
				/* Periodically try a different goal.
				 * Do not pre-clear reservations here; keep current staged ownership
				 * unless retargeting actually succeeds and rebuilds path state. */
				if (TryRetargetModularGroundGoal(v, st)) {
					v->taxi_wait_counter = 0;
				}
			}
			UpdateModularAircraftParkedDirection(v, st);
			return false;
		}

		v->taxi_path = std::make_unique<TaxiPath>(std::move(new_path));
		v->taxi_path_index = 0;
		v->taxi_current_segment = FindTaxiSegmentIndex(v->taxi_path.get(), 0);
		v->taxi_wait_counter = 0;
		SetTaxiReservationUnlessOperationRunway(v, v->tile);
	}

	if (v->taxi_path == nullptr || static_cast<size_t>(v->taxi_path_index) + 1 >= v->taxi_path->tiles.size()) {
		UpdateModularAircraftParkedDirection(v, st);
		ClearTaxiPathState(v, v->tile);
		v->ground_path_goal = INVALID_TILE;
		HandleModularGroundArrival(v);
		return true;
	}

	const uint16_t current_index = v->taxi_path_index;
	const uint16_t next_index = current_index + 1;
	const uint8_t next_segment = FindTaxiSegmentIndex(v->taxi_path.get(), next_index);
	if (next_segment >= v->taxi_path->segments.size()) {
		ClearTaxiPathState(v, v->tile);
		return false;
	}

	TileIndex next_tile = v->taxi_path->tiles[next_index];
	const TaxiSegmentType next_type = v->taxi_path->segments[next_segment].type;
	TaxiReserveResult reserve_result;
	/* Revalidate the same forward horizon before every step. Self-owned claims make
	 * this cheap after the first commit, while doing it unconditionally prevents a
	 * path-type transition or reconciled claim from weakening the entry contract. */
	if (!TryReserveTaxiSegment(v, st, next_segment, &reserve_result)) {
		v->taxi_wait_counter++;
		if (v->taxi_wait_counter >= 128 && (v->taxi_wait_counter % 128) == 0) {
			/* Report what the reservation actually refused, not what the next tile looks
			 * like. A segment claims more than one tile -- the full path horizon to the
			 * next safe stop -- so "next is free" and "the claim failed" are routinely
			 * both true, and re-deriving blockers from `next` used
			 * to print all-clear for a genuinely blocked aircraft. */
			Debug(misc, 1,
				"[ModAp] V{} unit#{} stuck(reserve) st={} wait={} state={} tile={} next={} seg={} goal={} tgt={} deny={} deny_tile={} deny_by=V{}",
				v->index, v->unitnumber, st->index.base(), v->taxi_wait_counter, v->state,
				IsValidTile(v->tile) ? v->tile.base() : 0,
				IsValidTile(next_tile) ? next_tile.base() : 0,
				static_cast<uint8_t>(next_type),
				IsValidTile(v->ground_path_goal) ? v->ground_path_goal.base() : 0,
				v->modular_ground_target,
				TaxiReserveFailureName(reserve_result.reason),
				IsValidTile(reserve_result.tile) ? reserve_result.tile.base() : 0,
				reserve_result.blocker == VehicleID::Invalid() ? 0 : reserve_result.blocker.base());

			/* Waiting on a tile the aircraft may not hold. The entry contracts are built so
			 * this cannot be reached by taxiing into it -- only by already being there when
			 * the route was lost, i.e. after a landing rollout. Distinct from ordinary
			 * contention: the aircraft is pinning a shared resource while it waits. */
			if (!IsModularSafeStopTile(st, v->tile, v->ground_path_goal) && IsPathTileRunwayPiece(st, v->tile)) {
				Debug(misc, 1,
					"[ModAp] V{} unit#{} runway-rest-invariant: waiting on runway tile={} wait={} goal={} tgt={} deny={} deny_tile={}",
					v->index, v->unitnumber,
					IsValidTile(v->tile) ? v->tile.base() : 0, v->taxi_wait_counter,
					IsValidTile(v->ground_path_goal) ? v->ground_path_goal.base() : 0,
					v->modular_ground_target,
					TaxiReserveFailureName(reserve_result.reason),
					IsValidTile(reserve_result.tile) ? reserve_result.tile.base() : 0);
			}

		}
		if (v->taxi_wait_counter > 64 && (v->taxi_wait_counter % 64) == 0) {
			/* Keep existing reservations unless retarget succeeds. */
			if (TryRetargetModularGroundGoal(v, st)) {
				v->taxi_wait_counter = 0;
			}
		}
		UpdateModularAircraftParkedDirection(v, st);
		return false;
	}
	v->taxi_wait_counter = 0;

	/* Final safety gate before movement: never enter a tile currently occupied by another aircraft,
	 * even if reservation ownership was stale. */
	if (next_tile != v->tile && IsModularTileOccupiedByOtherAircraft(st, next_tile, v->index)) {
		v->taxi_wait_counter++;
		if (v->taxi_wait_counter >= 128 && (v->taxi_wait_counter % 128) == 0) {
			Debug(misc, 1,
				"[ModAp] V{} unit#{} stuck(occupied) wait={} state={} tile={} next={} goal={} tgt={}",
				v->index, v->unitnumber, v->taxi_wait_counter, v->state,
				IsValidTile(v->tile) ? v->tile.base() : 0,
				IsValidTile(next_tile) ? next_tile.base() : 0,
				IsValidTile(v->ground_path_goal) ? v->ground_path_goal.base() : 0,
				v->modular_ground_target);
		}
		if (v->taxi_wait_counter > 64 && (v->taxi_wait_counter % 64) == 0) {
			/* Keep existing reservations unless retarget succeeds. */
			if (TryRetargetModularGroundGoal(v, st)) {
				v->taxi_wait_counter = 0;
			}
		}
		UpdateModularAircraftParkedDirection(v, st);
		return false;
	}

	const int target_x = TileX(next_tile) * TILE_SIZE + TILE_SIZE / 2;
	const int target_y = TileY(next_tile) * TILE_SIZE + TILE_SIZE / 2;
	const int dist = abs(v->x_pos - target_x) + abs(v->y_pos - target_y);

	if (v->vehstatus.Test(VehState::Hidden) && dist > 0) {
		AircraftLeaveHangar(v, GetModularHangarExitDirection(st, v->tile));
	}

	if (dist > 0) {
		Direction new_dir = GetDirectionTowards(v, target_x, target_y);
		if (new_dir != v->direction) {
			v->last_direction = v->direction;
			v->direction = new_dir;
			v->turn_counter = 0;
			v->number_consecutive_turns = 0;
			SetAircraftPosition(v, v->x_pos, v->y_pos, v->z_pos);
		}

		/* Match stock Brake behavior while rolling out on runway: soft decel to taxi speed. */
		int count = rollout_on_runway ?
				UpdateAircraftSpeed(v, SPEED_LIMIT_TAXI, false) :
				UpdateAircraftSpeed(v, SPEED_LIMIT_TAXI);
		while (count-- > 0) {
			GetNewVehiclePosResult gp = GetNewVehiclePos(v);
			v->x_pos = gp.x;
			v->y_pos = gp.y;
			SetAircraftPosition(v, v->x_pos, v->y_pos, v->z_pos);
			if (abs(v->x_pos - target_x) + abs(v->y_pos - target_y) == 0) break;
		}
	}

	if (v->x_pos != target_x || v->y_pos != target_y) return false;

	const uint8_t old_segment = FindTaxiSegmentIndex(v->taxi_path.get(), current_index);
	v->tile = next_tile;
	v->taxi_path_index = next_index;
	v->taxi_current_segment = next_segment;
	v->number_consecutive_turns = 0;
	if (v->tile != v->ground_path_goal && IsOneWayTaxiTile(st, v->tile)) {
		UpdateModularAircraftParkedDirection(v, st);
	}

	const TaxiSegmentType old_type = (old_segment < v->taxi_path->segments.size()) ? v->taxi_path->segments[old_segment].type : TaxiSegmentType::FreeMove;
	const bool runway_exit_transition = (old_type == TaxiSegmentType::Runway && next_type != TaxiSegmentType::Runway);
	if (rollout_on_runway && runway_exit_transition && v->cur_speed > scaled_taxi_limit) {
		/* Rollout soft-brake applies only while physically on runway; clamp immediately once exited. */
		v->cur_speed = scaled_taxi_limit;
		v->subspeed = 0;
		v->modular_takeoff_progress = 0;
	}
	/* Operation-runway tiles stay in the runway tracking vector; crossings and all
	 * other ground movement stay in taxi tracking. Never classify one tile as both. */
	SetTaxiReservationUnlessOperationRunway(v, v->tile);

	std::vector<TileIndex> keep_set;
	BuildReservationKeepSet(v, st, keep_set);
	ReconcileAircraftReservations(v, st, keep_set, "post-step");

	if (v->tile == v->ground_path_goal || static_cast<size_t>(v->taxi_path_index) + 1 >= v->taxi_path->tiles.size()) {
		UpdateModularAircraftParkedDirection(v, st);
		ClearTaxiPathState(v, v->tile);
		v->ground_path_goal = INVALID_TILE;
		HandleModularGroundArrival(v);
		return true;
	}

	return false;
}

void AirportMoveModularFlying(Aircraft *v, const Station *st)
{
	int target_x = 0;
	int target_y = 0;

	if (v->subtype == AIR_AIRCRAFT) {
		uint32_t nearest_wp = 0;
		GetModularHoldingWaypointTarget(v, st, &target_x, &target_y, &nearest_wp);
		/* Log at level 3 once every ~256 ticks per aircraft (staggered by vehicle index). */
		if ((TimerGameTick::counter & 0xFF) == (v->index.base() & 0xFF)) {
			const uint32_t n_wp = static_cast<uint32_t>(GetModularHoldingLoop(st).waypoints.size());
			Debug(misc, 3, "[ModAp] Hold V{}: nearest={}/{} pos=({},{}) target=({},{})",
				v->index, nearest_wp, n_wp, v->x_pos, v->y_pos, target_x, target_y);
		}
	} else {
		/* Helicopters always orbit the holding square while in FLYING.
		 * AircraftEventHandler_Flying runs immediately after this and decides
		 * whether to commit (reserve + transition to HELILANDING).  Once
		 * committed, AirportMoveModularLanding flies the heli from its current
		 * holding position to the landing tile.  Targeting the runway here too
		 * would make every flying heli redirect to the same point in the brief
		 * window between landings, even though only one of them will commit. */
		GetModularHeliHoldingTarget(v, st, &target_x, &target_y);
	}

	const int dist = abs(v->x_pos - target_x) + abs(v->y_pos - target_y);

	if (v->subtype == AIR_HELICOPTER) {
		Debug(misc, 3, "[ModAp] Fly: v=({},{},{}), target=({},{},?), dist={}",
			v->x_pos, v->y_pos, v->z_pos, target_x, target_y, dist);
	}

	/* Rate-limited turning: only apply a new heading once turn_counter reaches 0 and there
	 * is a non-zero distance to the target. Mirrors the FTA SlowTurn logic (~line 1138)
	 * which uses 2*plane_speed ticks between turns to prevent rapid heading flips. */
	if (dist > 0) {
		if (v->turn_counter > 0) {
			v->turn_counter--;
		} else {
			Direction new_dir = GetDirectionTowards(v, target_x, target_y);
			if (new_dir != v->direction) {
				if (new_dir == v->last_direction) {
					v->number_consecutive_turns = 0;
				} else {
					v->number_consecutive_turns++;
				}
				v->turn_counter = 2 * _settings_game.vehicle.plane_speed;
				v->last_direction = v->direction;
				v->direction = new_dir;
				SetAircraftPosition(v, v->x_pos, v->y_pos, v->z_pos);
			}
		}
	}

	/* Always update speed and move regardless of dist. The target is a lookahead waypoint
	 * that the ghost advances discretely every MODULAR_HOLDING_TICKS_PER_WP ticks; wrapping
	 * all movement inside `if (dist > 0)` caused the aircraft to freeze for up to one full
	 * waypoint period (~1 s) each time it caught the target. */
	int count = UpdateAircraftSpeed(v, SPEED_LIMIT_HOLD);
	for (int i = 0; i < count; i++) {
		GetNewVehiclePosResult gp = GetNewVehiclePos(v);
		v->x_pos = gp.x;
		v->y_pos = gp.y;
		v->tile = TileIndex{}; // In air

		int target_z = GetAircraftFlightLevel(v);
		if (v->z_pos < target_z) v->z_pos++;
		else if (v->z_pos > target_z) v->z_pos--;

		SetAircraftPosition(v, v->x_pos, v->y_pos, v->z_pos);
	}
}
