/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file modular_airport_cmd.h Declarations for modular airport movement and reservation logic. */

#ifndef MODULAR_AIRPORT_CMD_H
#define MODULAR_AIRPORT_CMD_H

#include "aircraft.h"
#include "station_base.h"
#include "airport_ground_pathfinder.h"
#include "airport.h"
#include "table/airporttile_ids.h"

#include "core/enum_type.hpp"

#include <span>
#include <string_view>
#include <cstdint>
#include <cstddef>
#include <vector>

inline constexpr uint8_t MGT_NONE = 0;
inline constexpr uint8_t MGT_TERMINAL = 1;
inline constexpr uint8_t MGT_HELIPAD = 2;
inline constexpr uint8_t MGT_HANGAR = 3;
inline constexpr uint8_t MGT_RUNWAY_TAKEOFF = 4;
inline constexpr uint8_t MGT_ROLLOUT = 5;
inline constexpr uint8_t MGT_HELI_TAKEOFF_TILE = 6;

inline constexpr int MIN_RUNWAY_LENGTH_TILES = 4; ///< Runways shorter than this are not usable for landing or takeoff

int UpdateAircraftSpeed(Aircraft *v, uint speed_limit = UINT16_MAX, bool hard_limit = true);
void AircraftEntersTerminal(Aircraft *v);
void MaybeServiceAircraftAtHelipad(Aircraft *v, bool at_helipad);
void PlayAircraftSound(const Vehicle *v);
Direction GetModularHangarExitDirection(const Station *st, TileIndex tile);
Direction GetModularAircraftParkedDirection(const Station *st, TileIndex tile);
/** Turn one step toward the parked direction; return true when no further turn is needed. */
bool UpdateModularAircraftParkedDirection(Aircraft *v, const Station *st);
void AircraftEventHandler_Landing(Aircraft *v, const AirportFTAClass *apc);
void AircraftEventHandler_EndLanding(Aircraft *v, const AirportFTAClass *apc);

/**
 * Whether this aircraft is heading for a hangar at @p st rather than a parking spot: it
 * either holds a depot order or has become due for automatic servicing, *and* the airport
 * actually has a hangar to head for.
 *
 * Every modular goal-selection site must answer this the same way. They run at different
 * moments of a single arrival -- landing-target choice while still airborne, ground goal at
 * landing commit, and again once rollout finishes -- so a disagreement between them surfaces
 * as an aircraft that lands somewhere it then refuses to taxi off.
 *
 * The hangar test is what keeps a serviceable-looking arrival from becoming an infinite
 * loop. Wanting a hangar suppresses helipad and stand selection, so at an airport with no
 * hangar the aircraft lands, finds nothing it is willing to park on, leaves by the
 * departure ladder, and picks the same airport again on the next approach -- a helicopter
 * bobbing over a pad forever. An airport that cannot service it is one it should simply
 * park at.
 */
inline bool ModularAircraftWantsHangar(const Aircraft *v, const Station *st)
{
	if (!st->airport.HasHangar()) return false;
	return v->current_order.IsType(OT_GOTO_DEPOT) || v->NeedsAutomaticServicing();
}

/**
 * Whether @p tile is a helipad from which a hangar can be reached by ground -- the only
 * kind a depot-bound helicopter may land on. Reads the layout-derived cache, so it is
 * cheap enough for the per-tick landing scan.
 */
bool IsModularPadWithHangarAccess(const Station *st, TileIndex tile);

inline bool IsModularRunwayPiece(uint8_t gfx)
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
 * Turn a template/layout runway flag value into the valid combination stored on
 * the airport. Zero and the fresh-tile default both need a canonical direction.
 * Shared by placement and script previews so they describe the same layout.
 */
inline uint8_t NormalizeModularRunwayFlags(uint8_t flags)
{
	const uint8_t mode_bits = flags & (RUF_LANDING | RUF_TAKEOFF);
	const uint8_t dir_bits = flags & (RUF_DIR_LOW | RUF_DIR_HIGH);

	uint8_t normalized = flags;
	if (mode_bits == 0) normalized |= (RUF_LANDING | RUF_TAKEOFF);
	if (dir_bits != RUF_DIR_LOW && dir_bits != RUF_DIR_HIGH) {
		normalized &= ~(RUF_DIR_LOW | RUF_DIR_HIGH);
		normalized |= RUF_DIR_LOW;
	}
	return normalized;
}

