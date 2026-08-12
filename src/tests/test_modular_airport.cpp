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
#include "../airport_template.h"
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
	st->airport.MarkLayoutDirty();
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

		/* The owner has to actually exist: a reservation held by a vehicle that does
		 * not is a dead claim, and the pathfinder deliberately routes past those. */
		Aircraft *other = CreateAircraft(VehicleID(4));
		other->tile = base + TileDiffXY(1, 0);
		SetModularAirportTileReservationOwner(base + TileDiffXY(1, 0), other->index);

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

/**
 * The template manager shows the catchment of a template before it is placed, so
 * a template must report exactly what the airport it was saved from reports.
 */
TEST_CASE("ModularAirportTemplateCatchment")
{
	Map::Allocate(64, 64);
	const TileIndex base = TileXY(2, 2);

	auto TemplateFromStation = [](const Station *st) {
		AirportTemplate templ;
		uint min_x = UINT_MAX;
		uint min_y = UINT_MAX;
		for (const ModularAirportTileData &d : *st->airport.modular_tile_data) {
			min_x = std::min<uint>(min_x, TileX(d.tile));
			min_y = std::min<uint>(min_y, TileY(d.tile));
		}
		for (const ModularAirportTileData &d : *st->airport.modular_tile_data) {
			AirportTemplateTile t{};
			t.dx = ClampTo<uint16_t>(TileX(d.tile) - min_x);
			t.dy = ClampTo<uint16_t>(TileY(d.tile) - min_y);
			t.piece_type = d.piece_type;
			t.rotation = d.rotation;
			t.runway_flags = IsModularRunwayPiece(d.piece_type) ? d.runway_flags : 0;
			templ.tiles.push_back(t);
		}
		return templ;
	};

	SECTION("An unsafe layout reports the minimum radius") {
		Station *st = SetupModularAirport(base, 30, 30);
		REQUIRE(st != nullptr);
		AddModularTile(st, base, APT_TOWER, 0);
		AddModularTile(st, base + TileDiffXY(1, 0), APT_ROUND_TERMINAL, 0);

		CHECK(TemplateFromStation(st).GetCatchmentRadius() == GetModularAirportCatchmentRadius(st));
		CHECK(TemplateFromStation(st).GetCatchmentRadius() == 4);
	}

	SECTION("Radius matches the source airport across every tier") {
		Station *st = SetupModularAirport(base, 30, 30);
		REQUIRE(st != nullptr);
		AddModularTile(st, base, APT_TOWER, 0);
		AddModularTile(st, base + TileDiffXY(1, 0), APT_ROUND_TERMINAL, 0);
		AddLargeRunway(st, base + TileDiffXY(0, 3), 6);
		CHECK(TemplateFromStation(st).GetCatchmentRadius() == 5);

		AddLargeRunway(st, base + TileDiffXY(0, 5), 6);
		CHECK(TemplateFromStation(st).GetCatchmentRadius() == 6);

		/* Grow both runways to 7 and add the remaining hub pieces. */
		Station *hub = SetupModularAirport(base, 30, 30);
		REQUIRE(hub != nullptr);
		AddModularTile(hub, base, APT_TOWER, 0);
		AddLargeRunway(hub, base + TileDiffXY(0, 3), 7);
		AddLargeRunway(hub, base + TileDiffXY(0, 5), 7);
		AddModularTile(hub, base + TileDiffXY(1, 0), APT_ROUND_TERMINAL, 0);
		AddModularTile(hub, base + TileDiffXY(2, 0), APT_BUILDING_1, 0);
		AddModularTile(hub, base + TileDiffXY(3, 0), APT_BUILDING_2, 0);
		AddModularTile(hub, base + TileDiffXY(4, 0), APT_HELIPAD_1, 0);
		AddModularTile(hub, base + TileDiffXY(5, 0), APT_RADAR_FENCE_NE, 0);
		CHECK(TemplateFromStation(hub).GetCatchmentRadius() == GetModularAirportCatchmentRadius(hub));
		CHECK(TemplateFromStation(hub).GetCatchmentRadius() == 8);
	}

	SECTION("Vertical runways are measured on their own axis") {
		Station *st = SetupModularAirport(base, 30, 30);
		REQUIRE(st != nullptr);
		AddModularTile(st, base + TileDiffXY(9, 0), APT_TOWER, 0);
		AddModularTile(st, base + TileDiffXY(9, 1), APT_ROUND_TERMINAL, 0);
		AddLargeRunway(st, base + TileDiffXY(0, 0), 6, 1);
		AddLargeRunway(st, base + TileDiffXY(2, 0), 6, 1);

		CHECK(TemplateFromStation(st).GetCatchmentRadius() == GetModularAirportCatchmentRadius(st));
		CHECK(TemplateFromStation(st).GetCatchmentRadius() == 6);
	}
}

