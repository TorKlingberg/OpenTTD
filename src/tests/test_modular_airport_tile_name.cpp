/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file test_modular_airport_tile_name.cpp Tests for modular airport tile names. */

#include "../stdafx.h"
#include "../3rdparty/catch2/catch.hpp"

#include "../modular_airport_tile_name.h"
#include "../landscape.h"
#include "../map_func.h"
#include "../newgrf_airport.h"
#include "../newgrf_airporttiles.h"
#include "../station_base.h"
#include "../station_map.h"
#include "../table/airporttile_ids.h"
#include "../table/strings.h"

#include "../safeguards.h"

TEST_CASE("ModularAirportTileNames")
{
	/* Keep this in AirportTiles order so every built-in value has an explicit
	 * expected name. A new built-in value changes APT_END and fails the size
	 * assertion until its land-info behaviour is specified here. */
	static constexpr StringID expected_names[] = {
		STR_STATION_BUILD_MODULAR_AIRPORT_PIECE_APRON,             // APT_APRON
		STR_STATION_BUILD_MODULAR_AIRPORT_PIECE_APRON,             // APT_APRON_FENCE_NW
		STR_STATION_BUILD_MODULAR_AIRPORT_PIECE_APRON,             // APT_APRON_FENCE_SW
		STR_STATION_BUILD_MODULAR_AIRPORT_PIECE_STAND,             // APT_STAND
		STR_LAI_STATION_DESCRIPTION_AIRPORT_APRON_EDGE,            // APT_APRON_W
		STR_LAI_STATION_DESCRIPTION_AIRPORT_APRON_EDGE,            // APT_APRON_S
		STR_STATION_BUILD_MODULAR_AIRPORT_PIECE_TAXIWAY_CROSS,     // APT_APRON_VER_CROSSING_S
		STR_STATION_BUILD_MODULAR_AIRPORT_PIECE_TAXIWAY_CROSS,     // APT_APRON_HOR_CROSSING_W
		STR_STATION_BUILD_MODULAR_AIRPORT_PIECE_TAXIWAY,           // APT_APRON_VER_CROSSING_N
		STR_STATION_BUILD_MODULAR_AIRPORT_PIECE_TAXIWAY_CROSS,     // APT_APRON_HOR_CROSSING_E
		STR_LAI_STATION_DESCRIPTION_AIRPORT_APRON_EDGE,            // APT_APRON_E
		STR_LAI_STATION_DESCRIPTION_AIRPORT_APRON_EDGE,            // APT_APRON_N
		STR_STATION_BUILD_MODULAR_AIRPORT_PIECE_TAXIWAY,           // APT_APRON_HOR
		STR_LAI_STATION_DESCRIPTION_AIRPORT_APRON_EDGE,            // APT_APRON_N_FENCE_SW
		STR_STATION_BUILD_MODULAR_AIRPORT_PIECE_RUNWAY,            // APT_RUNWAY_1
		STR_STATION_BUILD_MODULAR_AIRPORT_PIECE_RUNWAY,            // APT_RUNWAY_2
		STR_STATION_BUILD_MODULAR_AIRPORT_PIECE_RUNWAY,            // APT_RUNWAY_3
		STR_STATION_BUILD_MODULAR_AIRPORT_PIECE_RUNWAY,            // APT_RUNWAY_4
		STR_STATION_BUILD_MODULAR_AIRPORT_PIECE_RUNWAY_END,        // APT_RUNWAY_END_FENCE_SE
		STR_STATION_BUILD_MODULAR_AIRPORT_PIECE_TERMINAL_ALT,      // APT_BUILDING_2
		STR_STATION_BUILD_MODULAR_AIRPORT_PIECE_TOWER,             // APT_TOWER_FENCE_SW
		STR_STATION_BUILD_MODULAR_AIRPORT_PIECE_TERMINAL_ROUND,    // APT_ROUND_TERMINAL
		STR_STATION_BUILD_MODULAR_AIRPORT_PIECE_TERMINAL_OTHER,    // APT_BUILDING_3
		STR_STATION_BUILD_MODULAR_AIRPORT_PIECE_TERMINAL,          // APT_BUILDING_1
		STR_STATION_BUILD_MODULAR_AIRPORT_PIECE_HANGAR,            // APT_DEPOT_SE
		STR_STATION_BUILD_MODULAR_AIRPORT_PIECE_STAND_1,           // APT_STAND_1
		STR_STATION_BUILD_MODULAR_AIRPORT_PIECE_STAND_PIER,        // APT_STAND_PIER_NE
		STR_STATION_BUILD_MODULAR_AIRPORT_PIECE_APRON,             // APT_PIER_NW_NE
		STR_STATION_BUILD_MODULAR_AIRPORT_PIECE_APRON,             // APT_PIER
		STR_STATION_BUILD_MODULAR_AIRPORT_PIECE_EMPTY,             // APT_EMPTY
		STR_STATION_BUILD_MODULAR_AIRPORT_PIECE_EMPTY,             // APT_EMPTY_FENCE_NE
		STR_STATION_BUILD_MODULAR_AIRPORT_PIECE_RADAR_GRASS,       // APT_RADAR_GRASS_FENCE_SW
		STR_STATION_BUILD_MODULAR_AIRPORT_PIECE_RADIO_TOWER,       // APT_RADIO_TOWER_FENCE_NE
		STR_STATION_BUILD_MODULAR_AIRPORT_PIECE_SMALL_TERMINAL_3,  // APT_SMALL_BUILDING_3
		STR_STATION_BUILD_MODULAR_AIRPORT_PIECE_SMALL_TERMINAL_3,  // APT_SMALL_BUILDING_2
		STR_STATION_BUILD_MODULAR_AIRPORT_PIECE_SMALL_TERMINAL_3,  // APT_SMALL_BUILDING_1
		STR_STATION_BUILD_MODULAR_AIRPORT_PIECE_GRASS,             // APT_GRASS_FENCE_SW
		STR_STATION_BUILD_MODULAR_AIRPORT_PIECE_GRASS,             // APT_GRASS_2
		STR_STATION_BUILD_MODULAR_AIRPORT_PIECE_GRASS,             // APT_GRASS_1
		STR_STATION_BUILD_MODULAR_AIRPORT_PIECE_GRASS,             // APT_GRASS_FENCE_NE_FLAG
		STR_STATION_BUILD_MODULAR_AIRPORT_PIECE_RUNWAY_SMALL_NEAR, // APT_RUNWAY_SMALL_NEAR_END
		STR_STATION_BUILD_MODULAR_AIRPORT_PIECE_RUNWAY_SMALL_MID,  // APT_RUNWAY_SMALL_MIDDLE
		STR_STATION_BUILD_MODULAR_AIRPORT_PIECE_RUNWAY_SMALL_FAR,  // APT_RUNWAY_SMALL_FAR_END
		STR_STATION_BUILD_MODULAR_AIRPORT_PIECE_SMALL_HANGAR,      // APT_SMALL_DEPOT_SE
		STR_AIRPORT_HELIPORT,                                      // APT_HELIPORT
		STR_STATION_BUILD_MODULAR_AIRPORT_PIECE_RUNWAY_END,        // APT_RUNWAY_END
		STR_STATION_BUILD_MODULAR_AIRPORT_PIECE_RUNWAY,            // APT_RUNWAY_5
		STR_STATION_BUILD_MODULAR_AIRPORT_PIECE_TOWER,             // APT_TOWER
		STR_STATION_BUILD_MODULAR_AIRPORT_PIECE_APRON,             // APT_APRON_FENCE_NE
		STR_STATION_BUILD_MODULAR_AIRPORT_PIECE_RUNWAY_END,        // APT_RUNWAY_END_FENCE_NW
		STR_STATION_BUILD_MODULAR_AIRPORT_PIECE_RUNWAY,            // APT_RUNWAY_FENCE_NW
		STR_STATION_BUILD_MODULAR_AIRPORT_PIECE_RADAR,             // APT_RADAR_FENCE_SW
		STR_STATION_BUILD_MODULAR_AIRPORT_PIECE_RADAR,             // APT_RADAR_FENCE_NE
		STR_STATION_BUILD_MODULAR_AIRPORT_PIECE_HELIPAD,           // APT_HELIPAD_1
		STR_STATION_BUILD_MODULAR_AIRPORT_PIECE_HELIPAD,           // APT_HELIPAD_2_FENCE_NW
		STR_STATION_BUILD_MODULAR_AIRPORT_PIECE_HELIPAD,           // APT_HELIPAD_2
		STR_STATION_BUILD_MODULAR_AIRPORT_PIECE_APRON,             // APT_APRON_FENCE_NE_SW
		STR_STATION_BUILD_MODULAR_AIRPORT_PIECE_RUNWAY_END,        // APT_RUNWAY_END_FENCE_NW_SW
		STR_STATION_BUILD_MODULAR_AIRPORT_PIECE_RUNWAY_END,        // APT_RUNWAY_END_FENCE_SE_SW
		STR_STATION_BUILD_MODULAR_AIRPORT_PIECE_RUNWAY_END,        // APT_RUNWAY_END_FENCE_NE_NW
		STR_STATION_BUILD_MODULAR_AIRPORT_PIECE_RUNWAY_END,        // APT_RUNWAY_END_FENCE_NE_SE
		STR_STATION_BUILD_MODULAR_AIRPORT_PIECE_HELIPAD,           // APT_HELIPAD_2_FENCE_NE_SE
		STR_STATION_BUILD_MODULAR_AIRPORT_PIECE_APRON,             // APT_APRON_FENCE_SE_SW
		STR_STATION_BUILD_MODULAR_AIRPORT_PIECE_LOW_TERMINAL,      // APT_LOW_BUILDING_FENCE_N
		STR_STATION_BUILD_MODULAR_AIRPORT_PIECE_LOW_TERMINAL,      // APT_LOW_BUILDING_FENCE_NW
		STR_STATION_BUILD_MODULAR_AIRPORT_PIECE_APRON,             // APT_APRON_FENCE_SE
		STR_STATION_BUILD_MODULAR_AIRPORT_PIECE_PLAIN_H,           // APT_HELIPAD_3_FENCE_SE_SW
		STR_STATION_BUILD_MODULAR_AIRPORT_PIECE_PLAIN_H,           // APT_HELIPAD_3_FENCE_NW_SW
		STR_STATION_BUILD_MODULAR_AIRPORT_PIECE_PLAIN_H,           // APT_HELIPAD_3_FENCE_NW
		STR_STATION_BUILD_MODULAR_AIRPORT_PIECE_LOW_TERMINAL,      // APT_LOW_BUILDING
		STR_STATION_BUILD_MODULAR_AIRPORT_PIECE_APRON,             // APT_APRON_FENCE_NE_SE
		STR_STATION_BUILD_MODULAR_AIRPORT_PIECE_APRON_HALF_E,      // APT_APRON_HALF_EAST
		STR_STATION_BUILD_MODULAR_AIRPORT_PIECE_APRON_HALF_W,      // APT_APRON_HALF_WEST
		STR_STATION_BUILD_MODULAR_AIRPORT_PIECE_FLAG_GRASS,        // APT_GRASS_FENCE_NE_FLAG_2
		STR_STATION_BUILD_MODULAR_AIRPORT_PIECE_HANGAR,            // APT_DEPOT_SW
		STR_STATION_BUILD_MODULAR_AIRPORT_PIECE_HANGAR,            // APT_DEPOT_NW
		STR_STATION_BUILD_MODULAR_AIRPORT_PIECE_HANGAR,            // APT_DEPOT_NE
		STR_STATION_BUILD_MODULAR_AIRPORT_PIECE_SMALL_HANGAR,      // APT_SMALL_DEPOT_SW
		STR_STATION_BUILD_MODULAR_AIRPORT_PIECE_SMALL_HANGAR,      // APT_SMALL_DEPOT_NW
		STR_STATION_BUILD_MODULAR_AIRPORT_PIECE_SMALL_HANGAR,      // APT_SMALL_DEPOT_NE
	};
	static_assert(lengthof(expected_names) == APT_END);

	for (size_t piece_type = 0; piece_type < lengthof(expected_names); piece_type++) {
		CAPTURE(piece_type);
		CHECK(GetModularAirportTileName(static_cast<uint8_t>(piece_type)) == expected_names[piece_type]);
	}

	CHECK(GetModularAirportTileName(UINT8_MAX) == STR_NULL);
}