/** Runway end pieces -- the only valid landing/takeoff target tiles. */
inline bool IsModularRunwayEndPiece(uint8_t piece_type)
{
	switch (piece_type) {
		case APT_RUNWAY_END:
		case APT_RUNWAY_SMALL_NEAR_END:
		case APT_RUNWAY_SMALL_FAR_END:
			return true;
		default:
			return false;
	}
}

inline bool IsLegacySmallRunwayPiece(uint8_t piece_type)
{
	switch (piece_type) {
		case APT_RUNWAY_SMALL_NEAR_END:
		case APT_RUNWAY_SMALL_MIDDLE:
		case APT_RUNWAY_SMALL_FAR_END:
			return true;
		default:
			return false;
	}
}

inline bool IsLegacySmallHangarPiece(uint8_t piece_type)
{
	switch (piece_type) {
		case APT_SMALL_DEPOT_SE:
		case APT_SMALL_DEPOT_SW:
		case APT_SMALL_DEPOT_NW:
		case APT_SMALL_DEPOT_NE:
			return true;
		default:
			return false;
	}
}

/**
 * Check if a piece is non-rotatable (e.g. 3-tile small terminal buildings or small hangars).
 */
inline bool IsNonRotatableModularPiece(uint8_t piece_type)
{
	return piece_type == APT_SMALL_BUILDING_1 ||
	       piece_type == APT_SMALL_BUILDING_2 ||
	       piece_type == APT_SMALL_BUILDING_3 ||
	       IsLegacySmallHangarPiece(piece_type);
}

/**
 * Whether a piece is a hangar in its canonical, un-oriented form.
 *
 * Callers hand hangars over as APT_DEPOT_SE / APT_SMALL_DEPOT_SE and express the
 * facing through the tile's rotation; BuildModularAirportTile_Apply derives the
 * directional variant from the two. Anything that rotates a tile before it is
 * placed must leave these alone, or the facing ends up encoded twice.
 */
inline bool IsCanonicalHangarPiece(uint8_t piece_type)
{
	return piece_type == APT_DEPOT_SE || piece_type == APT_SMALL_DEPOT_SE;
}

/**
 * Swap piece variants when rotating by an odd number of quarter-turns.
 * - APT_BUILDING_1 and APT_BUILDING_2 are quarter-turn variants.
 * - Legacy small-runway near/far end sprites swap when axis flips.
 */
inline void SwapBuildingPieceForRotation(uint8_t &piece_type, uint8_t rotation)
{
	rotation &= 3;
	if (rotation == 0) return;

	/* Rotate directional hangar piece encodings with template/build rotations. */
	/* Rotation convention used throughout modular airport code:
	 * 0=SE, 1=NE, 2=NW, 3=SW (clockwise in world space).
	 * Keep this in sync with:
	 * - GetModularHangarTileLayoutByPiece() (station_cmd.cpp)
	 * - CalculateValidTaxiDirectionsForPiece() hangar handling (airport_pathfinder.cpp)
	 */
	auto rotate_directional_hangar = [&piece_type, rotation](uint8_t se, uint8_t ne, uint8_t nw, uint8_t sw) {
		uint8_t idx;
		if (piece_type == se) {
			idx = 0;
		} else if (piece_type == ne) {
			idx = 1;
		} else if (piece_type == nw) {
			idx = 2;
		} else if (piece_type == sw) {
			idx = 3;
		} else {
			return;
		}
		switch ((idx + rotation) & 3) {
			case 0: piece_type = se; break;
			case 1: piece_type = ne; break;
			case 2: piece_type = nw; break;
			default: piece_type = sw; break;
		}
	};

	rotate_directional_hangar(APT_DEPOT_SE, APT_DEPOT_NE, APT_DEPOT_NW, APT_DEPOT_SW);
	rotate_directional_hangar(APT_SMALL_DEPOT_SE, APT_SMALL_DEPOT_NE, APT_SMALL_DEPOT_NW, APT_SMALL_DEPOT_SW);

	if ((rotation & 1) != 0) {
		if (piece_type == APT_BUILDING_1) {
			piece_type = APT_BUILDING_2;
		} else if (piece_type == APT_BUILDING_2) {
			piece_type = APT_BUILDING_1;
		} else if (piece_type == APT_RUNWAY_SMALL_NEAR_END) {
			piece_type = APT_RUNWAY_SMALL_FAR_END;
		} else if (piece_type == APT_RUNWAY_SMALL_FAR_END) {
			piece_type = APT_RUNWAY_SMALL_NEAR_END;
		}
	}
}