TEST_CASE("ModularAirportRunwayGoalCrossing")
{
	Map::Allocate(64, 64);
	TileIndex base = TileXY(10, 10);
	Station *st = SetupModularAirport(base, 10, 10);
	REQUIRE(st != nullptr);

	/* Bardingbury-shaped layout: the aircraft's goal is itself a runway tile,
	 * reachable only by crossing a second runway.
	 *   Row 1: APRON(2,1)            <- start (a stand would be the real safe stop)
	 *   Row 2: RWY_END RWY_5 RWY_END <- runway A, crossed in transit
	 *   Row 3: APRON(2,3)            <- transit apron, not a safe stop
	 *   Row 4: RWY_END RWY_5 RWY_END <- runway B; (2,4) is the goal
	 * There is no stand, hangar, helipad or one-way tile anywhere past runway A,
	 * so the goal is the only thing that can terminate the crossing chain. */
	AddModularTile(st, base + TileDiffXY(2, 1), APT_APRON, 0);
	AddLargeRunway(st, base + TileDiffXY(0, 2), 3, 0, RUF_DEFAULT);
	AddModularTile(st, base + TileDiffXY(2, 3), APT_APRON, 0);
	AddLargeRunway(st, base + TileDiffXY(0, 4), 3, 0, RUF_DEFAULT);

	const TileIndex start = base + TileDiffXY(2, 1);
	const TileIndex goal = base + TileDiffXY(2, 4);

	auto setup_aircraft = [&](VehicleID id, uint8_t ground_target) -> Aircraft * {
		Aircraft *v = CreateAircraft(id);
		v->targetairport = st->index;
		v->tile = start;
		v->ground_path_goal = goal;
		v->modular_ground_target = ground_target;
		TaxiPath path = BuildTaxiPath(st, start, goal, v, true);
		REQUIRE(path.valid);
		REQUIRE(path.tiles.back() == goal);
		v->taxi_path = std::make_unique<TaxiPath>(std::move(path));
		v->taxi_path_index = 0;
		v->taxi_current_segment = FindTaxiSegmentIndex(v->taxi_path.get(), 0);
		return v;
	};

	/* First runway segment on the path = runway A, the one being crossed. */
	auto transit_runway_segment = [&](Aircraft *v) -> uint8_t {
		for (uint8_t s = 0; s < v->taxi_path->segments.size(); s++) {
			if (v->taxi_path->segments[s].type == TaxiSegmentType::RUNWAY) return s;
		}
		FAIL("no runway segment on path");
		return 0;
	};

	SECTION("A goal on a runway terminates the crossing chain") {
		SetupAircraftPool();
		Aircraft *v = setup_aircraft(VehicleID(10), MGT_HELI_TAKEOFF_TILE);

		/* Regression: this used to be denied forever on a completely empty airport,
		 * because the chain walk skipped runway tiles before testing them for being
		 * the goal, and the goal was a runway tile. */
		REQUIRE(TryReserveTaxiSegment(v, st, transit_runway_segment(v)));

		/* Runway A held atomically. */
		CheckReservedBy({base + TileDiffXY(0, 2), base + TileDiffXY(1, 2), base + TileDiffXY(2, 2)}, v->index);
		/* The transit apron between the runways. */
		CheckReservedBy({base + TileDiffXY(2, 3)}, v->index);
		/* Runway B held atomically too — the goal's own runway folds into the chain. */
		CheckReservedBy({base + TileDiffXY(0, 4), base + TileDiffXY(1, 4), goal}, v->index);
	}

	SECTION("A blocked chain past a runway goal denies entry without half-committing") {
		SetupAircraftPool();
		Aircraft *blocker = CreateAircraft(VehicleID(11));
		blocker->targetairport = st->index;
		blocker->tile = base + TileDiffXY(2, 3);
		SetTaxiReservation(blocker, base + TileDiffXY(2, 3));

		Aircraft *v = setup_aircraft(VehicleID(10), MGT_HELI_TAKEOFF_TILE);
		CHECK_FALSE(TryReserveTaxiSegment(v, st, transit_runway_segment(v)));

		/* Nothing taken: the aircraft waits before runway A. */
		CHECK_FALSE(IsModularAirportTileReservedBy(base + TileDiffXY(0, 2), v->index));
		CHECK_FALSE(IsModularAirportTileReservedBy(base + TileDiffXY(2, 2), v->index));
		CHECK_FALSE(IsModularAirportTileReservedBy(goal, v->index));
	}

	SECTION("A runway crossed on the way to a takeoff runway is transit, not terminal") {
		SetupAircraftPool();
		Aircraft *v = setup_aircraft(VehicleID(10), MGT_RUNWAY_TAKEOFF);
		v->modular_takeoff_tile = goal;

		REQUIRE(TryReserveTaxiSegment(v, st, transit_runway_segment(v)));

		/* Runway A must not be entered on its own: a takeoff target used to make
		 * every runway on the path "terminal", letting the aircraft halt on a
		 * runway it was only crossing. The full chain past it is held instead. */
		CheckReservedBy({base + TileDiffXY(2, 3)}, v->index);
		CheckReservedBy({goal}, v->index);
	}
}