TEST_CASE("ModularAirportLandInfoUsesTileName")
{
	Map::Allocate(64, 64);
	_station_pool.CleanPool();

	/* FillTileDescAirport expects the ordinary NewGRF class/spec tables to have
	 * completed their startup reset. Reproduce the airport part of that reset so
	 * this test is independent of which other test cases ran before it. */
	AirportClass::Reset();
	AirportClass::Get(AirportClass::Allocate("SMAL"))->name = STR_AIRPORT_CLASS_SMALL;
	AirportClass::Get(AirportClass::Allocate("LARG"))->name = STR_AIRPORT_CLASS_LARGE;
	AirportClass::Get(AirportClass::Allocate("HUB_"))->name = STR_AIRPORT_CLASS_HUB;
	AirportClass::Get(AirportClass::Allocate("HELI"))->name = STR_AIRPORT_CLASS_HELIPORTS;
	AirportSpec::ResetAirports();
	AirportTileSpec::ResetAirportTiles();
	BindAirportSpecs();

	const TileIndex tile = TileXY(10, 10);
	Station *st = Station::CreateAtIndex(StationID(0), tile);
	REQUIRE(st != nullptr);
	st->owner = OWNER_NONE;
	st->airport.tile = tile;
	st->airport.type = AT_MODULAR;
	st->airport.blocks.Set(AirportBlock::Modular);
	st->airport.EnsureModularDataExists();
	st->facilities.Set(StationFacility::Airport);
	MakeAirport(Tile(tile), st->owner, st->index, APT_STAND, WaterClass::Invalid);

	ModularAirportTileData data;
	data.tile = tile;
	data.piece_type = APT_STAND;
	st->airport.modular_tile_data->push_back(data);
	st->airport.modular_tile_index_dirty = true;

	TileDesc description;
	GetTileDesc(tile, description);
	CHECK(description.str == STR_LAI_STATION_DESCRIPTION_AIRPORT_TILE);
	CHECK(description.dparam == STR_STATION_BUILD_MODULAR_AIRPORT_PIECE_STAND.base());

	SECTION("Hangar variants are distinguished") {
		st->airport.GetModularTileData(tile)->piece_type = APT_SMALL_DEPOT_SE;
		description = {};
		GetTileDesc(tile, description);
		CHECK(description.str == STR_LAI_STATION_DESCRIPTION_AIRPORT_TILE);
		CHECK(description.dparam == STR_STATION_BUILD_MODULAR_AIRPORT_PIECE_SMALL_HANGAR.base());
	}

	SECTION("Unknown metadata retains the generic airport description") {
		st->airport.GetModularTileData(tile)->piece_type = UINT8_MAX;
		description = {};
		GetTileDesc(tile, description);
		CHECK(description.str == STR_LAI_STATION_DESCRIPTION_AIRPORT);
	}

	_station_pool.CleanPool();
}