/**
 * Rotate one template tile's metadata clockwise by r quarter-turns, in place.
 *
 * Templates are rotated twice on their way into the world: once client-side, on
 * AirportTemplateTile, to draw the placement preview, and once server-side, on
 * the ModularTemplatePlacementTile the command carries. Both have to agree
 * exactly -- if they ever drift, the preview shows one airport and placement
 * builds another -- so both go through this one routine. The two structs share
 * the field names it touches and differ only in the width of dx/dy, which is
 * why this is a template rather than a plain function.
 *
 * @tparam TTile AirportTemplateTile or ModularTemplatePlacementTile.
 * @param tile The tile to rotate.
 * @param r Number of 90-degree clockwise steps.
 * @param template_w Unrotated bounding-box width, at most MAX_TEMPLATE_DIM.
 * @param template_h Unrotated bounding-box height, at most MAX_TEMPLATE_DIM.
 */
template <typename TTile>
inline void RotateModularTemplateTile(TTile &tile, uint8_t r, uint16_t template_w, uint16_t template_h)
{
	using OffsetType = decltype(tile.dx);

	r &= 3;
	if (r == 0) return;

	const uint16_t ox = tile.dx;
	const uint16_t oy = tile.dy;
	const uint8_t old_rotation = tile.rotation;

	/* Offset transform. */
	switch (r) {
		case 1: // 90 CW
			tile.dx = static_cast<OffsetType>(template_h - 1 - oy);
			tile.dy = static_cast<OffsetType>(ox);
			break;
		case 2: // 180
			tile.dx = static_cast<OffsetType>(template_w - 1 - ox);
			tile.dy = static_cast<OffsetType>(template_h - 1 - oy);
			break;
		case 3: // 270 CW
			tile.dx = static_cast<OffsetType>(oy);
			tile.dy = static_cast<OffsetType>(template_w - 1 - ox);
			break;
		default: NOT_REACHED();
	}

	tile.rotation = (old_rotation + r) & 3;

	/* A hangar's facing is carried by `rotation` on the way in -- the script API and
	 * the builder both send the canonical APT_DEPOT_SE -- and
	 * BuildModularAirportTile_Apply turns that into the directional variant when the
	 * tile is placed. Rotating the piece type here as well would encode the turn
	 * twice, and because every reader resolves piece_type ahead of rotation, the
	 * piece would win and the caller's original facing would be silently discarded:
	 * a hangar authored facing NW came out facing whatever the template rotation
	 * alone said. Leave canonical hangars alone and let rotation speak for them.
	 *
	 * Pieces that genuinely encode orientation in the type itself -- the building
	 * 1/2 pair, small runway near/far ends -- still have to be swapped. */
	if (!IsCanonicalHangarPiece(tile.piece_type)) {
		SwapBuildingPieceForRotation(tile.piece_type, r);
	}

	/* Taxi and edge-block masks are both NESW bitmasks. */
	const auto rotate_mask = [r](uint8_t mask) -> uint8_t {
		uint8_t out = 0;
		for (uint8_t i = 0; i < 4; i++) {
			if ((mask & (1 << i)) != 0) out |= (1 << ((i + r) & 3));
		}
		return out;
	};

	tile.user_taxi_dir_mask = rotate_mask(tile.user_taxi_dir_mask);
	tile.edge_block_mask = rotate_mask(tile.edge_block_mask);

	/* Swap low/high when coordinate order along the original axis reverses. */
	const bool original_x_axis = (old_rotation % 2) == 0;
	const bool reverse = original_x_axis ? (r == 2 || r == 3) : (r == 1 || r == 2);
	if (reverse) {
		uint8_t flags = tile.runway_flags;
		const uint8_t low = flags & RUF_DIR_LOW;
		const uint8_t high = flags & RUF_DIR_HIGH;
		flags &= ~(RUF_DIR_LOW | RUF_DIR_HIGH);
		if (low != 0) flags |= RUF_DIR_HIGH;
		if (high != 0) flags |= RUF_DIR_LOW;
		tile.runway_flags = flags;
	}
}