TEST_CASE("ModularAirportSelfReservedStandRouting")
{
	Map::Allocate(64, 64);
	TileIndex base = TileXY(10, 10);
	Station *st = SetupModularAirport(base, 10, 10);
	REQUIRE(st != nullptr);

	/* Gretown-shaped layout: a helipad whose only non-runway exit is a stand.
	 *   (2,0) HANGAR  <- goal (service)
	 *   (2,1) APRON
	 *   (2,2) STAND   <- the only way off the helipad
	 *   (2,3) HELIPAD <- aircraft parks here
	 * Everything else is left out of the layout, so there is no alternative route. */
	AddModularTile(st, base + TileDiffXY(2, 0), APT_DEPOT_SE, 0);
	AddModularTile(st, base + TileDiffXY(2, 1), APT_APRON, 0);
	AddModularTile(st, base + TileDiffXY(2, 2), APT_STAND, 0);
	AddModularTile(st, base + TileDiffXY(2, 3), APT_HELIPAD_2, 0);

	const TileIndex helipad = base + TileDiffXY(2, 3);
	const TileIndex stand = base + TileDiffXY(2, 2);
	const TileIndex hangar = base + TileDiffXY(2, 0);

	SECTION("A stand reserved by the aircraft itself does not block its own route") {
		SetupAircraftPool();
		Aircraft *v = CreateAircraft(VehicleID(10));
		v->targetairport = st->index;
		v->tile = helipad;
		v->ground_path_goal = hangar;

		/* Reservations outlive the path that created them: the aircraft still holds
		 * the pass-through stand from an earlier, now-discarded path. */
		SetTaxiReservation(v, stand);
		REQUIRE(IsModularAirportTileReservedBy(stand, v->index));

		const AirportGroundPath path = FindAirportGroundPath(st, helipad, hangar, v, false, false);
		CHECK(path.found);
	}

	SECTION("A stand reserved by another aircraft still blocks the route") {
		SetupAircraftPool();
		Aircraft *other = CreateAircraft(VehicleID(11));
		other->targetairport = st->index;
		other->tile = stand;
		SetTaxiReservation(other, stand);

		Aircraft *v = CreateAircraft(VehicleID(10));
		v->targetairport = st->index;
		v->tile = helipad;
		v->ground_path_goal = hangar;

		const AirportGroundPath path = FindAirportGroundPath(st, helipad, hangar, v, false, false);
		CHECK_FALSE(path.found);
	}

	SECTION("A stand reserved by a vehicle that no longer exists does not block") {
		SetupAircraftPool();
		Aircraft *other = CreateAircraft(VehicleID(11));
		other->targetairport = st->index;
		other->tile = stand;
		SetTaxiReservation(other, stand);
		/* Hand the reservation to a vehicle that was never allocated: the state left
		 * behind when the owner is gone and nothing remains to release the tile. */
		SetModularAirportTileReservationOwner(stand, VehicleID(99));

		Aircraft *v = CreateAircraft(VehicleID(10));
		v->targetairport = st->index;
		v->tile = helipad;
		v->ground_path_goal = hangar;

		/* The reservation code clears this case (IsTaxiTileReservedByOther); the
		 * pathfinder may not mutate, so it must at least route past it rather than
		 * treat a dead claim as a permanent wall. */
		const AirportGroundPath path = FindAirportGroundPath(st, helipad, hangar, v, false, false);
		CHECK(path.found);
	}
}

