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

static std::string_view GetModularAirportDebugName(const Station *st);

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
	return HasModularAirportTileReservation(tile) && GetModularAirportTileReservationOwner(tile) == vid;
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

/** Reset all static state in modular airport code; called after loading a save. */
void ResetModularAirportStaticState()
{
	_rate_limit_last_tick.clear();
	_takeoff_fail_state.clear();
}

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
		case APT_APRON_N:
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
 * Check whether a modular airport has at least one runway safe for large aircraft
 * for the specified operation (landing or takeoff).
 */
static bool ModularAirportHasSafeRunwayFor(const Station *st, bool landing)
{
	if (st->airport.modular_tile_data == nullptr) return false;

	for (const ModularAirportTileData &data : *st->airport.modular_tile_data) {
		if (data.piece_type == APT_RUNWAY_END) {
			bool has_flag = landing ? (data.runway_flags & RUF_LANDING) : (data.runway_flags & RUF_TAKEOFF);
			if (has_flag && IsRunwaySafeForLarge(st, data.tile)) return true;
		}
	}

	return false;
}

/**
 * Lookup over pieces laid out on an abstract integer grid, so the layout-derived
 * properties can be measured for a layout that has not been placed yet (a saved
 * template, or one a script proposes). Shared by every *FromPieces entry point so
 * they all agree on what a contiguous runway is.
 */
class ModularPieceGrid {
public:
	explicit ModularPieceGrid(std::span<const ModularCatchmentPiece> pieces)
	{
		for (const ModularCatchmentPiece &p : pieces) this->grid[{p.x, p.y}] = &p;
	}

	const ModularCatchmentPiece *At(int x, int y) const
	{
		auto it = this->grid.find({x, y});
		return it == this->grid.end() ? nullptr : it->second;
	}

	/**
	 * Collect the whole contiguous runway containing \a start, walking back to its
	 * first tile first so any tile of a runway yields the same canonical list.
	 * Mirrors GetContiguousModularRunwayTiles() on the map.
	 */
	bool CollectRunway(const ModularCatchmentPiece &start, std::vector<const ModularCatchmentPiece *> &out) const
	{
		out.clear();
		if (!IsModularRunwayPiece(start.piece_type)) return false;

		const bool horizontal = (start.rotation % 2) == 0;
		const int dx = horizontal ? 1 : 0;
		const int dy = horizontal ? 0 : 1;

		int fx = start.x;
		int fy = start.y;
		while (OnAxis(this->At(fx - dx, fy - dy), horizontal)) {
			fx -= dx;
			fy -= dy;
		}

		int cx = fx;
		int cy = fy;
		while (true) {
			const ModularCatchmentPiece *cur = this->At(cx, cy);
			if (!OnAxis(cur, horizontal)) break;
			out.push_back(cur);
			if (!OnAxis(this->At(cx + dx, cy + dy), horizontal)) break;
			cx += dx;
			cy += dy;
		}
		return !out.empty();
	}

private:
	static bool OnAxis(const ModularCatchmentPiece *p, bool horizontal)
	{
		return p != nullptr && IsModularRunwayPiece(p->piece_type) && (((p->rotation % 2) == 0) == horizontal);
	}

	std::map<std::pair<int, int>, const ModularCatchmentPiece *> grid;
};

/**
 * Get the safety status for large aircraft of a layout on an abstract grid.
 * Returns a bitmask of MISSING requirements. Mirrors GetModularAirportSafetyStatus()
 * exactly — the elevated jet-overrun crash path depends on the two agreeing, so any
 * change here has to be made in both.
 * @param pieces The layout to measure.
 * @return Bitmask of the requirements the layout does not meet.
 */
ModularAirportSafetyRequirement GetModularAirportSafetyStatusFromPieces(std::span<const ModularCatchmentPiece> pieces)
{
	if (pieces.empty()) return MASR_NONE;

	const ModularPieceGrid grid(pieces);

	ModularAirportSafetyRequirement missing = MASR_NONE;
	bool has_tower = false;
	bool has_big_terminal = false;

	for (const ModularCatchmentPiece &p : pieces) {
		if (p.piece_type == APT_TOWER || p.piece_type == APT_TOWER_FENCE_SW) has_tower = true;
		if (IsBigTerminalPiece(p.piece_type)) has_big_terminal = true;
		if (has_tower && has_big_terminal) break;
	}

	std::vector<const ModularCatchmentPiece *> tiles;
	auto has_safe_runway_for = [&](bool landing) {
		for (const ModularCatchmentPiece &p : pieces) {
			if (p.piece_type != APT_RUNWAY_END) continue;
			if ((p.runway_flags & (landing ? RUF_LANDING : RUF_TAKEOFF)) == 0) continue;
			if (!grid.CollectRunway(p, tiles) || tiles.size() < 6) continue;
			if (std::all_of(tiles.begin(), tiles.end(),
					[](const ModularCatchmentPiece *t) { return IsLargeRunwayFamily(t->piece_type); })) {
				return true;
			}
		}
		return false;
	};

	if (!has_tower) missing |= MASR_TOWER;
	if (!has_big_terminal) missing |= MASR_BIG_TERMINAL;
	if (!has_safe_runway_for(true)) missing |= MASR_LANDING_RUNWAY;
	if (!has_safe_runway_for(false)) missing |= MASR_TAKEOFF_RUNWAY;

	return missing;
}

/**
 * Get the safety status of a modular airport for large aircraft.
 * Returns a bitmask of MISSING requirements.
 */
ModularAirportSafetyRequirement GetModularAirportSafetyStatus(const Station *st)
{
	if (st->airport.modular_tile_data == nullptr) return MASR_NONE;

	ModularAirportSafetyRequirement missing = MASR_NONE;
	bool has_tower = false;
	bool has_big_terminal = false;

	for (const ModularAirportTileData &data : *st->airport.modular_tile_data) {
		if (data.piece_type == APT_TOWER || data.piece_type == APT_TOWER_FENCE_SW) has_tower = true;
		if (IsBigTerminalPiece(data.piece_type)) has_big_terminal = true;
		if (has_tower && has_big_terminal) break;
	}

	if (!has_tower) missing |= MASR_TOWER;
	if (!has_big_terminal) missing |= MASR_BIG_TERMINAL;
	if (!ModularAirportHasSafeRunwayFor(st, true)) missing |= MASR_LANDING_RUNWAY;
	if (!ModularAirportHasSafeRunwayFor(st, false)) missing |= MASR_TAKEOFF_RUNWAY;

	return missing;
}

/**
 * Check whether a modular airport has the infrastructure to support large aircraft:
 * a tower, a big terminal, and at least one safe (long, large-family) runway
 * for both landing and takeoff.
 */