/**
 * Is this an aircraft stand?
 *
 * Only APT_STAND is placeable. The other two arrive with a converted stock
 * airport (or an old template) and behave as ordinary stands; in the stock city
 * airport they are the stands flanking the round terminal, which is why their
 * graphics carry a jetway.
 */
inline bool IsModularStandPiece(uint8_t piece_type)
{
	return piece_type == APT_STAND || piece_type == APT_STAND_1 || piece_type == APT_STAND_PIER_NE;
}

inline bool IsModularBuildingPiece(uint8_t piece_type)
{
	switch (piece_type) {
		case APT_STAND:
		case APT_STAND_1:
		case APT_ROUND_TERMINAL:
		case APT_DEPOT_SE:
		case APT_DEPOT_SW:
		case APT_DEPOT_NW:
		case APT_DEPOT_NE:
		case APT_SMALL_DEPOT_SE:
		case APT_SMALL_DEPOT_SW:
		case APT_SMALL_DEPOT_NW:
		case APT_SMALL_DEPOT_NE:
		case APT_TOWER:
		case APT_TOWER_FENCE_SW:
		case APT_RADIO_TOWER_FENCE_NE:
		case APT_RADAR_GRASS_FENCE_SW:
		case APT_RADAR_FENCE_SW:
		case APT_RADAR_FENCE_NE:
			return true;
		default:
			return false;
	}
}

inline bool IsTaxiwayPiece(uint8_t piece_type)
{
	switch (piece_type) {
		case APT_APRON_HOR:
		case APT_APRON_VER_CROSSING_N:
		case APT_APRON_HOR_CROSSING_E:
		case APT_APRON_VER_CROSSING_S:
		case APT_APRON:
		case APT_APRON_N:
		case APT_APRON_E:
		case APT_APRON_S:
		case APT_APRON_W:
		case APT_APRON_HALF_EAST:
		case APT_APRON_HALF_WEST:
			return true;
		default:
			return false;
	}
}

inline bool IsApronOrTaxiwayPiece(uint8_t piece_type)
{
	if (IsTaxiwayPiece(piece_type)) return true;
	switch (piece_type) {
		case APT_APRON_FENCE_NW:
		case APT_APRON_FENCE_SW:
		case APT_APRON_FENCE_NE:
		case APT_APRON_FENCE_NE_SW:
		case APT_APRON_FENCE_SE_SW:
		case APT_APRON_FENCE_SE:
		case APT_APRON_FENCE_NE_SE:
			return true;
		default:
			return false;
	}
}

inline bool IsRunwayPieceOnAxis(const ModularAirportTileData *data, bool horizontal)
{
	return data != nullptr && IsModularRunwayPiece(data->piece_type) && (((data->rotation % 2) == 0) == horizontal);
}