TEST_CASE("ModularAirportHelicopterParkingPolicy")
{
	Map::Allocate(64, 64);
	TileIndex base = TileXY(10, 10);

	const TileIndex stand = base + TileDiffXY(2, 1);
	const TileIndex pad = base + TileDiffXY(2, 2);

	SECTION("Helicopter takes no stand at an airport that has helipads") {
		Station *st = SetupModularAirport(base, 10, 10);
		REQUIRE(st != nullptr);
		AddModularTile(st, stand, APT_STAND, 0);
		AddModularTile(st, pad, APT_HELIPAD_2, 0);
		REQUIRE(ModularAirportHasHelipad(st));

		SetupAircraftPool();
		Aircraft *heli = CreateAircraft(VehicleID(10));
		heli->targetairport = st->index;
		heli->subtype = AIR_HELICOPTER;
		heli->tile = pad;

		/* Stock parity: with helipads present the helicopter waits for one rather than
		 * occupying a stand, even while the stand sits empty. */
		CHECK(FindFreeModularTerminal(st, heli) == INVALID_TILE);

		/* ...unless the caller is a ground-safety path that must not strand it. */
		CHECK(FindFreeModularTerminal(st, heli, INVALID_TILE, true) == stand);

		/* Fixed-wing aircraft are unaffected. */
		Aircraft *plane = CreateAircraft(VehicleID(11));
		plane->targetairport = st->index;
		plane->subtype = AIR_AIRCRAFT;
		plane->tile = stand;
		CHECK(FindFreeModularTerminal(st, plane) == stand);
	}

	SECTION("Helicopter uses a stand at an airport with no helipads") {
		Station *st = SetupModularAirport(base, 10, 10);
		REQUIRE(st != nullptr);
		AddModularTile(st, stand, APT_STAND, 0);
		AddModularTile(st, pad, APT_APRON, 0);
		REQUIRE_FALSE(ModularAirportHasHelipad(st));

		SetupAircraftPool();
		Aircraft *heli = CreateAircraft(VehicleID(10));
		heli->targetairport = st->index;
		heli->subtype = AIR_HELICOPTER;
		heli->tile = pad;

		CHECK(FindFreeModularTerminal(st, heli) == stand);
	}
}

TEST_CASE("ModularAirportRunwayRestKeepsSafeStop")
{
	Map::Allocate(64, 64);
	TileIndex base = TileXY(10, 10);
	Station *st = SetupModularAirport(base, 10, 10);
	REQUIRE(st != nullptr);

	/* A runway with a one-way queueing tile beyond it — the shape a no-ground-goal
	 * landing commits against: reserve the runway, reserve the buffer to queue on. */
	AddLargeRunway(st, base + TileDiffXY(0, 2), 3, 0, RUF_DEFAULT);
	ModularAirportTileData *oneway = AddModularTileWithData(st, base + TileDiffXY(2, 3), APT_APRON, 0);
	oneway->one_way_taxi = true;
	oneway->user_taxi_dir_mask = 0x04;
	AddModularTile(st, base + TileDiffXY(2, 4), APT_STAND, 0);

	const TileIndex rollout = base + TileDiffXY(2, 2);
	const TileIndex buffer = base + TileDiffXY(2, 3);
	REQUIRE(IsModularSafeStopTile(st, buffer));

	SECTION("A safe stop reserved from a runway survives reconciliation") {
		SetupAircraftPool();
		Aircraft *v = CreateAircraft(VehicleID(10));
		v->targetairport = st->index;
		v->tile = rollout;

		/* No taxi_path and no landing_chain_path — exactly what the no-ground-goal
		 * landing branch leaves behind. Nothing else justifies keeping the buffer. */
		SetTaxiReservation(v, buffer);
		REQUIRE(v->taxi_path == nullptr);
		REQUIRE(v->landing_chain_path == nullptr);

		std::vector<TileIndex> keep_set;
		BuildReservationKeepSet(v, st, keep_set);

		/* Regression: the buffer used to fall out of the keep set, so the reconciler
		 * released the very reservation the landing was permitted against, stranding
		 * the aircraft on the runway owning nothing. */
		CHECK(std::find(keep_set.begin(), keep_set.end(), buffer) != keep_set.end());
	}

	SECTION("The rule does not apply once the aircraft is on a safe stop") {
		SetupAircraftPool();
		Aircraft *v = CreateAircraft(VehicleID(10));
		v->targetairport = st->index;
		v->tile = buffer;

		TileIndex other = base + TileDiffXY(2, 4);
		SetTaxiReservation(v, other);

		std::vector<TileIndex> keep_set;
		BuildReservationKeepSet(v, st, keep_set);

		/* Standing somewhere it may wait indefinitely, normal reconciliation applies
		 * and an unjustified claim is released rather than pinned. */
		CHECK(std::find(keep_set.begin(), keep_set.end(), other) == keep_set.end());
	}
}

