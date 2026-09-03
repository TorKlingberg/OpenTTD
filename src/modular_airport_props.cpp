/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file modular_airport_props.cpp Layout-derived properties of a modular airport. */

/*
 * Everything here answers a question about what an airport layout *is* -- which
 * aircraft it accepts, whether it has a runway safe for large ones, its noise,
 * maintenance cost and catchment radius -- as a pure function of the pieces.
 * None of it touches an aircraft, a reservation or the map's reservation bits,
 * which is what keeps it separable from the movement code in
 * modular_airport_cmd.cpp.
 *
 * Each property has two entry points: a *FromPieces overload that measures an
 * abstract layout (a saved template, or one a script proposes, laid out on a
 * ModularPieceGrid) and a Station overload that measures a placed airport and
 * memoises the answer in the mutable cache on Airport. Those caches are
 * invalidated only by Airport::MarkLayoutDirty; see the pitfalls section of
 * CLAUDE.md before adding another one.
 */

#include "stdafx.h"
#include "station_base.h"
#include "station_map.h"
#include "airport.h"
#include "newgrf_airporttiles.h"
#include "timer/timer_game_calendar.h"
#include "modular_airport_cmd.h"
#include "settings_type.h"
#include "table/airporttile_ids.h"
#include "table/strings.h"
#include <map>
#include <span>
#include <utility>
#include <vector>

#include "safeguards.h"

bool IsModularHelipadPiece(ModularAirportPieceID gfx)
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

bool IsModernModularPiece(ModularAirportPieceID piece_type)
{
	switch (piece_type) {
		/* Legacy pieces -- always available */
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
		/* The metadata-only decorations depict modern airport infrastructure -- a
		 * multi-storey car park, a cargo warehouse, a fuel farm -- so they fall
		 * through to the modern branch and share the large airport's start year
		 * rather than being buildable from the first year of the game. */
		default:
			return true;
	}
}

TimerGameCalendar::Year GetModularPieceMinYear(ModularAirportPieceID piece_type)
{
	if (!IsModernModularPiece(piece_type)) return CalendarTime::MIN_YEAR;
	/* The heliport is the first stock airport with a dedicated helicopter pad. */
	if (IsModularHelipadPiece(piece_type)) return AirportSpec::Get(AT_HELIPORT)->min_year;
	return AirportSpec::Get(AT_LARGE)->min_year;
}

/**
 * Whether a piece needs one of the bitmap sprites added to openttd.grf.
 *
 * The metadata-only decorations have sprites of their own. The base sets also
 * lack the two closed-back small-hangar views, so those rotations use bitmaps
 * from openttd.grf. Runtime mirrors of base-set sprites are deliberately not
 * included: they follow the selected base set and remain available regardless
 * of this setting.
 *
 * @param piece_type Piece to check.
 * @param rotation Rotation the piece is placed in.
 * @return True if the piece is only drawn correctly with the new graphics.
 */
bool IsNewAirportGraphicsPiece(ModularAirportPieceID piece_type, uint8_t rotation)
{
	if (IsModularAirportDecorationPiece(piece_type)) return true;
	/* The base sets have only the two open-front small-hangar views. The two
	 * closed-back rotations come from this fork's fallback airport sprites. Use
	 * the renderer's compatibility mapping for old directional piece IDs. */
	if (IsLegacySmallHangarPiece(piece_type)) {
		const uint8_t visual_rotation = GetModularHangarVisualRotation(piece_type, rotation);
		return visual_rotation == 1 || visual_rotation == 2;
	}
	return false;
}

/**
 * Whether pieces backed by this fork's stored airport bitmaps may be built.
 *
 * Only the setting for them, deliberately: the modular airports setting has
 * never gated a command, and folding it in here would refuse those pieces with
 * an error naming the wrong setting. The settings window greys this one out
 * while the builder is off, which is where that dependency belongs.
 *
 * Already-placed pieces keep their graphics either way; this only says what may
 * be built now.
 *
 * @return True if those pieces may be built.
 */
bool AreNewAirportGraphicsAvailable()
{
	return _settings_game.station.new_airport_graphics;
}

/**
 * Why a modular piece may not be built right now.
 *
 * Every gate on placing a piece gathers here, so that the builder's greyed-out
 * buttons, the script API's availability query and the build commands cannot
 * answer the question differently. Rotation is part of the question: it is what
 * separates a view the base set draws from one only this fork's bitmaps do.
 *
 * @param piece_type Piece to check.
 * @param rotation Rotation the piece would be placed in.
 * @return The error explaining the refusal, or STR_NULL if the piece may be built.
 */
