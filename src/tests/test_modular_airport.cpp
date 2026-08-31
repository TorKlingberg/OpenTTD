/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file test_modular_airport.cpp Unit tests for modular airport logic. */

#include "../stdafx.h"
#include "../timer/timer_game_tick.h"
#include "../newgrf.h"
#include "../3rdparty/catch2/catch.hpp"

#include "../modular_airport_cmd.h"
#include "../modular_airport_build.h"
#include "../modular_airport_draw.h"
#include "../modular_airport_gui.h"
#include "../script/api/script_airport.hpp"
#include "../sprite.h"
#include "../table/sprites.h"
#include "../airport_template.h"
#include "../table/airporttile_ids.h"
#include "../map_func.h"
#include "../landscape.h"
#include "../station_base.h"
#include "../station_cmd.h"
#include "../misc/endian_buffer.hpp"
#include "../network/core/config.h"
#include "../station_map.h"
#include "../clear_map.h"
#include "../tree_map.h"
#include "../language.h"
#include "../town.h"
#include "../airport_ground_pathfinder.h"
#include "../airport_pathfinder.h"
#include "../viewport_kdtree.h"
#include "mock_environment.h"
#include "../vehicle_base.h"
#include "../vehicle_func.h"
#include "../engine_base.h"
#include "../company_base.h"
#include "../company_func.h"
#include "../economy_func.h"
#include "../newgrf_airport.h"
#include "../cheat_type.h"
#include "../settings_type.h"
#include "../table/strings.h"

#include "../safeguards.h"

static Station *SetupModularAirport(TileIndex base_tile, uint size_x, uint size_y)
{
	MockEnvironment::Instance();
	_station_pool.CleanPool();

	Station *st = Station::CreateAtIndex(StationID(0), base_tile);
	if (st == nullptr) return nullptr;

	st->airport.tile = base_tile;
	st->airport.w = size_x;
	st->airport.h = size_y;
	st->airport.EnsureModularDataExists();
	st->airport.blocks.Set(AirportBlock::Modular);
	st->airport.type = AT_MODULAR;
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
	_vehicle_pool.CleanPool();
	/* Several command-path tests create and destroy real vehicles before these
	 * bare-shell aircraft tests. Keep the spatial lookup in sync with the pool so
	 * occupancy validation never follows a stale test vehicle pointer. */
	ResetVehicleHash();
}

/* Bare-shell aircraft for tests: only fields read by the APIs under test
 * (index, subtype, tile, x_pos/y_pos, ground_path_goal) are populated by callers. */
static Aircraft *CreateAircraft(VehicleID index)
{
	Aircraft *v = Aircraft::CreateAtIndex(index);
	v->subtype = AIR_AIRCRAFT;
	/* Rollout length is derived from how far this aircraft takes to brake, so a
	 * bare shell needs a speed a real aircraft would have. Anything at or above
	 * SPEED_LIMIT_APPROACH * plane_speed saturates the braking distance, which at
	 * the ambient test plane_speed of 0 (clamped to 1) is 5 tiles -- longer than
	 * the runways these tests build, so they still roll to the far end, which is
	 * what every test written before early turn-off assumes. */
	v->vcache.cached_max_speed = 1000;
	return v;
}

/* Create an aircraft engine in the pool with the given AircraftVehicleInfo
 * subtype bits (AIR_FAST marks a large/fast jet). Returns its EngineID for
 * assigning to Aircraft::engine_type. */
static EngineID CreateAircraftEngine(EngineID index, uint8_t subtype)
{
	Engine *e = Engine::CreateAtIndex(index, VehicleType::Aircraft, 0xFFFF);
	e->VehInfo<AircraftVehicleInfo>().subtype = subtype;
	return index;
}

static void AddModularTile(Station *st, TileIndex tile, ModularAirportPieceID piece_type, uint8_t rotation = 0)
{
	ModularAirportTileData data;
	data.tile = tile;
	data.piece_type = piece_type;
	data.rotation = rotation;
	st->airport.modular_tile_data->push_back(data);
	st->airport.modular_tile_index_dirty = true;
	st->airport.MarkLayoutDirty();
}

static ModularAirportTileData *AddModularTileWithData(Station *st, TileIndex tile, ModularAirportPieceID piece_type, uint8_t rotation = 0)
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