TEST_CASE("ModularAirportUnstackParking")
{
	Map::Allocate(64, 64);
	TileIndex base = TileXY(10, 10);
	Station *st = SetupModularAirport(base, 10, 10);
	REQUIRE(st != nullptr);

	/* An airport that has a helipad, so the stock parking rule refuses stands to
	 * helicopters everywhere except this unstacking path. `occupied` is where the
	 * arriving aircraft finds itself stacked; `free_stand` is the way out. */
	const TileIndex occupied = base + TileDiffXY(2, 1);
	const TileIndex free_stand = base + TileDiffXY(2, 2);
	const TileIndex pad = base + TileDiffXY(2, 3);
	AddModularTile(st, occupied, APT_STAND, 0);
	AddModularTile(st, free_stand, APT_STAND, 0);
	AddModularTile(st, pad, APT_HELIPAD_2, 0);
	REQUIRE(ModularAirportHasHelipad(st));

	/* Park a squatter on `occupied` so the arriving aircraft cannot simply stay. */
	auto squat = [&](VehicleID id, TileIndex tile, uint8_t subtype) {
		Aircraft *other = CreateAircraft(id);
		other->targetairport = st->index;
		other->subtype = subtype;
		other->tile = tile;
		SetTaxiReservation(other, tile);
	};

	auto arriving = [&](uint8_t subtype) -> Aircraft * {
		Aircraft *v = CreateAircraft(VehicleID(10));
		v->targetairport = st->index;
		v->subtype = subtype;
		v->tile = occupied;
		v->modular_ground_target = MGT_TERMINAL;
		return v;
	};

	SECTION("A helicopter on MGT_TERMINAL is unstacked onto a stand") {
		SetupAircraftPool();
		squat(VehicleID(11), occupied, AIR_AIRCRAFT);
		squat(VehicleID(12), pad, AIR_HELICOPTER);

		/* Regression: MGT_TERMINAL took the plain lookup, which refuses a stand to a
		 * helicopter wherever helipads exist. Nothing was then free, so the caller fell
		 * through and stacked two aircraft on one tile — while HandleModularEndLanding
		 * and the helipad fallback both legitimately produce this exact state. */
		uint8_t target = MGT_NONE;
		CHECK(FindModularUnstackParkingTile(st, arriving(AIR_HELICOPTER), &target) == free_stand);
		CHECK(target == MGT_TERMINAL);
	}

	SECTION("A free helipad is preferred over a stand") {
		SetupAircraftPool();
		squat(VehicleID(11), occupied, AIR_AIRCRAFT);

		uint8_t target = MGT_NONE;
		CHECK(FindModularUnstackParkingTile(st, arriving(AIR_HELICOPTER), &target) == pad);
		CHECK(target == MGT_HELIPAD);
	}

	SECTION("A fixed-wing aircraft never gets a helipad") {
		SetupAircraftPool();
		squat(VehicleID(11), occupied, AIR_HELICOPTER);

		uint8_t target = MGT_NONE;
		CHECK(FindModularUnstackParkingTile(st, arriving(AIR_AIRCRAFT), &target) == free_stand);
		CHECK(target == MGT_TERMINAL);
	}

	SECTION("Nothing free yields INVALID_TILE and leaves the target alone") {
		SetupAircraftPool();
		squat(VehicleID(11), occupied, AIR_AIRCRAFT);
		squat(VehicleID(12), pad, AIR_HELICOPTER);
		squat(VehicleID(13), free_stand, AIR_AIRCRAFT);

		uint8_t target = MGT_NONE;
		CHECK(FindModularUnstackParkingTile(st, arriving(AIR_HELICOPTER), &target) == INVALID_TILE);
		CHECK(target == MGT_NONE);
	}
}

