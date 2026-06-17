/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file test_modular_airport.cpp Unit tests for modular airport logic. */

#include "../stdafx.h"
#include "../3rdparty/catch2/catch.hpp"

#include "../modular_airport_cmd.h"
#include "../table/airporttile_ids.h"
#include "../map_func.h"
#include "../station_base.h"
#include "../station_map.h"
#include "../airport_ground_pathfinder.h"
#include "mock_environment.h"
#include "../vehicle_base.h"
#include "../engine_base.h"
#include "../cheat_type.h"
#include "../settings_type.h"

#include "../safeguards.h"

static Station *SetupModularAirport(TileIndex base_tile, uint size_x, uint size_y)
{
	MockEnvironment::Instance();
	extern StationPool _station_pool;
	_station_pool.CleanPool();

	Station *st = Station::CreateAtIndex(StationID(0), base_tile);
	if (st == nullptr) return nullptr;

	st->airport.tile = base_tile;
	st->airport.w = size_x;
	st->airport.h = size_y;
	st->airport.EnsureModularDataExists();
	st->airport.blocks.Set(AirportBlock::Modular);
	st->facilities |= StationFacility::Airport;
	st->owner = OWNER_NONE;

	for (uint y = 0; y < size_y; y++) {
		for (uint x = 0; x < size_x; x++) {
			TileIndex t = base_tile + TileDiffXY(x, y);
			MakeStation(t, st->owner, st->index, StationType::Airport, 0);
		}
	}

	return st;
}

static void SetupAircraftPool()
{
	extern VehiclePool _vehicle_pool;
	_vehicle_pool.CleanPool();
}

/* Bare-shell aircraft for tests: only fields read by the APIs under test
 * (index, subtype, tile, x_pos/y_pos, ground_path_goal) are populated by callers. */
static Aircraft *CreateAircraft(VehicleID index)
{
	Aircraft *v = Aircraft::CreateAtIndex(index);
	v->subtype = AIR_AIRCRAFT;
	return v;
}

/* Create an aircraft engine in the pool with the given AircraftVehicleInfo
 * subtype bits (AIR_FAST marks a large/fast jet). Returns its EngineID for
 * assigning to Aircraft::engine_type. */
static EngineID CreateAircraftEngine(EngineID index, uint8_t subtype)
{
	Engine *e = Engine::CreateAtIndex(index, VEH_AIRCRAFT, 0xFFFF);
	e->VehInfo<AircraftVehicleInfo>().subtype = subtype;
	return index;
}

static void AddModularTile(Station *st, TileIndex tile, uint8_t piece_type, uint8_t rotation = 0)
{
	ModularAirportTileData data;
	data.tile = tile;
	data.piece_type = piece_type;
	data.rotation = rotation;
	st->airport.modular_tile_data->push_back(data);
	st->airport.modular_tile_index_dirty = true;
	st->airport.modular_catchment_dirty = true;
}

static ModularAirportTileData *AddModularTileWithData(Station *st, TileIndex tile, uint8_t piece_type, uint8_t rotation = 0)
{
	AddModularTile(st, tile, piece_type, rotation);
	return st->airport.GetModularTileData(tile);
}

static void AddLargeRunway(Station *st, TileIndex start, uint length, uint8_t rotation = 0, uint8_t flags = RUF_DEFAULT)
{
	for (uint i = 0; i < length; i++) {
		const TileIndex tile = start + ((rotation % 2) == 0 ? TileDiffXY(i, 0) : TileDiffXY(0, i));
		ModularAirportTileData *data = AddModularTileWithData(st, tile, (i == 0 || i + 1 == length) ? APT_RUNWAY_END : APT_RUNWAY_5, rotation);
		data->runway_flags = flags;
	}
}

static void CheckReservedBy(const std::vector<TileIndex> &tiles, VehicleID vid)
{
	for (TileIndex tile : tiles) {
		CHECK(IsModularAirportTileReservedBy(tile, vid));
	}
}

static void CheckUnreserved(const std::vector<TileIndex> &tiles)
{
	for (TileIndex tile : tiles) {
		CHECK_FALSE(HasModularAirportTileReservation(tile));
	}
}

TEST_CASE("ModularAirportSafety")
{
	Map::Allocate(64, 64);
	TileIndex base = TileXY(10, 10);
	Station *st = SetupModularAirport(base, 10, 10);
	REQUIRE(st != nullptr);

	SECTION("Safety Requirements") {
		// Empty airport
		ModularAirportSafetyRequirement status = GetModularAirportSafetyStatus(st);
		CHECK((status & MASR_TOWER) != 0);
		CHECK((status & MASR_BIG_TERMINAL) != 0);
		CHECK((status & MASR_LANDING_RUNWAY) != 0);
		CHECK((status & MASR_TAKEOFF_RUNWAY) != 0);
		
		// Add tower
		AddModularTile(st, base, APT_TOWER, 0);
		status = GetModularAirportSafetyStatus(st);
		CHECK((status & MASR_TOWER) == 0);
	}

	SECTION("Large Aircraft Requirements Are Independent") {
		AddModularTile(st, base, APT_TOWER, 0);
		AddModularTile(st, base + TileDiffXY(1, 0), APT_ROUND_TERMINAL, 0);
		AddLargeRunway(st, base + TileDiffXY(0, 3), 5);

		ModularAirportSafetyRequirement status = GetModularAirportSafetyStatus(st);
		CHECK((status & MASR_TOWER) == 0);
		CHECK((status & MASR_BIG_TERMINAL) == 0);
		CHECK((status & MASR_LANDING_RUNWAY) != 0);
		CHECK((status & MASR_TAKEOFF_RUNWAY) != 0);

		Station *large_st = SetupModularAirport(base, 10, 10);
		REQUIRE(large_st != nullptr);
		AddModularTile(large_st, base, APT_TOWER, 0);
		AddModularTile(large_st, base + TileDiffXY(1, 0), APT_ROUND_TERMINAL, 0);
		AddLargeRunway(large_st, base + TileDiffXY(0, 3), 6, 0, RUF_LANDING);
		status = GetModularAirportSafetyStatus(large_st);
		CHECK((status & MASR_LANDING_RUNWAY) == 0);
		CHECK((status & MASR_TAKEOFF_RUNWAY) != 0);

		Station *complete_st = SetupModularAirport(base, 10, 10);
		REQUIRE(complete_st != nullptr);
		AddModularTile(complete_st, base, APT_TOWER, 0);
		AddModularTile(complete_st, base + TileDiffXY(1, 0), APT_ROUND_TERMINAL, 0);
		AddLargeRunway(complete_st, base + TileDiffXY(0, 3), 6);
		CHECK(GetModularAirportSafetyStatus(complete_st) == MASR_NONE);
		CHECK(ModularAirportSupportsLargeAircraft(complete_st));
	}
}