bool IsModularHelipadPiece(uint8_t gfx);
bool IsRunwayEndLow(const Station *st, TileIndex tile);
uint8_t GetRunwayFlags(const Station *st, TileIndex tile);
TileIndex GetRunwayOtherEnd(const Station *st, TileIndex start_tile);
bool GetContiguousModularRunwayTiles(const Station *st, TileIndex start_tile, std::vector<TileIndex> &tiles);
void ClearModularRunwayReservation(Aircraft *v);
void ClearModularAirportReservationsByVehicle(const Station *st, VehicleID vid, TileIndex keep_tile = INVALID_TILE);
bool HasModularAirportTileReservation(TileIndex tile);
VehicleID GetModularAirportTileReservationOwner(TileIndex tile);
bool IsModularAirportTileReservedBy(TileIndex tile, VehicleID vid);
bool IsModularReservationOwnerGone(TileIndex tile);
void SetModularAirportTileReservationOwner(TileIndex tile, VehicleID vid);
void ClearModularAirportTileReservation(TileIndex tile);
bool ShouldLogModularRateLimited(VehicleID vid, uint8_t channel, uint32_t interval_ticks);
bool IsModularTileOccupiedByOtherAircraft(const Station *st, TileIndex tile, VehicleID self);
bool TryReserveContiguousModularRunway(Aircraft *v, const Station *st, TileIndex runway_tile, bool append_to_existing = false);
bool IsContiguousModularRunwayReservedByOther(const Aircraft *v, const Station *st, TileIndex runway_tile);
bool IsContiguousModularRunwayBusyByOther(const Aircraft *v, const Station *st, TileIndex runway_tile);
bool IsContiguousModularRunwayReservedInStateByOther(const Aircraft *v, const Station *st, std::span<const TileIndex> runway_tiles, VehicleID *blocker = nullptr);
bool IsContiguousModularRunwayQueuedForTakeoffByOther(const Aircraft *v, const Station *st, TileIndex runway_tile);
/** Scope and update the runway-state candidate cache around one CallVehicleTicks pass. */
void BeginModularAirportRunwayStateCache();
void UpdateModularAirportRunwayStateCache(const Aircraft *v);
void EndModularAirportRunwayStateCache();
void RefreshModularAircraftPathSegments(const Station *st);
void RestoreModularAircraftPathSegments();
void BuildReservationKeepSet(const Aircraft *v, const Station *st, std::vector<TileIndex> &keep_set);
void ReconcileAircraftReservations(Aircraft *v, const Station *st, std::span<const TileIndex> keep_set, const char *reason);
bool ShouldRetainRunwayReservation(const Aircraft *v, const Station *st);
void ClearTaxiPathReservation(Aircraft *v, TileIndex keep_tile = INVALID_TILE, bool force_clear_all = false, bool as_fallback = true);
void ClearTaxiPathState(Aircraft *v, TileIndex keep_tile = INVALID_TILE);
void SetTaxiReservation(Aircraft *v, TileIndex tile);
bool IsTaxiTileReservedByOther(const Station *st, TileIndex tile, VehicleID vid);
uint8_t FindTaxiSegmentIndex(const TaxiPath *path, uint16_t tile_index);
/** Why a segment reservation attempt was refused. */
enum class TaxiReserveFailure : uint8_t {
	None,                 ///< The attempt succeeded.
	NoPath,               ///< No usable taxi path or segment index.
	ReservedByOther,      ///< A tile in the claim is reserved by another aircraft.
	OccupiedByOther,      ///< A tile in the claim is physically occupied by another aircraft.
	RunwayBusy,           ///< A runway resource could not be acquired atomically.
	RunwayResourceError,  ///< A runway's contiguous extent could not be resolved.
	NoSafeStop,           ///< A crossing chain reached no terminator (contract violation).
};

/**
 * Detail of a refused reservation, so diagnostics can report the tile that actually
 * blocked rather than re-deriving a guess from the next path tile. The claim a
 * segment makes is frequently wider than one tile -- a whole FREE_MOVE segment, or a
 * crossing chain spanning several runways -- so "the next tile looks free" and "the
 * reservation failed" are routinely both true at once.
 */
struct TaxiReserveResult {
	TaxiReserveFailure reason = TaxiReserveFailure::None;
	TileIndex tile = INVALID_TILE;            ///< The tile that could not be claimed.
	VehicleID blocker = VehicleID::Invalid(); ///< Who holds it, where known.
};

/**
 * Routes tried per goal when looking for one that can be reserved, counting the shortest.
 * An aircraft whose preferred route is held by somebody else takes an alternative route instead of waiting.
 * 1 disables alternative route searching.
 */
inline constexpr uint8_t MODULAR_MAX_ROUTE_ATTEMPTS = 3;