TEST_CASE("ModularAirportHeliServiceTile")
{
	Map::Allocate(64, 64);
	TileIndex base = TileXY(10, 10);

	/* A rooftop heliport in the corner with no taxiable neighbour, and a hangar
	 * reached over two apron tiles. `near_apron` touches the hangar and so is never
	 * a parking candidate (hangars count as buildings); `far_apron` is the one a
	 * helicopter may land on. The hangar faces +y, so its entrance is `near_apron`. */
	const TileIndex heliport = base + TileDiffXY(0, 0);
	const TileIndex hangar = base + TileDiffXY(5, 2);
	const TileIndex near_apron = base + TileDiffXY(5, 3);
	const TileIndex far_apron = base + TileDiffXY(5, 4);
	const TileIndex pad = base + TileDiffXY(5, 5);

	auto build = [&](bool with_heliport, bool with_hangar, bool with_connected_pad) {
		Station *st = SetupModularAirport(base, 10, 10);
		REQUIRE(st != nullptr);
		if (with_heliport) AddModularTile(st, heliport, APT_HELIPORT, 0);
		if (with_hangar) AddModularTile(st, hangar, APT_DEPOT_SE, 0);
		AddModularTile(st, near_apron, APT_APRON, 0);
		AddModularTile(st, far_apron, APT_APRON, 0);
		if (with_connected_pad) AddModularTile(st, pad, APT_HELIPAD_2, 0);
		return st;
	};

	SECTION("A cut-off heliport yields a service tile that reaches the hangar") {
		Station *st = build(true, true, false);
		EnsureModularHeliTilesValid(st);

		/* Landing on the heliport for service is a trap: no ground path off it at
		 * all, so the helicopter lifts off and re-lands forever. */
		REQUIRE_FALSE(FindAirportGroundPath(st, heliport, hangar, nullptr).found);
		CHECK(st->airport.modular_heli_service_tile == far_apron);
		CHECK(FindAirportGroundPath(st, far_apron, hangar, nullptr).found);
	}

	SECTION("The choice is layout-pure: nearest apron wins, crossing cache untouched") {
		Station *st = build(true, true, false);
		/* Two more parkable aprons, further from the hangar, so the ranking has
		 * something to decide. Distances to the hangar at (5,2): far_apron 2, (5,5) 3,
		 * distant_apron 4. */
		const TileIndex distant_apron = base + TileDiffXY(5, 6);
		AddModularTile(st, base + TileDiffXY(5, 5), APT_APRON, 0);
		AddModularTile(st, distant_apron, APT_APRON, 0);

		ClearModularAirportCrossingPathCache();
		EnsureModularHeliTilesValid(st);
		REQUIRE(FindAirportGroundPath(st, distant_apron, hangar, nullptr).found);
		CHECK(st->airport.modular_heli_service_tile == far_apron);

		/* The probe must not write to the crossing cache. That cache is saved, synced
		 * state, and this computation runs lazily at a moment each client picks for
		 * itself — so a probe that inserted keys would mutate shared state off a
		 * client-local schedule. FindAirportGroundPath writes by default; the fix is
		 * that this caller opts out. */
		ClearModularAirportCrossingPathCache();
		st->airport.MarkLayoutDirty();
		EnsureModularHeliTilesValid(st);
		CHECK(_modular_airport_crossing_required_path_cache.empty());

		/* And the answer must not depend on what the cache already learned. A key here
		 * sends that pair down the crossing pass, which reports a different cost for an
		 * unchanged layout — ranking on cost would let the winner move. */
		const auto crossing_key = [](TileIndex start, TileIndex goal) {
			return (static_cast<uint64_t>(start.base()) << 32) | static_cast<uint64_t>(goal.base());
		};
		_modular_airport_crossing_required_path_cache.push_back(crossing_key(far_apron, hangar));
		NormalizeModularAirportCrossingPathCache();

		st->airport.MarkLayoutDirty();
		EnsureModularHeliTilesValid(st);
		CHECK(st->airport.modular_heli_service_tile == far_apron);

		ClearModularAirportCrossingPathCache();
	}

	SECTION("A helipad that reaches the hangar leaves the normal flow alone") {
		Station *st = build(true, true, true);
		REQUIRE(FindAirportGroundPath(st, pad, hangar, nullptr).found);
		EnsureModularHeliTilesValid(st);

		CHECK(st->airport.modular_heli_service_tile == INVALID_TILE);
	}

	SECTION("No hangar means there is nothing to route to") {
		Station *st = build(true, false, false);
		EnsureModularHeliTilesValid(st);

		CHECK(st->airport.modular_heli_service_tile == INVALID_TILE);
	}

	SECTION("Without helipads the ordinary computed heli tile already applies") {
		Station *st = build(false, true, false);
		EnsureModularHeliTilesValid(st);

		CHECK(st->airport.modular_heli_service_tile == INVALID_TILE);
		CHECK(st->airport.modular_heli_landing_tile != INVALID_TILE);
	}

	SECTION("A mixed layout keeps depot-bound helicopters off the cut-off pad") {
		/* One connected pad and one rooftop heliport. The heliport is the trap: a
		 * depot-bound helicopter that lands there can reach neither the hangar nor a
		 * runway, so it lifts off and picks it again — forever. */
		Station *st = build(true, true, false);
		const TileIndex good_pad = base + TileDiffXY(5, 5);
		AddModularTile(st, good_pad, APT_HELIPAD_2, 0);

		EnsureModularHeliTilesValid(st);
		REQUIRE(FindAirportGroundPath(st, good_pad, hangar, nullptr).found);
		REQUIRE_FALSE(FindAirportGroundPath(st, heliport, hangar, nullptr).found);

		/* Some pad works, so no service tile is needed — the filter carries this case. */
		CHECK(st->airport.modular_heli_service_tile == INVALID_TILE);
		CHECK(IsModularPadWithHangarAccess(st, good_pad));
		CHECK_FALSE(IsModularPadWithHangarAccess(st, heliport));

		SetupAircraftPool();
		Aircraft *heli = CreateAircraft(VehicleID(10));
		heli->targetairport = st->index;
		heli->subtype = AIR_HELICOPTER;
		heli->vehstatus.Set(VehState::Stopped); // keeps NeedsAutomaticServicing off the Company lookup
		/* Parked right over the heliport, so distance scoring prefers it outright. */
		heli->x_pos = TileX(heliport) * TILE_SIZE;
		heli->y_pos = TileY(heliport) * TILE_SIZE;

		/* An ordinary flight still uses the nearest pad, cut off or not — it only has to
		 * park there, and the heliport is a legal parking spot. */
		CHECK(FindModularLandingTarget(st, heli) == heliport);

		/* Heading for the hangar, it must give up the near pad for the reachable one. */
		heli->current_order.MakeGoToDepot(st->index, OrderDepotTypeFlag::Service);
		CHECK(FindModularLandingTarget(st, heli) == good_pad);
	}

	SECTION("Nothing reaches the hangar at all: still land, do not circle for good") {
		/* An isolated hangar, so neither a pad nor any parkable apron or stand can reach
		 * it — both the reachable-pad set and the service tile come up empty. There is
		 * then nothing to filter down to, and filtering anyway would reject every pad and
		 * strand the helicopter in the air permanently. Landing is strictly better:
		 * arriving on a pad services it, which clears the condition that sent it looking
		 * for a hangar. Reachable because IsModularHeliParkableApron excludes anything
		 * next to a building and hangars are buildings, so a compact airport with no
		 * stands has no candidate at all. */
		Station *st = SetupModularAirport(base, 10, 10);
		REQUIRE(st != nullptr);
		const TileIndex lone_pad = base + TileDiffXY(2, 2);
		const TileIndex lone_hangar = base + TileDiffXY(8, 8);
		AddModularTile(st, lone_pad, APT_HELIPAD_2, 0);
		AddModularTile(st, lone_hangar, APT_DEPOT_SE, 0);

		EnsureModularHeliTilesValid(st);
		REQUIRE(st->airport.modular_hangar_reachable_pads.empty());
		REQUIRE(st->airport.modular_heli_service_tile == INVALID_TILE);

		SetupAircraftPool();
		Aircraft *heli = CreateAircraft(VehicleID(10));
		heli->targetairport = st->index;
		heli->subtype = AIR_HELICOPTER;
		heli->vehstatus.Set(VehState::Stopped);
		heli->x_pos = TileX(lone_pad) * TILE_SIZE;
		heli->y_pos = TileY(lone_pad) * TILE_SIZE;
		heli->current_order.MakeGoToDepot(st->index, OrderDepotTypeFlag::Service);

		CHECK(FindModularLandingTarget(st, heli) == lone_pad);
	}

	SECTION("Every pad cut off falls back to the service tile") {
		/* Same as above but the connected pad is gone, so no pad qualifies and the
		 * apron fallback takes over. */
		Station *st = build(true, true, false);
		EnsureModularHeliTilesValid(st);

		CHECK(st->airport.modular_hangar_reachable_pads.empty());
		CHECK(st->airport.modular_heli_service_tile == far_apron);
	}

	SECTION("A depot-bound helicopter is sent to the service tile, not the heliport") {
		Station *st = build(true, true, false);
		SetupAircraftPool();

		Aircraft *heli = CreateAircraft(VehicleID(10));
		heli->targetairport = st->index;
		heli->subtype = AIR_HELICOPTER;
		heli->x_pos = TileX(heliport) * TILE_SIZE;
		heli->y_pos = TileY(heliport) * TILE_SIZE;
		/* Keeps NeedsAutomaticServicing() off the Company lookup this bare-shell
		 * aircraft has no owner for; the depot order below is what drives the test. */
		heli->vehstatus.Set(VehState::Stopped);

		/* Without a depot order the heliport is the only landing target, as before. */
		CHECK(FindModularLandingTarget(st, heli) == heliport);

		heli->current_order.MakeGoToDepot(st->index, OrderDepotTypeFlag::Service);
		CHECK(FindModularLandingTarget(st, heli) == far_apron);
	}
}

