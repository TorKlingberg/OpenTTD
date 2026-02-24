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

#include <span>
#include <string_view>
#include <cstdint>

inline constexpr uint8_t MGT_NONE = 0;
inline constexpr uint8_t MGT_TERMINAL = 1;
inline constexpr uint8_t MGT_HELIPAD = 2;
inline constexpr uint8_t MGT_HANGAR = 3;
inline constexpr uint8_t MGT_RUNWAY_TAKEOFF = 4;
inline constexpr uint8_t MGT_ROLLOUT = 5;

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
void ClearTaxiPathReservation(Aircraft *v, TileIndex keep_tile = INVALID_TILE);
void ClearTaxiPathState(Aircraft *v, TileIndex keep_tile = INVALID_TILE);
void SetTaxiReservation(Aircraft *v, TileIndex tile);
bool IsTaxiTileReservedByOther(const Station *st, TileIndex tile, VehicleID vid);
uint8_t FindTaxiSegmentIndex(const TaxiPath *path, uint16_t tile_index);
bool TryReserveTaxiSegment(Aircraft *v, const Station *st, uint8_t segment_idx);
TileIndex FindModularLandingGroundGoal(const Station *st, const Aircraft *v, uint8_t *target = nullptr);
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
TileIndex FindFreeModularTerminal(const Station *st, const Aircraft *v);
TileIndex FindFreeModularHelipad(const Station *st, const Aircraft *v);
TileIndex FindFreeModularHangar(const Station *st, const Aircraft *v);
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

bool IsRunwaySafeForLarge(const Station *st, TileIndex runway_end);
bool ModularAirportSupportsLargeAircraft(const Station *st);

#endif /* MODULAR_AIRPORT_CMD_H */