TEST_CASE("ModularAirportHoldingLoop")
{
	SECTION("IsHoldingGateActive") {
		// 8 waypoints loop
		CHECK(IsHoldingGateActive(0, 0, 8)); // AT gate
		CHECK(IsHoldingGateActive(7, 0, 8)); // Just before gate (wrap)
		CHECK_FALSE(IsHoldingGateActive(1, 0, 8)); // Just passed gate

		CHECK(IsHoldingGateActive(3, 4, 8)); // Just before gate
		CHECK(IsHoldingGateActive(4, 4, 8)); // AT gate
		CHECK_FALSE(IsHoldingGateActive(5, 4, 8)); // Just passed gate

		// Edge cases.
		CHECK_FALSE(IsHoldingGateActive(0, 0, 0)); // Empty loop is never active.
		CHECK(IsHoldingGateActive(0, 0, 1)); // Single waypoint: at-gate and "previous" alias.
	}

	SECTION("GetNearestModularHoldingWaypoint") {
		ModularHoldingLoop loop;
		loop.waypoints.push_back({100, 100});
		loop.waypoints.push_back({200, 100});
		loop.waypoints.push_back({200, 200});
		loop.waypoints.push_back({100, 200});

		SetupAircraftPool();
		Aircraft *v = CreateAircraft(VehicleID(0));
		
		v->x_pos = 110; v->y_pos = 110;
		CHECK(GetNearestModularHoldingWaypoint(v, loop) == 0);

		v->x_pos = 190; v->y_pos = 110;
		CHECK(GetNearestModularHoldingWaypoint(v, loop) == 1);

		v->x_pos = 150; v->y_pos = 300;
		// (150, 300) is closer to (100, 200) [dist^2 = 50^2 + 100^2 = 12500] 
		// or (200, 200) [dist^2 = 50^2 + 100^2 = 12500]? 
		// Tie goes to lower index (2).
		CHECK(GetNearestModularHoldingWaypoint(v, loop) == 2);
	}
}

TEST_CASE("ModularAirportPathfinding")
{
	Map::Allocate(64, 64);
	TileIndex base = TileXY(10, 10);
	Station *st = SetupModularAirport(base, 10, 10);
	REQUIRE(st != nullptr);

	SECTION("Simple Taxi Path") {
		// Hangar (10,10) -> Taxiway -> Stand (13,10)
		AddModularTile(st, base, APT_DEPOT_SE, 1); // rotation 1 allows East (dx=+1)
		AddModularTile(st, base + TileDiffXY(1, 0), APT_APRON, 0);
		AddModularTile(st, base + TileDiffXY(2, 0), APT_APRON, 0);
		AddModularTile(st, base + TileDiffXY(3, 0), APT_STAND, 0);

		AirportGroundPath path = FindAirportGroundPath(st, base, base + TileDiffXY(3, 0));
		CHECK(path.found);
		REQUIRE(path.tiles.size() == 4);
		CHECK(path.tiles[0] == base);
		CHECK(path.tiles[3] == base + TileDiffXY(3, 0));
	}

	SECTION("Avoid Occupied Stand") {
		AddModularTile(st, base, APT_APRON, 0);
		AddModularTile(st, base + TileDiffXY(1, 0), APT_STAND, 0); // Intermediate stand
		AddModularTile(st, base + TileDiffXY(2, 0), APT_APRON, 0);
		AddModularTile(st, base + TileDiffXY(2, 1), APT_STAND, 0); // Goal stand

		// Alternative path around (1,0)
		AddModularTile(st, base + TileDiffXY(0, 1), APT_APRON, 0);
		AddModularTile(st, base + TileDiffXY(1, 1), APT_APRON, 0);
		
		TileIndex start = base;

		// Path with avoidance
		SetupAircraftPool();
		Aircraft *self = CreateAircraft(VehicleID(1));
		self->tile = start;
		self->ground_path_goal = base + TileDiffXY(2, 1);
		SetModularAirportTileReservationOwner(base + TileDiffXY(1, 0), VehicleID(2));
		
		AirportGroundPath path = FindAirportGroundPath(st, start, base + TileDiffXY(2, 1), self);
		CHECK(path.found);
		for (TileIndex t : path.tiles) {
			CHECK(t != base + TileDiffXY(1, 0));
		}
	}

	SECTION("Reserved Stand Blocks When No Alternative Exists") {
		AddModularTile(st, base, APT_APRON, 0);
		AddModularTile(st, base + TileDiffXY(1, 0), APT_STAND, 0);
		AddModularTile(st, base + TileDiffXY(2, 0), APT_STAND, 0);

		SetupAircraftPool();
		Aircraft *self = CreateAircraft(VehicleID(3));
		self->tile = base;
		self->ground_path_goal = base + TileDiffXY(2, 0);
		SetModularAirportTileReservationOwner(base + TileDiffXY(1, 0), VehicleID(4));

		AirportGroundPath path = FindAirportGroundPath(st, base, base + TileDiffXY(2, 0), self);
		CHECK_FALSE(path.found);
	}

	SECTION("One Way Taxi Direction Is Enforced") {
		AddModularTile(st, base, APT_APRON, 0);
		AddModularTile(st, base + TileDiffXY(1, 0), APT_APRON, 0);
		AddModularTile(st, base + TileDiffXY(2, 0), APT_APRON, 0);
		ModularAirportTileData *one_way = st->airport.GetModularTileData(base + TileDiffXY(1, 0));
		REQUIRE(one_way != nullptr);
		one_way->one_way_taxi = true;
		one_way->user_taxi_dir_mask = 0x02; // East

		CHECK(FindAirportGroundPath(st, base, base + TileDiffXY(2, 0)).found);
		CHECK_FALSE(FindAirportGroundPath(st, base + TileDiffXY(2, 0), base).found);
	}

	SECTION("Edge Blocks Prevent Adjacent Taxi Movement") {
		AddModularTile(st, base, APT_APRON, 0);
		AddModularTile(st, base + TileDiffXY(1, 0), APT_APRON, 0);
		ModularAirportTileData *left = st->airport.GetModularTileData(base);
		REQUIRE(left != nullptr);
		left->edge_block_mask = 0x02; // East

		CHECK_FALSE(FindAirportGroundPath(st, base, base + TileDiffXY(1, 0)).found);
	}

	SECTION("Taxi Path Classification") {
		AddModularTile(st, base, APT_APRON, 0);
		AddModularTile(st, base + TileDiffXY(1, 0), APT_APRON, 0);
		AddModularTile(st, base + TileDiffXY(2, 0), APT_APRON, 0);
		
		// Set one-way flag manually and allow East
		ModularAirportTileData *td1 = st->airport.GetModularTileData(base + TileDiffXY(1, 0));
		td1->one_way_taxi = true;
		td1->user_taxi_dir_mask = 0x02; // East
		
		AddModularTile(st, base + TileDiffXY(3, 0), APT_RUNWAY_1, 0); // Runway segment (horizontal)

		TaxiPath path = BuildTaxiPath(st, base, base + TileDiffXY(3, 0));
		CHECK(path.valid);
		// Segments: Apron (FREE_MOVE), One-way (ONE_WAY), Apron (FREE_MOVE), Runway (RUNWAY)
		REQUIRE(path.segments.size() >= 4);
		CHECK(path.segments[0].type == TaxiSegmentType::FREE_MOVE);
		CHECK(path.segments[1].type == TaxiSegmentType::ONE_WAY);
		CHECK(path.segments[2].type == TaxiSegmentType::FREE_MOVE);
		CHECK(path.segments[3].type == TaxiSegmentType::RUNWAY);
		CHECK(path.segments[0].start_index == 0);
		CHECK(path.segments[0].end_index == 0);
		CHECK(path.segments[3].start_index == 3);
		CHECK(path.segments[3].end_index == 3);
		CHECK(FindTaxiSegmentIndex(&path, 0) == 0);
		CHECK(FindTaxiSegmentIndex(&path, 1) == 1);
		CHECK(FindTaxiSegmentIndex(&path, 3) == 3);
		CHECK(FindTaxiSegmentIndex(&path, 4) == path.segments.size());
	}
}