bool ModularAirportSupportsLargeAircraft(const Station *st)
{
	return GetModularAirportSafetyStatus(st) == MASR_NONE;
}

/**
 * Recompute what kinds of aircraft this modular airport's layout can take.
 *
 * Deliberately topological: it asks what the player built, not whether a route
 * through it is free right now. CanVehicleUseStation() runs this from order
 * validation and from the build-vehicle list, so it must stay cheap and must not
 * depend on transient occupancy — an airport does not stop accepting planes
 * because its only stand happens to be taken.
 *
 * Planes need a runway they can both arrive on and leave from: at least one
 * runway flagged RUF_LANDING and at least one flagged RUF_TAKEOFF, possibly the
 * same one. Both halves are required, because an airport a plane can land on but
 * never take off from strands it — worse than refusing the order outright.
 * Runway length is not tested here; that is ModularAirportSupportsLargeAircraft's
 * separate question, and a short runway is still a runway for a small plane.
 *
 * Flags are per-tile but CmdSetRunwayFlags propagates them across a whole
 * contiguous runway, so scanning every runway tile answers the same question as
 * walking each runway once, for less work.
 */
static void EnsureModularCapabilityValid(const Station *st)
{
	if (!st->airport.modular_capability_dirty) return;

	std::vector<ModularAirportCapabilityPiece> pieces;
	if (st->airport.modular_tile_data != nullptr) {
		pieces.reserve(st->airport.modular_tile_data->size());
		for (const ModularAirportTileData &data : *st->airport.modular_tile_data) {
			pieces.push_back({data.piece_type, data.runway_flags});
		}
	}
	st->airport.modular_accepts_planes = ModularAirportAcceptsPlanesFromPieces(pieces);

	/* Helicopters take a real helipad, or — on a layout with none — the apron or
	 * runway end the heli-tile machinery picks out for them. */
	EnsureModularHeliTilesValid(st);
	st->airport.modular_accepts_helicopters = ModularAirportHasHelipad(st) ||
			st->airport.modular_heli_landing_tile != INVALID_TILE;

	st->airport.modular_capability_dirty = false;
}

bool ModularAirportAcceptsPlanesFromPieces(std::span<const ModularAirportCapabilityPiece> pieces)
{
	bool has_landing = false;
	bool has_takeoff = false;
	for (const ModularAirportCapabilityPiece &piece : pieces) {
		if (!IsModularRunwayPiece(piece.piece_type)) continue;
		if (piece.runway_flags & RUF_LANDING) has_landing = true;
		if (piece.runway_flags & RUF_TAKEOFF) has_takeoff = true;
		if (has_landing && has_takeoff) return true;
	}
	return false;
}

/**
 * Whether a modular airport's layout can take fixed-wing aircraft.
 * @see EnsureModularCapabilityValid for the definition and why it is topological.
 */
bool ModularAirportAcceptsPlanes(const Station *st)
{
	EnsureModularCapabilityValid(st);
	return st->airport.modular_accepts_planes;
}

/**
 * Whether a modular airport's layout can take helicopters.
 * @see EnsureModularCapabilityValid.
 */
bool ModularAirportAcceptsHelicopters(const Station *st)
{
	EnsureModularCapabilityValid(st);
	return st->airport.modular_accepts_helicopters;
}

TTDPAirportType GetModularAirportNewGRFType(const Station *st)
{
	if (!ModularAirportAcceptsPlanes(st) && ModularAirportHasHelipad(st)) return ATP_TTDP_HELIPORT;
	return ModularAirportSupportsLargeAircraft(st) ? ATP_TTDP_LARGE : ATP_TTDP_SMALL;
}

static uint GetModularAirportPieceMaintenancePoints(uint8_t piece_type)
{
	switch (piece_type) {
		case APT_RUNWAY_1:
		case APT_RUNWAY_2:
		case APT_RUNWAY_3:
		case APT_RUNWAY_4:
		case APT_RUNWAY_5:
		case APT_RUNWAY_END:
		case APT_RUNWAY_SMALL_NEAR_END:
		case APT_RUNWAY_SMALL_MIDDLE:
		case APT_RUNWAY_SMALL_FAR_END:
			return 8;

		case APT_STAND:
		case APT_STAND_1:
		case APT_STAND_PIER_NE:
			return 5;

		case APT_APRON:
		case APT_APRON_FENCE_NW:
		case APT_APRON_FENCE_SW:
		case APT_APRON_W:
		case APT_APRON_S:
		case APT_APRON_VER_CROSSING_S:
		case APT_APRON_HOR_CROSSING_W:
		case APT_APRON_VER_CROSSING_N:
		case APT_APRON_HOR_CROSSING_E:
		case APT_APRON_E:
		case APT_APRON_N:
		case APT_APRON_HOR:
		case APT_APRON_N_FENCE_SW:
		case APT_PIER_NW_NE:
		case APT_PIER:
		case APT_APRON_FENCE_NE:
		case APT_APRON_FENCE_NE_SW:
		case APT_APRON_FENCE_SE_SW:
		case APT_APRON_FENCE_SE:
		case APT_APRON_FENCE_NE_SE:
		case APT_APRON_HALF_EAST:
		case APT_APRON_HALF_WEST:
			return 3;

		case APT_DEPOT_SE:
		case APT_DEPOT_SW:
		case APT_DEPOT_NW:
		case APT_DEPOT_NE:
			return 27;

		case APT_SMALL_DEPOT_SE:
		case APT_SMALL_DEPOT_SW:
		case APT_SMALL_DEPOT_NW:
		case APT_SMALL_DEPOT_NE:
			return 2;

		case APT_HELIPAD_1:
		case APT_HELIPAD_2_FENCE_NW:
		case APT_HELIPAD_2:
		case APT_HELIPAD_2_FENCE_NE_SE:
			return 22;

		case APT_HELIPAD_3_FENCE_SE_SW:
		case APT_HELIPAD_3_FENCE_NW_SW:
		case APT_HELIPAD_3_FENCE_NW:
			return 24;

		case APT_HELIPORT:
			return 32;

		case APT_BUILDING_1:
		case APT_BUILDING_2:
		case APT_BUILDING_3:
		case APT_ROUND_TERMINAL:
			return 9;

		case APT_SMALL_BUILDING_1:
		case APT_SMALL_BUILDING_2:
		case APT_SMALL_BUILDING_3:
			return 2;

		case APT_LOW_BUILDING:
		case APT_LOW_BUILDING_FENCE_N:
		case APT_LOW_BUILDING_FENCE_NW:
		case APT_TOWER:
		case APT_TOWER_FENCE_SW:
		case APT_RADAR_GRASS_FENCE_SW:
		case APT_RADAR_FENCE_SW:
		case APT_RADAR_FENCE_NE:
		case APT_RADIO_TOWER_FENCE_NE:
			return 4;

		case APT_GRASS_FENCE_NE_FLAG_2:
		case APT_EMPTY:
		case APT_EMPTY_FENCE_NE:
		case APT_GRASS_FENCE_SW:
		case APT_GRASS_2:
		case APT_GRASS_1:
		case APT_GRASS_FENCE_NE_FLAG:
			return 0;

		default:
			return 0;
	}
}