static void AddSmallRunway(Station *st, TileIndex start, uint length, uint8_t rotation = 0, uint8_t flags = RUF_DEFAULT)
{
	for (uint i = 0; i < length; i++) {
		const TileIndex tile = start + ((rotation % 2) == 0 ? TileDiffXY(i, 0) : TileDiffXY(0, i));
		uint8_t piece_type = APT_RUNWAY_SMALL_MIDDLE;
		if (i == 0) piece_type = APT_RUNWAY_SMALL_FAR_END;
		if (i + 1 == length) piece_type = APT_RUNWAY_SMALL_NEAR_END;
		ModularAirportTileData *data = AddModularTileWithData(st, tile, piece_type, rotation);
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

TEST_CASE("ModularAirportLayoutAccountingMatchesStockCalibration")
{
	Map::Allocate(64, 64);
	AirportSpec::ResetAirports();

	struct ExpectedAccounting {
		uint8_t airport_type;
		uint maintenance;
		uint8_t noise;
	};
	static constexpr ExpectedAccounting expected[] = {
		{AT_SMALL, 7, 3},
		{AT_COMMUTER, 20, 6},
		{AT_LARGE, 24, 5},
		{AT_METROPOLITAN, 28, 8},
		{AT_INTERNATIONAL, 42, 13},
		{AT_INTERCON, 72, 25},
		{AT_HELIPORT, 4, 1},
		{AT_HELIDEPOT, 7, 1},
		{AT_HELISTATION, 14, 3},
	};

	for (const ExpectedAccounting &entry : expected) {
		CAPTURE(entry.airport_type);
		const AirportSpec *as = AirportSpec::Get(entry.airport_type);
		REQUIRE(as->layouts.size() == 1);

		std::vector<ModularAirportPieceID> pieces;
		const TileIndex base = TileXY(1, 1);
		for (AirportTileTableIterator iter(as->layouts[0].tiles, base); iter != INVALID_TILE; ++iter) {
			const TileIndex tile = iter;
			const int dx = TileX(tile) - TileX(base);
			const int dy = TileY(tile) - TileY(base);
			pieces.push_back(ApplyStockTileOverride(entry.airport_type, dx, dy,
					MapStockGfxToModularPiece(iter.GetStationGfx())));
		}

		CHECK(GetModularAirportMaintenancePointsFromPieces(pieces) == entry.maintenance * 8);
		CHECK(GetModularAirportNoiseLevelFromPieces(pieces) == entry.noise);
	}
}

TEST_CASE("ModularAirportTypeSpecAndNewGRFReservation")
{
	AirportSpec::ResetAirports();
	_airport_mngr.ResetMapping();

	const AirportSpec *small = AirportSpec::Get(AT_SMALL);
	const AirportSpec *modular = AirportSpec::Get(AT_MODULAR);
	CHECK(modular == AirportSpec::GetWithoutOverride(AT_MODULAR));
	CHECK(modular->class_index == APC_SMALL);
	CHECK(modular->index == 0);
	CHECK_FALSE(modular->enabled);
	CHECK(modular->fsm == small->fsm);
	CHECK_FALSE(modular->layouts.empty());
	CHECK(modular->layouts.size() == small->layouts.size());
	CHECK(modular->depots.empty());
	CHECK(modular->size_x == small->size_x);
	CHECK(modular->size_y == small->size_y);
	CHECK(modular->noise_level == 0);
	CHECK(modular->catchment == 0);
	CHECK(modular->min_year == CalendarTime::MIN_YEAR);
	CHECK(modular->max_year == CalendarTime::MAX_YEAR);
	CHECK(modular->maintenance_cost == 0);
	CHECK(modular->name == STR_AIRPORT_MODULAR);
	CHECK(modular->ttd_airport_type == ATP_TTDP_SMALL);
	CHECK(modular->preview_sprite == 0);
	CHECK(modular->grf_prop.subst_id == AT_INVALID);
	CHECK(modular->grf_prop.override_id == AT_INVALID);
	CHECK(modular->badges.empty());

	/* A savegame from before modular airports existed can persist a NewGRF airport in
	 * runtime slot 127; it has to be moved aside before it overwrites the modular spec. */
	const GrfID test_grfid = UnflattenNewGRFLabel<GrfID>(0xAABBCCDD);
	_airport_mngr.mappings[AT_MODULAR] = {test_grfid, 17, AT_SMALL};
	CHECK(_airport_mngr.RelocateLegacyModularID() == NEW_AIRPORT_OFFSET);
	CHECK(_airport_mngr.mappings[NEW_AIRPORT_OFFSET].grfid == test_grfid);
	CHECK(_airport_mngr.mappings[NEW_AIRPORT_OFFSET].entity_id == 17);
	CHECK(_airport_mngr.mappings[AT_MODULAR].grfid.Empty());
	_airport_mngr.ResetMapping();

	/* AT_MODULAR's slot stays reserved for new mappings, and with every other slot taken
	 * there is nowhere to relocate a legacy one to. */
	for (uint16_t i = 0; i < 117; i++) {
		CHECK(_airport_mngr.AddEntityID(i + 1, UnflattenNewGRFLabel<GrfID>(0xA0000000U + i), AT_SMALL) == NEW_AIRPORT_OFFSET + i);
	}
	CHECK(_airport_mngr.AddEntityID(200, UnflattenNewGRFLabel<GrfID>(0xB0000000U), AT_SMALL) == AT_INVALID);
	CHECK(_airport_mngr.mappings[AT_MODULAR].grfid.Empty());
	_airport_mngr.mappings[AT_MODULAR] = {test_grfid, 17, AT_SMALL};
	CHECK(_airport_mngr.RelocateLegacyModularID() == AT_MODULAR);
	_airport_mngr.ResetMapping();
}

TEST_CASE("ModularAirportNoiseSaturatesAtStorageLimit")
{
	std::vector<ModularAirportPieceID> pieces(256, APT_HELIPORT);
	CHECK(GetModularAirportNoiseLevelFromPieces(pieces) == UINT8_MAX);

	Map::Allocate(64, 64);
	Station *st = SetupModularAirport(TileXY(2, 2), 16, 16);
	REQUIRE(st != nullptr);
	st->airport.modular_tile_data->clear();
	for (uint i = 0; i < 256; i++) {
		ModularAirportTileData data;
		data.tile = TileXY(2 + i % 16, 2 + i / 16);
		data.piece_type = APT_HELIPORT;
		st->airport.modular_tile_data->push_back(data);
	}
	st->airport.MarkLayoutDirty();
	CHECK(GetModularAirportNoiseLevel(st) == UINT8_MAX);
}

TEST_CASE("ModularAirportStockConversionMatchesManualMetadata")
{
	Map::Allocate(64, 64);
	AirportSpec::ResetAirports();
	const TileIndex base = TileXY(2, 2);

	auto expected_runway_flags = [](uint8_t airport_type, int dy) -> uint8_t {
		switch (airport_type) {
			case AT_SMALL: return dy == 2 ? RUF_LANDING | RUF_TAKEOFF | RUF_DIR_LOW : RUF_DEFAULT;
			case AT_COMMUTER: return dy == 3 ? RUF_LANDING | RUF_TAKEOFF | RUF_DIR_LOW : RUF_DEFAULT;
			case AT_LARGE: return dy == 5 ? RUF_LANDING | RUF_TAKEOFF | RUF_DIR_LOW : RUF_DEFAULT;
			case AT_METROPOLITAN:
				if (dy == 4) return RUF_TAKEOFF | RUF_DIR_LOW;
				if (dy == 5) return RUF_LANDING | RUF_DIR_LOW;
				return RUF_DEFAULT;
			case AT_INTERNATIONAL:
				if (dy == 0) return RUF_TAKEOFF | RUF_DIR_HIGH;
				if (dy == 6) return RUF_LANDING | RUF_DIR_LOW;
				return RUF_DEFAULT;
			case AT_INTERCON:
				if (dy == 0) return RUF_LANDING | RUF_DIR_HIGH;
				if (dy == 1) return RUF_TAKEOFF | RUF_DIR_HIGH;
				if (dy == 9) return RUF_TAKEOFF | RUF_DIR_LOW;
				if (dy == 10) return RUF_LANDING | RUF_DIR_LOW;
				return RUF_DEFAULT;
			default: return RUF_DEFAULT;
		}
	};

	for (uint8_t airport_type : {AT_SMALL, AT_LARGE, AT_HELIPORT, AT_METROPOLITAN,
			AT_INTERNATIONAL, AT_COMMUTER, AT_HELIDEPOT, AT_INTERCON, AT_HELISTATION}) {
		CAPTURE(airport_type);
		const AirportSpec *as = AirportSpec::Get(airport_type);
		std::vector<ModularAirportTileData> converted = ConvertStockAirportLayoutToModular(airport_type, 0, base);
		std::vector<ModularAirportTileData> manual;

		for (AirportTileTableIterator iter(as->layouts[0].tiles, base); iter != INVALID_TILE; ++iter) {
			const TileIndex tile = iter;
			const int dx = TileX(tile) - TileX(base);
			const int dy = TileY(tile) - TileY(base);
			const StationGfx stock_gfx = iter.GetStationGfx();
			ModularAirportTileData data;
			data.tile = tile;
			data.piece_type = ApplyStockTileOverride(airport_type, dx, dy, MapStockGfxToModularPiece(stock_gfx));
			data.rotation = 0;
			data.auto_taxi_dir_mask = CalculateAutoTaxiDirectionsForGfx(data.piece_type, 0);
			data.one_way_taxi = false;
			data.user_taxi_dir_mask = 0x0F;
			if (IsModularRunwayPiece(data.piece_type)) data.runway_flags = expected_runway_flags(airport_type, dy);
			data.edge_block_mask = GetStockFenceEdgeMask(stock_gfx);
			manual.push_back(data);
		}

		for (ModularAirportTileData &data : manual) {
			if (!IsModularRunwayPiece(data.piece_type)) continue;
			const bool large_family = IsLargeRunwayFamily(data.piece_type);
			auto find_data = [&](TileIndex tile) -> ModularAirportTileData * {
				auto it = std::find_if(manual.begin(), manual.end(),
						[=](const ModularAirportTileData &candidate) { return candidate.tile == tile; });
				return it != manual.end() ? &*it : nullptr;
			};
			const ModularAirportTileData *previous = find_data(data.tile - TileDiffXY(1, 0));
			if (previous != nullptr && IsModularRunwayPiece(previous->piece_type) &&
					IsLargeRunwayFamily(previous->piece_type) == large_family) {
				continue;
			}

			std::vector<ModularAirportTileData *> segment;
			for (TileIndex tile = data.tile;; tile += TileDiffXY(1, 0)) {
				ModularAirportTileData *candidate = find_data(tile);
				if (candidate == nullptr || !IsModularRunwayPiece(candidate->piece_type) ||
						IsLargeRunwayFamily(candidate->piece_type) != large_family) {
					break;
				}
				segment.push_back(candidate);
			}
			for (size_t i = 0; i < segment.size(); i++) {
				segment[i]->piece_type = GetCanonicalRunwaySegmentPiece(large_family, segment.size(), i);
				segment[i]->auto_taxi_dir_mask = CalculateAutoTaxiDirectionsForGfx(segment[i]->piece_type, 0);
			}
		}

		static constexpr struct { int8_t dx, dy; uint8_t bit, opposite; } edges[] = {
			{0, -1, 0x01, 0x04}, {1, 0, 0x02, 0x08}, {0, 1, 0x04, 0x01}, {-1, 0, 0x08, 0x02},
		};
		for (ModularAirportTileData &data : manual) {
			const uint8_t original_mask = data.edge_block_mask;
			for (const auto &edge : edges) {
				if ((original_mask & edge.bit) == 0) continue;
				const TileIndex neighbour = TileAddXY(data.tile, edge.dx, edge.dy);
				auto it = std::find_if(manual.begin(), manual.end(), [=](const ModularAirportTileData &candidate) { return candidate.tile == neighbour; });
				if (it != manual.end()) {
					it->edge_block_mask |= edge.opposite;
				} else {
					data.edge_block_mask &= ~edge.bit;
				}
			}
		}

		REQUIRE(converted.size() == manual.size());
		for (size_t i = 0; i < manual.size(); i++) {
			CHECK(converted[i].tile == manual[i].tile);
			CHECK(converted[i].piece_type == manual[i].piece_type);
			CHECK(converted[i].rotation == manual[i].rotation);
			CHECK(converted[i].auto_taxi_dir_mask == manual[i].auto_taxi_dir_mask);
			CHECK(converted[i].user_taxi_dir_mask == manual[i].user_taxi_dir_mask);
			CHECK(converted[i].one_way_taxi == manual[i].one_way_taxi);
			CHECK(converted[i].runway_flags == manual[i].runway_flags);
			CHECK(converted[i].edge_block_mask == manual[i].edge_block_mask);
			if (!IsModularRunwayPiece(converted[i].piece_type)) CHECK(converted[i].runway_flags == RUF_DEFAULT);
		}
	}
}

TEST_CASE("ModularAirportStockConversionRejectsNewGRFAirports")
{
	Map::Allocate(64, 64);
	CHECK(CmdBuildModularAirportFromStock({}, TileXY(2, 2), NEW_AIRPORT_OFFSET, 0, NEW_STATION, false).Failed());
}

TEST_CASE("ModularAirportLegacyUpgradeIsAtomic")
{
	Map::Allocate(64, 64);
	MockEnvironment::Instance();
	static LanguageMetadata test_language;
	const std::filesystem::path language_file = std::filesystem::exists("build/lang/english.lng") ?
			"build/lang/english.lng" : "lang/english.lng";
	test_language.file = std::filesystem::absolute(language_file);
	REQUIRE(ReadLanguagePack(&test_language));
	const CompanyID saved_company = _current_company;
	const TimerGameCalendar::Year saved_year = TimerGameCalendar::year;
	_current_company = CompanyID(0);
	TimerGameCalendar::year = TimerGameCalendar::Year{2100};

	auto reset_world = []() {
		_station_pool.CleanPool();
		_town_pool.CleanPool();
		SetupAircraftPool();
		RebuildStationKdtree();
		RebuildTownKdtree();
		RebuildViewportKdtree();
		Town *town = Town::CreateAtIndex(TownID(0), TileXY(32, 32));
		REQUIRE(town != nullptr);
		town->cache.population = 10000;
		RebuildTownKdtree();
	};

	auto setup_legacy_runway = []() {
		const TileIndex base = TileXY(10, 10);
		Station *st = SetupModularAirport(base, 8, 3);
		REQUIRE(st != nullptr);
		st->owner = _current_company;
		st->town = Town::Get(TownID(0));
		st->string_id = STR_SV_STNAME_AIRPORT;
		for (uint x = 0; x < 4; x++) {
			const TileIndex tile = base + TileDiffXY(x, 1);
			const uint8_t piece = x == 0 ? APT_RUNWAY_SMALL_FAR_END :
					x == 3 ? APT_RUNWAY_SMALL_NEAR_END : APT_RUNWAY_SMALL_MIDDLE;
			SetStationGfx(Tile(tile), piece);
			ModularAirportTileData *data = AddModularTileWithData(st, tile, piece, 0);
			data->runway_flags = RUF_DEFAULT;
		}
		return std::make_pair(st, base + TileDiffXY(0, 1));
	};

	SECTION("A whole runway upgrades in one area command") {
		reset_world();
		auto [st, start] = setup_legacy_runway();
		const TileIndex end = start + TileDiffXY(3, 0);
		REQUIRE(CmdUpgradeModularAirportTile(DoCommandFlag::Execute, end, start).Succeeded());

		for (uint x = 0; x < 4; x++) {
			const ModularAirportTileData *data = st->airport.GetModularTileData(start + TileDiffXY(x, 0));
			REQUIRE(data != nullptr);
			CHECK(IsLargeRunwayFamily(data->piece_type));
			CHECK(data->runway_flags == RUF_DEFAULT);
		}
	}

	SECTION("One occupied tile prevents every runway tile changing") {
		reset_world();
		auto [st, start] = setup_legacy_runway();
		const TileIndex occupied = start + TileDiffXY(2, 0);
		Aircraft *v = CreateAircraft(VehicleID(0));
		v->tile = occupied;
		v->x_pos = TileX(occupied) * TILE_SIZE;
		v->y_pos = TileY(occupied) * TILE_SIZE;
		v->z_pos = GetTileMaxPixelZ(occupied);
		v->UpdatePosition();

		CHECK(CmdUpgradeModularAirportTile(DoCommandFlag::Execute,
				start + TileDiffXY(3, 0), start).Failed());
		for (uint x = 0; x < 4; x++) {
			const ModularAirportTileData *data = st->airport.GetModularTileData(start + TileDiffXY(x, 0));
			REQUIRE(data != nullptr);
			CHECK(IsLegacySmallRunwayPiece(data->piece_type));
		}
	}

	SECTION("A single legacy hangar still upgrades by tile") {
		reset_world();
		const TileIndex base = TileXY(10, 10);
		Station *st = SetupModularAirport(base, 3, 3);
		REQUIRE(st != nullptr);
		st->owner = _current_company;
		st->town = Town::Get(TownID(0));
		st->string_id = STR_SV_STNAME_AIRPORT;
		SetStationGfx(Tile(base), APT_SMALL_DEPOT_SE);
		AddModularTile(st, base, APT_SMALL_DEPOT_SE, 0);

		REQUIRE(CmdUpgradeModularAirportTile(DoCommandFlag::Execute, base, base).Succeeded());
		const ModularAirportTileData *data = st->airport.GetModularTileData(base);
		REQUIRE(data != nullptr);
		CHECK(data->piece_type == APT_DEPOT_SE);
	}

	SetupAircraftPool();
	_current_company = saved_company;
	TimerGameCalendar::year = saved_year;
	_station_pool.CleanPool();
	_town_pool.CleanPool();
	RebuildStationKdtree();
	RebuildTownKdtree();
	RebuildViewportKdtree();
}

TEST_CASE("ModularAirportStockAndTileCommandsProduceEquivalentAirports")
{
	MockEnvironment::Instance();
	static LanguageMetadata test_language;
	const std::filesystem::path language_file = std::filesystem::exists("build/lang/english.lng") ?
			"build/lang/english.lng" : "lang/english.lng";
	test_language.file = std::filesystem::absolute(language_file);
	REQUIRE(ReadLanguagePack(&test_language));
	const CompanyID saved_company = _current_company;
	const bool saved_distant_join = _settings_game.station.distant_join_stations;
	const bool saved_never_expire = _settings_game.station.never_expire_airports;
	const uint8_t saved_station_spread = _settings_game.station.station_spread;
	const bool saved_noise = _settings_game.economy.station_noise_level;
	const uint8_t saved_tolerance = _settings_game.difficulty.town_council_tolerance;
	const TimerGameCalendar::Year saved_year = TimerGameCalendar::year;

	_settings_game.station.distant_join_stations = true;
	_settings_game.station.never_expire_airports = true;
	_settings_game.station.station_spread = 64;
	_settings_game.economy.station_noise_level = false;
	_settings_game.difficulty.town_council_tolerance = TOWN_COUNCIL_PERMISSIVE;
	TimerGameCalendar::year = TimerGameCalendar::Year{2100};

	for (uint8_t airport_type : {AT_SMALL, AT_LARGE, AT_HELIPORT, AT_METROPOLITAN,
			AT_INTERNATIONAL, AT_COMMUTER, AT_HELIDEPOT, AT_INTERCON, AT_HELISTATION}) {
		CAPTURE(airport_type);
		Map::Allocate(64, 64);
		_station_pool.CleanPool();
		_town_pool.CleanPool();
		_company_pool.CleanPool();
		RebuildStationKdtree();
		RebuildViewportKdtree();
		AirportSpec::ResetAirports();

		Company *company = Company::CreateAtIndex(CompanyID(0));
		REQUIRE(company != nullptr);
		company->money = INT64_MAX;
		company->clear_limit = UINT32_MAX;
		_current_company = company->index;

		Town *town = Town::CreateAtIndex(TownID(0), TileXY(32, 32));
		REQUIRE(town != nullptr);
		town->cache.population = 10000;
		RebuildTownKdtree();

		const TileIndex stock_base = TileXY(2, 2);
		const TileIndex manual_base = TileXY(24, 2);
		const CommandCost stock_build = CmdBuildModularAirportFromStock(DoCommandFlag::Execute,
				stock_base, airport_type, 0, NEW_STATION, false);
		CAPTURE(stock_build.GetErrorMessage(), stock_build.GetExtraErrorMessage());
		REQUIRE(stock_build.Succeeded());
		Station *stock = Station::GetByTile(stock_base);
		REQUIRE(stock != nullptr);

		const std::vector<ModularAirportTileData> desired =
				ConvertStockAirportLayoutToModular(airport_type, 0, manual_base);
		Station *manual = nullptr;
		for (const ModularAirportTileData &data : desired) {
			const StationID join = manual == nullptr ? NEW_STATION : manual->index;
			REQUIRE(CmdBuildModularAirportTile(DoCommandFlag::Execute, data.tile,
					data.piece_type, join, false, data.rotation, data.user_taxi_dir_mask,
					data.one_way_taxi, false).Succeeded());
			if (manual == nullptr) manual = Station::GetByTile(data.tile);
			REQUIRE(manual != nullptr);
		}

		for (const ModularAirportTileData &data : desired) {
			if (IsModularRunwayPiece(data.piece_type)) {
				REQUIRE(CmdSetRunwayFlags(DoCommandFlag::Execute, data.tile, data.runway_flags).Succeeded());
			}
			for (uint8_t edge_bit : {uint8_t{0x01}, uint8_t{0x02}, uint8_t{0x04}, uint8_t{0x08}}) {
				if ((data.edge_block_mask & edge_bit) == 0) continue;
				REQUIRE(CmdSetModularAirportEdgeFence(DoCommandFlag::Execute, data.tile, edge_bit, true).Succeeded());
			}
		}

		REQUIRE(manual != nullptr);
		CHECK(stock->airport.type == manual->airport.type);
		CHECK(stock->airport.layout == manual->airport.layout);
		CHECK(stock->airport.rotation == manual->airport.rotation);
		CHECK(stock->airport.w == manual->airport.w);
		CHECK(stock->airport.h == manual->airport.h);
		CHECK(stock->airport.blocks == manual->airport.blocks);
		REQUIRE(stock->airport.modular_tile_data != nullptr);
		REQUIRE(manual->airport.modular_tile_data != nullptr);
		REQUIRE(stock->airport.modular_tile_data->size() == manual->airport.modular_tile_data->size());

		for (const ModularAirportTileData &stock_data : *stock->airport.modular_tile_data) {
			const int dx = TileX(stock_data.tile) - TileX(stock_base);
			const int dy = TileY(stock_data.tile) - TileY(stock_base);
			const ModularAirportTileData *manual_data = manual->airport.GetModularTileData(
					TileAddXY(manual_base, dx, dy));
			REQUIRE(manual_data != nullptr);
			CHECK(stock_data.piece_type == manual_data->piece_type);
			CHECK(stock_data.rotation == manual_data->rotation);
			CHECK(stock_data.auto_taxi_dir_mask == manual_data->auto_taxi_dir_mask);
			CHECK(stock_data.user_taxi_dir_mask == manual_data->user_taxi_dir_mask);
			CHECK(stock_data.one_way_taxi == manual_data->one_way_taxi);
			CHECK(stock_data.runway_flags == manual_data->runway_flags);
			CHECK(stock_data.edge_block_mask == manual_data->edge_block_mask);
		}
	}

	_current_company = saved_company;
	_settings_game.station.distant_join_stations = saved_distant_join;
	_settings_game.station.never_expire_airports = saved_never_expire;
	_settings_game.station.station_spread = saved_station_spread;
	_settings_game.economy.station_noise_level = saved_noise;
	_settings_game.difficulty.town_council_tolerance = saved_tolerance;
	TimerGameCalendar::year = saved_year;

	_station_pool.CleanPool();
	_town_pool.CleanPool();
	_company_pool.CleanPool();
	RebuildStationKdtree();
	RebuildTownKdtree();
	RebuildViewportKdtree();
}

TEST_CASE("ModularAirportTemplatePlacementReplacesTileKinds")
{
	MockEnvironment::Instance();
	static LanguageMetadata test_language;
	const std::filesystem::path language_file = std::filesystem::exists("build/lang/english.lng") ?
			"build/lang/english.lng" : "lang/english.lng";
	test_language.file = std::filesystem::absolute(language_file);
	REQUIRE(ReadLanguagePack(&test_language));

	const CompanyID saved_company = _current_company;
	const bool saved_distant_join = _settings_game.station.distant_join_stations;
	const bool saved_never_expire = _settings_game.station.never_expire_airports;
	const uint8_t saved_station_spread = _settings_game.station.station_spread;
	const bool saved_noise = _settings_game.economy.station_noise_level;
	const uint8_t saved_tolerance = _settings_game.difficulty.town_council_tolerance;
	const TimerGameCalendar::Year saved_year = TimerGameCalendar::year;

	_settings_game.station.distant_join_stations = true;
	_settings_game.station.never_expire_airports = true;
	_settings_game.station.station_spread = 64;
	_settings_game.economy.station_noise_level = false;
	_settings_game.difficulty.town_council_tolerance = TOWN_COUNCIL_PERMISSIVE;
	TimerGameCalendar::year = TimerGameCalendar::Year{2100};

	Map::Allocate(64, 64);
	_station_pool.CleanPool();
	_town_pool.CleanPool();
	_company_pool.CleanPool();
	RebuildStationKdtree();
	RebuildViewportKdtree();
	AirportSpec::ResetAirports();

	Company *company = Company::CreateAtIndex(CompanyID(0));
	REQUIRE(company != nullptr);
	company->money = INT64_MAX;
	company->clear_limit = UINT32_MAX;
	_current_company = company->index;

	Town *town = Town::CreateAtIndex(TownID(0), TileXY(32, 32));
	REQUIRE(town != nullptr);
	town->cache.population = 10000;
	RebuildTownKdtree();

	const TileIndex base = TileXY(4, 4);
	REQUIRE(CmdBuildModularAirportTile(DoCommandFlag::Execute, base, APT_APRON,
			NEW_STATION, false, 0, 0x0F, false, false).Succeeded());
	Station *st = Station::GetByTile(base);
	REQUIRE(st != nullptr);

	ModularTemplatePlacementData data;
	data.width = 1;
	data.height = 1;
	data.tiles.emplace_back();
	ModularTemplatePlacementTile &tile = data.tiles.back();
	tile.piece_type = APT_RUNWAY_END;
	tile.runway_flags = RUF_LANDING | RUF_TAKEOFF | RUF_DIR_HIGH;

	CommandCost result = CmdPlaceModularAirportTemplate(DoCommandFlag::Execute, base, st->index, false, data);
	CAPTURE(result.GetErrorMessage(), result.GetExtraErrorMessage());
	REQUIRE(result.Succeeded());
	const ModularAirportTileData *tile_data = st->airport.GetModularTileData(base);
	REQUIRE(tile_data != nullptr);
	CHECK(IsModularRunwayPiece(tile_data->piece_type));
	CHECK(tile_data->runway_flags == tile.runway_flags);

	tile.piece_type = APT_APRON;
	tile.one_way_taxi = true;
	tile.user_taxi_dir_mask = 0x02;
	result = CmdPlaceModularAirportTemplate(DoCommandFlag::Execute, base, st->index, false, data);
	CAPTURE(result.GetErrorMessage(), result.GetExtraErrorMessage());
	REQUIRE(result.Succeeded());
	tile_data = st->airport.GetModularTileData(base);
	REQUIRE(tile_data != nullptr);
	CHECK(tile_data->piece_type == APT_APRON);
	CHECK(tile_data->one_way_taxi);
	CHECK(tile_data->user_taxi_dir_mask == 0x02);

	/* Metadata-only decorations must survive template placement without ever
	 * writing their pseudo piece ID into the map's airport-gfx field. */
	static constexpr std::pair<ModularAirportPieceID, uint8_t> decorations[] = {
		{APT_MODULAR_FIRE_STATION, APT_APRON},
		{APT_MODULAR_CARGO_TERMINAL, APT_APRON},
		{APT_MODULAR_FUEL_FARM, APT_APRON},
		{APT_MODULAR_APPROACH_LIGHTS, APT_GRASS_1},
	};
	for (const auto &[piece, map_gfx] : decorations) {
		tile.piece_type = piece;
		tile.one_way_taxi = false;
		tile.user_taxi_dir_mask = 0x0F;
		result = CmdPlaceModularAirportTemplate(DoCommandFlag::Execute, base, st->index, false, data);
		CAPTURE(piece, result.GetErrorMessage(), result.GetExtraErrorMessage());
		REQUIRE(result.Succeeded());
		tile_data = st->airport.GetModularTileData(base);
		REQUIRE(tile_data != nullptr);
		CHECK(tile_data->piece_type == piece);
		CHECK(GetStationGfx(base) == map_gfx);
	}

	_current_company = saved_company;
	_settings_game.station.distant_join_stations = saved_distant_join;
	_settings_game.station.never_expire_airports = saved_never_expire;
	_settings_game.station.station_spread = saved_station_spread;
	_settings_game.economy.station_noise_level = saved_noise;
	_settings_game.difficulty.town_council_tolerance = saved_tolerance;
	TimerGameCalendar::year = saved_year;
	_station_pool.CleanPool();
	_town_pool.CleanPool();
	_company_pool.CleanPool();
	RebuildStationKdtree();
	RebuildTownKdtree();
	RebuildViewportKdtree();
}

TEST_CASE("ModularAirportAtomicDragPreservesInteractiveBuildSemantics")
{
	MockEnvironment::Instance();
	static LanguageMetadata test_language;
	const std::filesystem::path language_file = std::filesystem::exists("build/lang/english.lng") ?
			"build/lang/english.lng" : "lang/english.lng";
	test_language.file = std::filesystem::absolute(language_file);
	REQUIRE(ReadLanguagePack(&test_language));

	const CompanyID saved_company = _current_company;
	const bool saved_distant_join = _settings_game.station.distant_join_stations;
	const bool saved_never_expire = _settings_game.station.never_expire_airports;
	const uint8_t saved_station_spread = _settings_game.station.station_spread;
	const bool saved_noise = _settings_game.economy.station_noise_level;
	const uint8_t saved_tolerance = _settings_game.difficulty.town_council_tolerance;
	const uint16_t saved_authority_distance = _settings_game.economy.dist_local_authority;
	const Money saved_airport_build_price = _price[Price::BuildStationAirport];
	const TimerGameCalendar::Year saved_year = TimerGameCalendar::year;

	_settings_game.station.distant_join_stations = true;
	_settings_game.station.never_expire_airports = true;
	_settings_game.station.station_spread = 64;
	_settings_game.economy.station_noise_level = false;
	_settings_game.difficulty.town_council_tolerance = TOWN_COUNCIL_PERMISSIVE;
	_settings_game.economy.dist_local_authority = 20;
	_price[Price::BuildStationAirport] = 100;
	TimerGameCalendar::year = TimerGameCalendar::Year{2100};

	auto reset_world = [](TileIndex town_tile) {
		Map::Allocate(64, 64);
		_station_pool.CleanPool();
		_town_pool.CleanPool();
		_company_pool.CleanPool();
		RebuildStationKdtree();
		RebuildViewportKdtree();
		AirportSpec::ResetAirports();

		Company *company = Company::CreateAtIndex(CompanyID(0));
		REQUIRE(company != nullptr);
		company->money = INT64_MAX;
		company->clear_limit = UINT32_MAX;
		_current_company = company->index;

		Town *town = Town::CreateAtIndex(TownID(0), town_tile);
		REQUIRE(town != nullptr);
		town->cache.population = 10000;
		RebuildTownKdtree();
		return std::pair{company, town};
	};

	SECTION("runway extensions inherit existing usage flags")
	{
		reset_world(TileXY(32, 32));
		const TileIndex base = TileXY(4, 4);
		for (uint x = 0; x < 3; x++) {
			REQUIRE(CmdBuildModularAirportTile(DoCommandFlag::Execute, TileAddXY(base, x, 0), APT_RUNWAY_5,
					x == 0 ? NEW_STATION : StationID::Invalid(), false, 0, 0x0F, false, false).Succeeded());
		}
		Station *st = Station::GetByTile(base);
		REQUIRE(st != nullptr);
		const uint8_t configured_flags = RUF_LANDING | RUF_TAKEOFF | RUF_DIR_HIGH;
		REQUIRE(CmdSetRunwayFlags(DoCommandFlag::Execute, base, configured_flags).Succeeded());

		ModularTemplatePlacementData data;
		data.width = 2;
		data.height = 1;
		data.is_drag_build = true;
		data.tiles = {{0, 0, APT_RUNWAY_5, 0}, {1, 0, APT_RUNWAY_END, 0}};
		const TileIndex extension = TileAddXY(base, 3, 0);
		REQUIRE(CmdPlaceModularAirportTemplate(DoCommandFlag::Execute, extension, st->index, false, data).Succeeded());

		for (uint x = 0; x < 5; x++) {
			const ModularAirportTileData *tile_data = st->airport.GetModularTileData(TileAddXY(base, x, 0));
			REQUIRE(tile_data != nullptr);
			CHECK(tile_data->runway_flags == configured_flags);
		}
	}

	SECTION("ground-only starter drags keep the Airport name class")
	{
		reset_world(TileXY(32, 32));
		const TileIndex base = TileXY(4, 4);
		ModularTemplatePlacementData data;
		data.width = 2;
		data.height = 1;
		data.is_drag_build = true;
		data.tiles = {{0, 0, APT_EMPTY}, {1, 0, APT_EMPTY}};
		REQUIRE(CmdPlaceModularAirportTemplate(DoCommandFlag::Execute, base, StationID::Invalid(), false, data).Succeeded());
		const Station *st = Station::GetByTile(base);
		REQUIRE(st != nullptr);
		CHECK(st->string_id == STR_SV_STNAME_AIRPORT);
		CHECK(Station::GetByTile(TileAddXY(base, 1, 0)) == st);
	}

	SECTION("the full drag cost is checked before construction")
	{
		Company *company = reset_world(TileXY(32, 32)).first;
		company->money = 0;
		const TileIndex base = TileXY(4, 4);
		ModularTemplatePlacementData data;
		data.width = 2;
		data.height = 1;
		data.is_drag_build = true;
		data.tiles = {{0, 0, APT_EMPTY}, {1, 0, APT_EMPTY}};
		CHECK(CmdPlaceModularAirportTemplate(DoCommandFlag::Execute, base, StationID::Invalid(), false, data).Failed());
		CHECK(!IsTileType(base, TileType::Station));
		CHECK(!IsTileType(TileAddXY(base, 1, 0), TileType::Station));
	}

	SECTION("cumulative tree-clearing penalties reject the whole drag")
	{
		auto [company, town] = reset_world(TileXY(10, 10));
		_settings_game.difficulty.town_council_tolerance = TOWN_COUNCIL_TOLERANT;
		town->ratings[company->index] = 0;
		const TileIndex base = TileXY(10, 11);
		ModularTemplatePlacementData data;
		data.width = 7;
		data.height = 1;
		data.is_drag_build = true;
		for (uint8_t x = 0; x < data.width; x++) {
			const TileIndex tile = TileAddXY(base, x, 0);
			MakeTree(Tile(tile), TREE_TEMPERATE, 0, TreeGrowthStage::Grown, TreeGround::Grass, 3);
			data.tiles.push_back({x, 0, APT_EMPTY});
		}

		const CommandCost result = Command<Commands::PlaceModularAirportTemplate>::Do(DoCommandFlag::Execute,
				base, StationID::Invalid(), false, data);
		CHECK(result.Failed());
		for (uint8_t x = 0; x < data.width; x++) CHECK(IsTileType(TileAddXY(base, x, 0), TileType::Trees));
		CHECK(town->ratings[company->index] == 0);
	}

	_current_company = saved_company;
	_settings_game.station.distant_join_stations = saved_distant_join;
	_settings_game.station.never_expire_airports = saved_never_expire;
	_settings_game.station.station_spread = saved_station_spread;
	_settings_game.economy.station_noise_level = saved_noise;
	_settings_game.difficulty.town_council_tolerance = saved_tolerance;
	_settings_game.economy.dist_local_authority = saved_authority_distance;
	_price[Price::BuildStationAirport] = saved_airport_build_price;
	TimerGameCalendar::year = saved_year;
	_station_pool.CleanPool();
	_town_pool.CleanPool();
	_company_pool.CleanPool();
	RebuildStationKdtree();
	RebuildTownKdtree();
	RebuildViewportKdtree();
}

TEST_CASE("ModularAirportTileBuildNamingFollowsTheFirstPiece")
{
	MockEnvironment::Instance();
	static LanguageMetadata test_language;
	const std::filesystem::path language_file = std::filesystem::exists("build/lang/english.lng") ?
			"build/lang/english.lng" : "lang/english.lng";
	test_language.file = std::filesystem::absolute(language_file);
	REQUIRE(ReadLanguagePack(&test_language));

	const CompanyID saved_company = _current_company;
	const bool saved_noise = _settings_game.economy.station_noise_level;
	const uint8_t saved_tolerance = _settings_game.difficulty.town_council_tolerance;
	const bool saved_never_expire = _settings_game.station.never_expire_airports;
	const uint8_t saved_station_spread = _settings_game.station.station_spread;
	const TimerGameCalendar::Year saved_year = TimerGameCalendar::year;
	_settings_game.economy.station_noise_level = false;
	_settings_game.difficulty.town_council_tolerance = TOWN_COUNCIL_PERMISSIVE;
	_settings_game.station.never_expire_airports = true;
	_settings_game.station.station_spread = 64;
	TimerGameCalendar::year = TimerGameCalendar::Year{2100};

	/* A station is named once, when its first tile creates it, so a tile-by-tile build
	 * has to answer "airport or heliport?" from one piece. A helipad is the only piece
	 * that answers it: nothing else is helicopter-only. Every other piece takes the
	 * generic name -- asking whether the piece is a runway instead named an airport
	 * begun with an apron "Heliport" for the rest of the game. */
	const std::array<std::pair<uint8_t, StringID>, 7> cases = {{
		{APT_APRON, STR_SV_STNAME_AIRPORT},
		{APT_STAND, STR_SV_STNAME_AIRPORT},
		{APT_DEPOT_SE, STR_SV_STNAME_AIRPORT},
		{APT_RUNWAY_END, STR_SV_STNAME_AIRPORT},
		{APT_HELIPAD_2, STR_SV_STNAME_HELIPORT},
		{APT_HELIPAD_3_FENCE_NW, STR_SV_STNAME_HELIPORT},
		/* The stock heliport is a single tile, so this is the whole layout: a
		 * hand-built heliport must end up named like the stock one built as modular. */
		{APT_HELIPORT, STR_SV_STNAME_HELIPORT},
	}};

	for (const auto &[first_piece, expected_name] : cases) {
		CAPTURE(first_piece);
		Map::Allocate(64, 64);
		_station_pool.CleanPool();
		_town_pool.CleanPool();
		_company_pool.CleanPool();
		RebuildStationKdtree();
		RebuildViewportKdtree();
		AirportSpec::ResetAirports();

		Company *company = Company::CreateAtIndex(CompanyID(0));
		REQUIRE(company != nullptr);
		company->money = INT64_MAX;
		company->clear_limit = UINT32_MAX;
		_current_company = company->index;

		Town *town = Town::CreateAtIndex(TownID(0), TileXY(32, 32));
		REQUIRE(town != nullptr);
		town->cache.population = 10000;
		RebuildTownKdtree();

		const TileIndex base = TileXY(4, 4);
		REQUIRE(CmdBuildModularAirportTile(DoCommandFlag::Execute, base, first_piece,
				NEW_STATION, false, 0, 0x0F, false, false).Succeeded());
		const Station *st = Station::GetByTile(base);
		REQUIRE(st != nullptr);

		CHECK(st->string_id == expected_name);
	}

	/* The other half of the claim: the stock heliport built as modular derives its
	 * name from the finished layout. Both paths must land on the same name for the
	 * same one-tile layout, which is what makes the helipad case above worth having. */
	{
		Map::Allocate(64, 64);
		_station_pool.CleanPool();
		_town_pool.CleanPool();
		_company_pool.CleanPool();
		RebuildStationKdtree();
		RebuildViewportKdtree();
		AirportSpec::ResetAirports();

		Company *company = Company::CreateAtIndex(CompanyID(0));
		REQUIRE(company != nullptr);
		company->money = INT64_MAX;
		company->clear_limit = UINT32_MAX;
		_current_company = company->index;

		Town *town = Town::CreateAtIndex(TownID(0), TileXY(32, 32));
		REQUIRE(town != nullptr);
		town->cache.population = 10000;
		RebuildTownKdtree();

		const TileIndex base = TileXY(4, 4);
		const CommandCost stock_build = CmdBuildModularAirportFromStock(DoCommandFlag::Execute,
				base, AT_HELIPORT, 0, NEW_STATION, false);
		CAPTURE(stock_build.GetErrorMessage(), stock_build.GetExtraErrorMessage());
		REQUIRE(stock_build.Succeeded());
		const Station *st = Station::GetByTile(base);
		REQUIRE(st != nullptr);

		CHECK(st->string_id == STR_SV_STNAME_HELIPORT);
	}

	_current_company = saved_company;
	_settings_game.economy.station_noise_level = saved_noise;
	_settings_game.difficulty.town_council_tolerance = saved_tolerance;
	_settings_game.station.never_expire_airports = saved_never_expire;
	_settings_game.station.station_spread = saved_station_spread;
	TimerGameCalendar::year = saved_year;

	_station_pool.CleanPool();
	_town_pool.CleanPool();
	_company_pool.CleanPool();
	RebuildStationKdtree();
	RebuildTownKdtree();
	RebuildViewportKdtree();
}

TEST_CASE("ModularAirportIncrementalNoiseMatchesFullRecompute")
{
	Map::Allocate(64, 64);
	MockEnvironment::Instance();
	_town_pool.CleanPool();
	Town *town = Town::CreateAtIndex(TownID(0), TileXY(4, 4));
	REQUIRE(town != nullptr);
	town->cache.population = 10000;
	RebuildTownKdtree();

	const TileIndex base = TileXY(10, 10);
	Station *st = SetupModularAirport(base, 8, 8);
	REQUIRE(st != nullptr);

	const std::array<std::pair<TileIndex, uint8_t>, 5> additions = {{
		{base, APT_APRON},
		{base + TileDiffXY(1, 0), APT_STAND},
		{base + TileDiffXY(2, 0), APT_DEPOT_SE},
		{base + TileDiffXY(3, 0), APT_RUNWAY_END},
		{base + TileDiffXY(4, 0), APT_RUNWAY_5},
	}};

	UpdateAirportsNoise();
	CHECK(town->noise_reached == 0);
	for (const auto &[tile, piece_type] : additions) {
		const ModularAirportNoiseSnapshot before = GetModularAirportNoiseSnapshot(st);
		AddModularTile(st, tile, piece_type);
		ApplyModularAirportNoiseChange(st, before);
		const uint16_t incremental = town->noise_reached;
		UpdateAirportsNoise();
		CHECK(town->noise_reached == incremental);
	}

	for (auto it = additions.rbegin(); it != additions.rend(); ++it) {
		const ModularAirportNoiseSnapshot before = GetModularAirportNoiseSnapshot(st);
		std::erase_if(*st->airport.modular_tile_data, [&](const ModularAirportTileData &data) { return data.tile == it->first; });
		st->airport.modular_tile_index_dirty = true;
		st->airport.MarkLayoutDirty();
		ApplyModularAirportNoiseChange(st, before);
		const uint16_t incremental = town->noise_reached;
		UpdateAirportsNoise();
		CHECK(town->noise_reached == incremental);
	}
	CHECK(town->noise_reached == 0);
}

TEST_CASE("ModularAirportIncrementalNoiseMovesBetweenNearestTowns")
{
	Map::Allocate(64, 64);
	MockEnvironment::Instance();
	_town_pool.CleanPool();
	Town *left_town = Town::CreateAtIndex(TownID(0), TileXY(4, 4));
	Town *right_town = Town::CreateAtIndex(TownID(1), TileXY(50, 4));
	REQUIRE(left_town != nullptr);
	REQUIRE(right_town != nullptr);
	left_town->cache.population = 10000;
	right_town->cache.population = 10000;
	RebuildTownKdtree();

	const TileIndex left_tile = TileXY(6, 4);
	const TileIndex right_tile = TileXY(50, 5);
	Station *st = SetupModularAirport(left_tile, 1, 1);
	REQUIRE(st != nullptr);
	AddModularTile(st, left_tile, APT_HELIPORT);
	UpdateAirportsNoise();
	CHECK(left_town->noise_reached == 1);
	CHECK(right_town->noise_reached == 0);

	const ModularAirportNoiseSnapshot before_add = GetModularAirportNoiseSnapshot(st);
	MakeStation(right_tile, st->owner, st->index, StationType::Airport, 0);
	st->airport.Add(right_tile);
	AddModularTile(st, right_tile, APT_HELIPORT);
	ApplyModularAirportNoiseChange(st, before_add);
	CHECK(left_town->noise_reached == 0);
	CHECK(right_town->noise_reached == 2);
	UpdateAirportsNoise();
	CHECK(left_town->noise_reached == 0);
	CHECK(right_town->noise_reached == 2);

	const ModularAirportNoiseSnapshot before_remove = GetModularAirportNoiseSnapshot(st);
	std::erase_if(*st->airport.modular_tile_data,
			[&](const ModularAirportTileData &data) { return data.tile == right_tile; });
	DoClearSquare(right_tile);
	st->airport.tile = left_tile;
	st->airport.w = 1;
	st->airport.h = 1;
	st->airport.modular_tile_index_dirty = true;
	st->airport.MarkLayoutDirty();
	ApplyModularAirportNoiseChange(st, before_remove);
	CHECK(left_town->noise_reached == 1);
	CHECK(right_town->noise_reached == 0);
	UpdateAirportsNoise();
	CHECK(left_town->noise_reached == 1);
	CHECK(right_town->noise_reached == 0);
}

TEST_CASE("ModularAirportSafety")
{
	Map::Allocate(64, 64);
	TileIndex base = TileXY(10, 10);
	Station *st = SetupModularAirport(base, 10, 10);
	REQUIRE(st != nullptr);

	SECTION("Safety Requirements") {
		/* Empty airport. */
		ModularAirportSafetyRequirement status = GetModularAirportSafetyStatus(st);
		CHECK((status & MASR_TOWER) != 0);
		CHECK((status & MASR_BIG_TERMINAL) != 0);
		CHECK((status & MASR_LANDING_RUNWAY) != 0);
		CHECK((status & MASR_TAKEOFF_RUNWAY) != 0);

		/* Add tower. */
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
		CHECK(GetModularAirportNewGRFType(complete_st) == ATP_TTDP_LARGE);
	}
}

TEST_CASE("ModularAirportHoldingLoop")
{
	SECTION("Runway approach direction follows world axes") {
		Map::Allocate(64, 64);
		const TileIndex base = TileXY(10, 10);
		Station *st = SetupModularAirport(base, 10, 10);
		REQUIRE(st != nullptr);

		const TileIndex horizontal_low = base + TileDiffXY(1, 1);
		const TileIndex vertical_low = base + TileDiffXY(5, 1);
		AddLargeRunway(st, horizontal_low, 3, 0);
		AddLargeRunway(st, vertical_low, 3, 1);

		CHECK(GetRunwayApproachDirection(st, horizontal_low) == Direction::SW);
		CHECK(GetRunwayApproachDirection(st, horizontal_low + TileDiffXY(2, 0)) == Direction::NE);
		CHECK(GetRunwayApproachDirection(st, vertical_low) == Direction::SE);
		CHECK(GetRunwayApproachDirection(st, vertical_low + TileDiffXY(0, 2)) == Direction::NW);
	}

	SECTION("IsHoldingGateActive") {
		/* 8 waypoints loop. */
		CHECK(IsHoldingGateActive(0, 0, 8)); // AT gate
		CHECK(IsHoldingGateActive(7, 0, 8)); // Just before gate (wrap)
		CHECK_FALSE(IsHoldingGateActive(1, 0, 8)); // Just passed gate

		CHECK(IsHoldingGateActive(3, 4, 8)); // Just before gate
		CHECK(IsHoldingGateActive(4, 4, 8)); // AT gate
		CHECK_FALSE(IsHoldingGateActive(5, 4, 8)); // Just passed gate

		/* Edge cases. */
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
		/* (150, 300) is equally close to (100, 200) and (200, 200):
		 * dist^2 = 50^2 + 100^2 = 12500. The tie goes to lower index (2). */
		CHECK(GetNearestModularHoldingWaypoint(v, loop) == 2);
	}
}

TEST_CASE("ModularAirportLargeAircraftLandingRunwayChoice")
{
	Map::Allocate(64, 64);
	const TileIndex base = TileXY(10, 10);
	Station *st = SetupModularAirport(base, 12, 8);
	REQUIRE(st != nullptr);

	/* Match the reported Pindborough layout: a short runway operating toward the
	 * high end and a long runway operating in the opposite direction. The holding
	 * loop reaches both gates, but a jet must wait for the long-runway gate. */
	const TileIndex short_low = base + TileDiffXY(1, 1);
	const TileIndex long_low = base + TileDiffXY(1, 3);
	const TileIndex long_high = long_low + TileDiffXY(5, 0);
	AddSmallRunway(st, short_low, 4, 0, RUF_LANDING | RUF_TAKEOFF | RUF_DIR_HIGH);
	AddLargeRunway(st, long_low, 6, 0, RUF_LANDING | RUF_TAKEOFF | RUF_DIR_LOW);

	_engine_pool.CleanPool();
	const EngineID jet_engine = CreateAircraftEngine(EngineID(0), AIR_FAST);
	const EngineID prop_engine = CreateAircraftEngine(EngineID(1), 0);
	SetupAircraftPool();
	Aircraft *jet = CreateAircraft(VehicleID(0));
	jet->engine_type = jet_engine;
	jet->x_pos = TileX(short_low) * TILE_SIZE;
	jet->y_pos = TileY(short_low) * TILE_SIZE;

	CHECK_FALSE(CanAircraftUseModularRunwayForLanding(st, jet, short_low));
	CHECK(CanAircraftUseModularRunwayForLanding(st, jet, long_high));
	CHECK(FindModularLandingTarget(st, jet) == long_high);

	/* Small aircraft may continue using the short strip. */
	Aircraft *prop = CreateAircraft(VehicleID(1));
	prop->engine_type = prop_engine;
	CHECK(CanAircraftUseModularRunwayForLanding(st, prop, short_low));

	/* Preserve the original fallback: when no large-safe runway accepts landings,
	 * a jet may use the short strip and receives the elevated overrun risk. */
	for (uint i = 0; i < 6; ++i) {
		st->airport.GetModularTileData(long_low + TileDiffXY(i, 0))->runway_flags = RUF_TAKEOFF | RUF_DIR_LOW;
	}
	/* Writing tile data behind the command's back skips the invalidation that
	 * SetRunwayFlags_Apply does, and the large-safe runway answer is cached. */
	st->airport.MarkLayoutDirty();
	CHECK(CanAircraftUseModularRunwayForLanding(st, jet, short_low));
	CHECK(FindModularLandingTarget(st, jet) == short_low);
}

TEST_CASE("ModularAirportPathfinding")
{
	Map::Allocate(64, 64);
	TileIndex base = TileXY(10, 10);
	Station *st = SetupModularAirport(base, 10, 10);
	REQUIRE(st != nullptr);

	SECTION("Simple Taxi Path") {
		/* Hangar (10,10) -> Taxiway -> Stand (13,10). */
		/* Rotation 3 is SW, which is dx=+1 -- the direction this chain runs.
		 * This said rotation 1 when the pathfinder had NE and SW swapped; rotation 1
		 * is NE, draws _station_display_modular_hangar_ne, and opens towards dx=-1. */
		AddModularTile(st, base, APT_DEPOT_SE, 3);
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

		/* Alternative path around (1,0). */
		AddModularTile(st, base + TileDiffXY(0, 1), APT_APRON, 0);
		AddModularTile(st, base + TileDiffXY(1, 1), APT_APRON, 0);

		TileIndex start = base;

		/* Path with avoidance. */
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

	SECTION("Fixed Wing Aircraft Do Not Taxi Across Helipads") {
		/* A single chain apron -> helipad -> apron, with no way around the pad. */
		AddModularTile(st, base, APT_APRON, 0);
		AddModularTile(st, base + TileDiffXY(1, 0), APT_HELIPAD_2, 0);
		AddModularTile(st, base + TileDiffXY(2, 0), APT_APRON, 0);

		const TileIndex pad = base + TileDiffXY(1, 0);
		const TileIndex far_apron = base + TileDiffXY(2, 0);

		SetupAircraftPool();
		Aircraft *plane = CreateAircraft(VehicleID(5));
		plane->tile = base;
		plane->ground_path_goal = far_apron;

		Aircraft *heli = CreateAircraft(VehicleID(6));
		heli->subtype = AIR_HELICOPTER;
		heli->tile = base;
		heli->ground_path_goal = far_apron;

		/* The pad is unreserved and unoccupied, so nothing but the type rule stops the
		 * plane -- this is exactly the case that used to route straight over it. */
		CHECK_FALSE(FindAirportGroundPath(st, base, far_apron, plane, false, false).found);
		CHECK(FindAirportGroundPath(st, base, far_apron, heli, false, false).found);

		/* A plane already standing on a pad must still be able to get off it. */
		CHECK(FindAirportGroundPath(st, pad, far_apron, plane, false, false).found);

		/* The restriction travels independently of the aircraft, so the reachability
		 * probes (which pass v = nullptr to ignore stand occupancy) get the same answer. */
		CHECK_FALSE(FindAirportGroundPath(st, base, far_apron, nullptr, false, false, GroundPathRestriction::FixedWing).found);
		CHECK(FindAirportGroundPath(st, base, far_apron, nullptr, false, false, GroundPathRestriction::None).found);

		CHECK(GetGroundPathRestriction(plane) == GroundPathRestriction::FixedWing);
		CHECK(GetGroundPathRestriction(heli) == GroundPathRestriction::None);
		CHECK(GetGroundPathRestriction(nullptr) == GroundPathRestriction::None);
	}

	SECTION("Fixed Wing Aircraft Detour Around A Helipad") {
		/* Same chain, but with a taxiable way around the pad. */
		AddModularTile(st, base, APT_APRON, 0);
		AddModularTile(st, base + TileDiffXY(1, 0), APT_HELIPAD_2, 0);
		AddModularTile(st, base + TileDiffXY(2, 0), APT_APRON, 0);
		AddModularTile(st, base + TileDiffXY(0, 1), APT_APRON, 0);
		AddModularTile(st, base + TileDiffXY(1, 1), APT_APRON, 0);
		AddModularTile(st, base + TileDiffXY(2, 1), APT_APRON, 0);

		SetupAircraftPool();
		Aircraft *plane = CreateAircraft(VehicleID(7));
		plane->tile = base;
		plane->ground_path_goal = base + TileDiffXY(2, 0);

		AirportGroundPath path = FindAirportGroundPath(st, base, base + TileDiffXY(2, 0), plane, false, false);
		REQUIRE(path.found);
		for (TileIndex t : path.tiles) {
			CHECK(t != base + TileDiffXY(1, 0));
		}
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

		/* Set one-way flag manually and allow east. */
		ModularAirportTileData *td1 = st->airport.GetModularTileData(base + TileDiffXY(1, 0));
		td1->one_way_taxi = true;
		td1->user_taxi_dir_mask = 0x02; // East

		AddModularTile(st, base + TileDiffXY(3, 0), APT_RUNWAY_1, 0); // Runway segment (horizontal)

		TaxiPath path = BuildTaxiPath(st, base, base + TileDiffXY(3, 0));
		CHECK(path.valid);
		/* Segments: apron (free move), one-way, apron (free move), runway. */
		REQUIRE(path.segments.size() >= 4);
		CHECK(path.segments[0].type == TaxiSegmentType::FreeMove);
		CHECK(path.segments[1].type == TaxiSegmentType::OneWay);
		CHECK(path.segments[2].type == TaxiSegmentType::FreeMove);
		CHECK(path.segments[3].type == TaxiSegmentType::Runway);
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

		/* Another aircraft trying to reserve. */
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
		/* Create a 3-tile horizontal runway. */
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
		CHECK(IsModularBuildingPiece(APT_MODULAR_FIRE_STATION));
		CHECK(IsModularBuildingPiece(APT_MODULAR_CARGO_TERMINAL));
		CHECK(IsModularBuildingPiece(APT_MODULAR_FUEL_FARM));
		CHECK_FALSE(IsModularBuildingPiece(APT_MODULAR_APPROACH_LIGHTS));
		CHECK_FALSE(IsModularBuildingPiece(APT_APRON));
		CHECK_FALSE(IsModularBuildingPiece(APT_RUNWAY_1));
	}

	SECTION("Decoration Map Graphics") {
		CHECK(IsModularAirportDecorationPiece(APT_MODULAR_FIRE_STATION));
		CHECK(IsModularAirportDecorationPiece(APT_MODULAR_APPROACH_LIGHTS));
		CHECK_FALSE(IsModularAirportDecorationPiece(APT_APRON));
		CHECK(GetModularAirportMapGfx(APT_MODULAR_FIRE_STATION) == APT_APRON);
		CHECK(GetModularAirportMapGfx(APT_MODULAR_CARGO_TERMINAL) == APT_APRON);
		CHECK(GetModularAirportMapGfx(APT_MODULAR_FUEL_FARM) == APT_APRON);
		CHECK(GetModularAirportMapGfx(APT_MODULAR_APPROACH_LIGHTS) == APT_GRASS_1);
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

TEST_CASE("ModularAirportDecorationDrawing")
{
	struct DecorationCase {
		ModularAirportPieceID piece;
		SpriteID sprite;
		SpriteID ground;
	};
	static constexpr DecorationCase cases[] = {
		{APT_MODULAR_FIRE_STATION, SPR_AIRPORT_FIRE_STATION, SPR_AIRPORT_APRON},
		{APT_MODULAR_CARGO_TERMINAL, SPR_AIRPORT_CARGO_TERMINAL, SPR_AIRPORT_APRON},
		{APT_MODULAR_FUEL_FARM, SPR_AIRPORT_FUEL_FARM, SPR_AIRPORT_APRON},
		{APT_MODULAR_APPROACH_LIGHTS, SPR_AIRPORT_APPROACH_LIGHTS, SPR_FLAT_GRASS_TILE},
	};

	for (const DecorationCase &c : cases) {
		CAPTURE(c.piece);
		const DrawTileSprites *layout = GetAirportTileLayoutWithModularOverrides(
				GetModularAirportMapGfx(c.piece), c.piece, 0, 0);
		REQUIRE(layout != nullptr);
		CHECK((layout->ground.sprite & SPRITE_MASK) == c.ground);
		bool found = false;
		for (const DrawTileSeqStruct &dtss : layout->GetSequence()) {
			if ((dtss.image.sprite & SPRITE_MASK) == c.sprite) found = true;
		}
		CHECK(found);
	}

	const DrawTileSprites *rotated = GetAirportTileLayoutWithModularOverrides(
			APT_GRASS_1, APT_MODULAR_APPROACH_LIGHTS, 1, 0);
	REQUIRE(rotated != nullptr);
	bool found_rotated = false;
	for (const DrawTileSeqStruct &dtss : rotated->GetSequence()) {
		if ((dtss.image.sprite & SPRITE_MASK) == SPR_AIRPORT_APPROACH_LIGHTS_OTHER) found_rotated = true;
	}
	CHECK(found_rotated);
}

TEST_CASE("ModularAirportBuilderVocabulary")
{
	/* Which airport graphics a modular airport may be built from is a design
	 * decision, made once, in the builder's own piece tables. Every other way of
	 * placing a modular tile has to answer to it.
	 *
	 * The one that matters is the script API: an AI or game script that could
	 * reach graphics the toolbar does not offer would build airports no player
	 * could, out of tiles cut from stock layouts -- the stock city airport's stands
	 * carry a jetway, several tiles carry a baked-in fence, and some are one half
	 * of a two-tile building. So the two sets are held equal here rather than
	 * merely overlapping: a piece added to one and not the other fails this test,
	 * in whichever direction it was forgotten. */
	SECTION("The script API places exactly the builder's pieces") {
		const std::vector<ModularAirportPieceID> from_builder = GetModularAirportBuilderPieceGfx();

		std::vector<ModularAirportPieceID> from_script;
		static constexpr ScriptAirport::ModularPiece script_pieces[] = {
			ScriptAirport::MP_APRON, ScriptAirport::MP_STAND, ScriptAirport::MP_RUNWAY,
			ScriptAirport::MP_RUNWAY_END, ScriptAirport::MP_RUNWAY_SMALL_MIDDLE,
			ScriptAirport::MP_RUNWAY_SMALL_NEAR_END, ScriptAirport::MP_RUNWAY_SMALL_FAR_END,
			ScriptAirport::MP_HANGAR, ScriptAirport::MP_SMALL_HANGAR, ScriptAirport::MP_HELIPAD,
			ScriptAirport::MP_HELIPAD_PLAIN, ScriptAirport::MP_HELIPORT, ScriptAirport::MP_TERMINAL,
			ScriptAirport::MP_TERMINAL_ALT, ScriptAirport::MP_TERMINAL_OTHER,
			ScriptAirport::MP_TERMINAL_ROUND, ScriptAirport::MP_LOW_TERMINAL,
			ScriptAirport::MP_SMALL_TERMINAL_3, ScriptAirport::MP_TOWER,
			ScriptAirport::MP_RADIO_TOWER, ScriptAirport::MP_RADAR, ScriptAirport::MP_RADAR_GRASS,
			ScriptAirport::MP_FLAG_GRASS, ScriptAirport::MP_GRASS, ScriptAirport::MP_EMPTY,
			ScriptAirport::MP_FIRE_STATION, ScriptAirport::MP_CARGO_TERMINAL,
			ScriptAirport::MP_FUEL_FARM, ScriptAirport::MP_APPROACH_LIGHTS,
		};
		for (ScriptAirport::ModularPiece piece : script_pieces) {
			CAPTURE(piece);
			const ModularAirportPieceID gfx = GetGfxForModularPiece(piece);
			REQUIRE(gfx != UINT16_MAX);
			from_script.push_back(gfx);
		}
		std::sort(from_script.begin(), from_script.end());
		from_script.erase(std::unique(from_script.begin(), from_script.end()), from_script.end());

		CHECK(from_script == from_builder);

		CHECK(ScriptAirport::MP_GRASS == 23);
		CHECK(ScriptAirport::MP_EMPTY == 24);
		CHECK(GetGfxForModularPiece(static_cast<ScriptAirport::ModularPiece>(29)) == UINT16_MAX);
	}

	SECTION("A compound piece is named by one of its own tiles") {
		/* Otherwise building it and reading it back disagree: the script asks for
		 * the naming graphic, and every tile that lands on the map reports some
		 * other piece -- or none. */
		for (ModularAirportPieceID gfx : GetModularAirportBuilderPieceGfx()) {
			CAPTURE(gfx);
			const std::span<const ModularCompoundPieceTile> compound = GetModularCompoundPieceTiles(gfx);
			if (compound.empty()) continue;

			bool names_itself = false;
			for (const ModularCompoundPieceTile &ct : compound) {
				if (ct.gfx == gfx) names_itself = true;
				/* And every tile of it reads back as the whole piece. */
				CHECK(GetModularPieceForGfx(ct.gfx) == GetModularPieceForGfx(gfx));
			}
			CHECK(names_itself);
		}
	}

	SECTION("The small terminal is three tiles in a row and never rotated") {
		const std::span<const ModularCompoundPieceTile> tiles = GetModularCompoundPieceTiles(APT_SMALL_BUILDING_2);
		REQUIRE(tiles.size() == 3);
		for (int i = 0; i < 3; i++) {
			CAPTURE(i);
			CHECK(tiles[i].dx == i);
			CHECK(tiles[i].dy == 0);
		}
		CHECK(GetModularCompoundPieceSize(APT_SMALL_BUILDING_2).width == 3);
		CHECK(GetModularCompoundPieceSize(APT_SMALL_BUILDING_2).height == 1);
		CHECK(GetModularPieceForGfx(APT_SMALL_BUILDING_1) == ScriptAirport::MP_SMALL_TERMINAL_3);
		CHECK(GetModularPieceForGfx(APT_SMALL_BUILDING_3) == ScriptAirport::MP_SMALL_TERMINAL_3);
	}

	SECTION("Ordinary pieces have no compound footprint") {
		CHECK(GetModularCompoundPieceTiles(APT_STAND).empty());
		CHECK(GetModularCompoundPieceSize(APT_STAND).width == 1);
		CHECK(GetModularCompoundPieceSize(APT_STAND).height == 1);
	}

	SECTION("The stock city airport's jetway stands are not placeable") {
		/* The case that put this test here. They stay readable -- a converted stock
		 * airport is full of them -- but as plain stands, under a name a script
		 * cannot pass back to the build command. */
		CHECK(GetModularPieceForGfx(APT_STAND_1) == ScriptAirport::MP_STAND);
		CHECK(GetModularPieceForGfx(APT_STAND_PIER_NE) == ScriptAirport::MP_STAND);
		CHECK(GetGfxForModularPiece(ScriptAirport::MP_STAND) == APT_STAND);
	}
}

TEST_CASE("ModularAirportStandJetwayDrawing")
{
	/* Does this layout draw a jetway on the tile? */
	auto HasJetway = [](const DrawTileSprites *t) {
		if (t == nullptr) return false;
		for (const DrawTileSeqStruct &dtss : t->GetSequence()) {
			SpriteID spr = dtss.image.sprite & SPRITE_MASK;
			if (spr == SPR_AIRPORT_JETWAY_1 || spr == SPR_AIRPORT_JETWAY_2 || spr == SPR_AIRPORT_JETWAY_3) return true;
		}
		return false;
	};

	/* A jetway bridges a stand to the round terminal beside it, so it belongs to
	 * the pair of tiles rather than to the stand. APT_STAND_1 and APT_STAND_PIER_NE
	 * carry one in their stock layouts only because the stock city airport never
	 * places them anywhere else; a modular airport can, and a script that picks
	 * them for their large-terminal value must not get a jetway to nowhere.
	 *
	 * Adjacency lives in ApplyModularAirportTileLayoutOverrides, which needs a map.
	 * This covers the context-free half: the default for every stand piece is a
	 * bare stand. */
	SECTION("Stand pieces default to no jetway") {
		CHECK_FALSE(HasJetway(GetAirportTileLayoutWithModularOverrides(APT_STAND, APT_STAND, 0, 0)));
		CHECK_FALSE(HasJetway(GetAirportTileLayoutWithModularOverrides(APT_STAND_1, APT_STAND_1, 0, 0)));
		CHECK_FALSE(HasJetway(GetAirportTileLayoutWithModularOverrides(APT_STAND_PIER_NE, APT_STAND_PIER_NE, 0, 0)));
	}

	SECTION("Stock layouts are the ones that bake a jetway in") {
		/* Guards the premise: if these ever stop carrying a jetway, the override
		 * above is dead code rather than a fix. */
		CHECK(HasJetway(GetStationTileLayout(StationType::Airport, APT_STAND_1)));
		CHECK(HasJetway(GetStationTileLayout(StationType::Airport, APT_STAND_PIER_NE)));
		CHECK_FALSE(HasJetway(GetStationTileLayout(StationType::Airport, APT_STAND)));
	}

	/* The half that needs a map: what is actually next to the stand. */
	Map::Allocate(64, 64);
	const TileIndex base = TileXY(10, 10);
	Station *st = SetupModularAirport(base, 4, 4);
	REQUIRE(st != nullptr);

	const TileIndex stand = base + TileDiffXY(1, 1);

	/* Ask the drawing code what it would put on the stand tile. */
	auto StandLayout = [&](uint8_t stand_piece) {
		AddModularTile(st, stand, stand_piece);
		TileInfo ti{};
		ti.tile = stand;
		StationGfx gfx = stand_piece;
		const DrawTileSprites *t = nullptr;
		ApplyModularAirportTileLayoutOverrides(&ti, gfx, t);
		return t;
	};

	SECTION("A round terminal to the SE gives the stand a jetway") {
		AddModularTile(st, stand + TileDiffXY(0, 1), APT_ROUND_TERMINAL);
		CHECK(HasJetway(StandLayout(APT_STAND)));
	}

	SECTION("A round terminal to the NE gives the stand a jetway") {
		AddModularTile(st, stand + TileDiffXY(-1, 0), APT_ROUND_TERMINAL);
		CHECK(HasJetway(StandLayout(APT_STAND)));
	}

	SECTION("A large-terminal stand gets a jetway from the terminal, not from itself") {
		/* The reported glitch: a script places APT_STAND_1 for its large-terminal
		 * value, next to a hangar, and the stand sprouts a jetway onto nothing. */
		AddModularTile(st, stand + TileDiffXY(0, 1), APT_DEPOT_SE);
		CHECK_FALSE(HasJetway(StandLayout(APT_STAND_1)));
	}

	SECTION("A large-terminal stand still gets its jetway beside a round terminal") {
		AddModularTile(st, stand + TileDiffXY(0, 1), APT_ROUND_TERMINAL);
		CHECK(HasJetway(StandLayout(APT_STAND_1)));
	}

	SECTION("A hangar beside a plain stand draws no jetway") {
		AddModularTile(st, stand + TileDiffXY(0, 1), APT_DEPOT_SE);
		AddModularTile(st, stand + TileDiffXY(-1, 0), APT_APRON);
		CHECK_FALSE(HasJetway(StandLayout(APT_STAND)));
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
		/* Large family. */
		CHECK(GetCanonicalRunwaySegmentPiece(true, 5, 0) == APT_RUNWAY_END);
		CHECK(GetCanonicalRunwaySegmentPiece(true, 5, 2) == APT_RUNWAY_5);
		CHECK(GetCanonicalRunwaySegmentPiece(true, 5, 4) == APT_RUNWAY_END);
		CHECK(GetCanonicalRunwaySegmentPiece(true, 1, 0) == APT_RUNWAY_END);

		/* Small family. */
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
		CHECK(DirectionsWithin45(Direction::N, Direction::N));
		CHECK(DirectionsWithin45(Direction::N, Direction::NE));
		CHECK(DirectionsWithin45(Direction::N, Direction::NW));
		CHECK_FALSE(DirectionsWithin45(Direction::N, Direction::E));
		CHECK_FALSE(DirectionsWithin45(Direction::N, Direction::S));

		/* Wrap around. */
		CHECK(DirectionsWithin45(Direction::NW, Direction::N));
		CHECK(DirectionsWithin45(Direction::NW, Direction::W));
	}
}

TEST_CASE("ModularAirportParkedDirection")
{
	Map::Allocate(64, 64);
	const TileIndex base = TileXY(10, 10);
	Station *st = SetupModularAirport(base, 5, 5);
	REQUIRE(st != nullptr);
	const TileIndex stand = base + TileDiffXY(2, 2);

	SECTION("One-way masks map from tile axes to viewport directions") {
		ModularAirportTileData *one_way = AddModularTileWithData(st, stand, APT_APRON);
		one_way->one_way_taxi = true;

		static constexpr std::array<std::pair<uint8_t, Direction>, 4> cases = {{
			{0x01, Direction::NW},
			{0x02, Direction::SW},
			{0x04, Direction::SE},
			{0x08, Direction::NE},
		}};
		for (const auto &[mask, direction] : cases) {
			one_way->user_taxi_dir_mask = mask;
			CHECK(GetModularAircraftParkedDirection(st, stand) == direction);
		}
	}

	SECTION("A stand faces a terminal on each edge") {
		AddModularTile(st, stand, APT_STAND);
		AddModularTile(st, stand + TileDiffXY(0, 1), APT_BUILDING_1);
		CHECK(GetModularAircraftParkedDirection(st, stand) == Direction::SE);
	}

	SECTION("A stand faces a terminal on its -X edge") {
		AddModularTile(st, stand, APT_STAND);
		AddModularTile(st, stand + TileDiffXY(-1, 0), APT_BUILDING_1);
		CHECK(GetModularAircraftParkedDirection(st, stand) == Direction::NE);
	}

	SECTION("A stand faces a terminal on its -Y edge") {
		AddModularTile(st, stand, APT_STAND);
		AddModularTile(st, stand + TileDiffXY(0, -1), APT_BUILDING_1);
		CHECK(GetModularAircraftParkedDirection(st, stand) == Direction::NW);
	}

	SECTION("A stand faces a terminal on its +X edge") {
		AddModularTile(st, stand, APT_STAND);
		AddModularTile(st, stand + TileDiffXY(1, 0), APT_BUILDING_1);
		CHECK(GetModularAircraftParkedDirection(st, stand) == Direction::SW);
	}

	SECTION("Every terminal-building family qualifies") {
		AddModularTile(st, stand, APT_STAND);
		ModularAirportTileData *terminal = AddModularTileWithData(st, stand + TileDiffXY(0, 1), APT_BUILDING_1);
		static constexpr std::array<uint8_t, 10> terminal_pieces = {{
			APT_BUILDING_1, APT_BUILDING_2, APT_BUILDING_3, APT_ROUND_TERMINAL,
			APT_LOW_BUILDING, APT_LOW_BUILDING_FENCE_N, APT_LOW_BUILDING_FENCE_NW,
			APT_SMALL_BUILDING_1, APT_SMALL_BUILDING_2, APT_SMALL_BUILDING_3,
		}};
		for (uint8_t piece_type : terminal_pieces) {
			terminal->piece_type = piece_type;
			CHECK(GetModularAircraftParkedDirection(st, stand) == Direction::SE);
		}
	}

	SECTION("Unrelated buildings do not set a stand direction") {
		AddModularTile(st, stand, APT_STAND);
		AddModularTile(st, stand + TileDiffXY(0, 1), APT_TOWER);
		CHECK(GetModularAircraftParkedDirection(st, stand) == Direction::Invalid);
	}

	SECTION("A jetway-compatible round terminal wins a deterministic tie") {
		AddModularTile(st, stand, APT_STAND);
		AddModularTile(st, stand + TileDiffXY(0, 1), APT_BUILDING_1);
		AddModularTile(st, stand + TileDiffXY(-1, 0), APT_ROUND_TERMINAL);
		CHECK(GetModularAircraftParkedDirection(st, stand) == Direction::NE);
	}

	SECTION("Parking waits for the smooth turn to finish") {
		AddModularTile(st, stand, APT_STAND);
		AddModularTile(st, stand + TileDiffXY(0, 1), APT_BUILDING_1);
		SetupAircraftPool();
		Aircraft *v = CreateAircraft(VehicleID(10));
		v->tile = stand;
		v->x_pos = TileX(stand) * TILE_SIZE + TILE_SIZE / 2;
		v->y_pos = TileY(stand) * TILE_SIZE + TILE_SIZE / 2;
		v->z_pos = GetTileMaxPixelZ(stand);
		v->direction = Direction::NW;

		static constexpr std::array<Direction, 4> expected = {
			Direction::N, Direction::NE, Direction::E, Direction::SE,
		};
		for (Direction direction : expected) {
			const bool finished = UpdateModularAircraftParkedDirection(v, st);
			CHECK(v->direction == direction);
			CHECK(finished == (direction == Direction::SE));
		}
	}

	SECTION("Loading event ticks continue the smooth parked turn") {
		AddModularTile(st, stand, APT_STAND);
		AddModularTile(st, stand + TileDiffXY(0, 1), APT_BUILDING_1);
		SetupAircraftPool();
		Aircraft *v = CreateAircraft(VehicleID(10));
		v->tile = stand;
		v->x_pos = TileX(stand) * TILE_SIZE + TILE_SIZE / 2;
		v->y_pos = TileY(stand) * TILE_SIZE + TILE_SIZE / 2;
		v->z_pos = GetTileMaxPixelZ(stand);
		v->targetairport = st->index;
		v->state = TERM1;
		v->direction = Direction::NW;
		v->current_order.MakeLoading(false);

		static constexpr std::array<Direction, 4> expected = {
			Direction::N, Direction::NE, Direction::E, Direction::SE,
		};
		for (Direction direction : expected) {
			REQUIRE(v->Tick());
			CHECK(v->current_order.IsType(OT_LOADING));
			CHECK(v->direction == direction);
		}
	}
}

/* The candidate cache is process-global, so an aborted REQUIRE between Begin and End
 * would leave it armed and silently switch every later test case onto the cached path. */
struct ScopedModularRunwayStateCache {
	ScopedModularRunwayStateCache() { BeginModularAirportRunwayStateCache(); }
	~ScopedModularRunwayStateCache() { EndModularAirportRunwayStateCache(); }
};

TEST_CASE("ModularAirportRunwayStateCacheTracksSameTickTransitions")
{
	Map::Allocate(64, 64);
	const TileIndex base = TileXY(10, 10);
	Station *st = SetupModularAirport(base, 10, 10);
	REQUIRE(st != nullptr);
	AddLargeRunway(st, base, 5, 0, RUF_DEFAULT);

	SetupAircraftPool();
	Aircraft *requester = CreateAircraft(VehicleID(10));
	requester->targetairport = st->index;
	Aircraft *blocker = CreateAircraft(VehicleID(11));
	blocker->targetairport = st->index;
	blocker->modular_landing_tile = base;
	blocker->state = FLYING;

	std::vector<TileIndex> runway_tiles;
	REQUIRE(GetContiguousModularRunwayTiles(st, base, runway_tiles));

	/* The first query lazily builds an empty candidate set. An aircraft that enters
	 * landing later in the same vehicle-tick pass must become visible immediately. */
	ScopedModularRunwayStateCache scoped_cache;
	VehicleID found = VehicleID::Invalid();
	CHECK_FALSE(IsContiguousModularRunwayReservedInStateByOther(requester, st, runway_tiles, &found));

	blocker->state = LANDING;
	UpdateModularAirportRunwayStateCache(blocker);
	found = VehicleID::Invalid();
	CHECK(IsContiguousModularRunwayReservedInStateByOther(requester, st, runway_tiles, &found));
	CHECK(found == blocker->index);

	/* Leaving a state does not need an erase: cached IDs are only candidates and
	 * every query still evaluates the aircraft's current fields. */
	blocker->state = FLYING;
	CHECK_FALSE(IsContiguousModularRunwayReservedInStateByOther(requester, st, runway_tiles));

	/* The same cache also serves the takeoff-queue check. */
	blocker->modular_ground_target = MGT_RUNWAY_TAKEOFF;
	blocker->modular_takeoff_tile = base;
	UpdateModularAirportRunwayStateCache(blocker);
	CHECK(IsContiguousModularRunwayQueuedForTakeoffByOther(requester, st, base));
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

TEST_CASE("ModularAirportOneWayWaitingDirection")
{
	Map::Allocate(64, 64);
	const TileIndex base = TileXY(10, 10);
	Station *st = SetupModularAirport(base, 8, 5);
	REQUIRE(st != nullptr);

	const TileIndex queue = base + TileDiffXY(2, 2);
	const TileIndex blocked = base + TileDiffXY(3, 2);
	const TileIndex goal = base + TileDiffXY(4, 2);
	ModularAirportTileData *one_way = AddModularTileWithData(st, queue, APT_APRON);
	one_way->one_way_taxi = true;
	one_way->user_taxi_dir_mask = 0x02; // +X, Direction::SW on screen.
	AddModularTile(st, blocked, APT_APRON);
	AddModularTile(st, goal, APT_STAND);

	SetupAircraftPool();
	Aircraft *v = CreateAircraft(VehicleID(10));
	v->targetairport = st->index;
	v->tile = queue;
	v->x_pos = TileX(queue) * TILE_SIZE + TILE_SIZE / 2;
	v->y_pos = TileY(queue) * TILE_SIZE + TILE_SIZE / 2;
	v->z_pos = GetTileMaxPixelZ(queue);
	v->direction = Direction::NE;
	v->ground_path_goal = goal;
	v->modular_ground_target = MGT_TERMINAL;
	CreateBlockerOnTile(st, VehicleID(11), blocked);

	/* Reservation failure leaves the aircraft at the queue centre, but each tick now
	 * turns it 45 degrees toward the one-way arrow instead of retaining its entry heading. */
	static constexpr std::array<Direction, 4> expected = {
		Direction::E, Direction::SE, Direction::S, Direction::SW,
	};
	for (Direction direction : expected) {
		CHECK_FALSE(AirportMoveModular(v, st));
		CHECK(v->tile == queue);
		CHECK(v->direction == direction);
	}
}

TEST_CASE("ModularAirportLandingChain")
{
	Map::Allocate(64, 64);
	TileIndex base = TileXY(10, 10);
	Station *st = SetupModularAirport(base, 10, 10);
	REQUIRE(st != nullptr);

	/* The landing helpers read the engine's subtype to tell a fast jet from
	 * everything else, so these aircraft need a real engine. A plain non-fast one
	 * keeps the runway-class rule out of the way of what is under test here. */
	_engine_pool.CleanPool();
	const EngineID prop_engine = CreateAircraftEngine(EngineID(0), 0);

	SECTION("Rejects path through occupied stand") {
		/* Layout (rotation 0, runway extends East along X):
		 *   Row 0: RWY_END  RWY_5    RWY_END     (rollout runway, tiles only)
		 *   Row 1:                   STAND_BLK   (sole exit from runway end)
		 *   Row 2:                   STAND_GOAL  (only reachable via STAND_BLK)
		 * Touchdown at (0,0), rollout at (2,0). Goal at (2,2).
		 * Planner returns path (2,0) -> (2,1) -> (2,2); the walk's blocked_by_other
		 * on (2,1) must reject the chain. */
		AddLargeRunway(st, base, 3, 0, RUF_DEFAULT);
		AddModularTile(st, base + TileDiffXY(2, 1), APT_STAND, 0);   // blocker sits here
		AddModularTile(st, base + TileDiffXY(2, 2), APT_STAND, 0);   // goal

		SetupAircraftPool();
		Aircraft *v = CreateAircraft(VehicleID(10));
		v->engine_type = prop_engine;
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

	SECTION("Reserves a transit runway crossing tile end-to-end") {
		/* Layout:
		 *   Row 0 (rollout runway):  RWY_END RWY_5 RWY_END
		 *   Row 1:                   APRON   APRON APRON
		 *   Row 2 (transit runway):  RWY_END RWY_5 RWY_END
		 *   Row 3:                   APRON   APRON STAND_GOAL
		 * Touchdown at (0,0), rollout (2,0). Goal at (2,3).
		 * Path: rollout -> apron(2,1) -> transit_runway(2,2) -> STAND_GOAL(2,3). */
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
		v->engine_type = prop_engine;
		v->targetairport = st->index;

		TileIndex touchdown = base;
		TileIndex goal = base + TileDiffXY(2, 3);
		REQUIRE(TryReserveLandingChain(v, st, touchdown, goal));

		/* Rollout runway atomically reserved. */
		for (int i = 0; i < 3; i++) {
			CHECK(IsModularAirportTileReservedBy(base + TileDiffXY(i, 0), v->index));
		}
		/* A crossed runway is ordinary path space: only the tile actually used. */
		CHECK_FALSE(HasModularAirportTileReservation(base + TileDiffXY(0, 2)));
		CHECK_FALSE(HasModularAirportTileReservation(base + TileDiffXY(1, 2)));
		CHECK(IsModularAirportTileReservedBy(base + TileDiffXY(2, 2), v->index));
		/* Goal stand reserved (end-to-end walk reaches the goal). */
		CHECK(IsModularAirportTileReservedBy(goal, v->index));
	}

	SECTION("Allows independent crossings at different runway tiles") {
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
		v->engine_type = prop_engine;
		v->targetairport = st->index;

		/* Another ground movement owns a different tile on the same runway. */
		CreateBlockerOnTile(st, VehicleID(11), base + TileDiffXY(0, 2));

		TileIndex touchdown = base;
		TileIndex goal = base + TileDiffXY(2, 3);
		REQUIRE(TryReserveLandingChain(v, st, touchdown, goal));

		/* The landing operation still owns its whole rollout runway. */
		for (int i = 0; i < 3; i++) {
			CHECK(IsModularAirportTileReservedBy(base + TileDiffXY(i, 0), v->index));
		}
		/* Only the crossing point is claimed, and the independent claim is untouched. */
		CHECK(IsModularAirportTileReservedBy(base + TileDiffXY(2, 2), v->index));
		CHECK(IsModularAirportTileReservedBy(base + TileDiffXY(0, 2), VehicleID(11)));
	}

	SECTION("A blocked crossing rejects the landing without a partial runway claim") {
		AddLargeRunway(st, base, 3, 0, RUF_DEFAULT);
		AddModularTile(st, base + TileDiffXY(2, 1), APT_APRON, 0);
		AddLargeRunway(st, base + TileDiffXY(0, 2), 3, 0, RUF_DEFAULT);
		AddModularTile(st, base + TileDiffXY(2, 3), APT_STAND, 0);

		SetupAircraftPool();
		Aircraft *v = CreateAircraft(VehicleID(10));
		v->engine_type = prop_engine;
		v->targetairport = st->index;
		CreateBlockerOnTile(st, VehicleID(11), base + TileDiffXY(2, 2));

		CHECK_FALSE(TryReserveLandingChain(v, st, base, base + TileDiffXY(2, 3)));
		for (int i = 0; i < 3; i++) {
			CHECK_FALSE(IsModularAirportTileReservedBy(base + TileDiffXY(i, 0), v->index));
		}
		CHECK(v->modular_runway_reservation.empty());
		CHECK(IsModularAirportTileReservedBy(base + TileDiffXY(2, 2), VehicleID(11)));
	}

	SECTION("Stops at first OneWay tile") {
		/* Layout:
		 *   Row 0: RWY_END RWY_5 RWY_END         (rollout runway)
		 *   Row 1: APRON   APRON APRON           (FreeMove)
		 *   Row 2: APRON   APRON APRON_ONEWAY    (OneWay tile at (2,2))
		 *   Row 3: APRON   APRON STAND_GOAL
		 * Path: rollout(2,0) -> (2,1) -> (2,2)[OneWay] -> (2,3)[goal].
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
		v->engine_type = prop_engine;
		v->targetairport = st->index;

		TileIndex touchdown = base;
		TileIndex goal = base + TileDiffXY(2, 3);
		REQUIRE(TryReserveLandingChain(v, st, touchdown, goal));

		/* Rollout runway reserved. */
		for (int i = 0; i < 3; i++) {
			CHECK(IsModularAirportTileReservedBy(base + TileDiffXY(i, 0), v->index));
		}
		/* (2,1) FreeMove reserved (segment before OneWay). */
		CHECK(IsModularAirportTileReservedBy(base + TileDiffXY(2, 1), v->index));
		/* (2,2) OneWay entry reserved -- the safe stop. */
		CHECK(IsModularAirportTileReservedBy(base + TileDiffXY(2, 2), v->index));
		/* (2,3) goal NOT reserved -- walk stopped at OneWay. */
		CHECK_FALSE(IsModularAirportTileReservedBy(base + TileDiffXY(2, 3), v->index));
	}
}

/**
 * Early rollout turn-off: a landing aircraft leaves the runway once it has braked to taxi
 * speed, instead of rolling to the far end of whatever runway it landed on.
 *
 * The distance is the aircraft's own braking distance, so these expectations are tied to
 * the deceleration model in UpdateAircraftSpeed and to the plane-speed setting. Each
 * section sets that setting explicitly: the test environment leaves it at 0, which no real
 * game ever has.
 */
TEST_CASE("ModularAirportEarlyRolloutTurnOff")
{
	const uint8_t saved_plane_speed = _settings_game.vehicle.plane_speed;

	Map::Allocate(64, 64);
	TileIndex base = TileXY(10, 10);
	Station *st = SetupModularAirport(base, 20, 8);
	REQUIRE(st != nullptr);

	_engine_pool.CleanPool();
	const EngineID prop_engine = CreateAircraftEngine(EngineID(0), 0);

	SECTION("Braking distance follows the speed model") {
		SetupAircraftPool();
		Aircraft *v = CreateAircraft(VehicleID(10));
		v->engine_type = prop_engine;

		_settings_game.vehicle.plane_speed = 4; // the game default: aircraft fly at 1/4 speed
		/* Anything at or above SPEED_LIMIT_APPROACH * 4 touches down at that limit. */
		v->vcache.cached_max_speed = 1000;
		CHECK(ModularRolloutBrakingTiles(v) == 7);
		v->vcache.cached_max_speed = 920;
		CHECK(ModularRolloutBrakingTiles(v) == 7);
		/* An early prop (Sampson U52 tops out at 473) touches down slower and stops sooner:
		 * 64 pixels, an exact 4 tiles, so the slack pixel puts the turn-off at 5. */
		v->vcache.cached_max_speed = 473;
		CHECK(ModularRolloutBrakingTiles(v) == 5);
		/* Never above taxi speed: there is nothing to brake off, but the aircraft still
		 * has to clear the touchdown tile. */
		v->vcache.cached_max_speed = 200;
		CHECK(ModularRolloutBrakingTiles(v) == 1);

		/* Undivided plane speed approaches at 230 rather than 920, so everything stops
		 * shorter -- the rollout has to follow the setting, not a fixed tile count. */
		_settings_game.vehicle.plane_speed = 1;
		v->vcache.cached_max_speed = 1000;
		CHECK(ModularRolloutBrakingTiles(v) == 5);
		v->vcache.cached_max_speed = 473;
		CHECK(ModularRolloutBrakingTiles(v) == 5);

		CHECK(ModularRolloutBrakingTiles(nullptr) == 1);
	}

	SECTION("The earliest turn-off is capped at a large runway's length") {
		_settings_game.vehicle.plane_speed = 4;
		AddLargeRunway(st, base, 12, 0, RUF_LANDING | RUF_TAKEOFF | RUF_DIR_HIGH);

		SetupAircraftPool();
		Aircraft *v = CreateAircraft(VehicleID(10));
		v->engine_type = prop_engine;
		v->targetairport = st->index;

		/* Touchdown at the low end rolls toward high. This aircraft needs 7 tiles to reach
		 * taxi speed, but the floor caps it at LARGE_RUNWAY_LENGTH_TILES of runway -- 5
		 * steps past touchdown -- so it may turn off there. */
		CHECK(ModularRolloutBrakingTiles(v) == 7);
		CHECK(FindModularRunwayRolloutPoint(st, v, base) == base + TileDiffXY(5, 0));
		/* Mirrored from the other end, so the direction follows the runway rather than
		 * the tile order. */
		CHECK(FindModularRunwayRolloutPoint(st, v, base + TileDiffXY(11, 0)) == base + TileDiffXY(6, 0));

		/* An aircraft that brakes in less than the cap still turns off where it stops:
		 * the cap is a ceiling on the floor, not a fixed distance. */
		v->vcache.cached_max_speed = 400;
		REQUIRE(ModularRolloutBrakingTiles(v) == 4);
		CHECK(FindModularRunwayRolloutPoint(st, v, base) == base + TileDiffXY(4, 0));
	}

	SECTION("A runway no longer than the braking distance still rolls to the far end") {
		_settings_game.vehicle.plane_speed = 4;
		AddLargeRunway(st, base, 6, 0, RUF_DEFAULT);

		SetupAircraftPool();
		Aircraft *v = CreateAircraft(VehicleID(10));
		v->engine_type = prop_engine;
		v->targetairport = st->index;

		/* 6 tiles is the shortest large-safe runway, and 5 tiles of roll is less than the
		 * 7 this aircraft needs: unchanged behaviour, the whole runway. */
		CHECK(FindModularRunwayRolloutPoint(st, v, base) == base + TileDiffXY(5, 0));
		CHECK(FindModularRunwayRolloutPoint(st, v, base + TileDiffXY(5, 0)) == base);
	}

	SECTION("Helicopters keep the far end") {
		_settings_game.vehicle.plane_speed = 4;
		AddLargeRunway(st, base, 12, 0, RUF_LANDING | RUF_TAKEOFF | RUF_DIR_HIGH);

		SetupAircraftPool();
		Aircraft *v = CreateAircraft(VehicleID(10));
		v->engine_type = prop_engine;
		v->targetairport = st->index;
		v->subtype = AIR_HELICOPTER;

		/* Nothing about a helicopter rolls out, and the only thing that reads this for one
		 * is landing-target scoring. Moving it would re-route helicopters. */
		CHECK(FindModularRunwayRolloutPoint(st, v, base) == base + TileDiffXY(11, 0));
	}

	SECTION("The landing chain is planned from the turn-off point") {
		_settings_game.vehicle.plane_speed = 4;
		/* Runway along row 0, with its only exit beside the turn-off point. */
		AddLargeRunway(st, base, 12, 0, RUF_LANDING | RUF_TAKEOFF | RUF_DIR_HIGH);
		AddModularTile(st, base + TileDiffXY(7, 1), APT_APRON, 0);
		AddModularTile(st, base + TileDiffXY(7, 2), APT_STAND, 0);

		SetupAircraftPool();
		Aircraft *v = CreateAircraft(VehicleID(10));
		v->engine_type = prop_engine;
		v->targetairport = st->index;

		const TileIndex rollout = base + TileDiffXY(7, 0);
		const TileIndex goal = base + TileDiffXY(7, 2);
		REQUIRE(TryReserveLandingChain(v, st, base, goal));

		/* The chain starts where the roll ends, which is what HandleModularGroundArrival
		 * requires to install it as the taxi path on arrival. */
		REQUIRE(v->landing_chain_path != nullptr);
		CHECK(v->landing_chain_path->tiles.front() == rollout);
		CHECK(v->landing_chain_path->tiles.back() == goal);

		/* Turning off early does not divide the runway: it is still claimed whole, for
		 * the whole operation. */
		for (uint i = 0; i < 12; i++) {
			CHECK(IsModularAirportTileReservedBy(base + TileDiffXY(i, 0), v->index));
		}
		CHECK(IsModularAirportTileReservedBy(goal, v->index));
	}

	SECTION("The rollout extends to an exit further along the runway") {
		_settings_game.vehicle.plane_speed = 4;
		AddLargeRunway(st, base, 12, 0, RUF_LANDING | RUF_TAKEOFF | RUF_DIR_HIGH);
		/* The only way off is at tile 10, past the turn-off floor at tile 5. */
		AddModularTile(st, base + TileDiffXY(10, 1), APT_APRON, 0);
		AddModularTile(st, base + TileDiffXY(10, 2), APT_STAND, 0);

		SetupAircraftPool();
		Aircraft *v = CreateAircraft(VehicleID(10));
		v->engine_type = prop_engine;
		v->targetairport = st->index;

		const TileIndex goal = base + TileDiffXY(10, 2);
		REQUIRE(TryReserveLandingChain(v, st, base, goal));
		REQUIRE(v->landing_chain_path != nullptr);
		/* The route's leading run along the runway is absorbed into the rollout, so the
		 * chain starts at the exit itself and leaves the runway on its first step. The
		 * aircraft rolls tiles 5..10 rather than taxiing them. */
		CHECK(v->landing_chain_path->tiles.front() == base + TileDiffXY(10, 0));
		CHECK(v->landing_chain_path->tiles[1] == base + TileDiffXY(10, 1));
		CHECK(v->landing_chain_path->tiles.back() == goal);
	}

	SECTION("A back-taxi to the only exit starts from the turn-off floor") {
		_settings_game.vehicle.plane_speed = 4;
		AddLargeRunway(st, base, 12, 0, RUF_LANDING | RUF_TAKEOFF | RUF_DIR_HIGH);
		/* The only way off is at tile 2, behind the floor at tile 5 -- the shape of
		 * Little Wondingbury, where every exit sits in the first half of the runway. */
		AddModularTile(st, base + TileDiffXY(2, 1), APT_APRON, 0);
		AddModularTile(st, base + TileDiffXY(2, 2), APT_STAND, 0);

		SetupAircraftPool();
		Aircraft *v = CreateAircraft(VehicleID(10));
		v->engine_type = prop_engine;
		v->targetairport = st->index;

		const TileIndex goal = base + TileDiffXY(2, 2);
		REQUIRE(TryReserveLandingChain(v, st, base, goal));
		REQUIRE(v->landing_chain_path != nullptr);
		/* Nothing is absorbed: the route turns back immediately. The aircraft still rolls
		 * only as far as the floor and back-taxis from there, rather than continuing to
		 * the braking distance and taxiing back further. */
		CHECK(v->landing_chain_path->tiles.front() == base + TileDiffXY(5, 0));
		CHECK(v->landing_chain_path->tiles[1] == base + TileDiffXY(4, 0));
		CHECK(v->landing_chain_path->tiles.back() == goal);
	}

	SECTION("Landing target scoring measures taxi distance from the rollout point") {
		_settings_game.vehicle.plane_speed = 4;
		/* Runway 1: 12 tiles at row 0. Landing end is base; turn-off floor is (5,0); the
		 *           physical far end is (11,0).
		 * Runway 2: 6 tiles at row 4. Landing end is (0,4); its floor is the far end (5,4).
		 * A stand at (5,1), joined to runway 2 by aprons at (5,3) and (5,2), so both
		 * candidates can actually reach it -- FindFreeModularTerminal pathfinds and skips
		 * stands it cannot route to, and with no connecting tiles neither candidate would
		 * score a taxi term at all.
		 *
		 * The aircraft sits midway between the two landing ends, so flight distance ties
		 * and the taxi term alone decides:
		 *   from runway 1's floor (5,0)     -> 1 tile   <- chosen
		 *   from runway 2's floor (5,4)     -> 3 tiles
		 *   from runway 1's far end (11,0)  -> 7 tiles  <- would lose to runway 2 */
		AddLargeRunway(st, base, 12, 0, RUF_LANDING | RUF_TAKEOFF | RUF_DIR_HIGH);
		const TileIndex rwy2 = base + TileDiffXY(0, 4);
		AddLargeRunway(st, rwy2, 6, 0, RUF_LANDING | RUF_TAKEOFF | RUF_DIR_HIGH);
		AddModularTile(st, base + TileDiffXY(5, 1), APT_STAND, 0);
		AddModularTile(st, base + TileDiffXY(5, 2), APT_APRON, 0);
		AddModularTile(st, base + TileDiffXY(5, 3), APT_APRON, 0);

		SetupAircraftPool();
		Aircraft *v = CreateAircraft(VehicleID(10));
		v->engine_type = prop_engine;
		v->targetairport = st->index;
		v->x_pos = TileX(base) * TILE_SIZE + TILE_SIZE / 2;
		v->y_pos = TileY(base + TileDiffXY(0, 2)) * TILE_SIZE + TILE_SIZE / 2;

		/* Guard the premise: if the stand were unreachable both candidates would score a
		 * bare flight distance and the tie would be broken by layout order, passing this
		 * test without ever consulting a rollout point. */
		REQUIRE(FindFreeModularTerminal(st, v, base + TileDiffXY(5, 0)) == base + TileDiffXY(5, 1));
		REQUIRE(FindFreeModularTerminal(st, v, base + TileDiffXY(5, 4)) == base + TileDiffXY(5, 1));

		CHECK(FindModularLandingTarget(st, v) == base);
	}

	_settings_game.vehicle.plane_speed = saved_plane_speed;
}

/**
 * Mid-runway takeoff start: a departure rolls from the first point with enough runway
 * ahead of it, rather than back-taxiing to the far end of a long runway.
 */
TEST_CASE("ModularAirportMidRunwayTakeoffStart")
{
	Map::Allocate(64, 64);
	TileIndex base = TileXY(10, 10);
	Station *st = SetupModularAirport(base, 20, 8);
	REQUIRE(st != nullptr);

	_engine_pool.CleanPool();
	const EngineID prop_engine = CreateAircraftEngine(EngineID(0), 0);
	const EngineID jet_engine = CreateAircraftEngine(EngineID(1), AIR_FAST);

	SECTION("Required run follows the runway lengths the airport is already built around") {
		SetupAircraftPool();
		Aircraft *v = CreateAircraft(VehicleID(10));

		v->engine_type = jet_engine;
		CHECK(ModularTakeoffRunTiles(v) == (uint)LARGE_RUNWAY_LENGTH_TILES);
		v->engine_type = prop_engine;
		CHECK(ModularTakeoffRunTiles(v) == (uint)MIN_RUNWAY_LENGTH_TILES);
	}

	SECTION("A roll may start wherever enough runway is left") {
		/* Rolling low -> high, so the run remaining at index i is 12 - i. */
		AddLargeRunway(st, base, 12, 0, RUF_TAKEOFF | RUF_DIR_HIGH);

		SetupAircraftPool();
		Aircraft *v = CreateAircraft(VehicleID(10));
		v->targetairport = st->index;
		v->modular_takeoff_tile = base;

		v->engine_type = jet_engine;
		for (uint i = 0; i < 12; i++) {
			CHECK(ModularTakeoffRunFitsFrom(st, v, base + TileDiffXY(i, 0)) == (12 - i >= 6));
		}

		/* A small aircraft needs less, so it may start further along the same runway. */
		v->engine_type = prop_engine;
		for (uint i = 0; i < 12; i++) {
			CHECK(ModularTakeoffRunFitsFrom(st, v, base + TileDiffXY(i, 0)) == (12 - i >= 4));
		}
	}

	SECTION("The direction is the runway's, not the tile order's") {
		/* Rolling high -> low, so the run remaining at index i is i + 1. */
		AddLargeRunway(st, base, 12, 0, RUF_TAKEOFF | RUF_DIR_LOW);

		SetupAircraftPool();
		Aircraft *v = CreateAircraft(VehicleID(10));
		v->targetairport = st->index;
		v->engine_type = jet_engine;
		v->modular_takeoff_tile = base + TileDiffXY(11, 0);

		for (uint i = 0; i < 12; i++) {
			CHECK(ModularTakeoffRunFitsFrom(st, v, base + TileDiffXY(i, 0)) == (i + 1 >= 6));
		}
	}

	SECTION("A runway with nothing to spare is used end to end") {
		/* Six tiles is the shortest large-safe runway: only its own end qualifies, so a
		 * jet taxis to the end and departs from there exactly as it always did. */
		AddLargeRunway(st, base, 6, 0, RUF_TAKEOFF | RUF_DIR_HIGH);

		SetupAircraftPool();
		Aircraft *v = CreateAircraft(VehicleID(10));
		v->targetairport = st->index;
		v->engine_type = jet_engine;
		v->modular_takeoff_tile = base;

		CHECK(ModularTakeoffRunFitsFrom(st, v, base));
		for (uint i = 1; i < 6; i++) {
			CHECK_FALSE(ModularTakeoffRunFitsFrom(st, v, base + TileDiffXY(i, 0)));
		}
	}

	SECTION("A short strip never permits an early start") {
		/* A jet is only ever sent to a runway this short when the airport has nothing
		 * better. It needs all of it, so nothing here qualifies and the aircraft keeps
		 * taxiing to the end. */
		AddLargeRunway(st, base, 4, 0, RUF_TAKEOFF | RUF_DIR_HIGH);

		SetupAircraftPool();
		Aircraft *v = CreateAircraft(VehicleID(10));
		v->targetairport = st->index;
		v->engine_type = jet_engine;
		v->modular_takeoff_tile = base;

		for (uint i = 0; i < 4; i++) {
			CHECK_FALSE(ModularTakeoffRunFitsFrom(st, v, base + TileDiffXY(i, 0)));
		}
		/* The same strip is long enough for a small aircraft, from its end only. */
		v->engine_type = prop_engine;
		CHECK(ModularTakeoffRunFitsFrom(st, v, base));
		CHECK_FALSE(ModularTakeoffRunFitsFrom(st, v, base + TileDiffXY(1, 0)));
	}

	SECTION("Another runway is not this aircraft's runway") {
		/* An aircraft crossing some other runway on the way to its own is transiting, and
		 * has no run ahead of it at all. */
		AddLargeRunway(st, base, 12, 0, RUF_TAKEOFF | RUF_DIR_HIGH);
		AddLargeRunway(st, base + TileDiffXY(0, 3), 12, 0, RUF_TAKEOFF | RUF_DIR_HIGH);

		SetupAircraftPool();
		Aircraft *v = CreateAircraft(VehicleID(10));
		v->targetairport = st->index;
		v->engine_type = jet_engine;
		v->modular_takeoff_tile = base;

		CHECK(ModularTakeoffRunFitsFrom(st, v, base + TileDiffXY(2, 0)));
		CHECK_FALSE(ModularTakeoffRunFitsFrom(st, v, base + TileDiffXY(2, 3)));
	}
}

/**
 * Alternate-route selection: where the shortest route off a runway is held by another
 * aircraft, a second exit is used instead of refusing the movement.
 * @see plans/route-selection-plan.md
 *
 * Every tile here sits at a non-negative offset from @c base, because the airport
 * rectangle starts there -- a tile outside it is not part of the layout and the
 * pathfinder will not route through it.
 */
TEST_CASE("ModularAirportAlternateRoutes")
{
	Map::Allocate(64, 64);
	const TileIndex base = TileXY(10, 10);
	Station *st = SetupModularAirport(base, 16, 16);
	REQUIRE(st != nullptr);

	_engine_pool.CleanPool();
	const EngineID prop_engine = CreateAircraftEngine(EngineID(0), 0);

	const auto one_way = [&](TileIndex tile, uint8_t dir_mask) {
		ModularAirportTileData *data = AddModularTileWithData(st, tile, APT_APRON, 0);
		data->one_way_taxi = true;
		data->user_taxi_dir_mask = dir_mask;
	};

	/* Two exits off one runway, both reaching the same stand.
	 *
	 *          x=0    x=1    x=2        x=3     x=4
	 *   y=1                  A_oneway   apron   apron
	 *   y=2    RWY    RWY    RWY_END            STAND
	 *   y=3                  B_oneway           apron
	 *   y=4                  apron      apron   apron
	 * Touchdown (0,2) rolls out to (2,2). Exit A reaches the stand in four steps and
	 * exit B in six, so A is what the unconstrained pathfinder returns. */
	const TileIndex runway_start = base + TileDiffXY(0, 2);
	const TileIndex rollout = base + TileDiffXY(2, 2);
	const TileIndex exit_a = base + TileDiffXY(2, 1);
	const TileIndex exit_b = base + TileDiffXY(2, 3);
	const TileIndex stand = base + TileDiffXY(4, 2);

	const auto build_two_exits = [&]() {
		AddLargeRunway(st, runway_start, 3, 0, RUF_DEFAULT);
		one_way(exit_a, 0x02); // East
		AddModularTile(st, base + TileDiffXY(3, 1), APT_APRON, 0);
		AddModularTile(st, base + TileDiffXY(4, 1), APT_APRON, 0);
		AddModularTile(st, stand, APT_STAND, 0);
		one_way(exit_b, 0x04); // South, into the longer corridor
		AddModularTile(st, base + TileDiffXY(2, 4), APT_APRON, 0);
		AddModularTile(st, base + TileDiffXY(3, 4), APT_APRON, 0);
		AddModularTile(st, base + TileDiffXY(4, 4), APT_APRON, 0);
		AddModularTile(st, base + TileDiffXY(4, 3), APT_APRON, 0);
		st->airport.MarkLayoutDirty();
	};

	SECTION("Shortest exit is used when it is free") {
		build_two_exits();
		SetupAircraftPool();
		Aircraft *v = CreateAircraft(VehicleID(10));
		v->engine_type = prop_engine;
		v->targetairport = st->index;

		REQUIRE(TryReserveLandingChain(v, st, runway_start, stand));
		CHECK(IsModularAirportTileReservedBy(exit_a, v->index));
		CHECK_FALSE(IsModularAirportTileReservedBy(exit_b, v->index));
	}

	SECTION("Second exit is taken when the first is reserved") {
		/* Guarded so the suite still builds and passes if MODULAR_MAX_ROUTE_ATTEMPTS is set
		 * back to 1; the pathfinder half is covered unconditionally further down. */
		if constexpr (MODULAR_MAX_ROUTE_ATTEMPTS <= 1) return;
		build_two_exits();
		SetupAircraftPool();
		Aircraft *v = CreateAircraft(VehicleID(10));
		v->engine_type = prop_engine;
		v->targetairport = st->index;

		/* A live owner: a reservation whose holder no longer exists is cleared rather
		 * than respected, so a dangling one would block nothing. */
		CreateBlockerOnTile(st, VehicleID(11), exit_a);

		REQUIRE(TryReserveLandingChain(v, st, runway_start, stand));
		CHECK(IsModularAirportTileReservedBy(exit_b, v->index));
		CHECK_FALSE(IsModularAirportTileReservedBy(exit_a, v->index));
		/* The runway is still claimed whole, as one landing operation. */
		for (int i = 0; i < 3; i++) {
			CHECK(IsModularAirportTileReservedBy(runway_start + TileDiffXY(i, 0), v->index));
		}
	}

	SECTION("Sole exit blocked is still refused") {
		/* The B corridor omitted: with no alternative the landing must be denied rather
		 * than admitted onto a route the aircraft cannot hold. */
		AddLargeRunway(st, runway_start, 3, 0, RUF_DEFAULT);
		one_way(exit_a, 0x02);
		AddModularTile(st, base + TileDiffXY(3, 1), APT_APRON, 0);
		AddModularTile(st, base + TileDiffXY(4, 1), APT_APRON, 0);
		AddModularTile(st, stand, APT_STAND, 0);
		st->airport.MarkLayoutDirty();

		SetupAircraftPool();
		Aircraft *v = CreateAircraft(VehicleID(10));
		v->engine_type = prop_engine;
		v->targetairport = st->index;
		CreateBlockerOnTile(st, VehicleID(11), exit_a);

		CHECK_FALSE(TryReserveLandingChain(v, st, runway_start, stand));
		CHECK(v->modular_runway_reservation.empty());
	}

	SECTION("Ban applies only inside the reservation horizon") {
		/* The blocked tile is the *only* way to the stand, but an alternative route
		 * reaches a one-way queue tile first and meets the blocked tile beyond the
		 * horizon, where nothing is claimed. Admitting this is the point of scoping the
		 * ban to the horizon: a ban applied to the whole route finds nothing and refuses
		 * a landing that is perfectly safe.
		 *
		 *          x=0    x=1    x=2        x=3
		 *   y=0                  STAND
		 *   y=1                  blocked    apron
		 *   y=2    RWY    RWY    RWY_END    queue_oneway(N)
		 * Shortest:  (2,2) -> (2,1) -> STAND                      -- 3 tiles.
		 * Alternate: (2,2) -> queue -> (3,1) -> (2,1) -> STAND    -- 5 tiles.
		 *
		 * The detour is deliberately +2, well inside MAX_ROUTE_DETOUR_TILES, so what this
		 * section tests is the horizon-scoped ban rather than the length cap. Keep it that
		 * way: an earlier version of this layout reached the stand by a +6 loop, which the
		 * cap refused when the budget was 4 and admits now that it is 6, so the assertion
		 * below would have been measuring the budget rather than the ban. */
		if constexpr (MODULAR_MAX_ROUTE_ATTEMPTS <= 1) return;
		AddLargeRunway(st, runway_start, 3, 0, RUF_DEFAULT);
		const TileIndex loop_goal = base + TileDiffXY(2, 0);
		const TileIndex blocked = base + TileDiffXY(2, 1);
		const TileIndex queue = base + TileDiffXY(3, 2);
		AddModularTile(st, loop_goal, APT_STAND, 0);
		AddModularTile(st, blocked, APT_APRON, 0);
		AddModularTile(st, base + TileDiffXY(3, 1), APT_APRON, 0);
		one_way(queue, 0x01); // North: entered from the runway, left towards (3,1)
		st->airport.MarkLayoutDirty();

		SetupAircraftPool();
		Aircraft *v = CreateAircraft(VehicleID(10));
		v->engine_type = prop_engine;
		v->targetairport = st->index;
		CreateBlockerOnTile(st, VehicleID(11), blocked);

		REQUIRE(TryReserveLandingChain(v, st, runway_start, loop_goal));
		/* Queued on the one-way tile; the blocked tile lies beyond the horizon and stays
		 * with its owner. */
		CHECK(IsModularAirportTileReservedBy(queue, v->index));
		CHECK_FALSE(IsModularAirportTileReservedBy(blocked, v->index));
	}

	SECTION("A stand that is not the goal is not somewhere to wait") {
		/* Routes may cross a stand when that is the only way through, but crossing is not
		 * stopping. If a foreign stand could end a reservation horizon, an aircraft would
		 * park on it and take it out of service for whoever it was meant for. */
		build_two_exits();
		st->airport.MarkLayoutDirty();

		/* The stand is a safe stop for the aircraft going there, and for nobody else. */
		CHECK(IsModularSafeStopTile(st, stand, stand));
		CHECK_FALSE(IsModularSafeStopTile(st, stand, exit_a));

		/* One-way queue tiles are unconditional: they are shared queue positions, not
		 * anybody's destination. */
		CHECK(IsModularSafeStopTile(st, exit_a, stand));

		/* No goal supplied means "anywhere will do"; route planning always has a goal
		 * and passes it, so this is only for callers with nothing to offer. */
		CHECK(IsModularSafeStopTile(st, stand));

		/* Helipads are parking too: a helicopter waiting on a pad it is not going to
		 * takes that pad out of service exactly as a stand would. */
		const TileIndex pad = base + TileDiffXY(0, 4);
		AddModularTile(st, pad, APT_HELIPAD_1, 0);
		st->airport.MarkLayoutDirty();
		CHECK(IsModularSafeStopTile(st, pad, pad));
		CHECK_FALSE(IsModularSafeStopTile(st, pad, stand));
	}

	SECTION("Avoid set diverts the raw pathfinder route") {
		build_two_exits();
		SetupAircraftPool();

		AirportGroundPath plain = FindAirportGroundPath(st, rollout, stand, nullptr, false, false);
		REQUIRE(plain.found);
		CHECK(std::find(plain.tiles.begin(), plain.tiles.end(), exit_a) != plain.tiles.end());

		const std::vector<TileIndex> avoid{exit_a};
		AirportGroundPath diverted = FindAirportGroundPath(st, rollout, stand, nullptr, false, false,
				GroundPathRestriction::None, avoid);
		REQUIRE(diverted.found);
		CHECK(std::find(diverted.tiles.begin(), diverted.tiles.end(), exit_a) == diverted.tiles.end());
		CHECK(std::find(diverted.tiles.begin(), diverted.tiles.end(), exit_b) != diverted.tiles.end());
	}

	SECTION("Avoid set stops applying past the first safe stop") {
		/* exit_b is one-way, so a route through it has reached a safe stop by its second
		 * tile. Banning a tile beyond that point must not remove the route: nothing claims
		 * that far ahead, so nothing there can deny entry. */
		build_two_exits();
		SetupAircraftPool();

		const TileIndex past_the_queue = base + TileDiffXY(3, 4);
		const std::vector<TileIndex> avoid{past_the_queue};
		AirportGroundPath via_b = FindAirportGroundPath(st, rollout, stand, nullptr, false, false,
				GroundPathRestriction::None, avoid);
		REQUIRE(via_b.found);

		/* The escape hatch this used to document is closed. Routing from (2,4), the search
		 * could step onto the adjacent one-way tile -- which counts as reaching a safe stop
		 * and lifts the ban -- then turn around and use the banned tile after all. The route
		 * it produced visited a tile twice, so the aircraft drove out, doubled back against
		 * the one-way arrow and drove out again, holding both tiles throughout. In the T7d
		 * fixture that filled small one-way rings until they deadlocked, and it accounted
		 * for 46% of permanently-stuck aircraft. Route-local ancestry now prevents the
		 * repeated tile, so the U-turn is unreachable without globally closing a merge tile.
		 * Refusing here is the intended answer: the caller waits on the direct route. */
		AirportGroundPath inside = FindAirportGroundPath(st, base + TileDiffXY(2, 4), stand, nullptr,
				false, false, GroundPathRestriction::None, avoid);
		CHECK_FALSE(inside.found);
	}

	SECTION("Routes may merge after reaching a safe stop") {
		/* The direct route reaches merge before a safe stop and is blocked at the next
		 * tile. The detour reaches a one-way queue first, then rejoins at merge and may
		 * use blocked beyond the reservation horizon. Neither route revisits a tile.
		 *
		 *   start -- junction -- merge -- blocked -- goal
		 *              |          |
		 *            queue ---- detour
		 */
		const TileIndex merge_start = base + TileDiffXY(0, 8);
		const TileIndex junction = base + TileDiffXY(1, 8);
		const TileIndex merge = base + TileDiffXY(2, 8);
		const TileIndex blocked = base + TileDiffXY(3, 8);
		const TileIndex merge_goal = base + TileDiffXY(4, 8);
		const TileIndex queue = base + TileDiffXY(1, 9);
		const TileIndex detour = base + TileDiffXY(2, 9);

		AddModularTile(st, merge_start, APT_APRON, 0);
		AddModularTile(st, junction, APT_APRON, 0);
		AddModularTile(st, merge, APT_APRON, 0);
		AddModularTile(st, blocked, APT_APRON, 0);
		AddModularTile(st, merge_goal, APT_STAND, 0);
		one_way(queue, 0x02); // East, from the queue into the detour.
		AddModularTile(st, detour, APT_APRON, 0);

		const std::vector<TileIndex> avoid{blocked};
		AirportGroundPath path = FindAirportGroundPath(st, merge_start, merge_goal, nullptr,
				false, false, GroundPathRestriction::None, avoid);
		REQUIRE(path.found);
		CHECK(std::find(path.tiles.begin(), path.tiles.end(), queue) != path.tiles.end());
		CHECK(std::find(path.tiles.begin(), path.tiles.end(), merge) != path.tiles.end());
		CHECK(std::find(path.tiles.begin(), path.tiles.end(), blocked) != path.tiles.end());
		std::vector<TileIndex> unique = path.tiles;
		std::sort(unique.begin(), unique.end());
		CHECK(std::adjacent_find(unique.begin(), unique.end()) == unique.end());
	}
}

/**
 * A large aircraft waits for a large-safe takeoff runway rather than downgrading to a
 * short strip that happens to be free. Route retry must not turn "the good runway is
 * busy" into "take the bad one" -- the preference is deliberate, and the tier that
 * returns a reachable-but-blocked end is what implements it.
 */
TEST_CASE("ModularAirportTakeoffPrefersLargeRunwayWhenBusy")
{
	Map::Allocate(64, 64);
	const TileIndex base = TileXY(10, 10);
	Station *st = SetupModularAirport(base, 16, 16);
	REQUIRE(st != nullptr);

	/*          x=0     x=1 .. x=6
	 *   y=0    STAND
	 *   y=1    apron
	 *   y=2    apron   large runway (1,2)..(6,2)
	 *   y=3    apron
	 *   y=4    apron   short runway (1,4)..(4,4)
	 * Both runways take off from their low end, and both are reachable from the stand. */
	const TileIndex stand = base;
	const TileIndex large_low = base + TileDiffXY(1, 2);
	const TileIndex short_low = base + TileDiffXY(1, 4);
	AddModularTile(st, stand, APT_STAND, 0);
	for (int y = 1; y <= 4; y++) AddModularTile(st, base + TileDiffXY(0, y), APT_APRON, 0);
	AddLargeRunway(st, large_low, 6, 0, RUF_TAKEOFF | RUF_DIR_HIGH);
	AddSmallRunway(st, short_low, 4, 0, RUF_TAKEOFF | RUF_DIR_HIGH);
	st->airport.MarkLayoutDirty();

	_engine_pool.CleanPool();
	const EngineID jet_engine = CreateAircraftEngine(EngineID(0), AIR_FAST);
	SetupAircraftPool();
	Aircraft *jet = CreateAircraft(VehicleID(10));
	jet->engine_type = jet_engine;
	jet->targetairport = st->index;
	jet->tile = stand;

	SECTION("Free large runway is chosen") {
		CHECK(FindModularRunwayTileForTakeoff(st, jet) == large_low);
	}

	SECTION("Busy large runway is still chosen over a free short one") {
		/* Somebody else holds the large runway. The jet must wait for it. */
		CreateBlockerOnTile(st, VehicleID(11), large_low + TileDiffXY(2, 0));

		const TileIndex chosen = FindModularRunwayTileForTakeoff(st, jet);
		CHECK(chosen != short_low);
		CHECK(chosen == large_low);
	}
}

TEST_CASE("ModularAirportAlternateTransitRunway")
{
	Map::Allocate(64, 64);
	const TileIndex base = TileXY(10, 10);
	Station *st = SetupModularAirport(base, 12, 12);
	REQUIRE(st != nullptr);
	if constexpr (MODULAR_MAX_ROUTE_ATTEMPTS <= 1) return;

	/* A landing rollout can reach its stand across either of two independent three-tile
	 * transit runways. The right crossing is two steps shorter. Holding it must divert
	 * the chain through the left crossing; it must not be mistaken for the busy operation
	 * runway. */
	const TileIndex touchdown = base + TileDiffXY(2, 0);
	const TileIndex rollout = base + TileDiffXY(4, 0);
	const TileIndex crossing_a = base + TileDiffXY(1, 3);
	const TileIndex crossing_b = base + TileDiffXY(5, 3);
	const TileIndex goal = base + TileDiffXY(3, 6);
	AddLargeRunway(st, touchdown, 3, 0, RUF_DEFAULT);
	AddLargeRunway(st, base + TileDiffXY(0, 3), 3, 0, RUF_DEFAULT);
	AddLargeRunway(st, base + TileDiffXY(4, 3), 3, 0, RUF_DEFAULT);
	AddModularTile(st, base + TileDiffXY(1, 0), APT_APRON, 0);
	AddModularTile(st, base + TileDiffXY(5, 0), APT_APRON, 0);
	for (int y = 1; y <= 2; ++y) {
		AddModularTile(st, base + TileDiffXY(1, y), APT_APRON, 0);
		AddModularTile(st, base + TileDiffXY(5, y), APT_APRON, 0);
	}
	for (int y = 4; y <= 5; ++y) {
		AddModularTile(st, base + TileDiffXY(1, y), APT_APRON, 0);
		AddModularTile(st, base + TileDiffXY(5, y), APT_APRON, 0);
	}
	AddModularTile(st, base + TileDiffXY(2, 5), APT_APRON, 0);
	AddModularTile(st, base + TileDiffXY(3, 5), APT_APRON, 0);
	AddModularTile(st, base + TileDiffXY(4, 5), APT_APRON, 0);
	AddModularTile(st, goal, APT_STAND, 0);

	AirportGroundPath preferred = FindAirportGroundPath(st, rollout, goal, nullptr,
			false, false, GroundPathRestriction::FixedWing);
	REQUIRE(preferred.found);
	const bool uses_a = std::find(preferred.tiles.begin(), preferred.tiles.end(), crossing_a) != preferred.tiles.end();
	const TileIndex busy = uses_a ? crossing_a : crossing_b;
	const TileIndex free = uses_a ? crossing_b : crossing_a;
	CHECK(std::find(preferred.tiles.begin(), preferred.tiles.end(), free) == preferred.tiles.end());

	_engine_pool.CleanPool();
	const EngineID prop_engine = CreateAircraftEngine(EngineID(0), 0);
	SetupAircraftPool();
	Aircraft *requester = CreateAircraft(VehicleID(10));
	requester->engine_type = prop_engine;
	requester->targetairport = st->index;
	Aircraft *blocker = CreateAircraft(VehicleID(11));
	blocker->engine_type = prop_engine;
	blocker->targetairport = st->index;
	blocker->state = LANDING;
	blocker->modular_landing_tile = busy;

	REQUIRE(TryReserveLandingChain(requester, st, touchdown, goal));
	CHECK(IsModularAirportTileReservedBy(free, requester->index));
	CHECK_FALSE(IsModularAirportTileReservedBy(busy, requester->index));
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
			if (v->taxi_path->segments[s].type == TaxiSegmentType::Runway) return s;
		}
		FAIL("no runway segment on path");
		return 0;
	};

	SECTION("Reserves the path through the crossing to the far-side safe stop") {
		SetupAircraftPool();
		Aircraft *v = setup_aircraft_on_path(VehicleID(10));
		REQUIRE(TryReserveTaxiSegment(v, st, runway_segment(v)));

		/* Only the runway tile on this path is reserved. */
		CHECK_FALSE(HasModularAirportTileReservation(base + TileDiffXY(0, 2)));
		CHECK_FALSE(HasModularAirportTileReservation(base + TileDiffXY(1, 2)));
		CHECK(IsModularAirportTileReservedBy(base + TileDiffXY(2, 2), v->index));
		/* Far-side transit grass reserved (would have been a strand point before). */
		CHECK(IsModularAirportTileReservedBy(base + TileDiffXY(2, 3), v->index));
		/* Goal stand reserved -- chain reached the safe stop. */
		CHECK(IsModularAirportTileReservedBy(goal, v->index));
	}

	SECTION("Denies runway entry when the far side is blocked; runway untouched") {
		SetupAircraftPool();
		Aircraft *v = setup_aircraft_on_path(VehicleID(10));
		/* Block the transit grass past the runway: there is no reserved safe stop
		 * beyond, so the aircraft must wait BEFORE the runway, not strand on grass. */
		CreateBlockerOnTile(st, VehicleID(11), base + TileDiffXY(2, 3));

		CHECK_FALSE(TryReserveTaxiSegment(v, st, runway_segment(v)));

		/* No half-commit: the crossing tile was never taken. */
		for (int i = 0; i < 3; i++) {
			CHECK_FALSE(IsModularAirportTileReservedBy(base + TileDiffXY(i, 2), v->index));
		}
		CHECK(v->modular_runway_reservation.empty());
		/* Blocker's reservation untouched. */
		CHECK(IsModularAirportTileReservedBy(base + TileDiffXY(2, 3), VehicleID(11)));
	}

	SECTION("A disjoint crossing claim denies a whole-runway operation") {
		SetupAircraftPool();
		Aircraft *crossing = setup_aircraft_on_path(VehicleID(10));
		REQUIRE(TryReserveTaxiSegment(crossing, st, runway_segment(crossing)));
		const TileIndex crossing_tile = base + TileDiffXY(2, 2);
		REQUIRE(IsModularAirportTileReservedBy(crossing_tile, crossing->index));

		/* Enter the same runway at its other end for takeoff. The crossing at x=2
		 * is disjoint from the x=0 entry tile, but a flight operation needs every
		 * tile and must therefore wait for that crossing claim. */
		const TileIndex takeoff_start = base + TileDiffXY(0, 1);
		const TileIndex takeoff_goal = base + TileDiffXY(0, 2);
		AddModularTile(st, takeoff_start, APT_APRON, 0);
		Aircraft *operation = CreateAircraft(VehicleID(11));
		operation->targetairport = st->index;
		operation->tile = takeoff_start;
		operation->ground_path_goal = takeoff_goal;
		operation->modular_ground_target = MGT_RUNWAY_TAKEOFF;
		operation->modular_takeoff_tile = takeoff_goal;
		TaxiPath operation_path = BuildTaxiPath(st, takeoff_start, takeoff_goal, operation, true);
		REQUIRE(operation_path.valid);
		operation->taxi_path = std::make_unique<TaxiPath>(std::move(operation_path));
		operation->taxi_path_index = 0;
		operation->taxi_current_segment = FindTaxiSegmentIndex(operation->taxi_path.get(), 0);

		TaxiReserveResult result;
		CHECK_FALSE(TryReserveTaxiSegment(operation, st, operation->taxi_current_segment, &result));
		CHECK(result.reason == TaxiReserveFailure::RunwayBusy);
		CHECK(result.tile == crossing_tile);
		CHECK(operation->modular_runway_reservation.empty());
		for (int i = 0; i < 3; i++) {
			CHECK_FALSE(IsModularAirportTileReservedBy(base + TileDiffXY(i, 2), operation->index));
		}
		CHECK(IsModularAirportTileReservedBy(crossing_tile, crossing->index));
	}
}

TEST_CASE("ModularAirportAdjacentRunwayLandingCrossing")
{
	Map::Allocate(64, 64);
	TileIndex base = TileXY(10, 10);
	Station *st = SetupModularAirport(base, 16, 8);
	REQUIRE(st != nullptr);

	/* The landing helpers read the engine's subtype to tell a fast jet from
	 * everything else, so these aircraft need a real engine. A plain non-fast one
	 * keeps the runway-class rule out of the way of what is under test here. */
	_engine_pool.CleanPool();
	const EngineID prop_engine = CreateAircraftEngine(EngineID(0), 0);

	/* Fort Bronhill-shaped overlap: the landing runway's rollout end is adjacent
	 * to the far end of a second parallel runway, which is the only route to the
	 * stand.
	 *
	 *   row 2: STAND .... U0 U1 U2 U3 U4 U5(touchdown)
	 *   row 3:      L0 L1 L2 L3 L4 L5
	 *                         path runs L5 -> L1 -> stand
	 */
	const TileIndex upper_low = base + TileDiffXY(6, 2);
	const TileIndex upper_high = base + TileDiffXY(11, 2);
	const TileIndex lower_low = base + TileDiffXY(1, 3);
	const TileIndex lower_high = base + TileDiffXY(6, 3);
	const TileIndex stand = base + TileDiffXY(2, 2);
	AddLargeRunway(st, upper_low, 6, 0, RUF_DEFAULT);
	AddLargeRunway(st, lower_low, 6, 0, RUF_DEFAULT);
	AddModularTile(st, stand, APT_STAND, 0);

	SECTION("Landing admission is one atomic runway-to-stand transaction") {
		SetupAircraftPool();
		Aircraft *v = CreateAircraft(VehicleID(10));
		v->engine_type = prop_engine;
		v->targetairport = st->index;
		CreateBlockerOnTile(st, VehicleID(11), lower_high);

		CHECK_FALSE(TryReserveLandingChain(v, st, upper_high, stand));
		for (int i = 0; i < 6; i++) {
			CHECK_FALSE(IsModularAirportTileReservedBy(upper_low + TileDiffXY(i, 0), v->index));
		}
		CHECK(v->modular_runway_reservation.empty());
		CHECK(IsModularAirportTileReservedBy(lower_high, VehicleID(11)));
	}

	SECTION("The real movement step releases the runway behind and never reacquires it") {
		SetupAircraftPool();
		Aircraft *v = CreateAircraft(VehicleID(10));
		v->engine_type = prop_engine;
		v->targetairport = st->index;
		v->ground_path_goal = stand;
		v->modular_ground_target = MGT_TERMINAL;
		REQUIRE(TryReserveLandingChain(v, st, upper_high, stand));

		/* Landing owns the whole operation runway, but only the tiles used to cross
		 * and traverse the lower runway. */
		for (int i = 0; i < 6; i++) {
			CHECK(IsModularAirportTileReservedBy(upper_low + TileDiffXY(i, 0), v->index));
		}
		CHECK_FALSE(HasModularAirportTileReservation(lower_low));
		for (int i = 1; i < 6; i++) {
			CHECK(IsModularAirportTileReservedBy(lower_low + TileDiffXY(i, 0), v->index));
		}

		REQUIRE(v->landing_chain_path != nullptr);
		v->tile = upper_low;
		v->taxi_path = std::move(v->landing_chain_path);
		v->taxi_path_index = 0;
		v->taxi_current_segment = FindTaxiSegmentIndex(v->taxi_path.get(), 0);
		REQUIRE(TryReserveTaxiSegment(v, st, v->taxi_current_segment));
		REQUIRE(v->taxi_path->tiles.size() >= 3);
		REQUIRE(v->taxi_path->tiles[1] == lower_high);

		/* Put the sprite at the next tile centre so AirportMoveModular executes one
		 * complete logical step without depending on speed/tick timing. The function
		 * must update tile/index and run its real post-step reconciliation. */
		v->x_pos = TileX(lower_high) * TILE_SIZE + TILE_SIZE / 2;
		v->y_pos = TileY(lower_high) * TILE_SIZE + TILE_SIZE / 2;
		v->z_pos = GetTileMaxPixelZ(lower_high);
		CHECK_FALSE(AirportMoveModular(v, st));
		CHECK(v->tile == lower_high);
		CHECK(v->taxi_path_index == 1);
		for (int i = 0; i < 6; i++) {
			CHECK_FALSE(IsModularAirportTileReservedBy(upper_low + TileDiffXY(i, 0), v->index));
		}
		CHECK(v->modular_runway_reservation.empty());

		/* A new claim behind the aircraft must not block the next real movement
		 * step; recomputing from a segment start would incorrectly demand it again. */
		CreateBlockerOnTile(st, VehicleID(11), upper_low + TileDiffXY(1, 0));
		const TileIndex next_lower_tile = v->taxi_path->tiles[2];
		v->x_pos = TileX(next_lower_tile) * TILE_SIZE + TILE_SIZE / 2;
		v->y_pos = TileY(next_lower_tile) * TILE_SIZE + TILE_SIZE / 2;
		v->z_pos = GetTileMaxPixelZ(next_lower_tile);
		CHECK_FALSE(AirportMoveModular(v, st));
		CHECK(v->tile == next_lower_tile);
		CHECK(v->taxi_path_index == 2);
		CHECK(IsModularAirportTileReservedBy(next_lower_tile, v->index));
	}
}

TEST_CASE("ModularAirportCrash")
{
	Map::Allocate(64, 64);
	const TileIndex base = TileXY(10, 10);

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

TEST_CASE("ModularAirportTemplateAvailabilityValidatesWidePieceIDs")
{
	AirportTemplate templ;
	AirportTemplateTile tile{};
	tile.piece_type = APT_MODULAR_FIRE_STATION;
	templ.tiles.push_back(tile);
	templ.CheckAvailability();
	CHECK(templ.is_available);

	templ.tiles.front().piece_type = UINT16_MAX;
	templ.CheckAvailability();
	CHECK_FALSE(templ.is_available);
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
	 * so the goal is the only thing that can terminate the forward horizon. */
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
			if (v->taxi_path->segments[s].type == TaxiSegmentType::Runway) return s;
		}
		FAIL("no runway segment on path");
		return 0;
	};

	SECTION("A goal on a runway terminates the forward horizon") {
		SetupAircraftPool();
		Aircraft *v = setup_aircraft(VehicleID(10), MGT_HELI_TAKEOFF_TILE);

		/* Regression: this used to be denied forever on a completely empty airport,
		 * because the chain walk skipped runway tiles before testing them for being
		 * the goal, and the goal was a runway tile. */
		REQUIRE(TryReserveTaxiSegment(v, st, transit_runway_segment(v)));

		/* The helicopter only needs the two runway tiles that are on its ground path. */
		CHECK_FALSE(HasModularAirportTileReservation(base + TileDiffXY(0, 2)));
		CHECK_FALSE(HasModularAirportTileReservation(base + TileDiffXY(1, 2)));
		CheckReservedBy({base + TileDiffXY(2, 2)}, v->index);
		/* The transit apron between the runways. */
		CheckReservedBy({base + TileDiffXY(2, 3)}, v->index);
		CHECK_FALSE(HasModularAirportTileReservation(base + TileDiffXY(0, 4)));
		CHECK_FALSE(HasModularAirportTileReservation(base + TileDiffXY(1, 4)));
		CheckReservedBy({goal}, v->index);
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

		/* Runway A is a tile-level crossing, while runway B is the takeoff operation. */
		CHECK_FALSE(HasModularAirportTileReservation(base + TileDiffXY(0, 2)));
		CHECK_FALSE(HasModularAirportTileReservation(base + TileDiffXY(1, 2)));
		CheckReservedBy({base + TileDiffXY(2, 2)}, v->index);
		CheckReservedBy({base + TileDiffXY(2, 3)}, v->index);
		CheckReservedBy({base + TileDiffXY(0, 4), base + TileDiffXY(1, 4), goal}, v->index);
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

	/* A runway with a one-way queueing tile beyond it -- the shape a no-ground-goal
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

		/* No taxi_path and no landing_chain_path -- exactly what the no-ground-goal
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

TEST_CASE("ModularAirportPathSegmentsFollowLayoutEdits")
{
	Map::Allocate(64, 64);
	TileIndex base = TileXY(10, 10);
	/* One tile wide, so the only route between the two stands is through the middle. */
	Station *st = SetupModularAirport(base, 1, 5);
	REQUIRE(st != nullptr);

	const TileIndex stand_a = base + TileDiffXY(0, 1);
	const TileIndex middle = base + TileDiffXY(0, 2);
	const TileIndex stand_b = base + TileDiffXY(0, 3);
	AddModularTile(st, stand_a, APT_STAND, 0);
	AddModularTile(st, middle, APT_APRON, 0);
	AddModularTile(st, stand_b, APT_STAND, 0);

	SetupAircraftPool();
	Aircraft *v = CreateAircraft(VehicleID(10));
	v->targetairport = st->index;
	v->tile = stand_a;
	v->taxi_path = std::make_unique<TaxiPath>(BuildTaxiPath(st, stand_a, stand_b, nullptr, false, GroundPathRestriction::None));
	REQUIRE(v->taxi_path->valid);

	const auto type_of_middle = [&]() {
		const TaxiPath &path = *v->taxi_path;
		const auto it = std::find(path.tiles.begin(), path.tiles.end(), middle);
		REQUIRE(it != path.tiles.end());
		const size_t index = static_cast<size_t>(it - path.tiles.begin());
		for (const TaxiSegment &segment : path.segments) {
			if (index >= segment.start_index && index <= segment.end_index) return segment.type;
		}
		FAIL("no segment covers the tile");
		return TaxiSegmentType::FreeMove;
	};

	REQUIRE(type_of_middle() == TaxiSegmentType::FreeMove);

	/* Segment types are a layout-derived cache that happens to live on the aircraft, so a
	 * tile turned one-way underneath an existing path has to update it too. Without the
	 * refresh the aircraft keeps taxiing on the classification the tile used to have. */
	ModularAirportTileData *middle_data = st->airport.GetModularTileData(middle);
	REQUIRE(middle_data != nullptr);
	middle_data->one_way_taxi = true;
	middle_data->user_taxi_dir_mask = 0x04;
	st->airport.MarkLayoutDirty();
	RefreshModularAircraftPathSegments(st);

	CHECK(type_of_middle() == TaxiSegmentType::OneWay);
	/* The route itself is untouched: refreshing is a reclassification, not a reroute. */
	CHECK(v->taxi_path->tiles.front() == stand_a);
	CHECK(v->taxi_path->tiles.back() == stand_b);
}

TEST_CASE("ModularAirportRestoresPathSegmentsAfterLoad")
{
	Map::Allocate(64, 64);
	TileIndex base = TileXY(10, 10);
	Station *st = SetupModularAirport(base, 1, 5);
	REQUIRE(st != nullptr);

	const TileIndex stand_a = base + TileDiffXY(0, 1);
	const TileIndex middle = base + TileDiffXY(0, 2);
	const TileIndex stand_b = base + TileDiffXY(0, 3);
	AddModularTile(st, stand_a, APT_STAND, 0);
	AddModularTile(st, middle, APT_APRON, 0);
	AddModularTile(st, stand_b, APT_STAND, 0);

	ModularAirportTileData *middle_data = st->airport.GetModularTileData(middle);
	REQUIRE(middle_data != nullptr);
	middle_data->one_way_taxi = true;
	middle_data->user_taxi_dir_mask = 0x04;
	st->airport.MarkLayoutDirty();

	SetupAircraftPool();
	Aircraft *v = CreateAircraft(VehicleID(10));
	v->targetairport = st->index;
	v->tile = stand_a;

	SECTION("A route loaded without segments is classified against the current layout") {
		/* What the save/load handler leaves behind: tiles and validity, no segments. */
		v->taxi_path = std::make_unique<TaxiPath>();
		v->taxi_path->valid = true;
		v->taxi_path->tiles = {stand_a, middle, stand_b};

		RestoreModularAircraftPathSegments();

		REQUIRE(v->taxi_path != nullptr);
		REQUIRE(v->taxi_path->segments.size() == 3);
		CHECK(v->taxi_path->segments[0].type == TaxiSegmentType::FreeMove);
		CHECK(v->taxi_path->segments[1].type == TaxiSegmentType::OneWay);
		CHECK(v->taxi_path->segments[2].type == TaxiSegmentType::FreeMove);
	}

	SECTION("A route whose traversed prefix was demolished is kept") {
		/* Demolition only refuses tiles an aircraft is standing on, so a player can remove
		 * the start of a route the aircraft has already taxied past. Rejecting the whole
		 * route for that would drop it on a joining client while the server taxis on --
		 * an immediate desync. */
		v->tile = middle;
		v->taxi_path = std::make_unique<TaxiPath>();
		v->taxi_path->valid = true;
		v->taxi_path->tiles = {stand_a, middle, stand_b};
		v->taxi_path_index = 1;

		auto &tile_data = *st->airport.modular_tile_data;
		tile_data.erase(std::remove_if(tile_data.begin(), tile_data.end(),
				[&](const ModularAirportTileData &d) { return d.tile == stand_a; }), tile_data.end());
		st->airport.modular_tile_index_dirty = true;
		st->airport.MarkLayoutDirty();
		MakeClear(stand_a, ClearGround::Grass, 0); // What DoClearSquare leaves behind.
		REQUIRE_FALSE(IsTileType(stand_a, TileType::Station));

		RestoreModularAircraftPathSegments();

		REQUIRE(v->taxi_path != nullptr);
		REQUIRE(v->taxi_path->tiles.size() == 3);
		/* The demolished tile classifies as FreeMove, exactly as it does on the server. */
		REQUIRE(v->taxi_path->segments.size() == 3);
		CHECK(v->taxi_path->segments[0].type == TaxiSegmentType::FreeMove);
		CHECK(v->taxi_path->segments[1].type == TaxiSegmentType::OneWay);

		/* ...and the live refresh still reaches it. */
		v->taxi_path->segments.clear();
		RefreshModularAircraftPathSegments(st);
		CHECK(v->taxi_path->segments.size() == 3);
	}

	SECTION("A route with no airport under it is discarded") {
		/* Tiles far outside any station -- there is nothing to classify against. */
		v->taxi_path = std::make_unique<TaxiPath>();
		v->taxi_path->valid = true;
		v->taxi_path->tiles = {TileXY(40, 40), TileXY(40, 41)};

		RestoreModularAircraftPathSegments();

		CHECK(v->taxi_path == nullptr);
	}

	SECTION("The owning airport is read from the route, not from targetairport") {
		/* An aborted landing leaves a chain behind while order processing moves
		 * targetairport on, so the two disagree. The route is still this airport's. */
		v->targetairport = StationID::Invalid();
		v->landing_chain_path = std::make_unique<TaxiPath>();
		v->landing_chain_path->valid = true;
		v->landing_chain_path->tiles = {stand_a, middle, stand_b};

		RestoreModularAircraftPathSegments();

		REQUIRE(v->landing_chain_path != nullptr);
		REQUIRE(v->landing_chain_path->segments.size() == 3);
		CHECK(v->landing_chain_path->segments[1].type == TaxiSegmentType::OneWay);
	}
}

TEST_CASE("ModularAirportKeepSetHoldsHelicopterLandingChain")
{
	Map::Allocate(64, 64);
	TileIndex base = TileXY(10, 10);
	Station *st = SetupModularAirport(base, 10, 10);
	REQUIRE(st != nullptr);

	/* A helicopter descends straight onto its pad, so its landing chain starts at the
	 * touchdown tile instead of at a runway rollout point. That makes it the one landing
	 * route which never reaches MGT_ROLLOUT while still airborne, and therefore the case
	 * the keep-set's landing-chain condition has to be right about on its own. */
	const TileIndex pad = base + TileDiffXY(2, 2);
	const TileIndex apron = base + TileDiffXY(2, 3);
	const TileIndex stand = base + TileDiffXY(2, 4);
	AddModularTile(st, pad, APT_HELIPAD_2, 0);
	AddModularTile(st, apron, APT_APRON, 0);
	AddModularTile(st, stand, APT_STAND, 0);

	SetupAircraftPool();
	Aircraft *v = CreateAircraft(VehicleID(10));
	v->subtype = AIR_HELICOPTER;
	v->targetairport = st->index;

	TaxiPath chain = BuildTaxiPath(st, pad, stand, nullptr, false, GetGroundPathRestriction(v));
	REQUIRE(chain.valid);
	REQUIRE(chain.tiles.front() == pad);

	const auto keep_set_for = [&]() {
		std::vector<TileIndex> keep_set;
		BuildReservationKeepSet(v, st, keep_set);
		return keep_set;
	};

	SECTION("Kept while the helicopter is still descending onto the pad") {
		v->tile = TileIndex{}; // In air, as the modular movement code leaves it.
		v->modular_landing_tile = pad;
		v->landing_chain_path = std::make_unique<TaxiPath>(chain);

		const std::vector<TileIndex> keep_set = keep_set_for();
		for (TileIndex tile : chain.tiles) {
			CHECK(std::find(keep_set.begin(), keep_set.end(), tile) != keep_set.end());
		}
	}

	SECTION("Kept from touchdown until ground movement takes the chain over") {
		/* Touchdown clears modular_landing_tile and sets MGT_ROLLOUT in the same call,
		 * so this is the whole of the rest of the chain's life. */
		v->tile = pad;
		v->modular_landing_tile = INVALID_TILE;
		v->modular_ground_target = MGT_ROLLOUT;
		v->landing_chain_path = std::make_unique<TaxiPath>(chain);

		const std::vector<TileIndex> keep_set = keep_set_for();
		for (TileIndex tile : chain.tiles) {
			CHECK(std::find(keep_set.begin(), keep_set.end(), tile) != keep_set.end());
		}
	}

	SECTION("Not kept once the landing is over and the chain is only a leftover") {
		/* Parked, landing long finished. Retaining the route here pinned tiles the
		 * aircraft has no claim on, which is what the condition was added to stop. */
		v->tile = stand;
		v->modular_landing_tile = INVALID_TILE;
		v->modular_ground_target = MGT_TERMINAL;
		v->landing_chain_path = std::make_unique<TaxiPath>(chain);

		const std::vector<TileIndex> keep_set = keep_set_for();
		CHECK(std::find(keep_set.begin(), keep_set.end(), pad) == keep_set.end());
		CHECK(std::find(keep_set.begin(), keep_set.end(), apron) == keep_set.end());
	}
}

TEST_CASE("ModularAirportRolloutHoldingUsesReservedBuffer")
{
	Map::Allocate(64, 64);
	TileIndex base = TileXY(10, 10);
	Station *st = SetupModularAirport(base, 10, 10);
	REQUIRE(st != nullptr);

	/* Two ways off one rollout end, with the nearest stand *not* on the same route
	 * as the far queue tile:
	 *
	 *   runway (0,2)-(2,2), rolling out at (2,2)
	 *   near exit: (2,3) one-way queue -> (2,4) stand          <- cheapest service tile
	 *   far exit:  (3,2) one-way queue -> (4,2) apron -> (5,2) stand
	 *
	 * A landing admitted with no free stand reserves the runway plus one one-way
	 * queue tile, and which one it gets depends on where the admission horizon
	 * ended -- not on which stand is nearest. So the reserved buffer routinely sits
	 * off the route to the cheapest stand, as it does here. */
	AddLargeRunway(st, base + TileDiffXY(0, 2), 3, 0, RUF_DEFAULT);

	ModularAirportTileData *near_queue = AddModularTileWithData(st, base + TileDiffXY(2, 3), APT_APRON, 0);
	near_queue->one_way_taxi = true;
	near_queue->user_taxi_dir_mask = 0x04; // +Y, entered from the rollout end
	AddModularTile(st, base + TileDiffXY(2, 4), APT_STAND, 0);

	ModularAirportTileData *far_queue = AddModularTileWithData(st, base + TileDiffXY(3, 2), APT_APRON, 0);
	far_queue->one_way_taxi = true;
	far_queue->user_taxi_dir_mask = 0x02; // +X, entered from the rollout end
	AddModularTile(st, base + TileDiffXY(4, 2), APT_APRON, 0);
	AddModularTile(st, base + TileDiffXY(5, 2), APT_STAND, 0);

	const TileIndex rollout = base + TileDiffXY(2, 2);
	const TileIndex near_stop = base + TileDiffXY(2, 3);
	const TileIndex near_stand = base + TileDiffXY(2, 4);
	const TileIndex far_buffer = base + TileDiffXY(3, 2);
	REQUIRE(IsModularSafeStopTile(st, far_buffer));

	SetupAircraftPool();
	Aircraft *v = CreateAircraft(VehicleID(10));
	v->targetairport = st->index;
	v->tile = rollout;

	/* The buffer this aircraft's landing was admitted against. */
	SetTaxiReservation(v, far_buffer);

	SECTION("Falls back to the reserved buffer when the nearest route is held") {
		Aircraft *blocker = CreateAircraft(VehicleID(11));
		blocker->targetairport = st->index;
		blocker->tile = near_stand;
		SetTaxiReservation(blocker, near_stop);
		SetTaxiReservation(blocker, near_stand);

		/* Regression: the search picks the single cheapest service tile and walks
		 * only that one route. With both of its safe stops held it used to give up
		 * outright, leaving the aircraft parked on the runway at landing speed with
		 * no goal -- while it owned a perfectly good one-way queue tile the other
		 * way. That produced `rollout fallback failed` immediately followed by
		 * `invalid ground state ... at speed`. */
		CHECK(FindModularRolloutHoldingTile(st, v, rollout) == far_buffer);
	}

	SECTION("Still prefers a clear stop on the route towards a stand") {
		/* Nothing is held, so the ordinary search wins and the aircraft keeps making
		 * progress towards parking instead of retreating onto its own buffer. */
		CHECK(FindModularRolloutHoldingTile(st, v, rollout) == near_stop);
	}

	SECTION("Falls back to the reserved buffer when no service tile is reachable") {
		/* An isolated runway + buffer with no reachable stand or hangar. */
		TileIndex iso_base = TileXY(25, 25);
		Station *iso_st = SetupModularAirport(iso_base, 10, 10);
		REQUIRE(iso_st != nullptr);
		AddLargeRunway(iso_st, iso_base + TileDiffXY(0, 2), 3, 0, RUF_DEFAULT);
		ModularAirportTileData *iso_queue = AddModularTileWithData(iso_st, iso_base + TileDiffXY(3, 2), APT_APRON, 0);
		iso_queue->one_way_taxi = true;
		iso_queue->user_taxi_dir_mask = 0x02; // +X

		Aircraft *iso_v = CreateAircraft(VehicleID(12));
		iso_v->targetairport = iso_st->index;
		iso_v->tile = iso_base + TileDiffXY(2, 2);
		SetTaxiReservation(iso_v, iso_base + TileDiffXY(3, 2));

		CHECK(FindModularRolloutHoldingTile(iso_st, iso_v, iso_base + TileDiffXY(2, 2)) == iso_base + TileDiffXY(3, 2));
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
		 * through and stacked two aircraft on one tile -- while HandleModularEndLanding
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
		 * itself -- so a probe that inserted keys would mutate shared state off a
		 * client-local schedule. FindAirportGroundPath writes by default; the fix is
		 * that this caller opts out. */
		ClearModularAirportCrossingPathCache();
		st->airport.MarkLayoutDirty();
		EnsureModularHeliTilesValid(st);
		CHECK(_modular_airport_crossing_required_path_cache.empty());

		/* And the answer must not depend on what the cache already learned. A key here
		 * sends that pair down the crossing pass, which reports a different cost for an
		 * unchanged layout -- ranking on cost would let the winner move. */
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
		 * runway, so it lifts off and picks it again -- forever. */
		Station *st = build(true, true, false);
		const TileIndex good_pad = base + TileDiffXY(5, 5);
		AddModularTile(st, good_pad, APT_HELIPAD_2, 0);

		EnsureModularHeliTilesValid(st);
		REQUIRE(FindAirportGroundPath(st, good_pad, hangar, nullptr).found);
		REQUIRE_FALSE(FindAirportGroundPath(st, heliport, hangar, nullptr).found);

		/* Some pad works, so no service tile is needed -- the filter carries this case. */
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

		/* An ordinary flight still uses the nearest pad, cut off or not -- it only has to
		 * park there, and the heliport is a legal parking spot. */
		CHECK(FindModularLandingTarget(st, heli) == heliport);

		/* Heading for the hangar, it must give up the near pad for the reachable one. */
		heli->current_order.MakeGoToDepot(st->index, OrderDepotTypeFlag::Service);
		CHECK(FindModularLandingTarget(st, heli) == good_pad);
	}

	SECTION("Nothing reaches the hangar at all: still land, do not circle for good") {
		/* An isolated hangar, so neither a pad nor any parkable apron or stand can reach
		 * it -- both the reachable-pad set and the service tile come up empty. There is
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

TEST_CASE("ModularAirportHangarPresence")
{
	Map::Allocate(64, 64);
	const TileIndex base = TileXY(10, 10);
	Station *st = SetupModularAirport(base, 10, 10);
	REQUIRE(st != nullptr);
	AirportSpec::ResetAirports();
	st->airport.type = AT_MODULAR;

	const TileIndex pad = base + TileDiffXY(2, 2);
	const TileIndex hangar = base + TileDiffXY(2, 3);
	AddModularTile(st, pad, APT_HELIPAD_2, 0);

	SECTION("A pad-only airport has no hangar") {
		CHECK_FALSE(st->airport.HasHangar());
	}

	SECTION("A hangar piece is what makes it true") {
		AddModularTile(st, hangar, APT_DEPOT_SE, 0);

		CHECK(st->airport.HasHangar());
	}

	SECTION("Losing the last hangar takes effect at once") {
		AddModularTile(st, hangar, APT_DEPOT_SE, 0);
		REQUIRE(st->airport.HasHangar());

		/* What the tile removal path does: drop the tile, then invalidate. The cache is
		 * layout-derived, so it must ride MarkLayoutDirty like the rest. */
		st->airport.modular_tile_data->pop_back();
		st->airport.MarkLayoutDirty();

		CHECK_FALSE(st->airport.HasHangar());
	}

	SECTION("An airport with no modular data at all has no hangar") {
		st->airport.modular_tile_data->clear();
		st->airport.MarkLayoutDirty();

		CHECK_FALSE(st->airport.HasHangar());
	}
}

TEST_CASE("ModularAirportWantsHangarNeedsOneToExist")
{
	Map::Allocate(64, 64);
	const TileIndex base = TileXY(10, 10);
	Station *st = SetupModularAirport(base, 10, 10);
	REQUIRE(st != nullptr);
	AirportSpec::ResetAirports();
	st->airport.type = AT_MODULAR;

	const TileIndex pad = base + TileDiffXY(2, 2);
	const TileIndex hangar = base + TileDiffXY(2, 3);
	AddModularTile(st, pad, APT_HELIPAD_2, 0);

	SetupAircraftPool();
	Aircraft *heli = CreateAircraft(VehicleID(10));
	heli->subtype = AIR_HELICOPTER;
	heli->targetairport = st->index;
	heli->tile = pad;
	/* No type flag: what "Send aircraft to hangar" issues. */
	heli->current_order.MakeGoToDepot(st->index, OrderDepotTypeFlags{});

	SECTION("A depot order does not aim at a hangar the airport has not got") {
		/* Wanting a hangar suppresses helipad and stand selection, so answering yes here
		 * is what strands the helicopter: it lands, refuses every parking spot, lifts off
		 * and picks the same airport again. */
		CHECK_FALSE(ModularAircraftWantsHangar(heli, st));
	}

	SECTION("The same order does aim at one that exists") {
		AddModularTile(st, hangar, APT_DEPOT_SE, 0);

		CHECK(ModularAircraftWantsHangar(heli, st));
	}

	SECTION("With neither an order nor service due, a hangar is not wanted") {
		AddModularTile(st, hangar, APT_DEPOT_SE, 0);
		heli->current_order.MakeDummy();
		/* NeedsServicing bails on a stopped vehicle before it looks up the company,
		 * which a bare-shell test aircraft does not have. */
		heli->vehstatus.Set(VehState::Stopped);

		CHECK_FALSE(ModularAircraftWantsHangar(heli, st));
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

TEST_CASE("ModularAirportHangarAccessors")
{
	Map::Allocate(64, 64);
	const TileIndex base = TileXY(10, 10);
	Station *st = SetupModularAirport(base, 10, 10);
	REQUIRE(st != nullptr);
	AirportSpec::ResetAirports();
	st->airport.type = AT_MODULAR;

	SECTION("No hangar tiles means no hangars") {
		AddModularTile(st, base + TileDiffXY(2, 2), APT_APRON, 0);
		CHECK(st->airport.GetNumHangars() == 0);
	}

	SECTION("One hangar tile is one hangar, found at its own tile") {
		const TileIndex hangar = base + TileDiffXY(4, 3);
		AddModularTile(st, base + TileDiffXY(2, 2), APT_APRON, 0);
		AddModularTile(st, hangar, APT_DEPOT_SE, 0);

		CHECK(st->airport.GetNumHangars() == 1);
		CHECK(st->airport.GetHangarTile(0) == hangar);
		CHECK(st->airport.GetHangarNum(hangar) == 0);
	}

	SECTION("Several hangars are numbered by ascending TileIndex, not build order") {
		/* Added deliberately out of order: modular_tile_data keeps build order, and
		 * hangar numbers reach saved orders and the script API, so they must not. */
		const TileIndex high = base + TileDiffXY(5, 5);
		const TileIndex low = base + TileDiffXY(1, 1);
		const TileIndex mid = base + TileDiffXY(3, 3);
		AddModularTile(st, high, APT_DEPOT_SE, 0);
		AddModularTile(st, low, APT_DEPOT_NE, 0);
		AddModularTile(st, mid, APT_SMALL_DEPOT_SE, 0);
		REQUIRE(low < mid);
		REQUIRE(mid < high);

		CHECK(st->airport.GetNumHangars() == 3);
		CHECK(st->airport.GetHangarTile(0) == low);
		CHECK(st->airport.GetHangarTile(1) == mid);
		CHECK(st->airport.GetHangarTile(2) == high);
		CHECK(st->airport.GetHangarNum(low) == 0);
		CHECK(st->airport.GetHangarNum(mid) == 1);
		CHECK(st->airport.GetHangarNum(high) == 2);

		/* Rebuilding the middle hangar moves it to the back of modular_tile_data.
		 * Its number must not move with it. */
		const size_t before = st->airport.modular_tile_data->size();
		std::erase_if(*st->airport.modular_tile_data,
			[mid](const ModularAirportTileData &d) { return d.tile == mid; });
		st->airport.modular_tile_index_dirty = true;
		st->airport.MarkLayoutDirty();
		AddModularTile(st, mid, APT_DEPOT_SE, 0);
		REQUIRE(st->airport.modular_tile_data->size() == before);
		REQUIRE(st->airport.modular_tile_data->back().tile == mid);

		CHECK(st->airport.GetHangarTile(1) == mid);
		CHECK(st->airport.GetHangarNum(mid) == 1);
		CHECK(st->airport.GetHangarNum(high) == 2);
	}

	SECTION("Exit direction comes from the piece, not the preset's layout rotation") {
		const TileIndex hangar = base + TileDiffXY(4, 3);

		/* Airport::GetHangarExitDirection used to run the preset's depot table through
		 * the preset's layout rotation. Pin it to the piece-derived answer the modular
		 * movement code has always used, so the two can no longer disagree. Each
		 * directional variant fixes its own hangar rotation regardless of the rotation
		 * stored on the tile; the suffixes are graphic-orientation labels, unrelated to
		 * the Direction enum the answer is expressed in (see coords.md). */
		for (uint8_t piece : {APT_DEPOT_SE, APT_DEPOT_SW, APT_DEPOT_NW, APT_DEPOT_NE,
				APT_SMALL_DEPOT_SE, APT_SMALL_DEPOT_SW, APT_SMALL_DEPOT_NW, APT_SMALL_DEPOT_NE}) {
			st->airport.modular_tile_data->clear();
			st->airport.modular_tile_index_dirty = true;
			st->airport.MarkLayoutDirty();
			AddModularTile(st, hangar, piece, 0);

			CHECK(st->airport.GetHangarExitDirection(hangar) == GetModularHangarExitDirection(st, hangar));
		}

		/* APT_DEPOT_SE is the generic piece the stored rotation still speaks for. At
		 * rotation 0 its door faces dy=+1, which is Direction::SE for the leaving aircraft. */
		st->airport.modular_tile_data->clear();
		st->airport.modular_tile_index_dirty = true;
		st->airport.MarkLayoutDirty();
		AddModularTile(st, hangar, APT_DEPOT_SE, 0);
		CHECK(st->airport.GetHangarExitDirection(hangar) == Direction::SE);
	}

	SECTION("Rotating a hangar piece rotates its exit direction by the same amount") {
		const TileIndex hangar = base + TileDiffXY(4, 3);

		/* Deliberately not asserting that APT_DEPOT_SW exits Direction::SW: the suffixes are
		 * graphic-orientation labels and arguing from them is what got this wrong in
		 * the first place. This asserts only self-consistency, which holds whatever
		 * the labels mean -- turning the piece a quarter-turn must turn the door a
		 * quarter-turn, in the same direction, every time.
		 *
		 * A template placed with rotation != 0 runs its pieces through
		 * SwapBuildingPieceForRotation, so any disagreement here lands an aircraft in
		 * a hangar whose door the movement code believes faces somewhere it does not.
		 * The aircraft then never leaves: no error, and nothing in the airport log. */
		for (uint8_t start : {APT_DEPOT_SE, APT_SMALL_DEPOT_SE}) {
			st->airport.modular_tile_data->clear();
			st->airport.modular_tile_index_dirty = true;
			st->airport.MarkLayoutDirty();
			AddModularTile(st, hangar, start, 0);
			const Direction base_dir = GetModularHangarExitDirection(st, hangar);

			for (uint8_t r = 1; r < 4; r++) {
				uint8_t piece = start;
				SwapBuildingPieceForRotation(piece, r);

				st->airport.modular_tile_data->clear();
				st->airport.modular_tile_index_dirty = true;
				st->airport.MarkLayoutDirty();
				AddModularTile(st, hangar, piece, 0);

				/* One quarter-turn is two steps of the eight-way Direction enum. The
				 * sign follows from rotation 0's own mapping, so it is derived rather
				 * than assumed. */
				const Direction want = static_cast<Direction>((to_underlying(base_dir) + 8 - 2 * r) % 8);
				CHECK(GetModularHangarExitDirection(st, hangar) == want);
			}
		}
	}

	SECTION("A hangar's taxi opening is the tile its door faces") {
		/* Two answers about the same door: GetModularHangarExitDirection tells the
		 * aircraft which way to drive out, and CalculateAutoTaxiDirectionsForGfx
		 * tells the ground pathfinder which neighbour the hangar connects to. If
		 * they disagree the pathfinder cannot find a route from the hangar to any
		 * stand, FindFreeModularTerminal returns nothing, and the aircraft waits in
		 * the hangar forever -- silently, since waiting for a free stand is normal.
		 *
		 * Direction bit order is the pathfinder's own (see coords.md):
		 * 0 = (0,-1), 1 = (+1,0), 2 = (0,+1), 3 = (-1,0). */
		const TileIndex hangar = base + TileDiffXY(4, 3);
		for (uint8_t piece : {APT_DEPOT_SE, APT_DEPOT_NE, APT_DEPOT_NW, APT_DEPOT_SW,
				APT_SMALL_DEPOT_SE, APT_SMALL_DEPOT_NE, APT_SMALL_DEPOT_NW, APT_SMALL_DEPOT_SW}) {
			st->airport.modular_tile_data->clear();
			st->airport.modular_tile_index_dirty = true;
			st->airport.MarkLayoutDirty();
			AddModularTile(st, hangar, piece, 0);

			const Direction exit = GetModularHangarExitDirection(st, hangar);
			const uint8_t opening = CalculateAutoTaxiDirectionsForGfx(piece, 0);

			int want_bit = -1;
			switch (exit) {
				case Direction::SE: want_bit = 2; break; // (0, +1)
				case Direction::SW: want_bit = 1; break; // (+1, 0)
				case Direction::NW: want_bit = 0; break; // (0, -1)
				case Direction::NE: want_bit = 3; break; // (-1, 0)
				default: break;
			}
			REQUIRE(want_bit >= 0);
			CHECK(opening == (1 << want_bit));
		}
	}
}

TEST_CASE("ModularAirportAircraftCapability")
{
	Map::Allocate(64, 64);
	const TileIndex base = TileXY(10, 10);
	Station *st = SetupModularAirport(base, 20, 20);
	REQUIRE(st != nullptr);
	AirportSpec::ResetAirports();
	/* The modular spec deliberately clones the country FSM, whose broad flags are
	 * still unsuitable for capability decisions; those remain layout-derived. */
	st->airport.type = AT_MODULAR;
	REQUIRE(AirportSpec::Get(AT_MODULAR)->fsm->flags.Test(AirportFTAClass::Flag::Airplanes));

	SECTION("A pad-only layout takes helicopters and refuses planes") {
		AddModularTile(st, base + TileDiffXY(2, 2), APT_HELIPAD_2, 0);
		AddModularTile(st, base + TileDiffXY(2, 3), APT_APRON, 0);

		CHECK_FALSE(ModularAirportAcceptsPlanes(st));
		CHECK(ModularAirportAcceptsHelicopters(st));
		CHECK(GetModularAirportNewGRFType(st) == ATP_TTDP_HELIPORT);
	}

	SECTION("A runway layout takes both") {
		AddLargeRunway(st, base + TileDiffXY(1, 1), 6, 0, RUF_LANDING | RUF_TAKEOFF | RUF_DIR_LOW);
		AddModularTile(st, base + TileDiffXY(1, 3), APT_APRON, 0);

		CHECK(ModularAirportAcceptsPlanes(st));
		/* No helipad, but the heli-tile machinery finds somewhere to put one down. */
		CHECK(ModularAirportAcceptsHelicopters(st));
		CHECK(GetModularAirportNewGRFType(st) == ATP_TTDP_SMALL);
	}

	SECTION("A mixed layout takes both") {
		AddLargeRunway(st, base + TileDiffXY(1, 1), 6, 0, RUF_LANDING | RUF_TAKEOFF | RUF_DIR_LOW);
		AddModularTile(st, base + TileDiffXY(1, 3), APT_APRON, 0);
		AddModularTile(st, base + TileDiffXY(2, 3), APT_HELIPAD_2, 0);

		CHECK(ModularAirportAcceptsPlanes(st));
		CHECK(ModularAirportAcceptsHelicopters(st));
	}

	SECTION("A runway a plane cannot leave from is not enough") {
		/* Landing but no takeoff: stranding the plane is worse than refusing it. */
		AddLargeRunway(st, base + TileDiffXY(1, 1), 6, 0, RUF_LANDING | RUF_DIR_LOW);
		AddModularTile(st, base + TileDiffXY(1, 3), APT_APRON, 0);

		CHECK_FALSE(ModularAirportAcceptsPlanes(st));
	}

	SECTION("Landing and takeoff may live on different runways") {
		AddLargeRunway(st, base + TileDiffXY(1, 1), 6, 0, RUF_LANDING | RUF_DIR_LOW);
		AddLargeRunway(st, base + TileDiffXY(1, 5), 6, 0, RUF_TAKEOFF | RUF_DIR_LOW);

		CHECK(ModularAirportAcceptsPlanes(st));
	}

	SECTION("A short runway still counts -- length is a separate question") {
		AddLargeRunway(st, base + TileDiffXY(1, 1), 3, 0, RUF_LANDING | RUF_TAKEOFF | RUF_DIR_LOW);

		CHECK(ModularAirportAcceptsPlanes(st));
		CHECK_FALSE(ModularAirportSupportsLargeAircraft(st));
	}

	SECTION("The answer follows the runway flags") {
		AddLargeRunway(st, base + TileDiffXY(1, 1), 6, 0, RUF_LANDING | RUF_TAKEOFF | RUF_DIR_LOW);
		REQUIRE(ModularAirportAcceptsPlanes(st));

		for (ModularAirportTileData &d : *st->airport.modular_tile_data) d.runway_flags = 0;
		st->airport.MarkLayoutDirty();

		CHECK_FALSE(ModularAirportAcceptsPlanes(st));
	}
}

TEST_CASE("ModularAirportNearestHangarRespectsHelicopterCapability")
{
	Map::Allocate(64, 64);
	const TileIndex base = TileXY(10, 10);
	Station *st = SetupModularAirport(base, 12, 12);
	REQUIRE(st != nullptr);
	AddModularTile(st, base + TileDiffXY(1, 1), APT_DEPOT_SE, 0);
	REQUIRE(st->airport.HasHangar());
	REQUIRE_FALSE(ModularAirportAcceptsHelicopters(st));

	_engine_pool.CleanPool();
	const EngineID helicopter_engine = CreateAircraftEngine(EngineID(0), 0);
	SetupAircraftPool();
	Aircraft *heli = CreateAircraft(VehicleID(0));
	heli->subtype = AIR_HELICOPTER;
	heli->engine_type = helicopter_engine;
	heli->owner = OWNER_NONE;
	heli->targetairport = StationID::Invalid();
	heli->x_pos = TileX(base) * TILE_SIZE;
	heli->y_pos = TileY(base) * TILE_SIZE;

	CHECK_FALSE(heli->FindClosestDepot().found);

	const TileIndex landing_tile = base + TileDiffXY(8, 8);
	AddModularTile(st, landing_tile, APT_APRON, 0);
	REQUIRE(ModularAirportAcceptsHelicopters(st));
	const ClosestDepot depot = heli->FindClosestDepot();
	CHECK(depot.found);
	CHECK(depot.location == st->xy);
}

TEST_CASE("ModularAirportTakeoffRetargetsUnreachableRunway")
{
	Map::Allocate(64, 64);
	TileIndex base = TileXY(10, 10);
	Station *st = SetupModularAirport(base, 10, 10);
	REQUIRE(st != nullptr);

	/* Prunfingbridge Woods shape: two parallel runways whose only connection is a
	 * pair of stands, with a hangar opening onto one of them.
	 *   y=0  runway A (x 0..4)
	 *   y=1  .  HANGAR APRON
	 *   y=2  .  STAND  STAND   <- the only link between the halves
	 *   y=3  .  APRON  APRON  APRON
	 *   y=4  runway B (x 0..4) */
	AddLargeRunway(st, base + TileDiffXY(0, 0), 5, 0, RUF_DEFAULT);
	AddModularTile(st, base + TileDiffXY(1, 1), APT_DEPOT_SE, 0);
	AddModularTile(st, base + TileDiffXY(2, 1), APT_APRON, 0);
	AddModularTile(st, base + TileDiffXY(1, 2), APT_STAND, 0);
	AddModularTile(st, base + TileDiffXY(2, 2), APT_STAND, 0);
	AddModularTile(st, base + TileDiffXY(1, 3), APT_APRON, 0);
	AddModularTile(st, base + TileDiffXY(2, 3), APT_APRON, 0);
	AddModularTile(st, base + TileDiffXY(3, 3), APT_APRON, 0);
	AddLargeRunway(st, base + TileDiffXY(0, 4), 5, 0, RUF_DEFAULT);

	const TileIndex own_stand = base + TileDiffXY(1, 2);
	const TileIndex link_stand = base + TileDiffXY(2, 2);
	const TileIndex runway_a_end = base + TileDiffXY(4, 0);

	auto on_runway_b = [&](TileIndex tile) {
		return IsValidTile(tile) && TileY(tile) == TileY(base) + 4;
	};

	/* A prop keeps the runway-class rule out of the way of what is under test here. */
	_engine_pool.CleanPool();
	const EngineID prop_engine = CreateAircraftEngine(EngineID(0), 0);

	SECTION("A takeoff end cut off after selection is replaced by a reachable one") {
		SetupAircraftPool();
		Aircraft *blocker = CreateAircraft(VehicleID(11));
		blocker->targetairport = st->index;
		blocker->engine_type = prop_engine;
		blocker->tile = link_stand;
		SetTaxiReservation(blocker, link_stand);

		Aircraft *v = CreateAircraft(VehicleID(10));
		v->targetairport = st->index;
		v->engine_type = prop_engine;
		v->tile = own_stand;
		v->modular_ground_target = MGT_RUNWAY_TAKEOFF;
		v->modular_takeoff_tile = runway_a_end;
		v->ground_path_goal = runway_a_end;

		/* Runway A was a valid pick when it was made; the blocker has since taken the
		 * only tile connecting the two halves. */
		REQUIRE_FALSE(FindAirportGroundPath(st, own_stand, runway_a_end, v, true, false).found);

		CHECK(TryRetargetModularGroundGoal(v, st));
		CHECK(on_runway_b(v->ground_path_goal));
		CHECK(v->modular_takeoff_tile == v->ground_path_goal);
	}

	SECTION("A still-reachable takeoff end is left alone") {
		SetupAircraftPool();
		Aircraft *v = CreateAircraft(VehicleID(10));
		v->targetairport = st->index;
		v->engine_type = prop_engine;
		v->tile = own_stand;
		v->modular_ground_target = MGT_RUNWAY_TAKEOFF;

		const TileIndex chosen = FindModularRunwayTileForTakeoff(st, v);
		REQUIRE(chosen != INVALID_TILE);
		v->modular_takeoff_tile = chosen;
		v->ground_path_goal = chosen;

		/* Nothing about the layout changed, so re-deciding must not churn the goal. */
		CHECK_FALSE(TryRetargetModularGroundGoal(v, st));
		CHECK(v->ground_path_goal == chosen);
		CHECK(v->modular_takeoff_tile == chosen);
	}
}

TEST_CASE("ModularAirportDepotOrderOverridesLatchedDeparture")
{
	Map::Allocate(64, 64);
	TileIndex base = TileXY(10, 10);
	Station *st = SetupModularAirport(base, 10, 10);
	REQUIRE(st != nullptr);

	/* Bindhattan shape: a single-file stand row whose east end is the only way into the
	 * hangar and whose west end is the only way to the runway. Two aircraft heading
	 * opposite ways along it cannot pass, so a depot order from the player is the only
	 * thing that can resolve the jam -- and it only can if it is allowed to override a
	 * departure the aircraft has already settled on.
	 *   y=0  runway (x 0..4)
	 *   y=1  APRON                             HANGAR
	 *   y=2  APRON  STAND  STAND  STAND  APRON        */
	AddLargeRunway(st, base + TileDiffXY(0, 0), 5, 0, RUF_DEFAULT);
	AddModularTile(st, base + TileDiffXY(0, 1), APT_APRON, 0);
	AddModularTile(st, base + TileDiffXY(0, 2), APT_APRON, 0);
	AddModularTile(st, base + TileDiffXY(1, 2), APT_STAND, 0);
	AddModularTile(st, base + TileDiffXY(2, 2), APT_STAND, 0);
	AddModularTile(st, base + TileDiffXY(3, 2), APT_STAND, 0);
	AddModularTile(st, base + TileDiffXY(4, 2), APT_APRON, 0);

	const TileIndex own_stand = base + TileDiffXY(2, 2);
	const TileIndex hangar = base + TileDiffXY(4, 1);
	const TileIndex runway_end = base + TileDiffXY(4, 0);

	_engine_pool.CleanPool();
	const EngineID prop_engine = CreateAircraftEngine(EngineID(0), 0);

	/* Parked at a stand with a takeoff already latched, exactly as HandleModularTerminal
	 * leaves an aircraft that decided to depart before the order arrived. */
	auto departing_aircraft = [&]() {
		SetupAircraftPool();
		/* The probe is staggered by vehicle index over a 64-tick cycle; index 0 at counter
		 * 0 is on-cycle, so the refusals below fail for their own reason, not the cadence. */
		TimerGameTick::counter = 0;
		Aircraft *v = CreateAircraft(VehicleID(0));
		v->targetairport = st->index;
		v->engine_type = prop_engine;
		v->tile = own_stand;
		v->x_pos = TileX(own_stand) * TILE_SIZE + TILE_SIZE / 2;
		v->y_pos = TileY(own_stand) * TILE_SIZE + TILE_SIZE / 2;
		v->modular_ground_target = MGT_RUNWAY_TAKEOFF;
		v->modular_takeoff_tile = runway_end;
		v->ground_path_goal = runway_end;
		return v;
	};

	SECTION("A depot order for this airport overrides the latched takeoff") {
		AddModularTile(st, hangar, APT_DEPOT_SE, 0);
		Aircraft *v = departing_aircraft();
		v->current_order.MakeGoToDepot(st->index, OrderDepotTypeFlag::PartOfOrders);

		CHECK(MaybeSwitchModularDepartureToHangar(v, st));
		CHECK(v->modular_ground_target == MGT_HANGAR);
		CHECK(v->ground_path_goal == hangar);
		CHECK(v->modular_takeoff_tile == INVALID_TILE);
	}

	SECTION("A depot order naming another airport still departs") {
		AddModularTile(st, hangar, APT_DEPOT_SE, 0);
		Aircraft *v = departing_aircraft();
		v->current_order.MakeGoToDepot(StationID(1), OrderDepotTypeFlag::PartOfOrders);

		CHECK_FALSE(MaybeSwitchModularDepartureToHangar(v, st));
		CHECK(v->modular_ground_target == MGT_RUNWAY_TAKEOFF);
		CHECK(v->ground_path_goal == runway_end);
	}

	SECTION("Without a depot order the departure stands") {
		AddModularTile(st, hangar, APT_DEPOT_SE, 0);
		Aircraft *v = departing_aircraft();
		v->current_order.MakeGoToStation(st->index);

		CHECK_FALSE(MaybeSwitchModularDepartureToHangar(v, st));
		CHECK(v->modular_ground_target == MGT_RUNWAY_TAKEOFF);
	}

	SECTION("A jammed aircraft still carrying taxi speed is retargeted") {
		AddModularTile(st, hangar, APT_DEPOT_SE, 0);
		Aircraft *v = departing_aircraft();
		v->current_order.MakeGoToDepot(st->index, OrderDepotTypeFlag::PartOfOrders);
		/* Modular ground movement never zeroes cur_speed when an aircraft stops: a blocked
		 * one returns before UpdateAircraftSpeed is reached, so it keeps taxi speed for as
		 * long as it waits. Aircraft jammed for thousands of ticks were observed at 200.
		 * Reading that as "in motion" would refuse exactly the aircraft this exists for. */
		v->cur_speed = 200;

		CHECK(MaybeSwitchModularDepartureToHangar(v, st));
		CHECK(v->modular_ground_target == MGT_HANGAR);
		CHECK(v->ground_path_goal == hangar);
	}

	SECTION("An aircraft taxiing out of a hangar is retargeted") {
		AddModularTile(st, hangar, APT_DEPOT_SE, 0);
		Aircraft *v = departing_aircraft();
		v->current_order.MakeGoToDepot(st->index, OrderDepotTypeFlag::PartOfOrders);
		/* HandleModularHangar latches the departure without changing state, and nothing
		 * changes it again before the runway, so state stays HANGAR across the whole taxi
		 * out. Only being hidden means still parked inside. */
		v->state = HANGAR;

		CHECK(MaybeSwitchModularDepartureToHangar(v, st));
		CHECK(v->modular_ground_target == MGT_HANGAR);
	}

	SECTION("An aircraft still inside the hangar is left alone") {
		AddModularTile(st, hangar, APT_DEPOT_SE, 0);
		Aircraft *v = departing_aircraft();
		v->current_order.MakeGoToDepot(st->index, OrderDepotTypeFlag::PartOfOrders);
		v->state = HANGAR;
		v->vehstatus.Set(VehState::Hidden);

		CHECK_FALSE(MaybeSwitchModularDepartureToHangar(v, st));
		CHECK(v->modular_ground_target == MGT_RUNWAY_TAKEOFF);
	}

	SECTION("The pathfinder probe is paced") {
		AddModularTile(st, hangar, APT_DEPOT_SE, 0);
		Aircraft *v = departing_aircraft();
		v->current_order.MakeGoToDepot(st->index, OrderDepotTypeFlag::PartOfOrders);
		/* Off-cycle: an aircraft whose hangar never becomes reachable stays eligible
		 * forever, so the A* behind this must not run on every tick. */
		TimerGameTick::counter = 1;

		CHECK_FALSE(MaybeSwitchModularDepartureToHangar(v, st));
		TimerGameTick::counter = 64;
		CHECK(MaybeSwitchModularDepartureToHangar(v, st));
	}

	SECTION("An aircraft stopped between two tiles is never retargeted") {
		AddModularTile(st, hangar, APT_DEPOT_SE, 0);
		Aircraft *v = departing_aircraft();
		v->current_order.MakeGoToDepot(st->index, OrderDepotTypeFlag::PartOfOrders);
		/* Aircraft::tile still reports the stand it left, but the aircraft is half a tile
		 * past it, so the stand's safe-stop status says nothing about where it is. */
		v->x_pos += TILE_SIZE / 2;

		CHECK_FALSE(MaybeSwitchModularDepartureToHangar(v, st));
		CHECK(v->modular_ground_target == MGT_RUNWAY_TAKEOFF);
	}

	SECTION("A terminal target is left to the arrival handler") {
		AddModularTile(st, hangar, APT_DEPOT_SE, 0);
		Aircraft *v = departing_aircraft();
		v->modular_ground_target = MGT_TERMINAL;
		v->ground_path_goal = base + TileDiffXY(3, 2);
		v->current_order.MakeGoToDepot(st->index, OrderDepotTypeFlag::PartOfOrders);

		/* Terminal targets are cleared on arrival, so they re-read the order by
		 * themselves and must not be churned part-way there. */
		CHECK_FALSE(MaybeSwitchModularDepartureToHangar(v, st));
		CHECK(v->modular_ground_target == MGT_TERMINAL);
	}

	SECTION("An airport with no hangar keeps departing") {
		Aircraft *v = departing_aircraft();
		v->current_order.MakeGoToDepot(st->index, OrderDepotTypeFlag::PartOfOrders);

		/* Nothing to switch to. Clearing the target here would let the departure ladder
		 * re-latch takeoff on the next tick, churning the taxi path forever. */
		REQUIRE_FALSE(st->airport.HasHangar());
		CHECK_FALSE(MaybeSwitchModularDepartureToHangar(v, st));
		CHECK(v->modular_ground_target == MGT_RUNWAY_TAKEOFF);
		CHECK(v->ground_path_goal == runway_end);
	}

	SECTION("A hangar with no ground route to it keeps departing") {
		/* Hangar walled off from the stand row: the aircraft cannot taxi to it, so the
		 * departure is still the only way it can leave. */
		AddModularTile(st, base + TileDiffXY(8, 8), APT_DEPOT_SE, 0);
		Aircraft *v = departing_aircraft();
		v->current_order.MakeGoToDepot(st->index, OrderDepotTypeFlag::PartOfOrders);

		REQUIRE(st->airport.HasHangar());
		CHECK_FALSE(MaybeSwitchModularDepartureToHangar(v, st));
		CHECK(v->modular_ground_target == MGT_RUNWAY_TAKEOFF);
	}
}

TEST_CASE("ModularAirportTemplatePlacementWireRoundTrip")
{
	/* Placement tile fields are bit-packed to keep a full-size layout inside one
	 * command payload. A field that did not survive the round trip would make a
	 * network client build a different airport than the server. */
	SECTION("every field survives the round trip")
	{
		ModularTemplatePlacementTile src{};
		src.dx = 63;
		src.dy = 42;
		src.piece_type = APT_MODULAR_APPROACH_LIGHTS;
		src.rotation = 3;
		src.runway_flags = RUF_LANDING | RUF_TAKEOFF | RUF_DIR_HIGH;
		src.one_way_taxi = true;
		src.user_taxi_dir_mask = 0x0A;
		src.edge_block_mask = 0x05;

		std::vector<uint8_t> buffer;
		EndianBufferWriter writer(buffer);
		writer << src;

		ModularTemplatePlacementTile dst{};
		EndianBufferReader reader(buffer);
		reader >> dst;

		CHECK(buffer.size() == 6);
		CHECK(dst.dx == src.dx);
		CHECK(dst.dy == src.dy);
		CHECK(dst.piece_type == src.piece_type);
		CHECK(dst.rotation == src.rotation);
		CHECK(dst.runway_flags == src.runway_flags);
		CHECK(dst.one_way_taxi == src.one_way_taxi);
		CHECK(dst.user_taxi_dir_mask == src.user_taxi_dir_mask);
		CHECK(dst.edge_block_mask == src.edge_block_mask);
	}

	SECTION("packed fields do not bleed into each other")
	{
		/* Set each packed field to its maximum in turn and check the others stay clear. */
		for (int field = 0; field < 5; field++) {
			ModularTemplatePlacementTile src{};
			src.rotation = (field == 0) ? 3 : 0;
			src.runway_flags = (field == 1) ? 0x0F : 0;
			src.one_way_taxi = (field == 2);
			src.user_taxi_dir_mask = (field == 3) ? 0x0F : 0;
			src.edge_block_mask = (field == 4) ? 0x0F : 0;

			std::vector<uint8_t> buffer;
			EndianBufferWriter writer(buffer);
			writer << src;

			ModularTemplatePlacementTile dst{};
			EndianBufferReader reader(buffer);
			reader >> dst;

			CHECK(dst.rotation == src.rotation);
			CHECK(dst.runway_flags == src.runway_flags);
			CHECK(dst.one_way_taxi == src.one_way_taxi);
			CHECK(dst.user_taxi_dir_mask == src.user_taxi_dir_mask);
			CHECK(dst.edge_block_mask == src.edge_block_mask);
		}
	}

	SECTION("a full-size layout fits in one command payload")
	{
		ModularTemplatePlacementData data;
		data.width = 64;
		data.height = 64;
		data.rotation = 0;
		data.is_drag_build = true;
		for (uint16_t i = 0; i < MAX_TEMPLATE_TILES; i++) {
			ModularTemplatePlacementTile t{};
			t.dx = static_cast<uint8_t>(i % 64);
			t.dy = static_cast<uint8_t>(i / 64);
			t.piece_type = APT_APRON;
			data.tiles.push_back(t);
		}
		REQUIRE(data.tiles.size() == 64 * 64);

		std::vector<uint8_t> buffer;
		EndianBufferWriter writer(buffer);
		writer << data;

		CHECK(buffer.size() <= MAX_COMMAND_PAYLOAD_SIZE);

		ModularTemplatePlacementData back;
		EndianBufferReader reader(buffer);
		reader >> back;
		REQUIRE(back.tiles.size() == data.tiles.size());
		CHECK(back.width == data.width);
		CHECK(back.height == data.height);
		CHECK(back.is_drag_build == data.is_drag_build);
		CHECK(back.tiles.back().dx == data.tiles.back().dx);
		CHECK(back.tiles.back().dy == data.tiles.back().dy);
	}
}

TEST_CASE("ModularAirportRebuiltInPlaceKeepsItsStation")
{
	MockEnvironment::Instance();
	static LanguageMetadata test_language;
	const std::filesystem::path language_file = std::filesystem::exists("build/lang/english.lng") ?
			"build/lang/english.lng" : "lang/english.lng";
	test_language.file = std::filesystem::absolute(language_file);
	REQUIRE(ReadLanguagePack(&test_language));

	const CompanyID saved_company = _current_company;
	const bool saved_distant_join = _settings_game.station.distant_join_stations;
	const bool saved_never_expire = _settings_game.station.never_expire_airports;
	const uint8_t saved_station_spread = _settings_game.station.station_spread;
	const bool saved_noise = _settings_game.economy.station_noise_level;
	const uint8_t saved_tolerance = _settings_game.difficulty.town_council_tolerance;
	const TimerGameCalendar::Year saved_year = TimerGameCalendar::year;

	_settings_game.station.distant_join_stations = true;
	_settings_game.station.never_expire_airports = true;
	_settings_game.station.station_spread = 64;
	_settings_game.economy.station_noise_level = false;
	_settings_game.difficulty.town_council_tolerance = TOWN_COUNCIL_PERMISSIVE;
	TimerGameCalendar::year = TimerGameCalendar::Year{2100};

	/* Demolishing an airport leaves its station behind with a grey sign, and rebuilding on
	 * the same ground is meant to pick it back up -- keeping the name, the index and every
	 * order that names it. */
	auto run = [](uint16_t w, uint16_t h, bool distant_join) {
		CAPTURE(w, h, distant_join);
		_settings_game.station.distant_join_stations = distant_join;
		Map::Allocate(64, 64);
		_station_pool.CleanPool();
		_town_pool.CleanPool();
		_company_pool.CleanPool();
		RebuildStationKdtree();
		RebuildViewportKdtree();
		AirportSpec::ResetAirports();

		Company *company = Company::CreateAtIndex(CompanyID(0));
		REQUIRE(company != nullptr);
		company->money = INT64_MAX;
		company->clear_limit = UINT32_MAX;
		_current_company = company->index;

		Town *town = Town::CreateAtIndex(TownID(0), TileXY(32, 32));
		REQUIRE(town != nullptr);
		town->cache.population = 10000;
		RebuildTownKdtree();

		ModularTemplatePlacementData data;
		data.width = w;
		data.height = h;
		data.rotation = 0;
		for (uint16_t dy = 0; dy < h; dy++) {
			for (uint16_t dx = 0; dx < w; dx++) {
				ModularTemplatePlacementTile t{};
				t.dx = static_cast<uint8_t>(dx);
				t.dy = static_cast<uint8_t>(dy);
				t.piece_type = APT_APRON;
				t.user_taxi_dir_mask = 0x0F;
				data.tiles.push_back(t);
			}
		}

		const TileIndex base = TileXY(4, 4);
		/* StationID::Invalid() is what a plain (non-ctrl) click passes: join whatever is
		 * adjacent, and reuse a nearby deleted station. NEW_STATION would mean the
		 * opposite -- the player explicitly asked for a separate station. */
		CommandCost built = CmdPlaceModularAirportTemplate(DoCommandFlag::Execute, base, StationID::Invalid(), false, data);
		CAPTURE(built.GetErrorMessage(), built.GetExtraErrorMessage());
		REQUIRE(built.Succeeded());

		Station *st = Station::GetByTile(base);
		REQUIRE(st != nullptr);
		const StationID old_id = st->index;
		st->name = "Testport";

		/* Demolish the whole thing, tile by tile in map order -- what a dragged clear does. */
		for (uint16_t dy = 0; dy < h; dy++) {
			for (uint16_t dx = 0; dx < w; dx++) {
				REQUIRE(RemoveModularAirportTile(TileAddXY(base, dx, dy), DoCommandFlag::Execute).Succeeded());
			}
		}
		REQUIRE(Station::IsValidID(old_id));
		REQUIRE(!Station::Get(old_id)->IsInUse());

		CommandCost rebuilt = CmdPlaceModularAirportTemplate(DoCommandFlag::Execute, base, StationID::Invalid(), false, data);
		CAPTURE(rebuilt.GetErrorMessage(), rebuilt.GetExtraErrorMessage());
		REQUIRE(rebuilt.Succeeded());

		Station *st2 = Station::GetByTile(base);
		REQUIRE(st2 != nullptr);
		CHECK(st2->index == old_id);
		CHECK(st2->name == "Testport");
	};

	SECTION("small airport") { run(3, 3, true); }
	SECTION("airport wider than the reuse radius") { run(8, 6, true); }
	/* Without distant join the placement has to grow outwards from a seed tile, and a
	 * station being taken back over has no tiles to grow from. */
	SECTION("small airport, no distant join") { run(3, 3, false); }
	SECTION("wide airport, no distant join") { run(8, 6, false); }

	_current_company = saved_company;
	_settings_game.station.distant_join_stations = saved_distant_join;
	_settings_game.station.never_expire_airports = saved_never_expire;
	_settings_game.station.station_spread = saved_station_spread;
	_settings_game.economy.station_noise_level = saved_noise;
	_settings_game.difficulty.town_council_tolerance = saved_tolerance;
	TimerGameCalendar::year = saved_year;

	_station_pool.CleanPool();
	_town_pool.CleanPool();
	_company_pool.CleanPool();
	RebuildStationKdtree();
	RebuildTownKdtree();
	RebuildViewportKdtree();
}