TEST_CASE("ModularAirportReservations")
{
	Map::Allocate(64, 64);
	TileIndex base = TileXY(10, 10);
	Station *st = SetupModularAirport(base, 5, 5);
	REQUIRE(st != nullptr);

	SECTION("Tile Reservation") {
		AddModularTile(st, base, APT_APRON, 0);
		VehicleID vid(123);
		SetModularAirportTileReservationOwner(base, vid);
		CHECK(HasModularAirportTileReservation(base));
		CHECK(GetModularAirportTileReservationOwner(base) == vid);
		CHECK(IsModularAirportTileReservedBy(base, vid));
		
		ClearModularAirportTileReservation(base);
		CHECK_FALSE(HasModularAirportTileReservation(base));
	}

	SECTION("Runway Reservation") {
		AddModularTile(st, base, APT_RUNWAY_END, 0);
		AddModularTile(st, base + TileDiffXY(1, 0), APT_RUNWAY_5, 0);
		AddModularTile(st, base + TileDiffXY(2, 0), APT_RUNWAY_END, 0);
		const std::vector<TileIndex> runway_tiles = {base, base + TileDiffXY(1, 0), base + TileDiffXY(2, 0)};

		SetupAircraftPool();
		Aircraft *v = CreateAircraft(VehicleID(10));
		CHECK(TryReserveContiguousModularRunway(v, st, base));
		
		CheckReservedBy(runway_tiles, v->index);
		
		// Another aircraft trying to reserve
		Aircraft *v2 = CreateAircraft(VehicleID(11));
		CHECK_FALSE(TryReserveContiguousModularRunway(v2, st, base));
		
		ClearModularRunwayReservation(v);
		CheckUnreserved(runway_tiles);
	}

	SECTION("Runway Reservation Failure Is Atomic") {
		AddModularTile(st, base, APT_RUNWAY_END, 0);
		AddModularTile(st, base + TileDiffXY(1, 0), APT_RUNWAY_5, 0);
		AddModularTile(st, base + TileDiffXY(2, 0), APT_RUNWAY_END, 0);
		const std::vector<TileIndex> runway_tiles = {base, base + TileDiffXY(1, 0), base + TileDiffXY(2, 0)};

		SetupAircraftPool();
		SetModularAirportTileReservationOwner(base + TileDiffXY(1, 0), VehicleID(12));
		Aircraft *v = CreateAircraft(VehicleID(13));

		CHECK_FALSE(TryReserveContiguousModularRunway(v, st, base));
		CHECK_FALSE(HasModularAirportTileReservation(base));
		CHECK(IsModularAirportTileReservedBy(base + TileDiffXY(1, 0), VehicleID(12)));
		CHECK_FALSE(HasModularAirportTileReservation(base + TileDiffXY(2, 0)));
		CHECK(v->modular_runway_reservation.empty());

		ClearModularAirportTileReservation(base + TileDiffXY(1, 0));
		CHECK(TryReserveContiguousModularRunway(v, st, base + TileDiffXY(1, 0)));
		CheckReservedBy(runway_tiles, v->index);
		CHECK(TryReserveContiguousModularRunway(v, st, base));
		CheckReservedBy(runway_tiles, v->index);
	}
}