TEST_CASE("ModularAirportStaleHeliDescentFlag")
{
	/* HelicopterDirectDescent blocks start/stop and autoreplace, so a leftover strands an
	 * aircraft on the ground. The load repair has to tell a leftover from a live descent
	 * using state plus airport, because state alone cannot: a stock helicopter takes its
	 * heading state before it physically descends and keeps it all the way down. */

	SECTION("Grounded at a modular airport, the flag can only be a leftover") {
		/* Modular touchdown clears the flag, so one still set here predates that. */
		CHECK(IsStaleHeliDescentFlag(HELIPAD1, true));
		CHECK(IsStaleHeliDescentFlag(HELIPAD3, true));
		CHECK(IsStaleHeliDescentFlag(HANGAR, true));
		CHECK(IsStaleHeliDescentFlag(TERM1, true));
	}

	SECTION("The same states at a stock airport are a live descent") {
		/* AircraftEventHandler_HeliEndLanding assigns HELIPAD1/2/3 or HANGAR before the
		 * HeliLower descent runs, so the flag is real and clearing it would let the game
		 * start or stop a helicopter hanging in mid-air. */
		CHECK_FALSE(IsStaleHeliDescentFlag(HELIPAD1, false));
		CHECK_FALSE(IsStaleHeliDescentFlag(HELIPAD2, false));
		CHECK_FALSE(IsStaleHeliDescentFlag(HELIPAD3, false));
		CHECK_FALSE(IsStaleHeliDescentFlag(HANGAR, false));
	}

	SECTION("Inside the in-flight band the state test already covers it") {
		for (bool modular : {true, false}) {
			CHECK_FALSE(IsStaleHeliDescentFlag(TAKEOFF, modular));
			CHECK_FALSE(IsStaleHeliDescentFlag(HELITAKEOFF, modular));
			CHECK_FALSE(IsStaleHeliDescentFlag(FLYING, modular));
			CHECK_FALSE(IsStaleHeliDescentFlag(HELILANDING, modular));
			CHECK_FALSE(IsStaleHeliDescentFlag(HELIENDLANDING, modular));
		}
	}

	SECTION("Both edges of the band are exact") {
		/* HELIENDLANDING(18) is the last in-band state and TERM7(19) the first out of it,
		 * so an off-by-one at the top would either strand parked aircraft or clear live
		 * descents. The lower edge is HELIPAD2(9) against TAKEOFF(10). */
		CHECK_FALSE(IsStaleHeliDescentFlag(HELIENDLANDING, true));
		CHECK(IsStaleHeliDescentFlag(TERM7, true));
		CHECK(IsStaleHeliDescentFlag(TERM8, true));
		CHECK(IsStaleHeliDescentFlag(HELIPAD2, true));
		CHECK_FALSE(IsStaleHeliDescentFlag(TAKEOFF, true));
	}
}