uint GetModularAirportMaintenancePointsFromPieces(std::span<const uint8_t> piece_types)
{
	uint points = 0;
	for (uint8_t piece_type : piece_types) points += GetModularAirportPieceMaintenancePoints(piece_type);
	return points;
}

uint GetModularAirportMaintenancePoints(const Station *st)
{
	uint points = 0;
	if (st->airport.modular_tile_data == nullptr) return points;
	for (const ModularAirportTileData &data : *st->airport.modular_tile_data) {
		points += GetModularAirportPieceMaintenancePoints(data.piece_type);
	}
	return points;
}

static uint GetModularAirportPieceNoisePoints(uint8_t piece_type)
{
	if (IsModularRunwayPiece(piece_type)) return 9;

	switch (piece_type) {
		case APT_STAND:
		case APT_STAND_1:
		case APT_STAND_PIER_NE:
			return 3;

		case APT_APRON:
		case APT_APRON_FENCE_NW:
		case APT_APRON_FENCE_SW:
		case APT_APRON_W:
		case APT_APRON_S:
		case APT_APRON_VER_CROSSING_S:
		case APT_APRON_HOR_CROSSING_W:
		case APT_APRON_VER_CROSSING_N:
		case APT_APRON_HOR_CROSSING_E:
		case APT_APRON_E:
		case APT_APRON_N:
		case APT_APRON_HOR:
		case APT_APRON_N_FENCE_SW:
		case APT_PIER_NW_NE:
		case APT_PIER:
		case APT_APRON_FENCE_NE:
		case APT_APRON_FENCE_NE_SW:
		case APT_APRON_FENCE_SE_SW:
		case APT_APRON_FENCE_SE:
		case APT_APRON_FENCE_NE_SE:
		case APT_APRON_HALF_EAST:
		case APT_APRON_HALF_WEST:
			return 1;

		case APT_DEPOT_SE:
		case APT_DEPOT_SW:
		case APT_DEPOT_NW:
		case APT_DEPOT_NE:
		case APT_SMALL_DEPOT_SE:
		case APT_SMALL_DEPOT_SW:
		case APT_SMALL_DEPOT_NW:
		case APT_SMALL_DEPOT_NE:
			return 4;

		case APT_HELIPORT:
		case APT_HELIPAD_1:
		case APT_HELIPAD_2_FENCE_NW:
		case APT_HELIPAD_2:
		case APT_HELIPAD_2_FENCE_NE_SE:
		case APT_HELIPAD_3_FENCE_SE_SW:
		case APT_HELIPAD_3_FENCE_NW_SW:
		case APT_HELIPAD_3_FENCE_NW:
			return 16;

		default:
			return 0;
	}
}

/**
 * Round an accumulated noise total in sixteenth-points to a stored noise level.
 *
 * The clamp is load-bearing, not defensive. Stock noise levels top out at 25, so
 * the uint8_t storage could never overflow; a layout-derived level can, and a
 * silent wrap would make a very large airport read as *quieter* than a small one
 * and sail through the local-authority gate. Shared so the rounding and the clamp
 * cannot drift between the cached and the abstract-layout entry points.
 */
static uint8_t ModularNoisePointsToLevel(uint points)
{
	return static_cast<uint8_t>(std::min<uint>((points + 8) / 16, UINT8_MAX));
}

uint8_t GetModularAirportNoiseLevelFromPieces(std::span<const uint8_t> piece_types)
{
	uint points = 0;
	for (uint8_t piece_type : piece_types) points += GetModularAirportPieceNoisePoints(piece_type);
	return ModularNoisePointsToLevel(points);
}

uint8_t GetModularAirportNoiseLevel(const Station *st)
{
	if (st->airport.modular_noise_dirty) {
		uint points = 0;
		if (st->airport.modular_tile_data != nullptr) {
			for (const ModularAirportTileData &data : *st->airport.modular_tile_data) {
				points += GetModularAirportPieceNoisePoints(data.piece_type);
			}
		}
		st->airport.modular_noise_cache = ModularNoisePointsToLevel(points);
		st->airport.modular_noise_dirty = false;
	}
	return st->airport.modular_noise_cache;
}

/** Radar pieces, counted towards the large-hub catchment tier. */
static bool IsRadarPiece(uint8_t piece_type)
{
	switch (piece_type) {
		case APT_RADAR_GRASS_FENCE_SW:
		case APT_RADAR_FENCE_SW:
		case APT_RADAR_FENCE_NE:
			return true;
		default:
			return false;
	}
}

/**
 * Compute the catchment radius of a modular airport from the infrastructure it
 * actually contains, rather than from a stored airport type. The tiers are
 * cumulative — each one requires everything the lower tiers require:
 *
 *   4  any modular airport (the minimum).
 *   5  safe for large/fast aircraft: a tower, a big terminal and a safe
 *      (long, large-family) runway for both landing and takeoff. This is exactly
 *      the ModularAirportSupportsLargeAircraft() contract, shared here.
 *   6  + two paved runways of at least 6 tiles.
 *   8  + two paved runways of at least 7 tiles, a helipad, a radar and three
 *      big terminal buildings.
 *  10  + four paved runways of at least 8 tiles.
 *
 * "Paved" means every tile of the runway is large-runway family (not the grass
 * small-runway family). Both freeform and stock-template modular airports run
 * through this, so an airport earns catchment by what it is built from.
 *
 * This works on an abstract grid so a saved template can be measured before it
 * is placed; @see GetModularAirportCatchmentRadius for the built-airport entry.
 */