TEST_CASE("ModularAirportMapDependentLogic")
{
	Map::Allocate(64, 64);
	TileIndex base = TileXY(10, 10);
	Station *st = SetupModularAirport(base, 5, 5);
	REQUIRE(st != nullptr);

	SECTION("Runway Discovery") {
		// Create a 3-tile horizontal runway
		AddModularTile(st, base, APT_RUNWAY_END, 0); // Rotation 0/2 is horizontal for runways
		AddModularTile(st, base + TileDiffXY(1, 0), APT_RUNWAY_5, 0);
		AddModularTile(st, base + TileDiffXY(2, 0), APT_RUNWAY_END, 0);

		std::vector<TileIndex> tiles;
		CHECK(GetContiguousModularRunwayTiles(st, base, tiles));
		CHECK(tiles.size() == 3);
		CHECK(tiles[0] == base);
		CHECK(tiles[2] == base + TileDiffXY(2, 0));

		CHECK(GetRunwayOtherEnd(st, base) == base + TileDiffXY(2, 0));
		CHECK(GetRunwayOtherEnd(st, base + TileDiffXY(2, 0)) == base);
		CHECK(GetRunwayOtherEnd(st, base + TileDiffXY(1, 0)) == base + TileDiffXY(2, 0));
	}

	SECTION("Vertical Runway Discovery From Middle Tile") {
		AddModularTile(st, base, APT_RUNWAY_END, 1);
		AddModularTile(st, base + TileDiffXY(0, 1), APT_RUNWAY_5, 1);
		AddModularTile(st, base + TileDiffXY(0, 2), APT_RUNWAY_END, 1);

		std::vector<TileIndex> tiles;
		CHECK(GetContiguousModularRunwayTiles(st, base + TileDiffXY(0, 1), tiles));
		REQUIRE(tiles.size() == 3);
		CHECK(tiles[0] == base);
		CHECK(tiles[1] == base + TileDiffXY(0, 1));
		CHECK(tiles[2] == base + TileDiffXY(0, 2));
		CHECK(GetRunwayOtherEnd(st, base) == base + TileDiffXY(0, 2));
	}

	SECTION("Runway Discovery Stops At Gaps And Mixed Axes") {
		AddModularTile(st, base, APT_RUNWAY_END, 0);
		AddModularTile(st, base + TileDiffXY(1, 0), APT_APRON, 0);
		AddModularTile(st, base + TileDiffXY(2, 0), APT_RUNWAY_END, 0);

		std::vector<TileIndex> tiles;
		CHECK(GetContiguousModularRunwayTiles(st, base, tiles));
		REQUIRE(tiles.size() == 1);
		CHECK(tiles[0] == base);

		Station *mixed_st = SetupModularAirport(base, 5, 5);
		REQUIRE(mixed_st != nullptr);
		AddModularTile(mixed_st, base, APT_RUNWAY_END, 0);
		AddModularTile(mixed_st, base + TileDiffXY(1, 0), APT_RUNWAY_5, 1);
		tiles.clear();
		CHECK(GetContiguousModularRunwayTiles(mixed_st, base, tiles));
		REQUIRE(tiles.size() == 1);
		tiles.clear();
		CHECK_FALSE(GetContiguousModularRunwayTiles(mixed_st, base + TileDiffXY(2, 0), tiles));
	}

	SECTION("Runway Axis") {
		AddModularTile(st, base, APT_RUNWAY_1, 0); // Horizontal
		AddModularTile(st, base + TileDiffXY(0, 1), APT_RUNWAY_1, 1); // Vertical

		CHECK(IsRunwayPieceOnAxis(st->airport.GetModularTileData(base), true));
		CHECK_FALSE(IsRunwayPieceOnAxis(st->airport.GetModularTileData(base), false));

		CHECK(IsRunwayPieceOnAxis(st->airport.GetModularTileData(base + TileDiffXY(0, 1)), false));
		CHECK_FALSE(IsRunwayPieceOnAxis(st->airport.GetModularTileData(base + TileDiffXY(0, 1)), true));
	}
}

TEST_CASE("ModularAirportPieceClassification")
{
	SECTION("Runway Pieces") {
		CHECK(IsModularRunwayPiece(APT_RUNWAY_1));
		CHECK(IsModularRunwayPiece(APT_RUNWAY_END));
		CHECK(IsModularRunwayPiece(APT_RUNWAY_SMALL_MIDDLE));
		CHECK_FALSE(IsModularRunwayPiece(APT_STAND));
		CHECK_FALSE(IsModularRunwayPiece(APT_APRON));
	}

	SECTION("Legacy Small Runway Pieces") {
		CHECK(IsLegacySmallRunwayPiece(APT_RUNWAY_SMALL_NEAR_END));
		CHECK(IsLegacySmallRunwayPiece(APT_RUNWAY_SMALL_MIDDLE));
		CHECK(IsLegacySmallRunwayPiece(APT_RUNWAY_SMALL_FAR_END));
		CHECK_FALSE(IsLegacySmallRunwayPiece(APT_RUNWAY_1));
	}

	SECTION("Building Pieces") {
		CHECK(IsModularBuildingPiece(APT_STAND));
		CHECK(IsModularBuildingPiece(APT_DEPOT_SE));
		CHECK(IsModularBuildingPiece(APT_SMALL_DEPOT_NW));
		CHECK(IsModularBuildingPiece(APT_TOWER));
		CHECK_FALSE(IsModularBuildingPiece(APT_APRON));
		CHECK_FALSE(IsModularBuildingPiece(APT_RUNWAY_1));
	}

	SECTION("Taxiway Pieces") {
		CHECK(IsTaxiwayPiece(APT_APRON_HOR));
		CHECK(IsTaxiwayPiece(APT_APRON));
		CHECK_FALSE(IsTaxiwayPiece(APT_STAND));
	}

	SECTION("Apron or Taxiway Pieces") {
		CHECK(IsApronOrTaxiwayPiece(APT_APRON));
		CHECK(IsApronOrTaxiwayPiece(APT_APRON_FENCE_NW));
		CHECK_FALSE(IsApronOrTaxiwayPiece(APT_STAND));
	}

	SECTION("Hangar Pieces") {
		CHECK(IsModularHangarPiece(APT_DEPOT_SE));
		CHECK(IsModularHangarPiece(APT_SMALL_DEPOT_NW));
		CHECK_FALSE(IsModularHangarPiece(APT_STAND));
	}

	SECTION("Legacy Small Hangar Pieces") {
		CHECK(IsLegacySmallHangarPiece(APT_SMALL_DEPOT_SE));
		CHECK(IsLegacySmallHangarPiece(APT_SMALL_DEPOT_NE));
		CHECK_FALSE(IsLegacySmallHangarPiece(APT_DEPOT_SE));
		CHECK_FALSE(IsLegacySmallHangarPiece(APT_STAND));
	}
}

TEST_CASE("ModularAirportRotationLogic")
{
	SECTION("Directional Hangar Rotation") {
		uint8_t piece = APT_DEPOT_SE;
		SwapBuildingPieceForRotation(piece, 1); // 90 deg clockwise
		CHECK(piece == APT_DEPOT_NE);
		
		SwapBuildingPieceForRotation(piece, 1); // another 90 deg
		CHECK(piece == APT_DEPOT_NW);
		
		SwapBuildingPieceForRotation(piece, 2); // 180 deg
		CHECK(piece == APT_DEPOT_SE);
	}

	SECTION("Small Hangar Rotation") {
		uint8_t piece = APT_SMALL_DEPOT_SE;
		SwapBuildingPieceForRotation(piece, 1);
		CHECK(piece == APT_SMALL_DEPOT_NE);
	}

	SECTION("Building 1/2 Swap") {
		uint8_t piece = APT_BUILDING_1;
		SwapBuildingPieceForRotation(piece, 1);
		CHECK(piece == APT_BUILDING_2);
		
		piece = APT_BUILDING_1;
		SwapBuildingPieceForRotation(piece, 2); // 180 deg shouldn't swap 1/2
		CHECK(piece == APT_BUILDING_1);
	}

	SECTION("Small Runway End Swap") {
		uint8_t piece = APT_RUNWAY_SMALL_NEAR_END;
		SwapBuildingPieceForRotation(piece, 1);
		CHECK(piece == APT_RUNWAY_SMALL_FAR_END);
		
		piece = APT_RUNWAY_SMALL_NEAR_END;
		SwapBuildingPieceForRotation(piece, 3);
		CHECK(piece == APT_RUNWAY_SMALL_FAR_END);
		
		piece = APT_RUNWAY_SMALL_NEAR_END;
		SwapBuildingPieceForRotation(piece, 2);
		CHECK(piece == APT_RUNWAY_SMALL_NEAR_END);
	}
}