TEST_CASE("ModularAirportHelipadServicing")
{
	Map::Allocate(64, 64);
	const TileIndex base = TileXY(10, 10);
	Station *st = SetupModularAirport(base, 10, 10);
	REQUIRE(st != nullptr);

	const TileIndex pad = base + TileDiffXY(2, 2);
	const TileIndex stand = base + TileDiffXY(2, 3);
	AddModularTile(st, pad, APT_HELIPAD_2, 0);
	AddModularTile(st, stand, APT_STAND, 0);

	extern EnginePool _engine_pool;
	_engine_pool.CleanPool();
	const EngineID eid = CreateAircraftEngine(EngineID(0), 0);
	Engine::Get(eid)->reliability = 0x7000;

	const bool saved_setting = _settings_game.order.serviceathelipad;

	/* An aircraft due for service: stale service date, a breakdown on the clock and
	 * reliability below the engine's. Servicing resets all three. */
	auto due_for_service = [&](TileIndex tile, uint8_t subtype) -> Aircraft * {
		SetupAircraftPool();
		Aircraft *v = CreateAircraft(VehicleID(10));
		v->targetairport = st->index;
		v->subtype = subtype;
		v->engine_type = eid;
		v->tile = tile;
		v->date_of_last_service = TimerGameEconomy::Date(0);
		v->breakdowns_since_last_service = 3;
		v->reliability = 0x1000;
		return v;
	};

	/* What the modular arrival path passes for `at_helipad`: the piece actually
	 * parked on, not the stock airport type's helipad count. */
	auto parked_on_helipad = [&](const Aircraft *v) {
		const ModularAirportTileData *d = st->airport.GetModularTileData(v->tile);
		return d != nullptr && IsModularHelipadPiece(d->piece_type);
	};

	SECTION("A helicopter parked on a modular helipad is serviced") {
		_settings_game.order.serviceathelipad = true;
		Aircraft *heli = due_for_service(pad, AIR_HELICOPTER);
		REQUIRE(parked_on_helipad(heli));

		MaybeServiceAircraftAtHelipad(heli, parked_on_helipad(heli));

		CHECK(heli->breakdowns_since_last_service == 0);
		CHECK(heli->reliability == 0x7000);
	}

	SECTION("A helicopter parked on a stand is not") {
		_settings_game.order.serviceathelipad = true;
		Aircraft *heli = due_for_service(stand, AIR_HELICOPTER);
		REQUIRE_FALSE(parked_on_helipad(heli));

		MaybeServiceAircraftAtHelipad(heli, parked_on_helipad(heli));

		CHECK(heli->breakdowns_since_last_service == 3);
		CHECK(heli->reliability == 0x1000);
	}

	SECTION("A fixed-wing aircraft is never serviced this way") {
		_settings_game.order.serviceathelipad = true;
		Aircraft *plane = due_for_service(pad, AIR_AIRCRAFT);

		MaybeServiceAircraftAtHelipad(plane, parked_on_helipad(plane));

		CHECK(plane->breakdowns_since_last_service == 3);
		CHECK(plane->reliability == 0x1000);
	}

	SECTION("The setting still switches it off") {
		_settings_game.order.serviceathelipad = false;
		Aircraft *heli = due_for_service(pad, AIR_HELICOPTER);

		MaybeServiceAircraftAtHelipad(heli, parked_on_helipad(heli));

		CHECK(heli->breakdowns_since_last_service == 3);
		CHECK(heli->reliability == 0x1000);
	}

	_settings_game.order.serviceathelipad = saved_setting;
}