uint GetModularAirportCatchmentRadiusFromPieces(std::span<const ModularCatchmentPiece> pieces)
{
	constexpr uint CATCH_MIN = 4;
	if (pieces.empty()) return CATCH_MIN;

	const ModularPieceGrid grid(pieces);

	auto is_paved = [](const std::vector<const ModularCatchmentPiece *> &tiles) {
		return std::all_of(tiles.begin(), tiles.end(), [](const ModularCatchmentPiece *p) { return IsLargeRunwayFamily(p->piece_type); });
	};
	auto collect_runway = [&grid](const ModularCatchmentPiece &start, std::vector<const ModularCatchmentPiece *> &out) {
		return grid.CollectRunway(start, out);
	};

	/* Count the pieces the tiers care about. */
	uint helipads = 0;
	uint radars = 0;
	uint big_terminals = 0;
	for (const ModularCatchmentPiece &p : pieces) {
		if (IsModularHelipadPiece(p.piece_type)) helipads++;
		if (IsRadarPiece(p.piece_type)) radars++;
		if (IsBigTerminalPiece(p.piece_type)) big_terminals++;
	}

	std::vector<const ModularCatchmentPiece *> tiles;

	/* Tier 5: large-aircraft safe (tower + big terminal + safe landing & takeoff
	 * runway). Shared with the built-airport path so the two cannot drift. */
	if (GetModularAirportSafetyStatusFromPieces(pieces) != MASR_NONE) return CATCH_MIN;

	/* Gather the tile lengths of each distinct fully-paved runway. */
	std::vector<size_t> paved_runways;
	for (const ModularCatchmentPiece &p : pieces) {
		if (!IsModularRunwayPiece(p.piece_type)) continue;
		if (!collect_runway(p, tiles) || tiles.empty()) continue;
		/* collect_runway normalises to the same first tile from any tile of the
		 * runway; count each runway exactly once at its canonical start. */
		if (tiles.front() != &p) continue;
		if (is_paved(tiles)) paved_runways.push_back(tiles.size());
	}

	auto paved_at_least = [&paved_runways](size_t len) {
		uint n = 0;
		for (size_t l : paved_runways) if (l >= len) n++;
		return n;
	};

	/* Tier 6: two paved runways of >= 6 tiles. */
	if (paved_at_least(6) < 2) return 5;

	/* Tier 8: two paved >= 7 tiles, plus a helipad, a radar and three big terminals. */
	if (paved_at_least(7) >= 2 && helipads >= 1 && radars >= 1 && big_terminals >= 3) {
		/* Tier 10: four paved runways of >= 8 tiles. */
		if (paved_at_least(8) >= 4) return 10;
		return 8;
	}
	return 6;
}

/**
 * Catchment radius of a modular airport, cached until its tiles or runway flags
 * change (see modular_catchment_dirty). @see GetModularAirportCatchmentRadiusFromPieces.
 */
uint GetModularAirportCatchmentRadius(const Station *st)
{
	if (st->airport.modular_catchment_dirty) {
		std::vector<ModularCatchmentPiece> pieces;
		if (st->airport.modular_tile_data != nullptr) {
			pieces.reserve(st->airport.modular_tile_data->size());
			for (const ModularAirportTileData &data : *st->airport.modular_tile_data) {
				pieces.push_back({static_cast<int>(TileX(data.tile)), static_cast<int>(TileY(data.tile)), data.piece_type, data.rotation, data.runway_flags});
			}
		}
		st->airport.modular_catchment_cache = static_cast<uint8_t>(GetModularAirportCatchmentRadiusFromPieces(pieces));
		st->airport.modular_catchment_dirty = false;
	}
	return st->airport.modular_catchment_cache;
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
		 * HelicopterDirectDescent matters most — CmdStartStopVehicle reads it as a
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
 * This test is deliberately goal-independent. The aircraft's own goal is also a
 * legal place to stop, but that is a property of the aircraft, not of the tile —
 * folding it in here once hid it behind a piece-type check that could skip it.
 * Callers test the goal explicitly, and first. */
bool IsModularSafeStopTile(const Station *st, TileIndex tile)
{
	const ModularAirportTileData *td = st->airport.GetModularTileData(tile);
	if (td == nullptr) return false;
	if (IsServiceStyleGroundPiece(td->piece_type)) return true;
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
	OK,
	RESOURCE_ERROR,
	NO_SAFE_STOP,
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
		return ForwardPlanStatus::RESOURCE_ERROR;
	}

	std::vector<TileIndex> operation_resource_tiles;
	if (IsValidTile(operation_runway)) {
		if (!GetContiguousModularRunwayTiles(st, operation_runway, operation_resource_tiles) ||
				operation_resource_tiles.empty()) {
			return ForwardPlanStatus::RESOURCE_ERROR;
		}
		SortAndUniqueTiles(operation_resource_tiles);
	}

	for (uint16_t i = start_index; i < path->tiles.size(); ++i) {
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
			return ForwardPlanStatus::OK;
		}

		/* Do not let the aircraft's present queue/stand tile terminate a departure
		 * plan without advancing. Every later safe stop is a valid horizon. */
		if (i > start_index && IsModularSafeStopTile(st, tile)) {
			out.safe_stop = tile;
			SortAndUniqueTiles(out.taxi_tiles);
			return ForwardPlanStatus::OK;
		}
	}

	return ForwardPlanStatus::NO_SAFE_STOP;
}

static bool TryCommitForwardReservationPlan(Aircraft *v, const Station *st,
		const ForwardReservationPlan &plan, TaxiReserveResult *out, bool log_success);