TEST_CASE("ModularAirportMetadata")
{
	SECTION("Large Runway Family") {
		CHECK(IsLargeRunwayFamily(APT_RUNWAY_1));
		CHECK(IsLargeRunwayFamily(APT_RUNWAY_END));
		CHECK_FALSE(IsLargeRunwayFamily(APT_RUNWAY_SMALL_MIDDLE));
	}

	SECTION("Canonical Runway Pieces") {
		// Large family
		CHECK(GetCanonicalRunwaySegmentPiece(true, 5, 0) == APT_RUNWAY_END);
		CHECK(GetCanonicalRunwaySegmentPiece(true, 5, 2) == APT_RUNWAY_5);
		CHECK(GetCanonicalRunwaySegmentPiece(true, 5, 4) == APT_RUNWAY_END);
		CHECK(GetCanonicalRunwaySegmentPiece(true, 1, 0) == APT_RUNWAY_END);

		// Small family
		CHECK(GetCanonicalRunwaySegmentPiece(false, 3, 0) == APT_RUNWAY_SMALL_FAR_END);
		CHECK(GetCanonicalRunwaySegmentPiece(false, 3, 1) == APT_RUNWAY_SMALL_MIDDLE);
		CHECK(GetCanonicalRunwaySegmentPiece(false, 3, 2) == APT_RUNWAY_SMALL_NEAR_END);
		CHECK(GetCanonicalRunwaySegmentPiece(false, 1, 0) == APT_RUNWAY_SMALL_NEAR_END);
	}

	SECTION("Modern Piece Availability") {
		CHECK_FALSE(IsModernModularPiece(APT_APRON));
		CHECK_FALSE(IsModernModularPiece(APT_SMALL_DEPOT_SE));
		CHECK(IsModernModularPiece(APT_RUNWAY_1));
		CHECK(GetModularPieceMinYear(APT_APRON) == CalendarTime::MIN_YEAR);
		CHECK(GetModularPieceMinYear(APT_RUNWAY_1) == AirportSpec::Get(AT_LARGE)->min_year);
	}
}

TEST_CASE("ModularAirportMovementHelpers")
{
	SECTION("DirectionsWithin45") {
		CHECK(DirectionsWithin45(DIR_N, DIR_N));
		CHECK(DirectionsWithin45(DIR_N, DIR_NE));
		CHECK(DirectionsWithin45(DIR_N, DIR_NW));
		CHECK_FALSE(DirectionsWithin45(DIR_N, DIR_E));
		CHECK_FALSE(DirectionsWithin45(DIR_N, DIR_S));

		// Wrap around
		CHECK(DirectionsWithin45(DIR_NW, DIR_N));
		CHECK(DirectionsWithin45(DIR_NW, DIR_W));
	}
}

/* Mark a tile as occupied by another aircraft for landing-chain validation tests.
 * Must register the tile in the blocker's taxi_reserved_tiles so that
 * TryClearStaleModularReservation does not auto-clear it. */
static Aircraft *CreateBlockerOnTile(Station *st, VehicleID blocker_id, TileIndex tile)
{
	Aircraft *blocker = CreateAircraft(blocker_id);
	blocker->targetairport = st->index;
	blocker->tile = tile;
	SetModularAirportTileReservationOwner(tile, blocker->index);
	blocker->taxi_reserved_tiles.push_back(tile);
	return blocker;
}