std::string_view TaxiReserveFailureName(TaxiReserveFailure reason);
bool TryReserveTaxiSegment(Aircraft *v, const Station *st, uint8_t segment_idx, TaxiReserveResult *out = nullptr);
TileIndex FindModularLandingGroundGoal(const Station *st, const Aircraft *v, uint8_t *target = nullptr, TileIndex rollout_tile = INVALID_TILE);
bool TryReserveLandingChain(Aircraft *v, const Station *st, TileIndex runway_tile, TileIndex ground_goal);
bool CanAircraftUseModularRunwayForLanding(const Station *st, const Aircraft *v, TileIndex runway_tile);
TileIndex FindModularLandingTarget(const Station *st, const Aircraft *v);
bool IsModularHeliLandingTileAvailable(const Station *st, const Aircraft *v, TileIndex tile);
void GetModularLandingApproachPoint(const Station *st, TileIndex runway_tile, int *target_x, int *target_y);
Direction GetRunwayApproachDirection(const Station *st, TileIndex runway_tile);
const ModularHoldingLoop &GetModularHoldingLoop(const Station *st);
void ComputeModularHoldingLoop(const Station *st, ModularHoldingLoop &loop);
uint32_t GetNearestModularHoldingWaypoint(const Aircraft *v, const ModularHoldingLoop &loop);
void GetModularHoldingWaypointTarget(Aircraft *v, const Station *st, int *target_x, int *target_y, uint32_t *wp_index = nullptr);
void GetModularHeliHoldingTarget(Aircraft *v, const Station *st, int *target_x, int *target_y);
bool IsHoldingGateActive(uint32_t aircraft_wp, uint32_t gate_wp, uint32_t n_wp);
bool DirectionsWithin45(Direction dir_a, Direction dir_b);
TileIndex FindModularRunwayRolloutPoint(const Station *st, TileIndex landing_tile);
TileIndex FindModularRolloutHoldingTile(const Station *st, const Aircraft *v, TileIndex start_tile);
TileIndex FindModularRunwayTileForTakeoff(const Station *st, const Aircraft *v);
TileIndex FindModularTakeoffQueueTile(const Station *st, const Aircraft *v, TileIndex runway_end);
bool IsModularHangarPiece(uint8_t piece_type);
bool IsModularHangarTile(const Station *st, TileIndex tile);
bool IsModularSafeStopTile(const Station *st, TileIndex tile, TileIndex goal = INVALID_TILE);
TileIndex FindFreeModularTerminal(const Station *st, const Aircraft *v, TileIndex from_tile = INVALID_TILE, bool allow_helicopter = false);
bool ModularAirportHasHelipad(const Station *st);
TileIndex FindFreeModularHelipad(const Station *st, const Aircraft *v, TileIndex from_tile = INVALID_TILE);
TileIndex FindFreeModularHangar(const Station *st, const Aircraft *v, TileIndex from_tile = INVALID_TILE);
/**
 * Pick somewhere else to park for an aircraft that has arrived on a tile another
 * aircraft already occupies. Unlike ordinary parking selection this may hand a
 * helicopter a stand at an airport that has helipads: stacking two aircraft on one
 * tile is the worse outcome.
 *
 * @param[out] target Ground target matching the returned tile; untouched on failure.
 * @return Tile to re-target to, or INVALID_TILE when nothing is free.
 */
TileIndex FindModularUnstackParkingTile(const Station *st, const Aircraft *v, uint8_t *target = nullptr);
bool CanUseModularGroundRouting(const Station *st, const Aircraft *v);
bool TryRetargetModularGroundGoal(Aircraft *v, const Station *st);
void HandleModularGroundArrival(Aircraft *v);
void LogModularVehicleReservationState(const Station *st, const Aircraft *v, std::string_view reason);
void LogModularTakeoffRunwayUnavailable(const Station *st, const Aircraft *v);
bool AirportMoveModular(Aircraft *v, const Station *st);
bool AirportMoveModularLanding(Aircraft *v, const Station *st);
bool AirportMoveModularHeliTakeoff(Aircraft *v, const Station *st);
bool AirportMoveModularTakeoff(Aircraft *v, const Station *st);
void AirportMoveModularFlying(Aircraft *v, const Station *st);

bool TeleportAircraftOnModularTile(TileIndex tile, Station *st, bool execute);
void ResetModularAirportStaticState();

void EnsureModularHeliTilesValid(const Station *st);

bool IsModernModularPiece(uint8_t piece_type);
TimerGameCalendar::Year GetModularPieceMinYear(uint8_t piece_type);

inline bool IsLargeRunwayFamily(uint8_t piece_type)
{
	switch (piece_type) {
		case APT_RUNWAY_1: case APT_RUNWAY_2: case APT_RUNWAY_3:
		case APT_RUNWAY_4: case APT_RUNWAY_5: case APT_RUNWAY_END:
			return true;
		default: return false;
	}
}

