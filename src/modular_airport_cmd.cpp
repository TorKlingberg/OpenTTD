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

#include "safeguards.h"

#include <map>
#include <set>
#include <chrono>
#include <cmath>
#include <algorithm>
#include <array>
#include <limits>

/**
 * Special velocities for aircraft.
 */
static constexpr uint16_t SPEED_LIMIT_TAXI = 50; ///< Maximum speed of an aircraft while taxiing
static constexpr uint16_t SPEED_LIMIT_APPROACH = 230; ///< Maximum speed of an aircraft on finals
static constexpr uint16_t SPEED_LIMIT_HOLD = 425; ///< Maximum speed of an aircraft that flies the holding pattern
static constexpr uint16_t SPEED_LIMIT_NONE = UINT16_MAX; ///< No environmental speed limit. Speed limit is type dependent

bool IsModularHelipadPiece(uint8_t gfx)
{
	switch (gfx) {
		case APT_HELIPORT:
		case APT_HELIPAD_1:
		case APT_HELIPAD_2:
		case APT_HELIPAD_2_FENCE_NW:
		case APT_HELIPAD_2_FENCE_NE_SE:
		case APT_HELIPAD_3_FENCE_SE_SW:
		case APT_HELIPAD_3_FENCE_NW_SW:
		case APT_HELIPAD_3_FENCE_NW:
			return true;
		default:
			return false;
	}
}

bool IsModernModularPiece(uint8_t piece_type)
{
	switch (piece_type) {
		/* Legacy pieces — always available */
		case APT_RUNWAY_SMALL_NEAR_END:
		case APT_RUNWAY_SMALL_MIDDLE:
		case APT_RUNWAY_SMALL_FAR_END:
		case APT_APRON:
		case APT_APRON_FENCE_NW:
		case APT_APRON_FENCE_SW:
		case APT_APRON_FENCE_NE:
		case APT_APRON_FENCE_NE_SW:
		case APT_APRON_FENCE_SE_SW:
		case APT_APRON_FENCE_SE:
		case APT_APRON_FENCE_NE_SE:
		case APT_APRON_W:
		case APT_APRON_S:
		case APT_APRON_VER_CROSSING_S:
		case APT_APRON_HOR_CROSSING_W:
		case APT_APRON_VER_CROSSING_N:
		case APT_APRON_HOR_CROSSING_E:
		case APT_APRON_E:
		case APT_ARPON_N:
		case APT_APRON_HOR:
		case APT_APRON_N_FENCE_SW:
		case APT_APRON_HALF_EAST:
		case APT_APRON_HALF_WEST:
		case APT_STAND:
		case APT_SMALL_DEPOT_SE:
		case APT_SMALL_DEPOT_SW:
		case APT_SMALL_DEPOT_NW:
		case APT_SMALL_DEPOT_NE:
		case APT_GRASS_1:
		case APT_GRASS_2:
		case APT_GRASS_FENCE_SW:
		case APT_GRASS_FENCE_NE_FLAG:
		case APT_GRASS_FENCE_NE_FLAG_2:
		case APT_EMPTY:
		case APT_EMPTY_FENCE_NE:
		case APT_LOW_BUILDING:
		case APT_LOW_BUILDING_FENCE_N:
		case APT_LOW_BUILDING_FENCE_NW:
		case APT_SMALL_BUILDING_1:
		case APT_SMALL_BUILDING_2:
		case APT_SMALL_BUILDING_3:
		case APT_STAND_1:
		case APT_RADIO_TOWER_FENCE_NE:
			return false;
		default:
			return true;
	}
}

TimerGameCalendar::Year GetModularPieceMinYear(uint8_t piece_type)
{
	if (!IsModernModularPiece(piece_type)) return CalendarTime::MIN_YEAR;
	return AirportSpec::Get(AT_LARGE)->min_year;
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
		const ModularAirportTileData *current_data = st->airport.GetModularTileData(current);
		if (!IsRunwayPieceOnAxis(current_data, horizontal)) break;
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
	if (tiles.size() < 6) return false;
	for (TileIndex t : tiles) {
		const ModularAirportTileData *td = st->airport.GetModularTileData(t);
		if (td == nullptr || !IsLargeRunwayFamily(td->piece_type)) return false;
	}
	return true;
}

static bool IsBigTerminalPiece(uint8_t piece_type)
{
	switch (piece_type) {
		case APT_ROUND_TERMINAL:
		case APT_BUILDING_1:
		case APT_BUILDING_2:
		case APT_BUILDING_3:
		case APT_STAND_1:
		case APT_STAND_PIER_NE:
			return true;
		default:
			return false;
	}
}

/**
 * Check whether a modular airport has the infrastructure to support large aircraft:
 * a tower, a big terminal, and at least one safe (long, large-family) runway.
 */
bool ModularAirportSupportsLargeAircraft(const Station *st)
{
	if (st->airport.modular_tile_data == nullptr) return false;

	bool has_tower = false;
	bool has_big_terminal = false;
	bool has_safe_runway = false;

	for (const ModularAirportTileData &data : *st->airport.modular_tile_data) {
		if (data.piece_type == APT_TOWER || data.piece_type == APT_TOWER_FENCE_SW) has_tower = true;
		if (IsBigTerminalPiece(data.piece_type)) has_big_terminal = true;

		/* Check runway ends for safety. */
		if (IsModularRunwayPiece(data.piece_type) &&
				(data.piece_type == APT_RUNWAY_END) &&
				!has_safe_runway) {
			has_safe_runway = IsRunwaySafeForLarge(st, data.tile);
		}

		if (has_tower && has_big_terminal && has_safe_runway) return true;
	}

	return has_tower && has_big_terminal && has_safe_runway;
}