TEST_CASE("ModularAirportLandingChain")
{
	Map::Allocate(64, 64);
	TileIndex base = TileXY(10, 10);
	Station *st = SetupModularAirport(base, 10, 10);
	REQUIRE(st != nullptr);

	SECTION("Rejects path through occupied stand") {
		/* Layout (rotation 0, runway extends East along X):
		 *   Row 0: RWY_END  RWY_5    RWY_END     (rollout runway, tiles only)
		 *   Row 1:                   STAND_BLK   (sole exit from runway end)
		 *   Row 2:                   STAND_GOAL  (only reachable via STAND_BLK)
		 * Touchdown at (0,0), rollout at (2,0). Goal at (2,2).
		 * Planner returns path (2,0) → (2,1) → (2,2); the walk's blocked_by_other
		 * on (2,1) must reject the chain. */
		AddLargeRunway(st, base, 3, 0, RUF_DEFAULT);
		AddModularTile(st, base + TileDiffXY(2, 1), APT_STAND, 0);   // blocker sits here
		AddModularTile(st, base + TileDiffXY(2, 2), APT_STAND, 0);   // goal

		SetupAircraftPool();
		Aircraft *v = CreateAircraft(VehicleID(10));
		v->targetairport = st->index;

		CreateBlockerOnTile(st, VehicleID(11), base + TileDiffXY(2, 1));

		TileIndex touchdown = base;
		TileIndex goal = base + TileDiffXY(2, 2);

		CHECK_FALSE(TryReserveLandingChain(v, st, touchdown, goal));
		/* Full rollback: rollout runway released. */
		for (int i = 0; i < 3; i++) {
			CHECK_FALSE(IsModularAirportTileReservedBy(base + TileDiffXY(i, 0), v->index));
		}
		CHECK(v->modular_runway_reservation.empty());
	}

	SECTION("Reserves transit runway end-to-end") {
		/* Layout:
		 *   Row 0 (rollout runway):  RWY_END RWY_5 RWY_END
		 *   Row 1:                   APRON   APRON APRON
		 *   Row 2 (transit runway):  RWY_END RWY_5 RWY_END
		 *   Row 3:                   APRON   APRON STAND_GOAL
		 * Touchdown at (0,0), rollout (2,0). Goal at (2,3).
		 * Path: rollout → apron(2,1) → transit_runway(2,2) → STAND_GOAL(2,3). */
		AddLargeRunway(st, base, 3, 0, RUF_DEFAULT);
		AddModularTile(st, base + TileDiffXY(0, 1), APT_APRON, 0);
		AddModularTile(st, base + TileDiffXY(1, 1), APT_APRON, 0);
		AddModularTile(st, base + TileDiffXY(2, 1), APT_APRON, 0);
		AddLargeRunway(st, base + TileDiffXY(0, 2), 3, 0, RUF_DEFAULT);
		AddModularTile(st, base + TileDiffXY(0, 3), APT_APRON, 0);
		AddModularTile(st, base + TileDiffXY(1, 3), APT_APRON, 0);
		AddModularTile(st, base + TileDiffXY(2, 3), APT_STAND, 0);

		SetupAircraftPool();
		Aircraft *v = CreateAircraft(VehicleID(10));
		v->targetairport = st->index;

		TileIndex touchdown = base;
		TileIndex goal = base + TileDiffXY(2, 3);
		REQUIRE(TryReserveLandingChain(v, st, touchdown, goal));

		/* Rollout runway atomically reserved. */
		for (int i = 0; i < 3; i++) {
			CHECK(IsModularAirportTileReservedBy(base + TileDiffXY(i, 0), v->index));
		}
		/* Transit runway atomically reserved too (full row 2). */
		for (int i = 0; i < 3; i++) {
			CHECK(IsModularAirportTileReservedBy(base + TileDiffXY(i, 2), v->index));
		}
		/* Goal stand reserved (end-to-end walk reaches the goal). */
		CHECK(IsModularAirportTileReservedBy(goal, v->index));
	}

	SECTION("Fails when transit runway is held; rollout fully rolled back") {
		AddLargeRunway(st, base, 3, 0, RUF_DEFAULT);
		AddModularTile(st, base + TileDiffXY(0, 1), APT_APRON, 0);
		AddModularTile(st, base + TileDiffXY(1, 1), APT_APRON, 0);
		AddModularTile(st, base + TileDiffXY(2, 1), APT_APRON, 0);
		AddLargeRunway(st, base + TileDiffXY(0, 2), 3, 0, RUF_DEFAULT);
		AddModularTile(st, base + TileDiffXY(0, 3), APT_APRON, 0);
		AddModularTile(st, base + TileDiffXY(1, 3), APT_APRON, 0);
		AddModularTile(st, base + TileDiffXY(2, 3), APT_STAND, 0);

		SetupAircraftPool();
		Aircraft *v = CreateAircraft(VehicleID(10));
		v->targetairport = st->index;

		/* Pre-occupy the transit runway. */
		CreateBlockerOnTile(st, VehicleID(11), base + TileDiffXY(0, 2));

		TileIndex touchdown = base;
		TileIndex goal = base + TileDiffXY(2, 3);
		CHECK_FALSE(TryReserveLandingChain(v, st, touchdown, goal));

		/* Rollout runway released — no half-commit. */
		for (int i = 0; i < 3; i++) {
			CHECK_FALSE(IsModularAirportTileReservedBy(base + TileDiffXY(i, 0), v->index));
		}
		CHECK(v->modular_runway_reservation.empty());
		/* Blocker's reservation on the transit runway untouched. */
		CHECK(IsModularAirportTileReservedBy(base + TileDiffXY(0, 2), VehicleID(11)));
	}

	SECTION("Stops at first ONE_WAY tile") {
		/* Layout:
		 *   Row 0: RWY_END RWY_5 RWY_END         (rollout runway)
		 *   Row 1: APRON   APRON APRON           (FREE_MOVE)
		 *   Row 2: APRON   APRON APRON_ONEWAY    (ONE_WAY tile at (2,2))
		 *   Row 3: APRON   APRON STAND_GOAL
		 * Path: rollout(2,0) → (2,1) → (2,2)[ONE_WAY] → (2,3)[goal].
		 * Walk reserves up to and including (2,2), then stops. (2,3) is NOT reserved. */
		AddLargeRunway(st, base, 3, 0, RUF_DEFAULT);
		AddModularTile(st, base + TileDiffXY(0, 1), APT_APRON, 0);
		AddModularTile(st, base + TileDiffXY(1, 1), APT_APRON, 0);
		AddModularTile(st, base + TileDiffXY(2, 1), APT_APRON, 0);
		AddModularTile(st, base + TileDiffXY(0, 2), APT_APRON, 0);
		AddModularTile(st, base + TileDiffXY(1, 2), APT_APRON, 0);
		ModularAirportTileData *one_way = AddModularTileWithData(st, base + TileDiffXY(2, 2), APT_APRON, 0);
		one_way->one_way_taxi = true;
		one_way->user_taxi_dir_mask = 0x04; // allow entry from (2,1) i.e. South (going down +Y)
		AddModularTile(st, base + TileDiffXY(2, 3), APT_STAND, 0);

		SetupAircraftPool();
		Aircraft *v = CreateAircraft(VehicleID(10));
		v->targetairport = st->index;

		TileIndex touchdown = base;
		TileIndex goal = base + TileDiffXY(2, 3);
		REQUIRE(TryReserveLandingChain(v, st, touchdown, goal));

		/* Rollout runway reserved. */
		for (int i = 0; i < 3; i++) {
			CHECK(IsModularAirportTileReservedBy(base + TileDiffXY(i, 0), v->index));
		}
		/* (2,1) FREE_MOVE reserved (segment before ONE_WAY). */
		CHECK(IsModularAirportTileReservedBy(base + TileDiffXY(2, 1), v->index));
		/* (2,2) ONE_WAY entry reserved — the safe stop. */
		CHECK(IsModularAirportTileReservedBy(base + TileDiffXY(2, 2), v->index));
		/* (2,3) goal NOT reserved — walk stopped at ONE_WAY. */
		CHECK_FALSE(IsModularAirportTileReservedBy(base + TileDiffXY(2, 3), v->index));
	}
}

