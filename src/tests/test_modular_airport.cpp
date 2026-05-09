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

static Aircraft *CreateAircraft(VehicleID index)
{
	Aircraft *v = Aircraft::CreateAtIndex(index);
	v->subtype = AIR_AIRCRAFT;
	return v;
}

static void AddModularTile(Station *st, TileIndex tile, uint8_t piece_type, uint8_t rotation = 0)
{
	ModularAirportTileData data;
	data.tile = tile;
	data.piece_type = piece_type;
	data.rotation = rotation;
	st->airport.modular_tile_data->push_back(data);
	st->airport.modular_tile_index_dirty = true;
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
		Aircraft *other = CreateAircraft(VehicleID(1));
		other->tile = base + TileDiffXY(1, 0); // Occupy intermediate stand
		
		AirportGroundPath path = FindAirportGroundPath(st, start, base + TileDiffXY(2, 1), other);
		CHECK(path.found);
		for (TileIndex t : path.tiles) {
			CHECK(t != other->tile);
		}
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

		SetupAircraftPool();
		Aircraft *v = CreateAircraft(VehicleID(10));
		CHECK(TryReserveContiguousModularRunway(v, st, base));
		
		CHECK(IsModularAirportTileReservedBy(base, v->index));
		CHECK(IsModularAirportTileReservedBy(base + TileDiffXY(1, 0), v->index));
		CHECK(IsModularAirportTileReservedBy(base + TileDiffXY(2, 0), v->index));
		
		// Another aircraft trying to reserve
		Aircraft *v2 = CreateAircraft(VehicleID(11));
		CHECK_FALSE(TryReserveContiguousModularRunway(v2, st, base));
		
		ClearModularRunwayReservation(v);
		CHECK_FALSE(HasModularAirportTileReservation(base));
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
