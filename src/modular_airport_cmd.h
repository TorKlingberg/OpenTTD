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

inline constexpr int MIN_RUNWAY_LENGTH_TILES = 4; ///< Runways shorter than this are not usable for landing or takeoff

int UpdateAircraftSpeed(Aircraft *v, uint speed_limit = UINT16_MAX, bool hard_limit = true);
void AircraftEntersTerminal(Aircraft *v);
void PlayAircraftSound(const Vehicle *v);
Direction GetModularHangarExitDirection(const Station *st, TileIndex tile);
void AircraftEventHandler_Landing(Aircraft *v, const AirportFTAClass *apc);
void AircraftEventHandler_EndLanding(Aircraft *v, const AirportFTAClass *apc);

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
bool ShouldLogModularRateLimited(VehicleID vid, uint8_t channel, uint32_t interval_ticks);
bool IsModularTileOccupiedByOtherAircraft(const Station *st, TileIndex tile, VehicleID self);
bool TryReserveContiguousModularRunway(Aircraft *v, const Station *st, TileIndex runway_tile);
bool IsContiguousModularRunwayReservedByOther(const Aircraft *v, const Station *st, TileIndex runway_tile);
bool IsContiguousModularRunwayBusyByOther(const Aircraft *v, const Station *st, TileIndex runway_tile);
bool IsContiguousModularRunwayReservedInStateByOther(const Aircraft *v, const Station *st, std::span<const TileIndex> runway_tiles, VehicleID *blocker = nullptr);
bool IsContiguousModularRunwayQueuedForTakeoffByOther(const Aircraft *v, const Station *st, TileIndex runway_tile);
void BuildReservationKeepSet(const Aircraft *v, const Station *st, std::vector<TileIndex> &keep_set);
void ReconcileAircraftReservations(Aircraft *v, const Station *st, std::span<const TileIndex> keep_set, const char *reason);
bool ShouldRetainRunwayReservation(const Aircraft *v, const Station *st);
void ClearTaxiPathReservation(Aircraft *v, TileIndex keep_tile = INVALID_TILE, bool force_clear_all = false, bool as_fallback = true);
void ClearTaxiPathState(Aircraft *v, TileIndex keep_tile = INVALID_TILE);
void SetTaxiReservation(Aircraft *v, TileIndex tile);
bool IsTaxiTileReservedByOther(const Station *st, TileIndex tile, VehicleID vid);
uint8_t FindTaxiSegmentIndex(const TaxiPath *path, uint16_t tile_index);
bool TryReserveTaxiSegment(Aircraft *v, const Station *st, uint8_t segment_idx);
TileIndex FindModularLandingGroundGoal(const Station *st, const Aircraft *v, uint8_t *target = nullptr, TileIndex rollout_tile = INVALID_TILE);
bool TryReserveLandingChain(Aircraft *v, const Station *st, TileIndex runway_tile, TileIndex ground_goal);
TileIndex FindModularLandingTarget(const Station *st, const Aircraft *v);
void GetModularLandingApproachPoint(const Station *st, TileIndex runway_tile, int *target_x, int *target_y);
Direction GetRunwayApproachDirection(const Station *st, TileIndex runway_tile);
const ModularHoldingLoop &GetModularHoldingLoop(const Station *st);
void ComputeModularHoldingLoop(const Station *st, ModularHoldingLoop &loop);
uint32_t GetNearestModularHoldingWaypoint(const Aircraft *v, const ModularHoldingLoop &loop);
void GetModularHoldingWaypointTarget(Aircraft *v, const Station *st, int *target_x, int *target_y, uint32_t *wp_index = nullptr);
bool IsHoldingGateActive(uint32_t aircraft_wp, uint32_t gate_wp, uint32_t n_wp);
bool DirectionsWithin45(Direction dir_a, Direction dir_b);
TileIndex FindModularRunwayRolloutPoint(const Station *st, TileIndex landing_tile);
TileIndex FindNearestModularRunwayExitTile(const Station *st, const Aircraft *v, TileIndex runway_tile);
TileIndex FindModularRolloutHoldingTile(const Station *st, const Aircraft *v, TileIndex start_tile);
TileIndex FindModularRunwayTileForTakeoff(const Station *st, const Aircraft *v);
TileIndex FindModularTakeoffQueueTile(const Station *st, const Aircraft *v, TileIndex runway_end);
bool IsModularHangarPiece(uint8_t piece_type);
bool IsModularHangarTile(const Station *st, TileIndex tile);
TileIndex FindFreeModularTerminal(const Station *st, const Aircraft *v, TileIndex from_tile = INVALID_TILE);
TileIndex FindFreeModularHelipad(const Station *st, const Aircraft *v, TileIndex from_tile = INVALID_TILE);
TileIndex FindFreeModularHangar(const Station *st, const Aircraft *v, TileIndex from_tile = INVALID_TILE);
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
bool ModularAirportSupportsLargeAircraft(const Station *st);

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

#endif /* MODULAR_AIRPORT_CMD_H */