void ClearModularRunwayReservation(Aircraft *v)
{
	for (TileIndex tile : v->modular_runway_reservation) {
		if (tile == INVALID_TILE || !IsTileType(tile, TileType::Station)) continue;
		Tile t(tile);
		if (!IsAirportTile(t)) continue;
		if (HasAirportTileReservation(t) && GetAirportTileReserver(t) == v->index) {
			SetAirportTileReservation(t, false);
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
		if (HasAirportTileReservation(t) && GetAirportTileReserver(t) == vid) {
			SetAirportTileReservation(t, false);
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

		v->ground_path_goal = INVALID_TILE;
		v->modular_ground_target = MGT_NONE;
		v->modular_landing_tile = INVALID_TILE;
		v->modular_landing_goal = INVALID_TILE;
		v->modular_landing_stage = 0;
		v->modular_takeoff_tile = INVALID_TILE;
		v->modular_takeoff_progress = 0;

		/* Move to hangar tile. */
		int hx = TileX(hangar) * TILE_SIZE + TILE_SIZE / 2;
		int hy = TileY(hangar) * TILE_SIZE + TILE_SIZE / 2;
		int hz = GetTileMaxPixelZ(hangar);

		v->tile = hangar;
		SetAircraftPosition(v, hx, hy, hz);
		VehicleEnterDepot(v);

		Debug(misc, 1, "[ModAp] Teleported vehicle {} from removed tile {} to hangar {}",
			v->index, tile.base(), hangar.base());
	}

	return true;
}

bool ShouldLogModularRateLimited(VehicleID vid, uint8_t channel, uint32_t interval_ticks)
{
	static std::unordered_map<uint32_t, uint64_t> last_tick_by_key;
	const uint32_t key = (uint32_t(vid.base()) << 8) | channel;
	const uint64_t now = TimerGameTick::counter;
	auto it = last_tick_by_key.find(key);
	if (it != last_tick_by_key.end() && now - it->second < interval_ticks) return false;
	last_tick_by_key[key] = now;
	return true;
}

bool TryReserveContiguousModularRunway(Aircraft *v, const Station *st, TileIndex runway_tile)
{
	std::vector<TileIndex> runway_tiles;
	if (!GetContiguousModularRunwayTiles(st, runway_tile, runway_tiles)) return false;

	VehicleID state_blocker = VehicleID::Invalid();
	if (IsContiguousModularRunwayReservedInStateByOther(v, st, runway_tiles, &state_blocker)) {
		if (ShouldLogModularRateLimited(v->index, 1, 128)) {
			Debug(misc, 2, "[ModAp] V{} runway-reserve denied: runway held in state by V{}", v->index, state_blocker.base());
		}
		if (!v->modular_runway_reservation.empty()) ClearModularRunwayReservation(v);
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
			if (!v->modular_runway_reservation.empty()) ClearModularRunwayReservation(v);
			if (ShouldLogModularRateLimited(v->index, 2, 128)) {
				LogModularVehicleReservationState(st, v, "reserve denied (occupied)");
			}
			return false;
		}

		Tile t(tile);
		if (!IsAirportTile(t)) continue;
		if (HasAirportTileReservation(t) && GetAirportTileReserver(t) != v->index) {
			if (ShouldLogModularRateLimited(v->index, 1, 128)) {
				Debug(misc, 2, "[ModAp] V{} runway-reserve denied: runway tile {} reserved by V{}", v->index, tile.base(), GetAirportTileReserver(t).base());
			}
			if (!v->modular_runway_reservation.empty()) ClearModularRunwayReservation(v);
			if (ShouldLogModularRateLimited(v->index, 2, 128)) {
				LogModularVehicleReservationState(st, v, "reserve denied (reserved)");
			}
			return false;
		}
	}

	const bool reservation_changed = (v->modular_runway_reservation != runway_tiles);
	if (reservation_changed) {
		ClearModularRunwayReservation(v);
	}

	for (TileIndex tile : runway_tiles) {
		Tile t(tile);
		if (!IsAirportTile(t)) continue;
		SetAirportTileReservation(t, true);
		SetAirportTileReserver(t, v->index);
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
		if (HasAirportTileReservation(t) && GetAirportTileReserver(t) != v->index) return true;
	}

	return false;
}

bool IsContiguousModularRunwayBusyByOther(const Aircraft *v, const Station *st, TileIndex runway_tile)
{
	std::vector<TileIndex> runway_tiles;
	if (!GetContiguousModularRunwayTiles(st, runway_tile, runway_tiles)) return false;

	for (TileIndex tile : runway_tiles) {
		if (IsModularTileOccupiedByOtherAircraft(st, tile, v->index)) return true;

		Tile t(tile);
		if (!IsAirportTile(t)) continue;
		if (HasAirportTileReservation(t) && GetAirportTileReserver(t) != v->index) return true;
	}

	return false;
}

bool IsContiguousModularRunwayReservedInStateByOther(const Aircraft *v, const Station *st, std::span<const TileIndex> runway_tiles, VehicleID *blocker)
{
	for (const Aircraft *other : Aircraft::Iterate()) {
		if (other->index == v->index) continue;
		if (!other->IsNormalAircraft()) continue;

		const bool tied_to_station = (other->targetairport == st->index || other->last_station_visited == st->index);
		if (!tied_to_station) continue;

		const ModularAirportTileData *other_tile_data = (IsValidTile(other->tile) ? st->airport.GetModularTileData(other->tile) : nullptr);
		const bool other_on_runway = (other_tile_data != nullptr && IsModularRunwayPiece(other_tile_data->piece_type));

		const bool in_runway_flow =
				other->state == LANDING || other->state == ENDLANDING ||
				other->state == HELILANDING || other->state == HELIENDLANDING ||
				other->state == TAKEOFF || other->state == STARTTAKEOFF || other->state == ENDTAKEOFF ||
				(other->modular_ground_target == MGT_ROLLOUT && other_on_runway);
		if (!in_runway_flow) continue;

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
	}

	return false;
}

bool IsContiguousModularRunwayQueuedForTakeoffByOther(const Aircraft *v, const Station *st, TileIndex runway_tile)
{
	std::vector<TileIndex> runway_tiles;
	if (!GetContiguousModularRunwayTiles(st, runway_tile, runway_tiles)) return false;

	for (const Aircraft *other : Aircraft::Iterate()) {
		if (other->index == v->index) continue;
		if (!other->IsNormalAircraft()) continue;
		if (other->targetairport != st->index && other->last_station_visited != st->index) continue;
		if (other->modular_ground_target != MGT_RUNWAY_TAKEOFF) continue;
		if (!IsValidTile(other->modular_takeoff_tile)) continue;

		if (std::find(runway_tiles.begin(), runway_tiles.end(), other->modular_takeoff_tile) != runway_tiles.end()) {
			return true;
		}
	}

	return false;
}

TileIndex FindModularLandingGroundGoal(const Station *st, const Aircraft *v, uint8_t *target)
{
	TileIndex goal = INVALID_TILE;
	uint8_t tgt = MGT_NONE;

	/* Only look for a hangar if the aircraft actually needs one (depot order / servicing). */
	bool wants_depot = v->current_order.IsType(OT_GOTO_DEPOT) || v->NeedsAutomaticServicing();

	if (wants_depot) {
		goal = FindFreeModularHangar(st, v);
		if (goal != INVALID_TILE) tgt = MGT_HANGAR;
	}
	if (goal == INVALID_TILE && v->subtype == AIR_HELICOPTER) {
		goal = FindFreeModularHelipad(st, v);
		if (goal != INVALID_TILE) tgt = MGT_HELIPAD;
	}
	if (goal == INVALID_TILE) {
		goal = FindFreeModularTerminal(st, v);
		if (goal != INVALID_TILE) tgt = MGT_TERMINAL;
	}

	if (target != nullptr) *target = tgt;
	return goal;
}

bool TryReserveLandingChain(Aircraft *v, const Station *st, TileIndex runway_tile, TileIndex ground_goal)
{
	/* Helper: check if a tile is blocked by another aircraft, exempting hangars (multi-capacity). */
	const auto blocked_by_other = [&](TileIndex tile) {
		if (IsModularHangarTile(st, tile)) return false;
		if (IsTaxiTileReservedByOther(st, tile, v->index)) return true;
		if (IsModularTileOccupiedByOtherAircraft(st, tile, v->index)) return true;
		return false;
	};
	TileIndex rollout = FindModularRunwayRolloutPoint(st, runway_tile);
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

	/* No ground goal: do not allow landing.
	 * For modular airports we prefer blocking arrivals over creating runway/gridlock
	 * when no stand/hangar destination can be reserved. */
	if (ground_goal == INVALID_TILE) {
		return log_chain_fail("no_goal_disallowed");
	}

	/* Normal landing chain: reserve runway + exit path to first safe queuing point. */
	if (!TryReserveContiguousModularRunway(v, st, runway_tile)) return log_chain_fail("runway_reserve_failed");

	/* Some runway layouts do not yield a distinct rollout tile; in that case,
	 * start path reservation from the runway tile itself instead of allowing a
	 * runway-only reservation. */
	TileIndex path_start = rollout != INVALID_TILE ? rollout : runway_tile;

	TaxiPath path = BuildTaxiPath(st, path_start, ground_goal, nullptr);
	if (!path.valid || path.tiles.empty() || path.segments.empty()) {
		ClearModularRunwayReservation(v);
		return log_chain_fail("path_invalid");
	}

	uint8_t seg_idx = FindTaxiSegmentIndex(&path, 0);
	while (seg_idx < path.segments.size() && path.segments[seg_idx].type == TaxiSegmentType::RUNWAY) seg_idx++;
	/* A valid ground goal must always yield a non-runway segment to reserve.
	 * If the path is runway-only, don't allow touchdown with runway-only locks. */
	if (seg_idx >= path.segments.size()) {
		ClearTaxiPathReservation(v, INVALID_TILE);
		ClearModularRunwayReservation(v);
		return log_chain_fail("no_non_runway_segment");
	}

	/* If the whole post-runway route is FREE_MOVE (no ONE_WAY buffering),
	 * require a full reservation all the way to the destination tile. */
	bool has_one_way_after_runway = false;
	for (uint8_t i = seg_idx; i < path.segments.size(); ++i) {
		if (path.segments[i].type == TaxiSegmentType::ONE_WAY) {
			has_one_way_after_runway = true;
			break;
		}
	}

	if (!has_one_way_after_runway) {
		std::vector<TileIndex> full_reserved;
		full_reserved.reserve(path.tiles.size() - path.segments[seg_idx].start_index);
		for (uint16_t i = path.segments[seg_idx].start_index; i < path.tiles.size(); ++i) {
			TileIndex tile = path.tiles[i];
			if (blocked_by_other(tile)) {
				ClearTaxiPathReservation(v, INVALID_TILE);
				ClearModularRunwayReservation(v);
				return log_chain_fail("fullpath_blocked", tile);
			}
			full_reserved.push_back(tile);
		}
		for (TileIndex tile : full_reserved) SetTaxiReservation(v, tile);
		return true;
	}

	/* Strict landing reservation: once a concrete ground goal is chosen,
	 * reserve the complete non-runway chain to that goal. */
	std::vector<TileIndex> full_reserved;
	full_reserved.reserve(path.tiles.size() - path.segments[seg_idx].start_index);
	for (uint16_t i = path.segments[seg_idx].start_index; i < path.tiles.size(); ++i) {
		TileIndex tile = path.tiles[i];
		if (blocked_by_other(tile)) {
			ClearTaxiPathReservation(v, INVALID_TILE);
			ClearModularRunwayReservation(v);
			return log_chain_fail("fullpath_blocked", tile);
		}
		full_reserved.push_back(tile);
	}

	for (TileIndex tile : full_reserved) SetTaxiReservation(v, tile);
	return true;
}

TileIndex FindModularLandingTarget(const Station *st, const Aircraft *v)
{
	if (st->airport.modular_tile_data == nullptr) return INVALID_TILE;
	if (st->airport.blocks.Test(AirportBlock::Zeppeliner)) return INVALID_TILE;

	TileIndex best_tile = INVALID_TILE;
	int best_score = INT_MAX;

	bool is_heli = v->subtype == AIR_HELICOPTER;
	int candidates_total = 0;
	int rejected_not_end = 0;
	int rejected_mode = 0;
	int rejected_direction = 0;
	int rejected_reserved = 0;
	int rejected_takeoff_queue = 0;

	TileIndex term_tile = FindFreeModularTerminal(st, v);
	bool has_term = (term_tile != INVALID_TILE);
	int term_x = has_term ? TileX(term_tile) * TILE_SIZE : 0;
	int term_y = has_term ? TileY(term_tile) * TILE_SIZE : 0;

	for (const ModularAirportTileData &data : *st->airport.modular_tile_data) {
		bool is_runway = IsModularRunwayPiece(data.piece_type);
		bool is_helipad = IsModularHelipadPiece(data.piece_type);

		if (!is_runway && !is_helipad) continue;

		if (is_heli) {
			/* Helicopters prefer helipads but can use runways */
		} else {
			if (is_helipad) continue; /* Planes can't land on helipads */
		}

		/* Only consider runway ends as valid landing targets */
		if (is_runway) {
			candidates_total++;
			bool is_end = (data.piece_type == APT_RUNWAY_END || data.piece_type == APT_RUNWAY_SMALL_NEAR_END || data.piece_type == APT_RUNWAY_SMALL_FAR_END);
			if (!is_end) {
				rejected_not_end++;
				continue;
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

			/* Large aircraft (AIR_FAST) require a long, large-family runway. */
			if ((AircraftVehInfo(v->engine_type)->subtype & AIR_FAST) &&
					!_cheats.no_jetcrash.value &&
					!IsRunwaySafeForLarge(st, data.tile)) {
				continue;
			}
		}

		int cx = TileX(data.tile) * TILE_SIZE + TILE_SIZE / 2;
		int cy = TileY(data.tile) * TILE_SIZE + TILE_SIZE / 2;
		int dist_flight = abs(cx - v->x_pos) + abs(cy - v->y_pos);

		int score = dist_flight;

		if (is_heli && is_runway) score += 1000000; /* Prefer helipads significantly */

		if (has_term) {
			/* Calculate distance from the *other* end of the runway to the terminal.
			   This favors landings that roll out towards the terminal. */
			if (is_runway) {
				TileIndex other_end = GetRunwayOtherEnd(st, data.tile);
				int end_x = TileX(other_end) * TILE_SIZE;
				int end_y = TileY(other_end) * TILE_SIZE;
				int dist_taxi = abs(end_x - term_x) + abs(end_y - term_y);
				score += dist_taxi * 4;
			} else {
				/* For helipad, taxi distance is from helipad to terminal */
				int dist_taxi = abs(cx - term_x) + abs(cy - term_y);
				score += dist_taxi * 4;
			}
		}

		if (score < best_score) {
			best_score = score;
			best_tile = data.tile;
		}
	}

	if (best_tile == INVALID_TILE && !is_heli && ShouldLogModularRateLimited(v->index, 18, 128)) {
		Debug(misc, 2,
			"[ModAp] Vehicle {} no landing runway: runway_tiles={} reject_not_end={} reject_mode={} reject_dir={} reject_reserved={} reject_takeoff_queue={}",
			v->index, candidates_total, rejected_not_end, rejected_mode, rejected_direction, rejected_reserved, rejected_takeoff_queue);
	}

	return best_tile;
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
	return diff == DIRDIFF_SAME || diff == DIRDIFF_45LEFT || diff == DIRDIFF_45RIGHT;
}

Direction GetRunwayApproachDirection(const Station *st, TileIndex runway_tile)
{
	int approach_x, approach_y;
	GetModularLandingApproachPoint(st, runway_tile, &approach_x, &approach_y);

	const int threshold_x = TileX(runway_tile) * TILE_SIZE + TILE_SIZE / 2;
	const int threshold_y = TileY(runway_tile) * TILE_SIZE + TILE_SIZE / 2;

	const int dx = threshold_x - approach_x;
	const int dy = threshold_y - approach_y;
	if (dx == 0 && dy == 0) return DIR_N;

	/* Match the vehicle movement vectors (see GetNewVehiclePos delta table). */
	static constexpr int8_t dir_dx[DIR_END] = {-1, -1, -1, 0, 1, 1, 1, 0};
	static constexpr int8_t dir_dy[DIR_END] = {-1, 0, 1, 1, 1, 0, -1, -1};

	Direction best_dir = DIR_N;
	int64_t best_dot = INT64_MIN;
	int64_t best_cross_abs = INT64_MAX;

	for (int d = DIR_BEGIN; d < DIR_END; ++d) {
		const int64_t vx = dir_dx[d];
		const int64_t vy = dir_dy[d];
		const int64_t dot = vx * dx + vy * dy;
		const int64_t cross_abs = std::abs(vx * dy - vy * dx);
		if (dot > best_dot || (dot == best_dot && cross_abs < best_cross_abs)) {
			best_dot = dot;
			best_cross_abs = cross_abs;
			best_dir = static_cast<Direction>(d);
		}
	}

	return best_dir;
}

struct DubinsArc {
	double cx;
	double cy;
	double r;
	double a0;
	double sweep; // CCW positive, CW negative
};

struct DubinsSeg {
	bool is_arc;
	DubinsArc arc;
	double x0;
	double y0;
	double x1;
	double y1;
};

struct DubinsPath {
	std::vector<DubinsSeg> segs;
	double length;
	bool valid;
};

static constexpr double DUBINS_PI = 3.14159265358979323846;

static double NormalizeAngle2Pi(double a)
{
	a = std::fmod(a, 2.0 * DUBINS_PI);
	if (a < 0.0) a += 2.0 * DUBINS_PI;
	return a;
}

static void DirToVec(Direction d, double &dx, double &dy)
{
	static constexpr int8_t dir_dx[DIR_END] = {-1, -1, -1, 0, 1, 1, 1, 0};
	static constexpr int8_t dir_dy[DIR_END] = {-1, 0, 1, 1, 1, 0, -1, -1};

	const int ix = dir_dx[d];
	const int iy = dir_dy[d];
	const double len = std::hypot(static_cast<double>(ix), static_cast<double>(iy));
	if (len <= 0.0) {
		dx = 1.0;
		dy = 0.0;
		return;
	}
	dx = ix / len;
	dy = iy / len;
}

static void AddWaypoint(std::vector<ModularHoldingLoop::Waypoint> &out, double x, double y)
{
	const int ix = static_cast<int>(std::lround(x));
	const int iy = static_cast<int>(std::lround(y));
	if (!out.empty() && out.back().x == ix && out.back().y == iy) return;
	out.push_back({ix, iy});
}

static void AppendFallbackRectLoopWaypoints(std::vector<ModularHoldingLoop::Waypoint> &out, int min_x, int min_y, int max_x, int max_y)
{
	const int loop_x0 = (min_x - MODULAR_HOLDING_MARGIN_TILES) * TILE_SIZE + TILE_SIZE / 2;
	const int loop_y0 = (min_y - MODULAR_HOLDING_MARGIN_TILES) * TILE_SIZE + TILE_SIZE / 2;
	const int loop_x1 = (max_x + MODULAR_HOLDING_MARGIN_TILES) * TILE_SIZE + TILE_SIZE / 2;
	const int loop_y1 = (max_y + MODULAR_HOLDING_MARGIN_TILES) * TILE_SIZE + TILE_SIZE / 2;
	const int cx = (loop_x0 + loop_x1) / 2;
	const int cy = (loop_y0 + loop_y1) / 2;

	out.clear();
	out.push_back({loop_x0, loop_y0});
	out.push_back({cx, loop_y0});
	out.push_back({loop_x1, loop_y0});
	out.push_back({loop_x1, cy});
	out.push_back({loop_x1, loop_y1});
	out.push_back({cx, loop_y1});
	out.push_back({loop_x0, loop_y1});
	out.push_back({loop_x0, cy});
}

static DubinsPath ComputeDubins(double x1, double y1, double hdx1, double hdy1,
		double x2, double y2, double hdx2, double hdy2, double radius)
{
	DubinsPath best = {};
	best.valid = false;
	best.length = std::numeric_limits<double>::infinity();

	if (radius <= 0.0) return best;

	const double th1 = std::atan2(hdy1, hdx1);
	const double th2 = std::atan2(hdy2, hdx2);
	const double dx = (x2 - x1) / radius;
	const double dy = (y2 - y1) / radius;
	const double d = std::hypot(dx, dy);
	if (d < 1e-9) return best;

	const double theta = std::atan2(dy, dx);
	const double alpha = NormalizeAngle2Pi(th1 - theta);
	const double beta = NormalizeAngle2Pi(th2 - theta);

	struct Candidate {
		double t;
		double p;
		double q;
		char types[3];
		bool valid;
	};

	auto eval_lsl = [&]() -> Candidate {
		Candidate c = {};
		c.types[0] = 'L'; c.types[1] = 'S'; c.types[2] = 'L';
		const double tmp0 = d + std::sin(alpha) - std::sin(beta);
		const double p2 = 2.0 + d * d - 2.0 * std::cos(alpha - beta) + 2.0 * d * (std::sin(alpha) - std::sin(beta));
		if (p2 < 0.0) return c;
		const double tmp1 = std::atan2(std::cos(beta) - std::cos(alpha), tmp0);
		c.t = NormalizeAngle2Pi(-alpha + tmp1);
		c.p = std::sqrt(std::max(0.0, p2));
		c.q = NormalizeAngle2Pi(beta - tmp1);
		c.valid = true;
		return c;
	};

	auto eval_rsr = [&]() -> Candidate {
		Candidate c = {};
		c.types[0] = 'R'; c.types[1] = 'S'; c.types[2] = 'R';
		const double tmp0 = d - std::sin(alpha) + std::sin(beta);
		const double p2 = 2.0 + d * d - 2.0 * std::cos(alpha - beta) + 2.0 * d * (-std::sin(alpha) + std::sin(beta));
		if (p2 < 0.0) return c;
		const double tmp1 = std::atan2(std::cos(alpha) - std::cos(beta), tmp0);
		c.t = NormalizeAngle2Pi(alpha - tmp1);
		c.p = std::sqrt(std::max(0.0, p2));
		c.q = NormalizeAngle2Pi(-beta + tmp1);
		c.valid = true;
		return c;
	};

	auto eval_lsr = [&]() -> Candidate {
		Candidate c = {};
		c.types[0] = 'L'; c.types[1] = 'S'; c.types[2] = 'R';
		const double p2 = -2.0 + d * d + 2.0 * std::cos(alpha - beta) + 2.0 * d * (std::sin(alpha) + std::sin(beta));
		if (p2 < 0.0) return c;
		const double p = std::sqrt(std::max(0.0, p2));
		const double tmp2 = std::atan2(-std::cos(alpha) - std::cos(beta), d + std::sin(alpha) + std::sin(beta)) - std::atan2(-2.0, p);
		c.t = NormalizeAngle2Pi(-alpha + tmp2);
		c.p = p;
		c.q = NormalizeAngle2Pi(-beta + tmp2);
		c.valid = true;
		return c;
	};

	auto eval_rsl = [&]() -> Candidate {
		Candidate c = {};
		c.types[0] = 'R'; c.types[1] = 'S'; c.types[2] = 'L';
		const double p2 = -2.0 + d * d + 2.0 * std::cos(alpha - beta) - 2.0 * d * (std::sin(alpha) + std::sin(beta));
		if (p2 < 0.0) return c;
		const double p = std::sqrt(std::max(0.0, p2));
		const double tmp2 = std::atan2(std::cos(alpha) + std::cos(beta), d - std::sin(alpha) - std::sin(beta)) - std::atan2(2.0, p);
		c.t = NormalizeAngle2Pi(alpha - tmp2);
		c.p = p;
		c.q = NormalizeAngle2Pi(beta - tmp2);
		c.valid = true;
		return c;
	};

	const std::array<Candidate, 4> candidates = {eval_rsr(), eval_lsl(), eval_rsl(), eval_lsr()};

	for (const Candidate &cand : candidates) {
		if (!cand.valid) continue;
		const double cand_len = (cand.t + cand.p + cand.q) * radius;
		if (cand_len >= best.length) continue;

		std::vector<DubinsSeg> segs;
		segs.reserve(3);
		double px = x1;
		double py = y1;
		double th = th1;

		auto append_arc = [&](char turn_type, double arc_angle) {
			if (arc_angle <= 1e-9) return;

			const bool left = (turn_type == 'L');
			const double cx = left ? (px - radius * std::sin(th)) : (px + radius * std::sin(th));
			const double cy = left ? (py + radius * std::cos(th)) : (py - radius * std::cos(th));
			const double a0 = std::atan2(py - cy, px - cx);
			const double sweep = left ? arc_angle : -arc_angle;
			const double a1 = a0 + sweep;
			const double nx = cx + radius * std::cos(a1);
			const double ny = cy + radius * std::sin(a1);

			DubinsSeg seg = {};
			seg.is_arc = true;
			seg.arc = {cx, cy, radius, a0, sweep};
			seg.x0 = px;
			seg.y0 = py;
			seg.x1 = nx;
			seg.y1 = ny;
			segs.push_back(seg);

			px = nx;
			py = ny;
			th = NormalizeAngle2Pi(th + sweep);
		};

		auto append_straight = [&](double dist) {
			if (dist <= 1e-9) return;
			const double nx = px + dist * std::cos(th);
			const double ny = py + dist * std::sin(th);
			DubinsSeg seg = {};
			seg.is_arc = false;
			seg.x0 = px;
			seg.y0 = py;
			seg.x1 = nx;
			seg.y1 = ny;
			segs.push_back(seg);
			px = nx;
			py = ny;
		};

		append_arc(cand.types[0], cand.t);
		append_straight(cand.p * radius);
		append_arc(cand.types[2], cand.q);

		best.segs = std::move(segs);
		best.length = cand_len;
		best.valid = true;
	}

	return best;
}

static void SampleDubinsPath(const DubinsPath &path, double step_px, std::vector<ModularHoldingLoop::Waypoint> &out)
{
	if (!path.valid || step_px <= 0.0) return;

	for (const DubinsSeg &seg : path.segs) {
		if (seg.is_arc) {
			const double radius = seg.arc.r;
			const double sweep_abs = std::abs(seg.arc.sweep);
			if (radius <= 0.0 || sweep_abs <= 1e-9) continue;

			const double step_ang = step_px / radius;
			if (step_ang <= 0.0) continue;
			const int count = static_cast<int>(std::floor(sweep_abs / step_ang));
			const double sign = (seg.arc.sweep >= 0.0) ? 1.0 : -1.0;
			for (int i = 1; i <= count; ++i) {
				const double a = seg.arc.a0 + sign * static_cast<double>(i) * step_ang;
				if (std::abs(a - (seg.arc.a0 + seg.arc.sweep)) <= 1e-9) break;
				const double x = seg.arc.cx + radius * std::cos(a);
				const double y = seg.arc.cy + radius * std::sin(a);
				AddWaypoint(out, x, y);
			}
		} else {
			const double dx = seg.x1 - seg.x0;
			const double dy = seg.y1 - seg.y0;
			const double len = std::hypot(dx, dy);
			if (len <= 1e-9) continue;

			const int count = static_cast<int>(std::floor(len / step_px + 1e-7));
			for (int i = 1; i <= count; ++i) {
				const double t = (static_cast<double>(i) * step_px) / len;
				if (t >= 1.0 - 1e-9) break;
				AddWaypoint(out, seg.x0 + dx * t, seg.y0 + dy * t);
			}
		}
	}
}

struct GateInfo {
	TileIndex runway_tile;
	int gate_x;
	int gate_y;
	int threshold_x;
	int threshold_y;
	Direction approach_dir;
	double hdx;
	double hdy;
	int rel_x; ///< gate_x - center_x (integer, for deterministic angular sort)
	int rel_y; ///< gate_y - center_y (integer, for deterministic angular sort)
};

static void GatherAndSortGates(const Station *st, std::vector<GateInfo> &gates)
{
	gates.clear();
	if (st->airport.modular_tile_data == nullptr) return;

	int min_x = INT_MAX;
	int min_y = INT_MAX;
	int max_x = INT_MIN;
	int max_y = INT_MIN;
	for (const ModularAirportTileData &data : *st->airport.modular_tile_data) {
		const int tx = static_cast<int>(TileX(data.tile));
		const int ty = static_cast<int>(TileY(data.tile));
		min_x = std::min(min_x, tx);
		min_y = std::min(min_y, ty);
		max_x = std::max(max_x, tx);
		max_y = std::max(max_y, ty);
	}
	if (min_x == INT_MAX) return;

	/* Use doubled coordinates to avoid fractional center (integer-exact for deterministic sorting). */
	const int center_2x = (min_x + max_x) * TILE_SIZE + TILE_SIZE;
	const int center_2y = (min_y + max_y) * TILE_SIZE + TILE_SIZE;

	for (const ModularAirportTileData &data : *st->airport.modular_tile_data) {
		if (!IsModularRunwayPiece(data.piece_type)) continue;
		const bool is_end = data.piece_type == APT_RUNWAY_END || data.piece_type == APT_RUNWAY_SMALL_NEAR_END || data.piece_type == APT_RUNWAY_SMALL_FAR_END;
		if (!is_end) continue;

		const uint8_t flags = GetRunwayFlags(st, data.tile);
		if ((flags & RUF_LANDING) == 0) continue;

		const bool is_low = IsRunwayEndLow(st, data.tile);
		if (is_low && (flags & RUF_DIR_HIGH) == 0) continue;
		if (!is_low && (flags & RUF_DIR_LOW) == 0) continue;

		int approach_x = 0;
		int approach_y = 0;
		GetModularLandingApproachPoint(st, data.tile, &approach_x, &approach_y);
		const int threshold_x = TileX(data.tile) * TILE_SIZE + TILE_SIZE / 2;
		const int threshold_y = TileY(data.tile) * TILE_SIZE + TILE_SIZE / 2;
		const Direction approach_dir = GetRunwayApproachDirection(st, data.tile);

		double hdx = 1.0;
		double hdy = 0.0;
		DirToVec(approach_dir, hdx, hdy);

		GateInfo gate = {};
		gate.runway_tile = data.tile;
		gate.gate_x = approach_x;
		gate.gate_y = approach_y;
		gate.threshold_x = threshold_x;
		gate.threshold_y = threshold_y;
		gate.approach_dir = approach_dir;
		gate.hdx = hdx;
		gate.hdy = hdy;
		gate.rel_x = approach_x * 2 - center_2x;
		gate.rel_y = approach_y * 2 - center_2y;
		gates.push_back(gate);
	}

	/* Deterministic angular sort using integer quadrant + cross-product (no transcendental functions).
	 * This avoids floating-point non-determinism that could cause multiplayer desyncs. */
	auto Quadrant = [](int x, int y) -> int {
		if (x > 0 && y >= 0) return 0;
		if (x <= 0 && y > 0) return 1;
		if (x < 0 && y <= 0) return 2;
		return 3;
	};
	std::sort(gates.begin(), gates.end(), [&Quadrant](const GateInfo &a, const GateInfo &b) {
		int qa = Quadrant(a.rel_x, a.rel_y);
		int qb = Quadrant(b.rel_x, b.rel_y);
		if (qa != qb) return qa < qb;
		/* Same quadrant: use cross-product for deterministic ordering. */
		int64_t cross = static_cast<int64_t>(a.rel_x) * b.rel_y - static_cast<int64_t>(a.rel_y) * b.rel_x;
		if (cross != 0) return cross > 0;
		return a.runway_tile.base() < b.runway_tile.base();
	});
}

const ModularHoldingLoop &GetModularHoldingLoop(const Station *st)
{
	if (st->airport.modular_holding_loop_dirty || st->airport.modular_holding_loop == nullptr) {
		if (st->airport.modular_holding_loop == nullptr) {
			st->airport.modular_holding_loop = new ModularHoldingLoop();
		}
		ComputeModularHoldingLoop(st, *st->airport.modular_holding_loop);
		st->airport.modular_holding_loop_dirty = false;
	}
	return *st->airport.modular_holding_loop;
}

void ComputeModularHoldingLoop(const Station *st, ModularHoldingLoop &loop)
{
	int min_x = INT_MAX;
	int min_y = INT_MAX;
	int max_x = INT_MIN;
	int max_y = INT_MIN;

	if (st->airport.modular_tile_data != nullptr && !st->airport.modular_tile_data->empty()) {
		for (const ModularAirportTileData &data : *st->airport.modular_tile_data) {
			const int tx = static_cast<int>(TileX(data.tile));
			const int ty = static_cast<int>(TileY(data.tile));
			min_x = std::min(min_x, tx);
			min_y = std::min(min_y, ty);
			max_x = std::max(max_x, tx);
			max_y = std::max(max_y, ty);
		}
	} else {
		const int tx = static_cast<int>(TileX(st->xy));
		const int ty = static_cast<int>(TileY(st->xy));
		min_x = max_x = tx;
		min_y = max_y = ty;
	}

	loop.waypoints.clear();
	loop.gates.clear();

	std::vector<GateInfo> gates;
	GatherAndSortGates(st, gates);
	if (gates.empty()) {
		AppendFallbackRectLoopWaypoints(loop.waypoints, min_x, min_y, max_x, max_y);
		return;
	}

	const double overshoot_px = static_cast<double>(MODULAR_HOLDING_OVERSHOOT_TILES * TILE_SIZE);
	const double radius_px = static_cast<double>(MODULAR_HOLDING_TURN_RADIUS_TILES * TILE_SIZE);
	const double sample_px = static_cast<double>(MODULAR_HOLDING_SAMPLE_INTERVAL_PX);

	for (size_t i = 0; i < gates.size(); ++i) {
		const GateInfo &cur = gates[i];
		const GateInfo &next = gates[(i + 1) % gates.size()];

		ModularHoldingLoop::Gate gate = {};
		gate.runway_tile = cur.runway_tile;
		gate.wp_index = static_cast<uint32_t>(loop.waypoints.size());
		gate.approach_x = cur.gate_x;
		gate.approach_y = cur.gate_y;
		gate.threshold_x = cur.threshold_x;
		gate.threshold_y = cur.threshold_y;
		gate.approach_dir = cur.approach_dir;
		loop.gates.push_back(gate);

		AddWaypoint(loop.waypoints, cur.gate_x, cur.gate_y);

		const double ex = static_cast<double>(cur.gate_x) + cur.hdx * overshoot_px;
		const double ey = static_cast<double>(cur.gate_y) + cur.hdy * overshoot_px;
		const double ox = ex - static_cast<double>(cur.gate_x);
		const double oy = ey - static_cast<double>(cur.gate_y);
		const double over_len = std::hypot(ox, oy);
		if (over_len > 1e-9) {
			const int count = static_cast<int>(std::floor(over_len / sample_px));
			for (int s = 1; s <= count; ++s) {
				const double t = (static_cast<double>(s) * sample_px) / over_len;
				if (t >= 1.0 - 1e-9) break;
				AddWaypoint(loop.waypoints, static_cast<double>(cur.gate_x) + ox * t, static_cast<double>(cur.gate_y) + oy * t);
			}
		}
		AddWaypoint(loop.waypoints, ex, ey);

		DubinsPath path = ComputeDubins(ex, ey, cur.hdx, cur.hdy,
				static_cast<double>(next.gate_x), static_cast<double>(next.gate_y), next.hdx, next.hdy,
				radius_px);
		if (!path.valid) {
			path = ComputeDubins(ex, ey, cur.hdx, cur.hdy,
					static_cast<double>(next.gate_x), static_cast<double>(next.gate_y), next.hdx, next.hdy,
					radius_px * 0.5);
		}
		if (!path.valid) {
			const double sx = static_cast<double>(next.gate_x) - ex;
			const double sy = static_cast<double>(next.gate_y) - ey;
			const double slen = std::hypot(sx, sy);
			if (slen > 1e-9) {
				const int count = static_cast<int>(std::floor(slen / sample_px));
				for (int s = 1; s <= count; ++s) {
					const double t = (static_cast<double>(s) * sample_px) / slen;
					if (t >= 1.0 - 1e-9) break;
					AddWaypoint(loop.waypoints, ex + sx * t, ey + sy * t);
				}
			}
			continue;
		}

		SampleDubinsPath(path, sample_px, loop.waypoints);
	}
}

/**
 * Find the waypoint in the holding loop closest to the aircraft's current pixel position.
 * Position-based (not time-based) so it works correctly regardless of aircraft speed,
 * and avoids the "ghost laps the plane" problem that occurs with phase-driven targeting.
 */
uint32_t GetNearestModularHoldingWaypoint(const Aircraft *v, const ModularHoldingLoop &loop)
{
	const uint32_t n_wp = static_cast<uint32_t>(loop.waypoints.size());
	if (n_wp == 0) return 0;
	uint32_t nearest = 0;
	int64_t min_d2 = INT64_MAX;
	for (uint32_t i = 0; i < n_wp; ++i) {
		const int64_t dx = v->x_pos - loop.waypoints[i].x;
		const int64_t dy = v->y_pos - loop.waypoints[i].y;
		const int64_t d2 = dx * dx + dy * dy;
		if (d2 < min_d2) { min_d2 = d2; nearest = i; }
	}
	return nearest;
}

bool IsHoldingGateActive(uint32_t aircraft_wp, uint32_t gate_wp, uint32_t n_wp)
{
	if (n_wp == 0) return false;
	const uint32_t diff = (aircraft_wp + n_wp - gate_wp) % n_wp;
	return diff == 0 || diff == (n_wp - 1);
}

void GetModularHoldingWaypointTarget(Aircraft *v, const Station *st, int *target_x, int *target_y, uint32_t *wp_index)
{
	const ModularHoldingLoop &loop = GetModularHoldingLoop(st);
	const uint32_t n_wp = static_cast<uint32_t>(loop.waypoints.size());
	if (n_wp == 0) {
		TileIndex target = st->airport.tile;
		if (target == INVALID_TILE) target = st->xy;
		*target_x = TileX(target) * TILE_SIZE + TILE_SIZE / 2;
		*target_y = TileY(target) * TILE_SIZE + TILE_SIZE / 2;
		if (wp_index != nullptr) *wp_index = 0;
		return;
	}

	static constexpr uint32_t LOOKAHEAD = 5; ///< Waypoints ahead of base to steer toward.
	/* ADVANCE_DIST_SQ: when the aircraft is within this squared-pixel distance of the
	 * current target waypoint, advance the base index by one.  Advancing early (before
	 * reaching the exact pixel) prevents the aircraft from catching the target and
	 * circling it while waiting for the ghost clock to tick. */
	static constexpr int ADVANCE_DIST    = TILE_SIZE * 3; // 3 tiles
	static constexpr int ADVANCE_DIST_SQ = ADVANCE_DIST * ADVANCE_DIST;

	/* Initialise or reinitialise the per-aircraft base index from the time-based ghost.
	 * The ghost spreads aircraft around the whole loop via their vehicle-index offset, so
	 * planes that arrive at the airport at different times start at different loop phases
	 * and visit both runway gates.  UINT32_MAX (or a stale index >= n_wp) means the
	 * index needs to be set. */
	if (v->modular_holding_wp_index == UINT32_MAX || v->modular_holding_wp_index >= n_wp) {
		const uint64_t offset   = static_cast<uint64_t>(v->index.base() % n_wp) * MODULAR_HOLDING_TICKS_PER_WP;
		const uint64_t phase    = TimerGameTick::counter + offset;
		v->modular_holding_wp_index = static_cast<uint32_t>((phase / MODULAR_HOLDING_TICKS_PER_WP) % n_wp);
	}

	/* Advance the base index whenever the aircraft closes within ADVANCE_DIST of the
	 * current lookahead target.  This decouples progress from the ghost clock and means
	 * the target moves continuously as the aircraft flies, preventing it from catching
	 * a fixed point and circling it. */
	const uint32_t target_wp = (v->modular_holding_wp_index + LOOKAHEAD) % n_wp;
	const int64_t tdx = v->x_pos - loop.waypoints[target_wp].x;
	const int64_t tdy = v->y_pos - loop.waypoints[target_wp].y;
	if (tdx * tdx + tdy * tdy <= ADVANCE_DIST_SQ) {
		v->modular_holding_wp_index = (v->modular_holding_wp_index + 1) % n_wp;
	}

	/* Recompute target after possible advance. */
	const uint32_t final_target_wp = (v->modular_holding_wp_index + LOOKAHEAD) % n_wp;
	*target_x = loop.waypoints[final_target_wp].x;
	*target_y = loop.waypoints[final_target_wp].y;
	if (wp_index != nullptr) *wp_index = v->modular_holding_wp_index;
}


bool AirportMoveModularLanding(Aircraft *v, const Station *st)
{
	if (st->airport.blocks.Test(AirportBlock::Zeppeliner)) {
		/* Match stock behavior: abort modular landing while zeppeliner wreck blocks the airport. */
		ClearTaxiPathReservation(v);
		ClearModularRunwayReservation(v);
		v->modular_landing_goal = INVALID_TILE;
		v->modular_landing_tile = INVALID_TILE;
		v->modular_landing_stage = 0;
		v->state = FLYING;
		return false;
	}

	if (v->modular_landing_tile == INVALID_TILE) {
		v->modular_landing_tile = FindModularLandingTarget(st, v);
		v->modular_landing_stage = 0;
		if (v->modular_landing_tile == INVALID_TILE) {
			Debug(misc, 3, "[ModAp] no runway/helipad tile found for landing at station {}", st->index);
			return false;
		}
		/* Helicopters landing on a helipad skip the FAF approach stage —
		 * they descend vertically, no runway-style approach needed. */
		if (v->subtype == AIR_HELICOPTER) {
			const ModularAirportTileData *land_data = st->airport.GetModularTileData(v->modular_landing_tile);
			if (land_data != nullptr && IsModularHelipadPiece(land_data->piece_type)) {
				v->modular_landing_stage = 1;
			}
		}
		Debug(misc, 3, "[ModAp] Vehicle {} starting approach to {} tile {}, stage={}, pos=({},{},{})",
			v->index, v->modular_landing_stage == 1 ? "helipad" : "runway",
			v->modular_landing_tile.base(), v->modular_landing_stage, v->x_pos, v->y_pos, v->z_pos);
	}

	int target_x, target_y;
	int airport_z = GetTileMaxPixelZ(v->modular_landing_tile) + 1;
	/* Match stock heliport behavior: rooftop touchdown uses +60 px (afc->delta_z). */
	if (v->subtype == AIR_HELICOPTER) {
		const ModularAirportTileData *landing_data = st->airport.GetModularTileData(v->modular_landing_tile);
		if (landing_data != nullptr && landing_data->piece_type == APT_HELIPORT) airport_z += 60;
	}
	int target_z = airport_z;

	if (v->modular_landing_stage == 0) {
		/* Approach phase: fly to FAF */
		GetModularLandingApproachPoint(st, v->modular_landing_tile, &target_x, &target_y);
		target_z = airport_z + 20 * 5; // Stay high
	} else {
		/* Final phase: fly to threshold */
		target_x = TileX(v->modular_landing_tile) * TILE_SIZE + TILE_SIZE / 2;
		target_y = TileY(v->modular_landing_tile) * TILE_SIZE + TILE_SIZE / 2;
		
		/* Helicopters maintain altitude in final phase until over the pad */
		if (v->subtype == AIR_HELICOPTER) target_z = airport_z + 20 * 5;
	}

	/* Calculate distance to target */
	int dist = abs(v->x_pos - target_x) + abs(v->y_pos - target_y);

	/* Update speed for approach/landing */
	uint speed_limit = (v->modular_landing_stage == 0) ? SPEED_LIMIT_HOLD : SPEED_LIMIT_APPROACH;
	int count = UpdateAircraftSpeed(v, speed_limit, false);

	/* Only move if speed allows it */
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
			if (v->modular_landing_stage == 0 && v->subtype == AIR_AIRCRAFT) {
				if (v->turn_counter > 0) {
					v->turn_counter--;
					int z = v->z_pos;
					if (z < target_z) z++; else if (z > target_z) z--;
					SetAircraftPosition(v, v->x_pos, v->y_pos, z);
					return false;
				}
				v->turn_counter = 1;
			}
			v->last_direction = v->direction;
			v->direction = desired_dir;
		}
	}

	/* Aircraft in air has no tile */
	v->tile = TileIndex{};

	/* Altitude logic */
	int z = v->z_pos;
	if (v->modular_landing_stage == 0) {
		/* Maintain approach altitude */
		/* Simple seek towards target_z */
		if (z < target_z) z++; else if (z > target_z) z--;
	} else {
		/* Final phase */
		if (v->subtype == AIR_HELICOPTER) {
			/* Helicopters: Fly to target at altitude, then descend vertically */
			if (dist > 0) {
				/* Maintain high altitude while moving horizontally */
				if (z < target_z) z++; else if (z > target_z) z--;
			} else {
				/* Vertically descend to ground */
				if (z > airport_z) z--;
			}
		} else {
			/* Planes: Glide slope for final */
			if (z > airport_z) {
				int t = std::max(1, dist - 4);
				int delta = z - airport_z;
				if (delta >= t) {
					z -= CeilDiv(z - airport_z, t);
				}
			}
		}
	}

	SetAircraftPosition(v, new_x, new_y, z);

	Debug(misc, 5, "[ModAp] Vehicle {} landing stage {}: pos=({},{},{}), target=({},{},{}), dist={}",
		v->index, v->modular_landing_stage, v->x_pos, v->y_pos, v->z_pos, target_x, target_y, target_z, dist);

	/* Check if reached target */
	if (v->x_pos == target_x && v->y_pos == target_y) {
		if (v->modular_landing_stage == 0) {
			/* Reached FAF, switch to final */
			v->modular_landing_stage = 1;
		} else {
			if (v->z_pos > airport_z) return false; // Still descending

			/* Reached threshold, land and start rollout */
			Debug(misc, 3, "[ModAp] Vehicle {} touchdown at ({},{},{})", v->index, target_x, target_y, airport_z);
			RecordAirportMovement(v->targetairport, true);
			v->tile = v->modular_landing_tile;

			/* Set up rollout phase - taxi along runway to opposite end */
			TileIndex rollout_point = FindModularRunwayRolloutPoint(st, v->modular_landing_tile);

			v->modular_landing_tile = INVALID_TILE;
			v->modular_landing_stage = 0;

			AircraftEventHandler_Landing(v, st->airport.GetFTA());

			/* If we have a rollout point, go there first, otherwise go directly to terminal */
			if (rollout_point != INVALID_TILE) {
				Debug(misc, 3, "[ModAp] Vehicle {} starting rollout to tile {}", v->index, rollout_point.base());
				v->ground_path_goal = rollout_point;
				v->modular_ground_target = MGT_ROLLOUT;
				v->state = TERM1;
			} else {
				/* No rollout, go directly to terminal */
				AircraftEventHandler_EndLanding(v, st->airport.GetFTA());
			}
			return true;
		}
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

	/* Reached altitude, transition to flying */
	v->state = FLYING;
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
		TileIndex queue = FindModularTakeoffQueueTile(st, v, runway);
		if (queue == INVALID_TILE) queue = runway;
		v->ground_path_goal = queue;
		v->modular_ground_target = MGT_RUNWAY_TAKEOFF;
		v->state = TERM1;
		Debug(misc, 2, "[ModAp] V{} takeoff recovery: requeue goal={} runway={}", v->index, queue.base(), runway.base());
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

	if (v->modular_takeoff_progress == 0) {
		/* Determine takeoff direction by finding the other end of the runway */
		TileIndex end_tile = GetRunwayOtherEnd(st, v->modular_takeoff_tile);
		int end_x = TileX(end_tile) * TILE_SIZE + TILE_SIZE / 2;
		int end_y = TileY(end_tile) * TILE_SIZE + TILE_SIZE / 2;
		
		/* If single tile runway, end_tile == start_tile. 
		   Fallback to rotation-based direction if we can't determine direction from length. */
		if (end_tile == v->modular_takeoff_tile) {
			Direction dir = horizontal ? DIR_SE : DIR_SW;
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

		/* Calculate altitude - gradual climb starting after initial acceleration */
		int z = v->z_pos;
		int target_z = GetAircraftFlightLevel(v, true);

		/* Start climbing after 1 tile of acceleration, climb at ~1.5 pixels per tile traveled */
		if (v->modular_takeoff_progress > TILE_SIZE) {
			int climb_progress = v->modular_takeoff_progress - TILE_SIZE;
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
			ClearTaxiPathReservation(v);
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
 * Find a free modular terminal for an aircraft.
 * @param st The station.
 * @param v The aircraft.
 * @return Terminal tile or INVALID_TILE if none found.
 */
TileIndex FindModularRunwayRolloutPoint(const Station *st, TileIndex landing_tile)
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

	/* Always roll out to the far end of the contiguous runway.
	 * If taxi egress exists only near touchdown, pathfinding can taxi back later,
	 * but touchdown should never stop short of rollout. */
	return GetRunwayOtherEnd(st, landing_tile);
}

TileIndex FindNearestModularRunwayExitTile(const Station *st, const Aircraft *v, TileIndex runway_tile)
{
	const ModularAirportTileData *td = st->airport.GetModularTileData(runway_tile);
	if (td == nullptr || !IsModularRunwayPiece(td->piece_type)) return INVALID_TILE;

	const auto has_onward_route = [&](TileIndex from_tile) -> bool {
		if (st->airport.modular_tile_data == nullptr) return false;
		for (const ModularAirportTileData &data : *st->airport.modular_tile_data) {
			const bool is_service = (data.piece_type == APT_STAND || data.piece_type == APT_STAND_1 ||
					IsModularHangarPiece(data.piece_type) ||
					IsModularHelipadPiece(data.piece_type));
			if (!is_service) continue;
			AirportGroundPath p = FindAirportGroundPath(st, from_tile, data.tile, nullptr);
			if (p.found) return true;
		}
		return false;
	};

	std::vector<TileIndex> runway_tiles;
	if (!GetContiguousModularRunwayTiles(st, runway_tile, runway_tiles) || runway_tiles.empty()) return INVALID_TILE;

	auto it = std::find(runway_tiles.begin(), runway_tiles.end(), runway_tile);
	if (it == runway_tiles.end()) return INVALID_TILE;
	const size_t idx = static_cast<size_t>(it - runway_tiles.begin());

	static const TileIndexDiff kNeighbours[] = {
		TileDiffXY(1, 0), TileDiffXY(-1, 0), TileDiffXY(0, 1), TileDiffXY(0, -1),
	};

	for (size_t step = 0; step < runway_tiles.size(); ++step) {
		const size_t cand[2] = {
			(idx >= step) ? (idx - step) : runway_tiles.size(),
			(idx + step < runway_tiles.size()) ? (idx + step) : runway_tiles.size(),
		};

		for (size_t ci = 0; ci < 2; ++ci) {
			if (cand[ci] >= runway_tiles.size()) continue;
			TileIndex rt = runway_tiles[cand[ci]];

			for (TileIndexDiff d : kNeighbours) {
				TileIndex n = rt + d;
				const ModularAirportTileData *nd = st->airport.GetModularTileData(n);
				if (nd == nullptr || IsModularRunwayPiece(nd->piece_type)) continue;

				Tile nt(n);
				if (!IsAirportTile(nt)) continue;
				if (HasAirportTileReservation(nt) && GetAirportTileReserver(nt) != v->index) continue;
				if (IsModularTileOccupiedByOtherAircraft(st, n, v->index)) continue;
				if (!has_onward_route(n)) continue;
				return n;
			}
		}
	}

	return INVALID_TILE;
}

TileIndex FindModularRolloutHoldingTile(const Station *st, const Aircraft *v, TileIndex start_tile)
{
	if (!IsValidTile(start_tile) || st->airport.modular_tile_data == nullptr) return INVALID_TILE;

	TileIndex best_target = INVALID_TILE;
	int best_cost = INT_MAX;
	for (const ModularAirportTileData &data : *st->airport.modular_tile_data) {
		const bool is_service = (data.piece_type == APT_STAND || data.piece_type == APT_STAND_1 ||
				IsModularHangarPiece(data.piece_type) ||
				IsModularHelipadPiece(data.piece_type));
		if (!is_service) continue;
		AirportGroundPath p = FindAirportGroundPath(st, start_tile, data.tile, nullptr);
		if (!p.found) continue;
		if (best_target == INVALID_TILE || p.cost < best_cost) {
			best_target = data.tile;
			best_cost = p.cost;
		}
	}
	if (best_target == INVALID_TILE) return INVALID_TILE;

	TaxiPath path = BuildTaxiPath(st, start_tile, best_target, nullptr);
	if (!path.valid || path.tiles.size() < 2 || path.segments.empty()) return INVALID_TILE;

	for (const TaxiSegment &seg : path.segments) {
		if (seg.type != TaxiSegmentType::FREE_MOVE) continue;
		TileIndex tile = path.tiles[seg.start_index];
		Tile t(tile);
		if (!IsAirportTile(t)) continue;
		if (HasAirportTileReservation(t) && GetAirportTileReserver(t) != v->index) continue;
		if (IsModularTileOccupiedByOtherAircraft(st, tile, v->index)) continue;
		return tile;
	}

	/* Fallback: any first non-runway step that is currently clear. */
	for (const TaxiSegment &seg : path.segments) {
		if (seg.type == TaxiSegmentType::RUNWAY) continue;
		TileIndex tile = path.tiles[seg.start_index];
		Tile t(tile);
		if (!IsAirportTile(t)) continue;
		if (HasAirportTileReservation(t) && GetAirportTileReserver(t) != v->index) continue;
		if (IsModularTileOccupiedByOtherAircraft(st, tile, v->index)) continue;
		return tile;
	}

	return INVALID_TILE;
}

bool IsModularTileOccupiedByOtherAircraft(const Station *st, TileIndex tile, VehicleID self)
{
	/* Hangars can hold multiple aircraft; never treat them as occupied. */
	if (IsModularHangarTile(st, tile)) return false;

	for (const Aircraft *other : Aircraft::Iterate()) {
		if (other->index == self) continue;
		if (!other->IsNormalAircraft()) continue;
		if (!IsValidTile(other->tile)) continue;
		if (other->tile != tile) continue;
		if (!st->TileBelongsToAirport(other->tile)) continue;
		return true;
	}
	return false;
}

TileIndex FindFreeModularTerminal(const Station *st, [[maybe_unused]] const Aircraft *v)
{
	if (st->airport.modular_tile_data == nullptr) return INVALID_TILE;
	const bool can_ground_route = CanUseModularGroundRouting(st, v);
	TileIndex best_tile = INVALID_TILE;
	int best_score = INT_MAX;

	/* Terminal piece types: APT_STAND, APT_STAND_1 */
	/* TODO: Also support hangars (9,10) and helipads (11) */
	for (const ModularAirportTileData &data : *st->airport.modular_tile_data) {
		if (data.piece_type == APT_STAND || data.piece_type == APT_STAND_1) {
			/* Check if tile is free */
			Tile t(data.tile);
			if (HasAirportTileReservation(t)) {
				/* If reserved by us, it's fine (we might be re-evaluating) */
				if (v != nullptr && GetAirportTileReserver(t) == v->index) return data.tile;
				continue;
			}
			if (v != nullptr && IsModularTileOccupiedByOtherAircraft(st, data.tile, v->index)) continue;

			/* Avoid assigning stands that are currently unreachable from our position. */
			int score = 0;
			if (can_ground_route) {
				AirportGroundPath path = FindAirportGroundPath(st, v->tile, data.tile, nullptr);
				if (!path.found) continue;
				score = path.cost;
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

TileIndex FindFreeModularHelipad(const Station *st, const Aircraft *v)
{
	if (st->airport.modular_tile_data == nullptr) return INVALID_TILE;
	if (v != nullptr && v->subtype != AIR_HELICOPTER) return INVALID_TILE;
	const bool can_ground_route = CanUseModularGroundRouting(st, v);
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
			Tile t(data.tile);
			if (HasAirportTileReservation(t)) {
				/* If reserved by us, it's fine */
				if (v != nullptr && GetAirportTileReserver(t) == v->index) return data.tile;
				continue;
			}

			/* Check for physical occupancy. */
			if (v != nullptr && IsModularTileOccupiedByOtherAircraft(st, data.tile, v->index)) continue;

			int score = 0;
			if (can_ground_route) {
				AirportGroundPath path = FindAirportGroundPath(st, v->tile, data.tile, nullptr);
				if (!path.found) continue;
				score = path.cost;
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

TileIndex FindFreeModularHangar(const Station *st, const Aircraft *v)
{
	if (st->airport.modular_tile_data == nullptr) return INVALID_TILE;
	const bool can_ground_route = CanUseModularGroundRouting(st, v);

	TileIndex best_path_tile = INVALID_TILE;
	int best_path_score = INT_MAX;
	TileIndex best_fallback_tile = INVALID_TILE;
	int best_fallback_score = INT_MAX;

	for (const ModularAirportTileData &data : *st->airport.modular_tile_data) {
		if (!IsModularHangarPiece(data.piece_type)) continue;

		int fallback_score = 0;
		if (v != nullptr) {
			const int cx = TileX(data.tile) * TILE_SIZE + TILE_SIZE / 2;
			const int cy = TileY(data.tile) * TILE_SIZE + TILE_SIZE / 2;
			fallback_score = abs(cx - v->x_pos) + abs(cy - v->y_pos);
		}
		if (best_fallback_tile == INVALID_TILE || fallback_score < best_fallback_score) {
			best_fallback_score = fallback_score;
			best_fallback_tile = data.tile;
		}

		if (!can_ground_route) continue;

		AirportGroundPath path = FindAirportGroundPath(st, v->tile, data.tile, nullptr);
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

bool TryClearStaleModularReservation(const Station *st, TileIndex tile, VehicleID reserver)
{
	if (st == nullptr || !IsValidTile(tile)) return false;
	Tile t(tile);
	if (!IsAirportTile(t)) return false;
	if (!HasAirportTileReservation(t) || GetAirportTileReserver(t) != reserver) return false;

	Vehicle *veh = Vehicle::GetIfValid(reserver);
	if (veh == nullptr || veh->type != VEH_AIRCRAFT) {
		SetAirportTileReservation(t, false);
		return true;
	}

	Aircraft *a = Aircraft::From(veh);
	if (!a->IsNormalAircraft()) {
		SetAirportTileReservation(t, false);
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
		SetAirportTileReservation(t, false);
		return true;
	}

	/* Aircraft is active on this station ground, but this tile is not part of any tracked intent. */
	SetAirportTileReservation(t, false);
	return true;
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
	const auto tile_blocked = [&](TileIndex tile) -> bool {
		if (IsTaxiTileReservedByOther(st, tile, v->index)) return true;
		if (tile != v->tile && IsModularTileOccupiedByOtherAircraft(st, tile, v->index)) return true;
		return false;
	};
	const auto path_enterable = [&](const TaxiPath &taxi_path) -> bool {
		if (!taxi_path.valid || taxi_path.tiles.size() < 2 || taxi_path.segments.empty()) return false;
		const uint8_t seg_idx = FindTaxiSegmentIndex(&taxi_path, 1);
		if (seg_idx >= taxi_path.segments.size()) return false;
		const TaxiSegment &seg = taxi_path.segments[seg_idx];
		if (seg.type == TaxiSegmentType::RUNWAY) {
			return !IsContiguousModularRunwayBusyByOther(v, st, taxi_path.tiles[seg.start_index]);
		}
		if (seg.type == TaxiSegmentType::ONE_WAY) return !tile_blocked(taxi_path.tiles[1]);
		for (uint16_t i = seg.start_index; i <= seg.end_index; ++i) {
			if (tile_blocked(taxi_path.tiles[i])) return false;
		}
		if (seg.end_index + 1 < taxi_path.tiles.size()) {
			const uint8_t next_seg = seg_idx + 1;
			TileIndex exit_tile = taxi_path.tiles[seg.end_index + 1];
			if (next_seg < taxi_path.segments.size() && taxi_path.segments[next_seg].type == TaxiSegmentType::RUNWAY) {
				if (IsContiguousModularRunwayBusyByOther(v, st, exit_tile)) return false;
			} else if (tile_blocked(exit_tile)) {
				return false;
			}
		}
		return true;
	};

	TileIndex best_path_tile = INVALID_TILE;
	int best_path_score = INT_MAX;
	TileIndex best_non_runway_taxi_tile = INVALID_TILE;
	int best_non_runway_taxi_score = INT_MAX;
	TileIndex best_fallback_tile = INVALID_TILE;
	int best_fallback_score = INT_MAX;

	for (const ModularAirportTileData &data : *st->airport.modular_tile_data) {
		bool is_end = (data.piece_type == APT_RUNWAY_END || data.piece_type == APT_RUNWAY_SMALL_NEAR_END || data.piece_type == APT_RUNWAY_SMALL_FAR_END);
		if (!is_end) continue;

		const uint8_t flags = GetRunwayFlags(st, data.tile);
		if ((flags & RUF_TAKEOFF) == 0) continue;

		/* Direction bits are interpreted as travel direction.
		 * Takeoff from low end travels toward high end, and vice versa. */
		const bool is_low = IsRunwayEndLow(st, data.tile);
		if (is_low && (flags & RUF_DIR_HIGH) == 0) continue;
		if (!is_low && (flags & RUF_DIR_LOW) == 0) continue;

		/* Large aircraft require a long, large-family runway for takeoff too. */
		if (v != nullptr && (AircraftVehInfo(v->engine_type)->subtype & AIR_FAST) &&
				!_cheats.no_jetcrash.value &&
				!IsRunwaySafeForLarge(st, data.tile)) {
			continue;
		}

		/* Keep a distance-based fallback so we don't deadlock if path probing fails transiently. */
		int fallback_score = 0;
		if (v != nullptr) {
			int cx = TileX(data.tile) * TILE_SIZE + TILE_SIZE / 2;
			int cy = TileY(data.tile) * TILE_SIZE + TILE_SIZE / 2;
			fallback_score = abs(cx - v->x_pos) + abs(cy - v->y_pos);
		}
		if (best_fallback_tile == INVALID_TILE || fallback_score < best_fallback_score) {
			best_fallback_score = fallback_score;
			best_fallback_tile = data.tile;
		}

		/* Prefer reachable takeoff ends. */
		if (!can_ground_route) continue;
		TaxiPath taxi_path = BuildTaxiPath(st, v->tile, data.tile, v);
		if (!taxi_path.valid) {
			if (v != nullptr && ShouldLogModularRateLimited(v->index, 35, 128)) {
				Debug(misc, 2, "[ModAp] V{} takeoff-path invalid: from={} to={}", v->index, v->tile.base(), data.tile.base());
			}
			continue;
		}
		if (!path_enterable(taxi_path)) {
			if (v != nullptr && ShouldLogModularRateLimited(v->index, 36, 128)) {
				/* Determine why path is not enterable */
				const uint8_t pe_seg_idx = FindTaxiSegmentIndex(&taxi_path, 1);
				const char *pe_reason = "unknown";
				if (pe_seg_idx >= taxi_path.segments.size()) {
					pe_reason = "seg_idx_oob";
				} else {
					const TaxiSegment &pe_seg = taxi_path.segments[pe_seg_idx];
					if (pe_seg.type == TaxiSegmentType::RUNWAY) {
						pe_reason = "runway_busy";
					} else if (pe_seg.type == TaxiSegmentType::ONE_WAY) {
						pe_reason = "oneway_blocked";
					} else {
						pe_reason = "freemove_blocked";
					}
				}
				Debug(misc, 2, "[ModAp] V{} takeoff-path not enterable: from={} to={} reason={}", v->index, v->tile.base(), data.tile.base(), pe_reason);
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
	/* With route context, an unreachable takeoff runway should be treated as unavailable. */
	if (can_ground_route) return INVALID_TILE;
	return best_fallback_tile;
}

/**
 * Find a queue tile just before entering the selected takeoff runway end.
 * Returns runway_end when no non-runway queue point exists.
 */
TileIndex FindModularTakeoffQueueTile(const Station *st, const Aircraft *v, TileIndex runway_end)
{
	if (runway_end == INVALID_TILE || v == nullptr) return runway_end;
	if (!CanUseModularGroundRouting(st, v)) return runway_end;

	AirportGroundPath path = FindAirportGroundPath(st, v->tile, runway_end, v);
	if (!path.found || path.tiles.empty()) return INVALID_TILE;

	TileIndex queue_tile = runway_end;
	TileIndex best_queue_tile = INVALID_TILE;
	for (TileIndex tile : path.tiles) {
		const ModularAirportTileData *td = st->airport.GetModularTileData(tile);
		if (td != nullptr && IsModularRunwayPiece(td->piece_type)) {
			break;
		}

		/* Queueing for takeoff should happen on taxi/apron tiles, not stands/hangars/helipads,
		 * and only on currently free tiles to avoid hard blocking by parked aircraft. */
		const bool service_tile = (td != nullptr) &&
				(td->piece_type == APT_STAND || td->piece_type == APT_STAND_1 ||
				 IsModularHangarPiece(td->piece_type) ||
				 IsModularHelipadPiece(td->piece_type));
		if (service_tile) {
			queue_tile = tile;
			continue;
		}

		const bool blocked_by_reservation = IsTaxiTileReservedByOther(st, tile, v->index);
		if (blocked_by_reservation || IsModularTileOccupiedByOtherAircraft(st, tile, v->index)) {
			queue_tile = tile;
			continue;
		}

		best_queue_tile = tile;
		queue_tile = tile;
	}

	if (best_queue_tile != INVALID_TILE) return best_queue_tile;

	/* If no safe queue tile exists, only use runway end if it's currently clear. */
	Tile runway_t(runway_end);
	if (!IsTaxiTileReservedByOther(st, runway_end, v->index)) {
		if (!IsModularTileOccupiedByOtherAircraft(st, runway_end, v->index)) return runway_end;
	}

	return INVALID_TILE;

}

bool CanUseModularGroundRouting(const Station *st, const Aircraft *v)
{
	return v != nullptr && IsValidTile(v->tile) && st->TileBelongsToAirport(v->tile);
}

void ClearTaxiPathReservation(Aircraft *v, TileIndex keep_tile)
{
	for (TileIndex tile : v->taxi_reserved_tiles) {
		if (tile == keep_tile) continue;
		if (std::find(v->modular_runway_reservation.begin(), v->modular_runway_reservation.end(), tile) != v->modular_runway_reservation.end()) continue;
		Tile t(tile);
		if (!IsAirportTile(t)) continue;
		if (HasAirportTileReservation(t) && GetAirportTileReserver(t) == v->index) SetAirportTileReservation(t, false);
	}
	v->taxi_reserved_tiles.clear();

	if (keep_tile != INVALID_TILE) {
		Tile keep(keep_tile);
		if (IsAirportTile(keep) && HasAirportTileReservation(keep) && GetAirportTileReserver(keep) == v->index) {
			v->taxi_reserved_tiles.push_back(keep_tile);
		}
	}
}

void ClearTaxiPathState(Aircraft *v, TileIndex keep_tile)
{
	ClearTaxiPathReservation(v, keep_tile);
	delete v->taxi_path;
	v->taxi_path = nullptr;
	v->taxi_path_index = 0;
	v->taxi_current_segment = 0;
	v->taxi_wait_counter = 0;
}

uint8_t FindTaxiSegmentIndex(const TaxiPath *path, uint16_t tile_index)
{
	if (path == nullptr) return 0;
	for (uint8_t i = 0; i < path->segments.size(); ++i) {
		const TaxiSegment &seg = path->segments[i];
		if (tile_index >= seg.start_index && tile_index <= seg.end_index) return i;
	}
	return static_cast<uint8_t>(path->segments.size());
}

bool IsTaxiTileReservedByOther(const Station *st, TileIndex tile, VehicleID vid)
{
	Tile t(tile);
	if (!IsAirportTile(t)) return false;
	if (!HasAirportTileReservation(t)) return false;
	const VehicleID reserver = GetAirportTileReserver(t);
	if (reserver == vid) return false;
	if (TryClearStaleModularReservation(st, tile, reserver)) return false;
	return HasAirportTileReservation(t) && GetAirportTileReserver(t) != vid;
}

struct ModularCrossingQueueState {
	std::map<VehicleID, uint64_t> waiting_since;
};

static std::map<uint64_t, ModularCrossingQueueState> _modular_crossing_queues;

static uint64_t BuildModularCrossingQueueKey(const Station *st, std::span<const TileIndex> runway_resource_keys, TileIndex exit_tile)
{
	uint64_t key = 1469598103934665603ULL;
	const auto mix = [&](uint64_t value) {
		key ^= value + 0x9e3779b97f4a7c15ULL + (key << 6) + (key >> 2);
	};

	mix(static_cast<uint64_t>(st->index.base()));
	mix(static_cast<uint64_t>(exit_tile.base()));
	for (TileIndex tile : runway_resource_keys) mix(static_cast<uint64_t>(tile.base()));
	return key;
}

static bool CanGrantRunwayCrossingNow(const Station *st, uint64_t queue_key, VehicleID vid)
{
	/* Periodically sweep all queues to purge empty/stale entries (every 256 ticks). */
	const uint64_t now = TimerGameTick::counter;
	if ((now & 0xFF) == 0) {
		for (auto it = _modular_crossing_queues.begin(); it != _modular_crossing_queues.end(); ) {
			if (it->second.waiting_since.empty()) {
				it = _modular_crossing_queues.erase(it);
			} else {
				++it;
			}
		}
	}

	ModularCrossingQueueState &queue = _modular_crossing_queues[queue_key];

	/* Drop stale waiters first. */
	for (auto it = queue.waiting_since.begin(); it != queue.waiting_since.end(); ) {
		const Aircraft *a = Aircraft::GetIfValid(it->first);
		if (a == nullptr || !a->IsNormalAircraft() || (a->targetairport != st->index && a->last_station_visited != st->index)) {
			it = queue.waiting_since.erase(it);
		} else {
			++it;
		}
	}

	if (!queue.waiting_since.contains(vid)) queue.waiting_since[vid] = now;

	VehicleID best_vid = vid;
	uint64_t best_wait = UINT64_MAX;
	for (const auto &[wait_vid, wait_since] : queue.waiting_since) {
		if (wait_since < best_wait || (wait_since == best_wait && wait_vid < best_vid)) {
			best_wait = wait_since;
			best_vid = wait_vid;
		}
	}
	return best_vid == vid;
}

static void MarkRunwayCrossingGranted(uint64_t queue_key, VehicleID vid)
{
	auto it = _modular_crossing_queues.find(queue_key);
	if (it == _modular_crossing_queues.end()) return;
	it->second.waiting_since.erase(vid);
	if (it->second.waiting_since.empty()) _modular_crossing_queues.erase(it);
}

static bool IsPathTileRunwayPiece(const Station *st, TileIndex tile)
{
	const ModularAirportTileData *data = st->airport.GetModularTileData(tile);
	return data != nullptr && IsModularRunwayPiece(data->piece_type);
}

static bool TryReserveRunwayResourcesAtomic(Aircraft *v, const Station *st, const std::vector<std::vector<TileIndex>> &resources, bool log_success)
{
	if (resources.empty()) return false;

	/* Validate all runway resources before mutating any reservation state. */
	for (const std::vector<TileIndex> &resource : resources) {
		VehicleID state_blocker = VehicleID::Invalid();
		if (IsContiguousModularRunwayReservedInStateByOther(v, st, resource, &state_blocker)) {
			if (ShouldLogModularRateLimited(v->index, 1, 128)) {
				Debug(misc, 2, "[ModAp] V{} runway-reserve denied: runway held in state by V{}", v->index, state_blocker.base());
			}
			return false;
		}

		for (TileIndex tile : resource) {
			if (IsModularTileOccupiedByOtherAircraft(st, tile, v->index)) {
				if (ShouldLogModularRateLimited(v->index, 1, 128)) {
					Debug(misc, 2, "[ModAp] V{} runway-reserve denied: runway tile {} occupied by other aircraft", v->index, tile.base());
				}
				return false;
			}

			Tile t(tile);
			if (!IsAirportTile(t)) continue;
			if (IsTaxiTileReservedByOther(st, tile, v->index)) {
				if (ShouldLogModularRateLimited(v->index, 1, 128)) {
					VehicleID blocker = VehicleID::Invalid();
					if (HasAirportTileReservation(t) && GetAirportTileReserver(t) != v->index) blocker = GetAirportTileReserver(t);
					Debug(misc, 2, "[ModAp] V{} runway-reserve denied: runway tile {} reserved by V{}", v->index, tile.base(), blocker.base());
				}
				return false;
			}
		}
	}

	std::vector<TileIndex> combined;
	for (const std::vector<TileIndex> &resource : resources) {
		for (TileIndex tile : resource) {
			if (std::find(combined.begin(), combined.end(), tile) == combined.end()) combined.push_back(tile);
		}
	}
	std::sort(combined.begin(), combined.end(), [](TileIndex a, TileIndex b) { return a.base() < b.base(); });

	const bool reservation_changed = (v->modular_runway_reservation != combined);
	if (reservation_changed) ClearModularRunwayReservation(v);

	for (TileIndex tile : combined) {
		Tile t(tile);
		if (!IsAirportTile(t)) continue;
		SetAirportTileReservation(t, true);
		SetAirportTileReserver(t, v->index);
	}
	v->modular_runway_reservation = std::move(combined);

	if (reservation_changed && log_success && ShouldLogModularRateLimited(v->index, 32, 16)) {
		LogModularVehicleReservationState(st, v, "reserve granted");
	}
	return true;
}

void SetTaxiReservation(Aircraft *v, TileIndex tile)
{
	Tile t(tile);
	if (!IsAirportTile(t)) return;
	SetAirportTileReservation(t, true);
	SetAirportTileReserver(t, v->index);
	if (std::find(v->taxi_reserved_tiles.begin(), v->taxi_reserved_tiles.end(), tile) == v->taxi_reserved_tiles.end()) {
		v->taxi_reserved_tiles.push_back(tile);
	}
}

bool TryReserveTaxiSegment(Aircraft *v, const Station *st, uint8_t segment_idx)
{
	if (v->taxi_path == nullptr || segment_idx >= v->taxi_path->segments.size()) return false;
	const TaxiSegment &seg = v->taxi_path->segments[segment_idx];
	const auto &tiles = v->taxi_path->tiles;

	if (seg.type == TaxiSegmentType::RUNWAY) {
		/* Crossing path between two non-runway areas must reserve atomically:
		 * hold position + all touched runway resources + first exit tile. */
		const bool crossing_request =
				seg.start_index > 0 &&
				(seg.end_index + 1) < tiles.size() &&
				!IsPathTileRunwayPiece(st, tiles[seg.start_index - 1]) &&
				!IsPathTileRunwayPiece(st, tiles[seg.end_index + 1]);

		std::map<TileIndex, std::vector<TileIndex>> runway_resources;
		for (uint16_t i = seg.start_index; i <= seg.end_index; ++i) {
			const TileIndex runway_tile = tiles[i];
			if (!IsPathTileRunwayPiece(st, runway_tile)) continue;

			std::vector<TileIndex> resource_tiles;
			if (!GetContiguousModularRunwayTiles(st, runway_tile, resource_tiles) || resource_tiles.empty()) return false;
			const TileIndex resource_key = resource_tiles.front();
			runway_resources.emplace(resource_key, std::move(resource_tiles));
		}
		if (runway_resources.empty()) return false;

		std::vector<TileIndex> resource_keys;
		std::vector<std::vector<TileIndex>> ordered_resources;
		resource_keys.reserve(runway_resources.size());
		ordered_resources.reserve(runway_resources.size());
		for (auto &[resource_key, resource_tiles] : runway_resources) {
			resource_keys.push_back(resource_key);
			ordered_resources.push_back(resource_tiles);
		}

		TileIndex exit_tile = INVALID_TILE;
		if (crossing_request) {
			exit_tile = tiles[seg.end_index + 1];
			if (!IsModularHangarTile(st, exit_tile)) {
				if (IsTaxiTileReservedByOther(st, exit_tile, v->index)) return false;
				if (exit_tile != v->tile && IsModularTileOccupiedByOtherAircraft(st, exit_tile, v->index)) return false;
			}

			const uint64_t queue_key = BuildModularCrossingQueueKey(st, resource_keys, exit_tile);
			if (!CanGrantRunwayCrossingNow(st, queue_key, v->index)) return false;
			if (!TryReserveRunwayResourcesAtomic(v, st, ordered_resources, true)) return false;

			/* Keep hold and exit non-runway tiles reserved while crossing runway resources. */
			SetTaxiReservation(v, v->tile);
			SetTaxiReservation(v, exit_tile);
			MarkRunwayCrossingGranted(queue_key, v->index);
			return true;
		}

		return TryReserveRunwayResourcesAtomic(v, st, ordered_resources, true);
	}

	if (seg.type == TaxiSegmentType::ONE_WAY) {
		if (v->taxi_path_index + 1 >= tiles.size()) return true;
		TileIndex next = tiles[v->taxi_path_index + 1];
		if (IsTaxiTileReservedByOther(st, next, v->index)) return false;
		if (IsModularTileOccupiedByOtherAircraft(st, next, v->index)) return false;
		SetTaxiReservation(v, next);
		return true;
	}

	/* FREE_MOVE segment: reserve whole segment atomically, plus first tile of the next segment.
	 * Hangar tiles are multi-capacity and never block reservations. */
	std::vector<TileIndex> to_reserve;
	to_reserve.reserve(seg.end_index - seg.start_index + 2);

	for (uint16_t i = seg.start_index; i <= seg.end_index; ++i) {
		TileIndex tile = tiles[i];
		if (!IsModularHangarTile(st, tile)) {
			if (IsTaxiTileReservedByOther(st, tile, v->index)) return false;
			if (tile != v->tile && IsModularTileOccupiedByOtherAircraft(st, tile, v->index)) return false;
		}
		to_reserve.push_back(tile);
	}

	if (seg.end_index + 1 < tiles.size()) {
		TileIndex exit_tile = tiles[seg.end_index + 1];
		const uint8_t next_seg = segment_idx + 1;
		if (next_seg < v->taxi_path->segments.size() && v->taxi_path->segments[next_seg].type == TaxiSegmentType::RUNWAY) {
			if (!TryReserveContiguousModularRunway(v, st, exit_tile)) return false;
		} else if (!IsModularHangarTile(st, exit_tile)) {
			if (IsTaxiTileReservedByOther(st, exit_tile, v->index)) return false;
			if (IsModularTileOccupiedByOtherAircraft(st, exit_tile, v->index)) return false;
			to_reserve.push_back(exit_tile);
		} else {
			to_reserve.push_back(exit_tile);
		}
	}

	for (TileIndex tile : to_reserve) SetTaxiReservation(v, tile);
	return true;
}

bool TryRetargetModularGroundGoal(Aircraft *v, const Station *st)
{
	TileIndex alt_goal = INVALID_TILE;
	uint8_t alt_target = v->modular_ground_target;

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
		case MGT_ROLLOUT:
			alt_goal = FindModularLandingGroundGoal(st, v, &alt_target);
			break;
		default:
			return false;
	}

	if (alt_goal == INVALID_TILE || alt_goal == v->ground_path_goal) {
		if (v->modular_ground_target == MGT_HANGAR && ShouldLogModularRateLimited(v->index, 47, 128)) {
			Debug(misc, 2, "[ModAp] V{} retarget-hangar failed: tile={} goal={} alt={} wait={}",
				v->index,
				IsValidTile(v->tile) ? v->tile.base() : 0,
				IsValidTile(v->ground_path_goal) ? v->ground_path_goal.base() : 0,
				IsValidTile(alt_goal) ? alt_goal.base() : 0,
				v->taxi_wait_counter);
		}
		return false;
	}

	v->ground_path_goal = alt_goal;
	v->modular_ground_target = alt_target;
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
				bool wants_depot = v->current_order.IsType(OT_GOTO_DEPOT) || v->NeedsAutomaticServicing();
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
					v->ground_path_goal = goal;
					v->modular_ground_target = target;
					v->state = (target == MGT_HANGAR) ? HANGAR : TERM1;
				} else {
					/* No immediate service destination from rollout completion:
					 * first vacate runway to the nearest non-runway airport tile. */
					TileIndex exit_tile = FindNearestModularRunwayExitTile(st, v, v->tile);
					TileIndex holding_tile = (exit_tile != INVALID_TILE) ? exit_tile : FindModularRolloutHoldingTile(st, v, v->tile);
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
			}
			break;

		case MGT_TERMINAL:
		case MGT_HELIPAD:
			if ((v->modular_ground_target == MGT_TERMINAL || v->modular_ground_target == MGT_HELIPAD) &&
					IsModularTileOccupiedByOtherAircraft(st, v->tile, v->index)) {
				/* Reservation desync safety: if another aircraft is already on this stand or pad,
				 * re-target to a different one instead of stacking aircraft on one tile. */
				TileIndex goal = (v->modular_ground_target == MGT_HELIPAD) ? FindFreeModularHelipad(st, v) : FindFreeModularTerminal(st, v);
				if (goal == INVALID_TILE && v->modular_ground_target == MGT_HELIPAD) {
					/* Helicopter couldn't find a helipad, try a stand as fallback. */
					goal = FindFreeModularTerminal(st, v);
					if (goal != INVALID_TILE) v->modular_ground_target = MGT_TERMINAL;
				}
				if (goal != INVALID_TILE && goal != v->tile) {
					v->ground_path_goal = goal;
					v->state = TERM1;
					return;
				}
			}
			if (IsAirportTile(v->tile)) {
				Tile t(v->tile);
				SetAirportTileReservation(t, true);
				SetAirportTileReserver(t, v->index);
			}
			AircraftEntersTerminal(v);
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
				Tile t(v->tile);
				SetAirportTileReservation(t, true);
				SetAirportTileReserver(t, v->index);
			}
			Debug(misc, 3, "[ModAp] Vehicle {} entering hangar at tile {}", v->index, v->tile.base());
			VehicleEnterDepot(v);
			v->state = HANGAR;
			v->modular_ground_target = MGT_NONE;
			break;

			case MGT_RUNWAY_TAKEOFF:
				if (st->airport.blocks.Test(AirportBlock::Zeppeliner)) {
					/* Airport-wide zeppeliner block: hold departures until the wreck is cleared. */
					ClearTaxiPathReservation(v);
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
						const bool has_extra_taxi_reservation = std::any_of(v->taxi_reserved_tiles.begin(), v->taxi_reserved_tiles.end(), [&](TileIndex tile) { return tile != v->tile; });
						if (has_extra_taxi_reservation || !v->modular_runway_reservation.empty()) {
							ClearModularRunwayReservation(v);
							ClearModularAirportReservationsByVehicle(st, v->index, v->tile);
						}
						if (IsAirportTile(v->tile)) {
							Tile t(v->tile);
							SetAirportTileReservation(t, true);
							SetAirportTileReserver(t, v->index);
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
				Tile t(v->tile);
				SetAirportTileReservation(t, true);
				SetAirportTileReserver(t, v->index);
			}
			v->state = TAKEOFF;
			v->modular_takeoff_tile = v->tile;
			v->modular_takeoff_progress = 0;
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
		if (!HasAirportTileReservation(t) || GetAirportTileReserver(t) != v->index) continue;
		owned_tiles.push_back(data.tile);
		if (IsModularRunwayPiece(data.piece_type)) owned_runway_tiles.push_back(data.tile);
	}

	std::sort(owned_tiles.begin(), owned_tiles.end(), [](TileIndex a, TileIndex b) { return a.base() < b.base(); });
	std::sort(owned_runway_tiles.begin(), owned_runway_tiles.end(), [](TileIndex a, TileIndex b) { return a.base() < b.base(); });

	uint32_t tracked_but_unowned = 0;
	for (TileIndex tile : v->modular_runway_reservation) {
		if (std::find(owned_tiles.begin(), owned_tiles.end(), tile) == owned_tiles.end()) tracked_but_unowned++;
	}

	uint32_t owned_runway_untracked = 0;
	for (TileIndex tile : owned_runway_tiles) {
		if (std::find(v->modular_runway_reservation.begin(), v->modular_runway_reservation.end(), tile) == v->modular_runway_reservation.end()) owned_runway_untracked++;
	}

	size_t path_len = (v->taxi_path != nullptr) ? v->taxi_path->tiles.size() : 0;
	Debug(misc, 2,
		"[ModAp] V{} reserve-state reason='{}' state={} tile={} goal={} tgt={} path={}/{} runway_res={} owned={} owned_rw={} tracked_not_owned={} owned_rw_not_tracked={}",
		v->index, reason, v->state,
		IsValidTile(v->tile) ? v->tile.base() : 0,
		IsValidTile(v->ground_path_goal) ? v->ground_path_goal.base() : 0,
		v->modular_ground_target,
		v->taxi_path_index, path_len,
		v->modular_runway_reservation.size(), owned_tiles.size(), owned_runway_tiles.size(),
		tracked_but_unowned, owned_runway_untracked);

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

struct ModularTakeoffFailLogState {
	uint64_t last_tick = 0;
	uint32_t suppressed_count = 0;
};

void LogModularTakeoffRunwayUnavailable(const Station *st, const Aircraft *v)
{
	static std::map<VehicleID, ModularTakeoffFailLogState> fail_state;
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
			const bool is_end = (data.piece_type == APT_RUNWAY_END || data.piece_type == APT_RUNWAY_SMALL_NEAR_END || data.piece_type == APT_RUNWAY_SMALL_FAR_END);
			if (!is_end) continue;

			const uint8_t flags = GetRunwayFlags(st, data.tile);
			const bool mode_ok = (flags & RUF_TAKEOFF) != 0;
			const bool is_low = IsRunwayEndLow(st, data.tile);
			const bool dir_ok = is_low ? ((flags & RUF_DIR_HIGH) != 0) : ((flags & RUF_DIR_LOW) != 0);

			bool path_ok = false;
			int path_cost = -1;
			if (can_ground_route) {
				AirportGroundPath path = FindAirportGroundPath(st, v->tile, data.tile, nullptr);
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

	/* Ground-path movement must never run at flight/takeoff speeds.
	 * If landing/takeoff transitions leave residual high speed, clamp before any
	 * pathing/reservation decisions to avoid long-tail runway deadlocks. */
	const uint scaled_taxi_limit = SPEED_LIMIT_TAXI * _settings_game.vehicle.plane_speed;
	if (v->cur_speed > scaled_taxi_limit) {
		if (ShouldLogModularRateLimited(v->index, 35, 128)) {
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

	if (!IsValidTile(v->tile) || !st->TileBelongsToAirport(v->tile)) {
		ClearTaxiPathState(v);
		return false;
	}

	const ModularAirportTileData *goal_data = st->airport.GetModularTileData(v->ground_path_goal);
	if (goal_data == nullptr) {
		ClearTaxiPathState(v);
		v->ground_path_goal = INVALID_TILE;
		v->modular_ground_target = MGT_NONE;
		return true;
	}

	if (v->tile == v->ground_path_goal) {
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
		ClearTaxiPathState(v, v->tile);
		TaxiPath new_path = BuildTaxiPath(st, v->tile, v->ground_path_goal, v);
		if (!new_path.valid || new_path.tiles.size() < 2 || new_path.segments.empty()) {
			v->taxi_wait_counter++;
			if (v->taxi_wait_counter >= 128 && (v->taxi_wait_counter % 128) == 0) {
				AirportGroundPath dbg_path = FindAirportGroundPath(st, v->tile, v->ground_path_goal, v);
				Debug(misc, 1,
					"[ModAp] V{} stuck(no-path) wait={} state={} tile={} goal={} tgt={} path_found={} cost={}",
					v->index, v->taxi_wait_counter, v->state,
					IsValidTile(v->tile) ? v->tile.base() : 0,
					IsValidTile(v->ground_path_goal) ? v->ground_path_goal.base() : 0,
					v->modular_ground_target, dbg_path.found, dbg_path.cost);
			}
			if (v->taxi_wait_counter > 64) {
				if (!TryRetargetModularGroundGoal(v, st)) {
					ClearTaxiPathState(v, v->tile);
					v->taxi_wait_counter = 0;
				}
			}
			return false;
		}

		v->taxi_path = new TaxiPath(std::move(new_path));
		v->taxi_path_index = 0;
		v->taxi_current_segment = FindTaxiSegmentIndex(v->taxi_path, 0);
		v->taxi_wait_counter = 0;
		SetTaxiReservation(v, v->tile);
	}

	if (v->taxi_path == nullptr || v->taxi_path_index + 1 >= v->taxi_path->tiles.size()) {
		ClearTaxiPathState(v, v->tile);
		v->ground_path_goal = INVALID_TILE;
		HandleModularGroundArrival(v);
		return true;
	}

	const uint16_t current_index = v->taxi_path_index;
	const uint16_t next_index = current_index + 1;
	const uint8_t next_segment = FindTaxiSegmentIndex(v->taxi_path, next_index);
	if (next_segment >= v->taxi_path->segments.size()) {
		ClearTaxiPathState(v, v->tile);
		return false;
	}

	TileIndex next_tile = v->taxi_path->tiles[next_index];
	const TaxiSegmentType next_type = v->taxi_path->segments[next_segment].type;
	bool need_reserve = (next_type == TaxiSegmentType::ONE_WAY);
	if (!need_reserve) {
		Tile t(next_tile);
		need_reserve = !IsAirportTile(t) || !HasAirportTileReservation(t) || GetAirportTileReserver(t) != v->index;
	}
	if (need_reserve && !TryReserveTaxiSegment(v, st, next_segment)) {
		v->taxi_wait_counter++;
		if (v->taxi_wait_counter >= 128 && (v->taxi_wait_counter % 128) == 0) {
			Tile t(next_tile);
			const bool reserved_by_other = IsAirportTile(t) && HasAirportTileReservation(t) && GetAirportTileReserver(t) != v->index;
			const VehicleID reserver = reserved_by_other ? GetAirportTileReserver(t) : VehicleID::Invalid();
			const bool occupied_by_other = IsModularTileOccupiedByOtherAircraft(st, next_tile, v->index);
			const bool runway_busy = (next_type == TaxiSegmentType::RUNWAY) && IsContiguousModularRunwayBusyByOther(v, st, next_tile);
			Debug(misc, 1,
				"[ModAp] V{} stuck(reserve) wait={} state={} tile={} next={} seg={} goal={} tgt={} reserved_by_other={} reserver={} occupied_by_other={} runway_busy={}",
				v->index, v->taxi_wait_counter, v->state,
				IsValidTile(v->tile) ? v->tile.base() : 0,
				IsValidTile(next_tile) ? next_tile.base() : 0,
				static_cast<uint8_t>(next_type),
				IsValidTile(v->ground_path_goal) ? v->ground_path_goal.base() : 0,
				v->modular_ground_target,
				reserved_by_other,
				reserved_by_other ? reserver.base() : 0,
				occupied_by_other,
				runway_busy);
		}
		if (v->taxi_wait_counter > 64) {
			if (!TryRetargetModularGroundGoal(v, st)) {
				ClearTaxiPathState(v, v->tile);
				v->taxi_wait_counter = 0;
			}
		}
		return false;
	}
	v->taxi_wait_counter = 0;

	/* Final safety gate before movement: never enter a tile currently occupied by another aircraft,
	 * even if reservation ownership was stale. */
	if (next_tile != v->tile && IsModularTileOccupiedByOtherAircraft(st, next_tile, v->index)) {
		v->taxi_wait_counter++;
		if (v->taxi_wait_counter >= 128 && (v->taxi_wait_counter % 128) == 0) {
			Debug(misc, 1,
				"[ModAp] V{} stuck(occupied) wait={} state={} tile={} next={} goal={} tgt={}",
				v->index, v->taxi_wait_counter, v->state,
				IsValidTile(v->tile) ? v->tile.base() : 0,
				IsValidTile(next_tile) ? next_tile.base() : 0,
				IsValidTile(v->ground_path_goal) ? v->ground_path_goal.base() : 0,
				v->modular_ground_target);
		}
		if (v->taxi_wait_counter > 64) {
			if (!TryRetargetModularGroundGoal(v, st)) {
				ClearTaxiPathState(v, v->tile);
				v->taxi_wait_counter = 0;
			}
		}
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

		int count = UpdateAircraftSpeed(v, SPEED_LIMIT_TAXI);
		while (count-- > 0) {
			GetNewVehiclePosResult gp = GetNewVehiclePos(v);
			v->x_pos = gp.x;
			v->y_pos = gp.y;
			SetAircraftPosition(v, v->x_pos, v->y_pos, v->z_pos);
			if (abs(v->x_pos - target_x) + abs(v->y_pos - target_y) == 0) break;
		}
	}

	if (v->x_pos != target_x || v->y_pos != target_y) return false;

	const TileIndex old_tile = v->tile;
	const uint8_t old_segment = FindTaxiSegmentIndex(v->taxi_path, current_index);
	v->tile = next_tile;
	v->taxi_path_index = next_index;
	v->taxi_current_segment = next_segment;
	v->number_consecutive_turns = 0;

	if (!v->modular_runway_reservation.empty()) {
		const ModularAirportTileData *tile_data = st->airport.GetModularTileData(v->tile);
		if (tile_data == nullptr || !IsModularRunwayPiece(tile_data->piece_type)) ClearModularRunwayReservation(v);
	}

	const TaxiSegmentType old_type = (old_segment < v->taxi_path->segments.size()) ? v->taxi_path->segments[old_segment].type : TaxiSegmentType::FREE_MOVE;
	if (old_type == TaxiSegmentType::ONE_WAY) {
		Tile t(old_tile);
		if (IsAirportTile(t) && HasAirportTileReservation(t) && GetAirportTileReserver(t) == v->index) SetAirportTileReservation(t, false);
	}
	if (old_segment != next_segment && old_type == TaxiSegmentType::FREE_MOVE && next_type != TaxiSegmentType::RUNWAY) {
		ClearTaxiPathReservation(v, v->tile);
	}
	if (old_segment != next_segment && old_type == TaxiSegmentType::RUNWAY && next_type != TaxiSegmentType::RUNWAY) {
		ClearTaxiPathReservation(v, v->tile);
	}
	SetTaxiReservation(v, v->tile);

	if (v->tile == v->ground_path_goal || v->taxi_path_index + 1 >= v->taxi_path->tiles.size()) {
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
	TileIndex runway = INVALID_TILE;

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
		/* Helicopter: fly directly towards helipad tile center (no runway-style
		 * approach offset), or fall back to station center if no target found. */
		TileIndex target = st->airport.tile;
		if (target == INVALID_TILE) target = st->xy;
		runway = FindModularLandingTarget(st, v);
		if (runway != INVALID_TILE) {
			const ModularAirportTileData *land_data = st->airport.GetModularTileData(runway);
			if (land_data != nullptr && IsModularHelipadPiece(land_data->piece_type)) {
				/* Helipad: fly directly to tile center, no FAF offset. */
				target_x = TileX(runway) * TILE_SIZE + TILE_SIZE / 2;
				target_y = TileY(runway) * TILE_SIZE + TILE_SIZE / 2;
			} else {
				/* Runway landing: use standard approach point. */
				GetModularLandingApproachPoint(st, runway, &target_x, &target_y);
			}
		} else {
			target_x = TileX(target) * TILE_SIZE + TILE_SIZE / 2;
			target_y = TileY(target) * TILE_SIZE + TILE_SIZE / 2;
		}
	}

	const int dist = abs(v->x_pos - target_x) + abs(v->y_pos - target_y);

	if (v->subtype == AIR_HELICOPTER) {
		Debug(misc, 3, "[ModAp] Fly: v=({},{},{}), target=({},{},?), dist={}, runway={}", 
			v->x_pos, v->y_pos, v->z_pos, target_x, target_y, dist, runway.base());
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