TEST_CASE("ModularAirportTransitRunwayContract")
{
	Map::Allocate(64, 64);
	TileIndex base = TileXY(10, 10);
	Station *st = SetupModularAirport(base, 10, 10);
	REQUIRE(st != nullptr);

	/* Layout: aircraft taxis +Y across a transit runway to a stand.
	 *   Row 1: APRON(2,1)            <- start
	 *   Row 2: RWY_END RWY_5 RWY_END <- transit runway (single resource)
	 *   Row 3: APRON(2,3)            <- transit grass on the far side (NOT a safe stop)
	 *   Row 4: STAND(2,4)            <- goal (the only far-side safe stop)
	 * Path: (2,1) -> (2,2)[RWY] -> (2,3)[APRON] -> (2,4)[STAND]. */
	AddModularTile(st, base + TileDiffXY(2, 1), APT_APRON, 0);
	AddLargeRunway(st, base + TileDiffXY(0, 2), 3, 0, RUF_DEFAULT);
	AddModularTile(st, base + TileDiffXY(2, 3), APT_APRON, 0);
	AddModularTile(st, base + TileDiffXY(2, 4), APT_STAND, 0);

	const TileIndex start = base + TileDiffXY(2, 1);
	const TileIndex goal = base + TileDiffXY(2, 4);

	auto setup_aircraft_on_path = [&](VehicleID id) -> Aircraft * {
		Aircraft *v = CreateAircraft(id);
		v->targetairport = st->index;
		v->tile = start;
		v->ground_path_goal = goal;
		TaxiPath path = BuildTaxiPath(st, start, goal, v, true);
		REQUIRE(path.valid);
		REQUIRE_FALSE(path.segments.empty());
		v->taxi_path = std::make_unique<TaxiPath>(std::move(path));
		v->taxi_path_index = 0;
		v->taxi_current_segment = FindTaxiSegmentIndex(v->taxi_path.get(), 0);
		return v;
	};

	auto runway_segment = [&](Aircraft *v) -> uint8_t {
		for (uint8_t s = 0; s < v->taxi_path->segments.size(); s++) {
			if (v->taxi_path->segments[s].type == TaxiSegmentType::RUNWAY) return s;
		}
		FAIL("no runway segment on path");
		return 0;
	};

	SECTION("Reserves the full crossing chain to the far-side safe stop") {
		SetupAircraftPool();
		Aircraft *v = setup_aircraft_on_path(VehicleID(10));
		REQUIRE(TryReserveTaxiSegment(v, st, runway_segment(v)));

		/* Whole transit runway atomically reserved. */
		for (int i = 0; i < 3; i++) {
			CHECK(IsModularAirportTileReservedBy(base + TileDiffXY(i, 2), v->index));
		}
		/* Far-side transit grass reserved (would have been a strand point before). */
		CHECK(IsModularAirportTileReservedBy(base + TileDiffXY(2, 3), v->index));
		/* Goal stand reserved — chain reached the safe stop. */
		CHECK(IsModularAirportTileReservedBy(goal, v->index));
	}

	SECTION("Denies runway entry when the far side is blocked; runway untouched") {
		SetupAircraftPool();
		Aircraft *v = setup_aircraft_on_path(VehicleID(10));
		/* Block the transit grass past the runway: there is no reserved safe stop
		 * beyond, so the aircraft must wait BEFORE the runway, not strand on grass. */
		CreateBlockerOnTile(st, VehicleID(11), base + TileDiffXY(2, 3));

		CHECK_FALSE(TryReserveTaxiSegment(v, st, runway_segment(v)));

		/* No half-commit: the runway resource was never taken. */
		for (int i = 0; i < 3; i++) {
			CHECK_FALSE(IsModularAirportTileReservedBy(base + TileDiffXY(i, 2), v->index));
		}
		CHECK(v->modular_runway_reservation.empty());
		/* Blocker's reservation untouched. */
		CHECK(IsModularAirportTileReservedBy(base + TileDiffXY(2, 3), VehicleID(11)));
	}
}

TEST_CASE("ModularAirportCrash")
{
	Map::Allocate(64, 64);
	const TileIndex base = TileXY(10, 10);

	extern EnginePool _engine_pool;
	_engine_pool.CleanPool();
	/* Engine 0 = fast jet (AIR_FAST); engine 1 = small/non-fast plane. */
	const EngineID jet = CreateAircraftEngine(EngineID(0), AIR_FAST);
	const EngineID prop = CreateAircraftEngine(EngineID(1), 0);

	/* Save crash-related globals so this test cannot leak into others. */
	const bool saved_nojetcrash = _cheats.no_jetcrash.value;
	const uint8_t saved_plane_crashes = _settings_game.vehicle.plane_crashes;
	_cheats.no_jetcrash.value = false;

	/* Build a complete airport that satisfies the large-aircraft safety
	 * requirements (tower + big terminal + 6-tile landing & takeoff runway). */
	auto build_safe_airport = [&]() -> Station * {
		Station *st = SetupModularAirport(base, 10, 10);
		REQUIRE(st != nullptr);
		AddModularTile(st, base, APT_TOWER, 0);
		AddModularTile(st, base + TileDiffXY(1, 0), APT_ROUND_TERMINAL, 0);
		AddLargeRunway(st, base + TileDiffXY(0, 3), 6);
		REQUIRE(ModularAirportSupportsLargeAircraft(st));
		return st;
	};

	SECTION("Elevated overrun risk: fast jet only, on an unsafe airport") {
		Station *unsafe = SetupModularAirport(base, 10, 10); // empty -> unsafe
		REQUIRE(unsafe != nullptr);
		REQUIRE_FALSE(ModularAirportSupportsLargeAircraft(unsafe));

		SetupAircraftPool();
		Aircraft *v = CreateAircraft(VehicleID(0));
		v->engine_type = jet;

		/* Fast jet on an unsafe airport -> elevated risk. */
		CHECK(ModularAircraftHasElevatedOverrunRisk(v, unsafe));

		/* Non-fast plane on the same airport -> never elevated. */
		v->engine_type = prop;
		CHECK_FALSE(ModularAircraftHasElevatedOverrunRisk(v, unsafe));

		/* Helicopter -> never elevated (and engine is never dereferenced). */
		v->subtype = AIR_HELICOPTER;
		v->engine_type = jet;
		CHECK_FALSE(ModularAircraftHasElevatedOverrunRisk(v, unsafe));
	}

	SECTION("No elevated risk on a safe airport, even for a fast jet") {
		Station *safe = build_safe_airport();

		SetupAircraftPool();
		Aircraft *v = CreateAircraft(VehicleID(0));
		v->engine_type = jet;
		CHECK_FALSE(ModularAircraftHasElevatedOverrunRisk(v, safe));
	}

	SECTION("no_jetcrash cheat suppresses the elevated risk") {
		Station *unsafe = SetupModularAirport(base, 10, 10);
		REQUIRE(unsafe != nullptr);

		SetupAircraftPool();
		Aircraft *v = CreateAircraft(VehicleID(0));
		v->engine_type = jet;

		_cheats.no_jetcrash.value = true;
		CHECK_FALSE(ModularAircraftHasElevatedOverrunRisk(v, unsafe));
		_cheats.no_jetcrash.value = false;
	}

	SECTION("Helicopters never crash via the modular path") {
		Station *unsafe = SetupModularAirport(base, 10, 10);
		REQUIRE(unsafe != nullptr);

		SetupAircraftPool();
		Aircraft *v = CreateAircraft(VehicleID(0));
		v->subtype = AIR_HELICOPTER;
		v->engine_type = jet;

		/* Early return before any RNG roll, regardless of settings. */
		_settings_game.vehicle.plane_crashes = 4;
		CHECK_FALSE(MaybeCrashModularAircraft(v, unsafe));
	}

	SECTION("General crashes disabled: non-elevated planes never roll a crash") {
		Station *unsafe = SetupModularAirport(base, 10, 10);
		REQUIRE(unsafe != nullptr);

		/* plane_crashes == 0 means a non-elevated plane returns false before the
		 * RNG roll, so this is deterministic and never reaches CrashAirplane. */
		_settings_game.vehicle.plane_crashes = 0;

		SetupAircraftPool();
		Aircraft *v = CreateAircraft(VehicleID(0));

		/* Non-fast plane: not elevated, general crashes off -> no crash. */
		v->engine_type = prop;
		CHECK_FALSE(MaybeCrashModularAircraft(v, unsafe));

		/* Fast jet with the no_jetcrash cheat: not elevated either -> no crash. */
		v->engine_type = jet;
		_cheats.no_jetcrash.value = true;
		CHECK_FALSE(MaybeCrashModularAircraft(v, unsafe));
		_cheats.no_jetcrash.value = false;
	}

	_cheats.no_jetcrash.value = saved_nojetcrash;
	_settings_game.vehicle.plane_crashes = saved_plane_crashes;
}