StringID GetModularPieceUnavailableReason(ModularAirportPieceID piece_type, uint8_t rotation)
{
	/* Modern pieces are unavailable before their introduction year. */
	if (IsModernModularPiece(piece_type) && TimerGameCalendar::year < GetModularPieceMinYear(piece_type)) {
		return STR_ERROR_MODULAR_PIECE_NOT_YET_AVAILABLE;
	}

	/* Pieces backed by this fork's stored airport bitmaps follow the setting for them. */
	if (IsNewAirportGraphicsPiece(piece_type, rotation) && !AreNewAirportGraphicsAvailable()) {
		return STR_ERROR_NEW_AIRPORT_GRAPHICS_DISABLED;
	}

	return STR_NULL;
}

/**
 * Whether a modular piece may be built right now.
 * @param piece_type Piece to check.
 * @param rotation Rotation the piece would be placed in.
 * @return True if nothing currently refuses it.
 */
bool IsModularPieceBuildable(ModularAirportPieceID piece_type, uint8_t rotation)
{
	return GetModularPieceUnavailableReason(piece_type, rotation) == STR_NULL;
}

static bool IsBigTerminalPiece(ModularAirportPieceID piece_type)
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
 * Scan the layout for a runway safe for large aircraft that permits the given
 * operation. Walks every runway end, so each call is a whole-airport sweep with a
 * runway walk per end; go through ModularAirportHasSafeRunwayFor rather than
 * calling this directly.
 */
static bool ScanModularAirportForSafeRunway(const Station *st, bool landing)
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
 * Check whether a modular airport has at least one runway safe for large aircraft
 * for the specified operation (landing or takeoff).
 *
 * Layout-derived, so cached behind Airport::MarkLayoutDirty. The answer is asked
 * once per candidate runway end and once per holding gate while an aircraft is in
 * the air, which is a per-tick path -- the underlying sweep is far too expensive to
 * repeat there. Both operations are computed together because the sweep costs the
 * same either way and the callers that ask for one usually ask for the other.
 */
bool ModularAirportHasSafeRunwayFor(const Station *st, bool landing)
{
	if (st->airport.modular_large_safe_runway_dirty) {
		st->airport.modular_has_large_safe_landing_runway = ScanModularAirportForSafeRunway(st, true);
		st->airport.modular_has_large_safe_takeoff_runway = ScanModularAirportForSafeRunway(st, false);
		st->airport.modular_large_safe_runway_dirty = false;
	}

	return landing ? st->airport.modular_has_large_safe_landing_runway :
			st->airport.modular_has_large_safe_takeoff_runway;
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
 * exactly -- the elevated jet-overrun crash path depends on the two agreeing, so any
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
 * depend on transient occupancy -- an airport does not stop accepting planes
 * because its only stand happens to be taken.
 *
 * Planes need a runway they can both arrive on and leave from: at least one
 * runway flagged RUF_LANDING and at least one flagged RUF_TAKEOFF, possibly the
 * same one. Both halves are required, because an airport a plane can land on but
 * never take off from strands it -- worse than refusing the order outright.
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

	/* Helicopters take a real helipad, or -- on a layout with none -- the apron or
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

static uint GetModularAirportPieceMaintenancePoints(ModularAirportPieceID piece_type)
{
	/* One rate for every metadata-only decoration; see the build cost. */
	if (IsModularAirportDecorationPiece(piece_type)) return 4;

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

uint GetModularAirportMaintenancePointsFromPieces(std::span<const ModularAirportPieceID> piece_types)
{
	uint points = 0;
	for (ModularAirportPieceID piece_type : piece_types) points += GetModularAirportPieceMaintenancePoints(piece_type);
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

static uint GetModularAirportPieceNoisePoints(ModularAirportPieceID piece_type)
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

uint8_t GetModularAirportNoiseLevelFromPieces(std::span<const ModularAirportPieceID> piece_types)
{
	uint points = 0;
	for (ModularAirportPieceID piece_type : piece_types) points += GetModularAirportPieceNoisePoints(piece_type);
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
static bool IsRadarPiece(ModularAirportPieceID piece_type)
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
 * cumulative -- each one requires everything the lower tiers require:
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