void BuildReservationKeepSet(const Aircraft *v, const Station *st, std::vector<TileIndex> &keep_set)
{
	keep_set.clear();
	if (v == nullptr || st == nullptr || st->airport.modular_tile_data == nullptr) return;

	if (IsValidTile(v->tile) && st->TileBelongsToAirport(v->tile)) keep_set.push_back(v->tile);

	/* An aircraft standing somewhere it may not wait keeps everything it holds.
	 *
	 * Retention is otherwise justified by a path — the active taxi_path or the stored
	 * landing_chain_path. A landing committed through the no-ground-goal branch has
	 * neither: it reserves a runway plus a one-way buffer to queue on and deliberately
	 * resets the path. Nothing then justified the buffer, so the very next reconcile
	 * released it, and the aircraft arrived at the rollout end owning nothing — on a
	 * runway, with the guarantee that permitted the landing already thrown away.
	 *
	 * Landing is only allowed against a reserved route to a safe stop, so until the
	 * aircraft is actually standing on one, that route is what makes its position
	 * legal and it is never ours to reclaim. Normal reconciliation resumes the moment
	 * it reaches a safe stop. */
	if (IsValidTile(v->tile) && st->TileBelongsToAirport(v->tile) && IsPathTileRunwayPiece(st, v->tile)) {
		for (TileIndex tile : v->taxi_reserved_tiles) {
			if (IsValidTile(tile) && IsModularSafeStopTile(st, tile)) keep_set.push_back(tile);
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
				v->ground_path_goal, operation_runway, plan) == ForwardPlanStatus::OK) {
			keep_set.insert(keep_set.end(), plan.taxi_tiles.begin(), plan.taxi_tiles.end());
			keep_set.insert(keep_set.end(), plan.operation_runway_tiles.begin(), plan.operation_runway_tiles.end());
		}
	}

	if (ShouldRetainRunwayReservation(v, st)) {
		for (TileIndex tile : v->modular_runway_reservation) keep_set.push_back(tile);
	}

	if (v->landing_chain_path != nullptr && v->landing_chain_path->valid) {
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
	 * which map bits it owns — every setter (SetTaxiReservation,
	 * TryReserveContiguousModularRunway, TryCommitForwardReservationPlan) records
	 * the tile here — so we walk those vectors instead of scanning the whole
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
	TileIndex rollout = touchdown_on_runway ? FindModularRunwayRolloutPoint(st, runway_tile) : INVALID_TILE;
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

	TaxiPath path = BuildTaxiPath(st, chain_origin, path_goal, nullptr);
	if (!path.valid || path.tiles.empty()) return log_chain_fail("path_invalid");

	ForwardReservationPlan plan;
	const TileIndex operation_runway = touchdown_on_runway ? runway_tile : INVALID_TILE;
	const ForwardPlanStatus plan_status = BuildForwardReservationPlan(v, st, &path, 0,
			path_goal, operation_runway, plan);
	if (plan_status == ForwardPlanStatus::RESOURCE_ERROR) return log_chain_fail("resource_error", operation_runway);
	if (plan_status == ForwardPlanStatus::NO_SAFE_STOP) return log_chain_fail("no_safe_stop", path_goal);

	if (!IsValidTile(ground_goal) && !IsOneWayTaxiTile(st, plan.safe_stop)) {
		return log_chain_fail("no_goal_no_one_way_buffer", plan.safe_stop);
	}

	TaxiReserveResult reserve_result;
	if (!TryCommitForwardReservationPlan(v, st, plan, &reserve_result, false)) {
		return log_chain_fail(TaxiReserveFailureName(reserve_result.reason), reserve_result.tile);
	}

	if (IsValidTile(ground_goal)) {
		v->landing_chain_path = std::make_unique<TaxiPath>(std::move(path));
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
	const bool is_large_plane = !is_heli && (AircraftVehInfo(v->engine_type)->subtype & AIR_FAST) != 0;

	/* A helicopter heading for a hangar must touch down where it can taxi to one.
	 * Where no helipad on this airport can reach one — a rooftop heliport has no
	 * taxiable neighbour at all — landing on a pad strands it: it reaches neither
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
		 * point means the service-tile search came up empty as well — nothing anywhere on
		 * this airport reaches a hangar — and filtering would then reject every pad and
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
	TileIndex best_small_runway = INVALID_TILE;
	int best_small_runway_score = INT_MAX;
	/* Whether a large-safe runway end serves the required landing direction (ignoring
	 * transient occupancy). When true, a large aircraft never falls back to a short
	 * runway — if every good runway is momentarily busy it keeps holding instead. */
	bool good_runway_exists_for_direction = false;

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

			/* This end is a directionally-valid landing target. Record whether a
			 * large-safe runway exists for this direction before any occupancy checks,
			 * so a busy good runway still suppresses the short-runway fallback.
			 * Only large planes care, so skip the runway walk for everything else. */
			const bool large_safe = is_large_plane && IsRunwaySafeForLarge(st, data.tile);
			if (large_safe) good_runway_exists_for_direction = true;

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

			/* Large aircraft (AIR_FAST) require a long, large-family runway.
			 * A short runway is only used as a last resort when no good runway exists
			 * for this direction at all (handled after the loop). This holds even with
			 * the no-jetcrash cheat on. */
			if (is_large_plane && !large_safe) {
				/* Track best small-runway fallback for large aircraft. */
				int fx = TileX(data.tile) * TILE_SIZE + TILE_SIZE / 2;
				int fy = TileY(data.tile) * TILE_SIZE + TILE_SIZE / 2;
				int fscore = abs(fx - v->x_pos) + abs(fy - v->y_pos);
				if (best_small_runway == INVALID_TILE || fscore < best_small_runway_score) {
					best_small_runway_score = fscore;
					best_small_runway = data.tile;
				}
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
			TileIndex rollout = FindModularRunwayRolloutPoint(st, data.tile);
			TileIndex term_tile = FindFreeModularTerminal(st, v, rollout);
			if (term_tile != INVALID_TILE) {
				TileIndex other_end = GetRunwayOtherEnd(st, data.tile);
				int end_x = TileX(other_end) * TILE_SIZE;
				int end_y = TileY(other_end) * TILE_SIZE;
				int tx = TileX(term_tile) * TILE_SIZE;
				int ty = TileY(term_tile) * TILE_SIZE;
				int dist_taxi = abs(end_x - tx) + abs(end_y - ty);
				score += dist_taxi * 4;
			}
		} else {
			/* Helipads: prefer ones near a stand (cheap euclidean — helicopters don't taxi,
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

	/* Large aircraft fall back to a small runway only when NO large-safe runway exists
	 * for this direction. If a good runway exists but is currently busy, best_tile stays
	 * INVALID and the aircraft keeps holding until one frees. */
	if (best_tile == INVALID_TILE && is_large_plane && !good_runway_exists_for_direction &&
			best_small_runway != INVALID_TILE) {
		Debug(misc, 2, "[ModAp] V{} landing-small-runway: no large-safe runway for direction, using small runway {}", v->index, best_small_runway.base());
		best_tile = best_small_runway;
	}

	if (best_tile == INVALID_TILE && !is_heli && ShouldLogModularRateLimited(v->index, 18, 128)) {
		Debug(misc, 2,
			"[ModAp] Vehicle {} no landing runway: runway_tiles={} reject_not_end={} reject_mode={} reject_dir={} reject_reserved={} reject_takeoff_queue={}",
			v->index, candidates_total, rejected_not_end, rejected_mode, rejected_direction, rejected_reserved, rejected_takeoff_queue);
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

	/* landing_chain_path is not saved. If an aircraft already committed to modular
	 * landing has no active saved reservations, reclaim the landing chain before
	 * continuing descent; otherwise a second aircraft can choose the same
	 * helipad/touchdown tile. */
	if (v->taxi_reserved_tiles.empty() && v->modular_runway_reservation.empty() && v->landing_chain_path == nullptr) {
		/* Helicopters require a concrete ground goal to land (they'd otherwise circle
		 * forever — see aircraft_cmd.cpp commit path that rejects helicopter landing
		 * when goal is INVALID_TILE). Re-derive the goal here when the saved value
		 * was lost. Fixed-wing INVALID_TILE is preserved as-is: it represents a
		 * deliberate "queue on a one-way buffer" landing handled by TryReserveLandingChain. */
		if (v->subtype == AIR_HELICOPTER && v->modular_landing_goal == INVALID_TILE) {
			TileIndex rollout = FindModularRunwayRolloutPoint(st, v->modular_landing_tile);
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
		 * while parked refuses every manual start and stop, and fails autoreplace — which
		 * stops and restarts the vehicle around the swap — with "Aircraft is in flight".
		 * Its orders still run, which is what makes the symptom so confusing. Stock clears
		 * the flag when the rotors reach full speed in AircraftController's HeliLower;
		 * touchdown is the modular equivalent moment, and modular landing has no other
		 * exit that leaves the aircraft on the ground. */
		v->flags.Reset(VehicleAirFlag::HelicopterDirectDescent);

		v->modular_landing_tile = INVALID_TILE;

		AircraftEventHandler_Landing(v, st->airport.GetFTA());

		if (v->subtype == AIR_HELICOPTER && v->modular_landing_goal != INVALID_TILE) {
			/* Helicopters never do runway rollout — go straight to the
			 * pre-selected ground destination regardless of landing surface. */
			v->ground_path_goal = v->tile;
			v->modular_ground_target = MGT_ROLLOUT;
			HandleModularGroundArrival(v);
		} else if (v->subtype == AIR_HELICOPTER) {
			Debug(misc, 1, "[ModAp] V{} helicopter touchdown without ground goal — should not happen", v->index);
			AircraftEventHandler_EndLanding(v, st->airport.GetFTA());
		} else {
			TileIndex rollout_point = FindModularRunwayRolloutPoint(st, v->tile);
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

	/* Reached altitude, transition to flying — force-clear all reservations. */
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

	/* Compute runway length in tiles for liftoff point calculation. */
	std::vector<TileIndex> takeoff_runway_tiles;
	int runway_length_tiles = 1;
	if (GetContiguousModularRunwayTiles(st, v->modular_takeoff_tile, takeoff_runway_tiles)) {
		runway_length_tiles = std::max(1, (int)takeoff_runway_tiles.size());
	}
	/* Liftoff after 2/3 of runway length (in sub-tile progress units). */
	int liftoff_progress = runway_length_tiles * TILE_SIZE * 2 / 3;

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

	/* Return the nearest safe-stop tile along the path (one-way taxiway queue tile
	 * or a stand/hangar/helipad) that is currently clear. An aircraft must never
	 * stop on a free-move apron/grass tile: that pins a shared transit section. If
	 * no safe stop is reachable and clear, return INVALID and let the caller hold. */
	for (TileIndex tile : path.tiles) {
		if (tile == start_tile) continue;
		if (!IsModularSafeStopTile(st, tile)) continue;
		Tile t(tile);
		if (!IsAirportTile(t)) continue;
		if (HasModularAirportTileReservation(tile) && GetModularAirportTileReservationOwner(tile) != v->index) continue;
		if (IsModularTileOccupiedByOtherAircraft(st, tile, v->index)) continue;
		return tile;
	}

	return INVALID_TILE;
}

bool IsModularTileOccupiedByOtherAircraft(const Station *st, TileIndex tile, VehicleID self)
{
	/* Hangars can hold multiple aircraft; never treat them as occupied. */
	if (IsModularHangarTile(st, tile)) return false;
	if (!st->TileBelongsToAirport(tile)) return false;

	return HasVehicleOnTile(tile, [self](const Vehicle *v) {
		if (v->type != VEH_AIRCRAFT) return false;
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
	 * Where helipads exist but are momentarily taken, the helicopter waits — circling
	 * if airborne — rather than occupying a stand a fixed-wing aircraft needs.
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
				AirportGroundPath path = FindAirportGroundPath(st, origin, data.tile, nullptr);
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
				AirportGroundPath path = FindAirportGroundPath(st, origin, data.tile, nullptr);
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

		AirportGroundPath path = FindAirportGroundPath(st, origin, data.tile, nullptr);
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
		 * stand and the caller would fall through to stacking — the exact outcome
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
	if (veh == nullptr || veh->type != VEH_AIRCRAFT) return true;
	return !Aircraft::From(veh)->IsNormalAircraft();
}

bool TryClearStaleModularReservation(const Station *st, TileIndex tile, VehicleID reserver)
{
	if (st == nullptr || !IsValidTile(tile)) return false;
	Tile t(tile);
	if (!IsAirportTile(t)) return false;
	if (!IsModularAirportTileReservedBy(tile, reserver)) return false;

	Vehicle *veh = Vehicle::GetIfValid(reserver);
	if (veh == nullptr || veh->type != VEH_AIRCRAFT) {
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

/** Why a runway-end tile is or isn't a usable takeoff end (occupancy/safety aside). */
enum class ModularTakeoffEndStatus : uint8_t {
	OK,         ///< Usable: a real end, long enough, takeoff-flagged, direction matches.
	NOT_END,    ///< Not a runway-end piece.
	TOO_SHORT,  ///< Runway shorter than MIN_RUNWAY_LENGTH_TILES.
	NO_TAKEOFF, ///< RUF_TAKEOFF not set.
	WRONG_DIR,  ///< Direction bits do not permit a takeoff roll toward the far end.
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
	if (!IsModularRunwayEndPiece(piece_type)) return ModularTakeoffEndStatus::NOT_END;

	std::vector<TileIndex> rwy;
	if (!GetContiguousModularRunwayTiles(st, tile, rwy) || (int)rwy.size() < MIN_RUNWAY_LENGTH_TILES) return ModularTakeoffEndStatus::TOO_SHORT;

	const uint8_t flags = GetRunwayFlags(st, tile);
	if ((flags & RUF_TAKEOFF) == 0) return ModularTakeoffEndStatus::NO_TAKEOFF;

	const bool is_low = IsRunwayEndLow(st, tile);
	if (is_low && (flags & RUF_DIR_HIGH) == 0) return ModularTakeoffEndStatus::WRONG_DIR;
	if (!is_low && (flags & RUF_DIR_LOW) == 0) return ModularTakeoffEndStatus::WRONG_DIR;
	return ModularTakeoffEndStatus::OK;
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
	const auto tile_blocked = [&](TileIndex tile) -> bool {
		Tile t(tile);
		if (IsAirportTile(t) && HasModularAirportTileReservation(tile) && GetModularAirportTileReservationOwner(tile) != v->index) return true;
		if (tile != v->tile && IsModularTileOccupiedByOtherAircraft(st, tile, v->index)) return true;
		return false;
	};
	const auto path_enterable = [&](const TaxiPath &taxi_path, TileIndex takeoff_runway) -> bool {
		if (!taxi_path.valid || taxi_path.tiles.size() < 2 || taxi_path.segments.empty()) return false;

		ForwardReservationPlan plan;
		if (BuildForwardReservationPlan(v, st, &taxi_path, 0, takeoff_runway,
				takeoff_runway, plan) != ForwardPlanStatus::OK) {
			return false;
		}

		if (!plan.operation_runway_tiles.empty()) {
			VehicleID state_blocker = VehicleID::Invalid();
			if (IsContiguousModularRunwayReservedInStateByOther(v, st,
					plan.operation_runway_tiles, &state_blocker)) {
				return false;
			}
			for (TileIndex tile : plan.operation_runway_tiles) {
				if (tile_blocked(tile)) return false;
			}
		}

		for (TileIndex tile : plan.taxi_tiles) {
			if (IsModularHangarTile(st, tile)) continue;
			if (IsPathTileRunwayPiece(st, tile)) {
				std::vector<TileIndex> runway;
				VehicleID state_blocker = VehicleID::Invalid();
				if (!GetContiguousModularRunwayTiles(st, tile, runway) || runway.empty() ||
						IsContiguousModularRunwayReservedInStateByOther(v, st, runway, &state_blocker)) {
					return false;
				}
			}
			if (tile_blocked(tile)) return false;
		}
		return true;
	};

	/* Strict large-runway preference: a large aircraft only uses a short runway when NO
	 * large-safe runway end serves its takeoff direction. Determine this up front,
	 * ignoring transient occupancy — if a good runway is merely busy, the aircraft waits
	 * (returns a reachable-but-blocked end) rather than downgrading to a short runway. */
	bool good_takeoff_runway_exists = false;
	if (large_takeoff_required) {
		for (const ModularAirportTileData &data : *st->airport.modular_tile_data) {
			if (ClassifyModularTakeoffEnd(st, data.tile, data.piece_type) != ModularTakeoffEndStatus::OK) continue;
			if (IsRunwaySafeForLarge(st, data.tile)) { good_takeoff_runway_exists = true; break; }
		}
	}

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
			if (status != ModularTakeoffEndStatus::OK) {
				if (status == ModularTakeoffEndStatus::WRONG_DIR && v != nullptr && pass == 0 && ShouldLogModularRateLimited(v->index, 40, 256)) {
					Debug(misc, 2, "[ModAp] V{} takeoff-skip dir: tile={} is_low={} flags={}", v->index, data.tile.base(), IsRunwayEndLow(st, data.tile), GetRunwayFlags(st, data.tile));
				}
				continue;
			}

			/* When a large-safe runway exists for this direction, large aircraft must use
			 * it — short runways are skipped entirely. Only when no good runway exists at
			 * all do we allow a best-effort short-runway takeoff. */
			if (large_takeoff_required && good_takeoff_runway_exists && !IsRunwaySafeForLarge(st, data.tile)) {
				if (pass == 0 && ShouldLogModularRateLimited(v->index, 42, 256)) {
					Debug(misc, 2, "[ModAp] V{} takeoff-skip short (good runway exists): tile={}", v->index, data.tile.base());
				}
				continue;
			}

			/* Prefer reachable takeoff ends. */
			if (!can_ground_route) continue;
			TaxiPath taxi_path = BuildTaxiPath(st, v->tile, data.tile, v, allow_crossing);
			if (!taxi_path.valid) {
				if (v != nullptr && pass == 0 && ShouldLogModularRateLimited(v->index, 35, 128)) {
					Debug(misc, 2, "[ModAp] V{} takeoff-path invalid: from={} to={}", v->index, v->tile.base(), data.tile.base());
				}
				continue;
			}
			if (!path_enterable(taxi_path, data.tile)) {
				if (v != nullptr && pass == 0 && ShouldLogModularRateLimited(v->index, 36, 128)) {
					/* Determine why path is not enterable for easier debugging. */
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
				/* Track as "reachable but blocked" — prefer over unreachable Manhattan fallback. */
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
		/* Topologically reachable but temporarily blocked — aircraft will wait for traffic to clear. */
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

	/* Runway already selected — allow crossing if needed. */
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
			 * Tiles not on the path were reserved by landing chain or similar — preserve them. */
			if (v->taxi_path != nullptr) {
				bool on_path = std::find(v->taxi_path->tiles.begin(), v->taxi_path->tiles.end(), tile) != v->taxi_path->tiles.end();
				if (!on_path) {
					preserved.push_back(tile);
					continue;
				}
			} else {
				/* No path — preserve all non-runway reservations (landing chain tiles survive touchdown). */
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
	/* Hangars are multi-capacity — never treat as reserved. */
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
	/* Hangars are multi-capacity — never set map-level reservation bits.
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

/** Validate and commit one forward plan without partial acquisition. */
static bool TryCommitForwardReservationPlan(Aircraft *v, const Station *st,
		const ForwardReservationPlan &plan, TaxiReserveResult *out, bool log_success)
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
			return fail(TaxiReserveFailure::RUNWAY_BUSY, plan.operation_runway_tiles.front(), state_blocker);
		}
		for (TileIndex tile : plan.operation_runway_tiles) {
			if (IsModularTileOccupiedByOtherAircraft(st, tile, v->index)) {
				return fail(TaxiReserveFailure::RUNWAY_BUSY, tile);
			}
			if (IsTaxiTileReservedByOther(st, tile, v->index)) {
				return fail(TaxiReserveFailure::RUNWAY_BUSY, tile,
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
				return fail(TaxiReserveFailure::RUNWAY_RESOURCE_ERROR, tile);
			}
			VehicleID state_blocker = VehicleID::Invalid();
			if (IsContiguousModularRunwayReservedInStateByOther(v, st, runway, &state_blocker)) {
				return fail(TaxiReserveFailure::RUNWAY_BUSY, tile, state_blocker);
			}
		}
		if (IsTaxiTileReservedByOther(st, tile, v->index)) {
			return fail(TaxiReserveFailure::RESERVED_BY_OTHER, tile,
					HasModularAirportTileReservation(tile) ? GetModularAirportTileReservationOwner(tile) : VehicleID::Invalid());
		}
		if (tile != v->tile && IsModularTileOccupiedByOtherAircraft(st, tile, v->index)) {
			return fail(TaxiReserveFailure::OCCUPIED_BY_OTHER, tile);
		}
	}

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
		case TaxiReserveFailure::NONE: return "none";
		case TaxiReserveFailure::NO_PATH: return "no_path";
		case TaxiReserveFailure::RESERVED_BY_OTHER: return "reserved_by_other";
		case TaxiReserveFailure::OCCUPIED_BY_OTHER: return "occupied_by_other";
		case TaxiReserveFailure::RUNWAY_BUSY: return "runway_busy";
		case TaxiReserveFailure::RUNWAY_RESOURCE_ERROR: return "runway_resource_error";
		case TaxiReserveFailure::NO_SAFE_STOP: return "no_safe_stop";
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
		return fail(TaxiReserveFailure::NO_PATH);
	}

	ForwardReservationPlan plan;
	const TileIndex operation_runway = GetGroundOperationRunwayTile(v, st);
	switch (BuildForwardReservationPlan(v, st, v->taxi_path.get(), v->taxi_path_index,
			v->ground_path_goal, operation_runway, plan)) {
		case ForwardPlanStatus::OK:
			break;
		case ForwardPlanStatus::RESOURCE_ERROR:
			return fail(TaxiReserveFailure::RUNWAY_RESOURCE_ERROR,
					IsValidTile(operation_runway) ? operation_runway : v->tile);
		case ForwardPlanStatus::NO_SAFE_STOP:
			return fail(TaxiReserveFailure::NO_SAFE_STOP, v->ground_path_goal);
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
		case MGT_ROLLOUT:
			alt_goal = FindModularLandingGroundGoal(st, v, &alt_target);
			break;
		case MGT_HELI_TAKEOFF_TILE:
			EnsureModularHeliTilesValid(st);
			alt_goal = st->airport.modular_heli_takeoff_tile;
			alt_target = MGT_HELI_TAKEOFF_TILE;
			if (alt_goal == INVALID_TILE) {
				/* The computed takeoff tile is gone — the airport gained a helipad (which
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
					} else if (!IsModularSafeStopTile(st, v->tile)) {
						/* The precomputed path could not be installed and the aircraft is
						 * standing where it may not wait — in practice the rollout end, on the
						 * runway. That is only a contract violation if it also no longer owns
						 * a reserved route to somewhere it *can* wait. A missing path object
						 * is not itself the test: the no-ground-goal landing branch reserves
						 * a runway plus a one-way buffer and resets the path deliberately, and
						 * that aircraft is perfectly safe — it owns its queueing tile. */
						const bool owns_safe_stop = std::any_of(v->taxi_reserved_tiles.begin(), v->taxi_reserved_tiles.end(),
							[&](TileIndex t) { return IsValidTile(t) && (t == goal || IsModularSafeStopTile(st, t)); });
						if (!owns_safe_stop) {
							Debug(misc, 1,
								"[ModAp] V{} unit#{} landing-chain-invariant: off a safe stop with no reserved route to one tile={} goal={} owned={}",
								v->index, v->unitnumber,
								IsValidTile(v->tile) ? v->tile.base() : 0,
								IsValidTile(goal) ? goal.base() : 0,
								v->taxi_reserved_tiles.size());
						}
					}
				} else if (!IsModularSafeStopTile(st, v->tile)) {
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
				/* else: already parked on a safe stop with no service tile free — stay
				 * idle in MGT_ROLLOUT and let the keepalive re-poll for a stand. */

				/* Discard any remaining landing chain path — either installed above or no longer needed. */
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
			v->modular_takeoff_tile = v->tile;
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
				AirportGroundPath path = FindAirportGroundPath(st, v->tile, data.tile, nullptr, false, false);
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
	 * never brakes, so — like stock — there is no takeoff crash.) */
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
		/* Allow runway crossing when already committed to a goal — the two-pass
		 * selection in FindModularRunwayTileForTakeoff ensures crossing goals
		 * are only assigned when no strict path exists. */
		TaxiPath new_path = BuildTaxiPath(st, v->tile, v->ground_path_goal, v, true);
		if (!new_path.valid || new_path.tiles.size() < 2 || new_path.segments.empty()) {
			v->taxi_wait_counter++;
			if (_debug_misc_level >= 1 && v->taxi_wait_counter >= 128 && (v->taxi_wait_counter % 128) == 0) {
				/* Diagnostic A* only when someone is listening — gate on debug level. */
				AirportGroundPath dbg_path = FindAirportGroundPath(st, v->tile, v->ground_path_goal, v);
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
			return false;
		}

		v->taxi_path = std::make_unique<TaxiPath>(std::move(new_path));
		v->taxi_path_index = 0;
		v->taxi_current_segment = FindTaxiSegmentIndex(v->taxi_path.get(), 0);
		v->taxi_wait_counter = 0;
		SetTaxiReservationUnlessOperationRunway(v, v->tile);
	}

	if (v->taxi_path == nullptr || v->taxi_path_index + 1 >= v->taxi_path->tiles.size()) {
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
			 * like. A segment claims more than one tile — the full path horizon to the
			 * next safe stop — so "next is free" and "the claim failed" are routinely
			 * both true, and re-deriving blockers from `next` used
			 * to print all-clear for a genuinely blocked aircraft. */
			Debug(misc, 1,
				"[ModAp] V{} unit#{} stuck(reserve) wait={} state={} tile={} next={} seg={} goal={} tgt={} deny={} deny_tile={} deny_by=V{}",
				v->index, v->unitnumber, v->taxi_wait_counter, v->state,
				IsValidTile(v->tile) ? v->tile.base() : 0,
				IsValidTile(next_tile) ? next_tile.base() : 0,
				static_cast<uint8_t>(next_type),
				IsValidTile(v->ground_path_goal) ? v->ground_path_goal.base() : 0,
				v->modular_ground_target,
				TaxiReserveFailureName(reserve_result.reason),
				IsValidTile(reserve_result.tile) ? reserve_result.tile.base() : 0,
				reserve_result.blocker == VehicleID::Invalid() ? 0 : reserve_result.blocker.base());

			/* Waiting on a tile the aircraft may not hold. The entry contracts are built so
			 * this cannot be reached by taxiing into it — only by already being there when
			 * the route was lost, i.e. after a landing rollout. Distinct from ordinary
			 * contention: the aircraft is pinning a shared resource while it waits. */
			if (!IsModularSafeStopTile(st, v->tile) && IsPathTileRunwayPiece(st, v->tile)) {
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

	const TaxiSegmentType old_type = (old_segment < v->taxi_path->segments.size()) ? v->taxi_path->segments[old_segment].type : TaxiSegmentType::FREE_MOVE;
	const bool runway_exit_transition = (old_type == TaxiSegmentType::RUNWAY && next_type != TaxiSegmentType::RUNWAY);
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