TEST_CASE("ModularAirportCatchment")
{
	Map::Allocate(64, 64);
	const TileIndex base = TileXY(2, 2);

	SECTION("Empty / unsafe airport gets the minimum radius (4)") {
		Station *st = SetupModularAirport(base, 30, 30);
		REQUIRE(st != nullptr);
		CHECK(GetModularAirportCatchmentRadius(st) == 4);

		/* Infrastructure that is not yet large-safe stays at the minimum. */
		AddModularTile(st, base, APT_TOWER, 0);
		AddModularTile(st, base + TileDiffXY(1, 0), APT_ROUND_TERMINAL, 0);
		CHECK(GetModularAirportCatchmentRadius(st) == 4);
	}

	SECTION("Large-aircraft-safe airport gets radius 5") {
		Station *st = SetupModularAirport(base, 30, 30);
		REQUIRE(st != nullptr);
		AddModularTile(st, base, APT_TOWER, 0);
		AddModularTile(st, base + TileDiffXY(1, 0), APT_ROUND_TERMINAL, 0);
		/* A single 6-tile paved runway, landing + takeoff (RUF_DEFAULT). */
		AddLargeRunway(st, base + TileDiffXY(0, 3), 6);

		REQUIRE(ModularAirportSupportsLargeAircraft(st));
		CHECK(GetModularAirportCatchmentRadius(st) == 5);
	}

	SECTION("Two 6-tile paved runways reach radius 6") {
		Station *st = SetupModularAirport(base, 30, 30);
		REQUIRE(st != nullptr);
		AddModularTile(st, base, APT_TOWER, 0);
		AddModularTile(st, base + TileDiffXY(1, 0), APT_ROUND_TERMINAL, 0);
		AddLargeRunway(st, base + TileDiffXY(0, 3), 6);
		CHECK(GetModularAirportCatchmentRadius(st) == 5);

		/* Second 6-tile runway, two rows down so it is a distinct segment. */
		AddLargeRunway(st, base + TileDiffXY(0, 5), 6);
		CHECK(GetModularAirportCatchmentRadius(st) == 6);
	}

	SECTION("Hub infrastructure reaches radius 8") {
		Station *st = SetupModularAirport(base, 30, 30);
		REQUIRE(st != nullptr);
		AddModularTile(st, base, APT_TOWER, 0);
		/* Two 7-tile paved runways. */
		AddLargeRunway(st, base + TileDiffXY(0, 3), 7);
		AddLargeRunway(st, base + TileDiffXY(0, 5), 7);
		/* Three big terminals. */
		AddModularTile(st, base + TileDiffXY(1, 0), APT_ROUND_TERMINAL, 0);
		AddModularTile(st, base + TileDiffXY(2, 0), APT_BUILDING_1, 0);
		AddModularTile(st, base + TileDiffXY(3, 0), APT_BUILDING_2, 0);

		/* Still only 6 until the helipad + radar are present. */
		CHECK(GetModularAirportCatchmentRadius(st) == 6);

		AddModularTile(st, base + TileDiffXY(4, 0), APT_HELIPAD_1, 0);
		CHECK(GetModularAirportCatchmentRadius(st) == 6);

		AddModularTile(st, base + TileDiffXY(5, 0), APT_RADAR_FENCE_NE, 0);
		CHECK(GetModularAirportCatchmentRadius(st) == 8);
	}

	SECTION("Four 8-tile paved runways reach radius 10") {
		Station *st = SetupModularAirport(base, 30, 30);
		REQUIRE(st != nullptr);
		AddModularTile(st, base, APT_TOWER, 0);
		AddModularTile(st, base + TileDiffXY(1, 0), APT_ROUND_TERMINAL, 0);
		AddModularTile(st, base + TileDiffXY(2, 0), APT_BUILDING_1, 0);
		AddModularTile(st, base + TileDiffXY(3, 0), APT_BUILDING_2, 0);
		AddModularTile(st, base + TileDiffXY(4, 0), APT_HELIPAD_1, 0);
		AddModularTile(st, base + TileDiffXY(5, 0), APT_RADAR_FENCE_NE, 0);

		/* Four 8-tile paved runways, each on its own row (two-row spacing). */
		AddLargeRunway(st, base + TileDiffXY(0, 3), 8);
		AddLargeRunway(st, base + TileDiffXY(0, 5), 8);
		CHECK(GetModularAirportCatchmentRadius(st) == 8);

		AddLargeRunway(st, base + TileDiffXY(0, 7), 8);
		CHECK(GetModularAirportCatchmentRadius(st) == 8);

		AddLargeRunway(st, base + TileDiffXY(0, 9), 8);
		CHECK(GetModularAirportCatchmentRadius(st) == 10);
	}

	SECTION("Grass (small-family) runways are not paved and do not raise the tier") {
		Station *st = SetupModularAirport(base, 30, 30);
		REQUIRE(st != nullptr);
		AddModularTile(st, base, APT_TOWER, 0);
		AddModularTile(st, base + TileDiffXY(1, 0), APT_ROUND_TERMINAL, 0);
		AddLargeRunway(st, base + TileDiffXY(0, 3), 6);
		CHECK(GetModularAirportCatchmentRadius(st) == 5);

		/* A small grass runway is not large-family, so it does not count as a
		 * second paved runway: the radius stays at 5. */
		for (uint i = 0; i < 6; i++) {
			AddModularTile(st, base + TileDiffXY(i, 5), APT_RUNWAY_SMALL_MIDDLE, 0);
		}
		CHECK(GetModularAirportCatchmentRadius(st) == 5);
	}
}