inline uint8_t GetCanonicalRunwaySegmentPiece(bool large_family, size_t segment_length, size_t index_in_segment)
{
	if (large_family) {
		if (segment_length == 1 || index_in_segment == 0 || index_in_segment + 1 == segment_length) return APT_RUNWAY_END;
		return APT_RUNWAY_5;
	}

	if (segment_length == 1) return APT_RUNWAY_SMALL_NEAR_END;
	if (index_in_segment == 0) return APT_RUNWAY_SMALL_FAR_END;
	if (index_in_segment + 1 == segment_length) return APT_RUNWAY_SMALL_NEAR_END;
	return APT_RUNWAY_SMALL_MIDDLE;
}

bool IsRunwaySafeForLarge(const Station *st, TileIndex runway_end);
bool ModularAirportHasSafeRunwayFor(const Station *st, bool landing);
bool ModularAirportSupportsLargeAircraft(const Station *st);

/* What a modular airport's layout can take instead of the generic AT_MODULAR FTA
 * flags. Layout-derived, cached behind Airport::MarkLayoutDirty, topological
 * (no occupancy, no reachability) because CanVehicleUseStation calls them. */
bool ModularAirportAcceptsPlanes(const Station *st);
bool ModularAirportAcceptsHelicopters(const Station *st);
TTDPAirportType GetModularAirportNewGRFType(const Station *st);

struct ModularAirportCapabilityPiece {
	uint8_t piece_type;
	uint8_t runway_flags;
};

bool ModularAirportAcceptsPlanesFromPieces(std::span<const ModularAirportCapabilityPiece> pieces);

/** Maintenance numerator in eighths of a stock maintenance-cost point. */
uint GetModularAirportMaintenancePointsFromPieces(std::span<const uint8_t> piece_types);
uint GetModularAirportMaintenancePoints(const Station *st);

/** Noise level derived from the operating surfaces in a modular layout. */
uint8_t GetModularAirportNoiseLevelFromPieces(std::span<const uint8_t> piece_types);
uint8_t GetModularAirportNoiseLevel(const Station *st);

uint GetModularAirportCatchmentRadius(const Station *st);

/**
 * One modular airport piece placed on an abstract integer grid. Lets the
 * catchment tiers be computed both for a built airport (grid = tile X/Y) and
 * for a saved template that has not been placed yet (grid = template dx/dy).
 */
struct ModularCatchmentPiece {
	int x; ///< Grid X coordinate.
	int y; ///< Grid Y coordinate.
	uint8_t piece_type;
	uint8_t rotation;
	uint8_t runway_flags;
};

uint GetModularAirportCatchmentRadiusFromPieces(std::span<const ModularCatchmentPiece> pieces);

/* Defined in aircraft_cmd.cpp; mirrors stock MaybeCrashAirplane for a plane
 * braking on a modular runway (short-strip overrun + the general "Plane crashes"
 * setting). Pass the airport the plane is physically rolling out on. True if it
 * crashed. */
bool MaybeCrashModularAircraft(Aircraft *v, const Station *st);

/* Defined in aircraft_cmd.cpp. Pure predicate (no RNG, no side effects): whether
 * a plane braking on modular airport \a st faces the elevated short-strip overrun
 * crash risk. True only for a fast jet, with the no-jetcrash cheat off, on an
 * airport that lacks the large-aircraft safety requirements. Helicopters and
 * non-fast planes are never elevated. */
bool ModularAircraftHasElevatedOverrunRisk(const Aircraft *v, const Station *st);

/** Requirements for a modular airport to be safe for large aircraft. */
enum ModularAirportSafetyRequirement : uint8_t {
	MASR_NONE           = 0,
	MASR_TOWER          = 1 << 0, ///< Missing control tower
	MASR_BIG_TERMINAL   = 1 << 1, ///< Missing large terminal building
	MASR_LANDING_RUNWAY = 1 << 2, ///< Missing 6-tile large landing runway
	MASR_TAKEOFF_RUNWAY = 1 << 3, ///< Missing 6-tile large takeoff runway
};
DECLARE_ENUM_AS_BIT_SET(ModularAirportSafetyRequirement)

ModularAirportSafetyRequirement GetModularAirportSafetyStatus(const Station *st);

/* Abstract-grid variant, for measuring a layout that has not been placed yet.
 * Must stay in exact agreement with the built-airport version above: the elevated
 * jet-overrun crash path is gated on it. */
ModularAirportSafetyRequirement GetModularAirportSafetyStatusFromPieces(std::span<const ModularCatchmentPiece> pieces);

#endif /* MODULAR_AIRPORT_CMD_H */
