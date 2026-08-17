/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file script_airport.cpp Implementation of ScriptAirport. */

#include "../../stdafx.h"
#include "script_airport.hpp"
#include "script_station.hpp"
#include "../../station_base.h"
#include "../../station_func.h"
#include "../../town.h"
#include "../../landscape_cmd.h"
#include "../../station_cmd.h"
#include "../../modular_airport_cmd.h"
/* For the builder's own piece vocabulary, which this API is held equal to. */
#include "../../modular_airport_gui.h"
#include "../../table/airporttile_ids.h"
#include "../../timer/timer_game_calendar.h"

#include "../../safeguards.h"

/* static */ bool ScriptAirport::IsValidAirportType(AirportType type)
{
	return IsAirportInformationAvailable(type) && ::AirportSpec::Get(type)->IsAvailable();
}

/* static */ bool ScriptAirport::IsAirportInformationAvailable(AirportType type)
{
	return type >= 0 && type < (AirportType)NUM_AIRPORTS && AirportSpec::Get(type)->enabled;
}

/* static */ Money ScriptAirport::GetPrice(AirportType type)
{
	if (!IsValidAirportType(type)) return -1;

	const AirportSpec *as = ::AirportSpec::Get(type);
	return _price[Price::BuildStationAirport] * as->size_x * as->size_y;
}

/* static */ bool ScriptAirport::IsHangarTile(TileIndex tile)
{
	if (!::IsValidTile(tile)) return false;

	return ::IsTileType(tile, TileType::Station) && ::IsHangar(tile);
}

/* static */ bool ScriptAirport::IsAirportTile(TileIndex tile)
{
	if (!::IsValidTile(tile)) return false;

	return ::IsTileType(tile, TileType::Station) && ::IsAirport(tile);
}

/* static */ SQInteger ScriptAirport::GetAirportWidth(AirportType type)
{
	if (!IsAirportInformationAvailable(type)) return -1;

	return ::AirportSpec::Get(type)->size_x;
}

/* static */ SQInteger ScriptAirport::GetAirportHeight(AirportType type)
{
	if (!IsAirportInformationAvailable(type)) return -1;

	return ::AirportSpec::Get(type)->size_y;
}

/* static */ SQInteger ScriptAirport::GetAirportCoverageRadius(AirportType type)
{
	if (!IsAirportInformationAvailable(type)) return -1;

	return _settings_game.station.modified_catchment ? ::AirportSpec::Get(type)->catchment : (uint)CA_UNMODIFIED;
}

/* static */ bool ScriptAirport::BuildAirport(TileIndex tile, AirportType type, StationID station_id)
{
	EnforceCompanyModeValid(false);
	EnforcePrecondition(false, ::IsValidTile(tile));
	EnforcePrecondition(false, IsValidAirportType(type));
	EnforcePrecondition(false, station_id == ScriptStation::STATION_NEW || station_id == ScriptStation::STATION_JOIN_ADJACENT || ScriptStation::IsValidStation(station_id));

	return ScriptObject::Command<CMD_BUILD_AIRPORT>::Do(tile, type, 0, (ScriptStation::IsValidStation(station_id) ? station_id : StationID::Invalid()), station_id != ScriptStation::STATION_JOIN_ADJACENT);
}

/* static */ bool ScriptAirport::RemoveAirport(TileIndex tile)
{
	EnforceCompanyModeValid(false);
	EnforcePrecondition(false, ::IsValidTile(tile))
	EnforcePrecondition(false, IsAirportTile(tile) || IsHangarTile(tile));

	return ScriptObject::Command<CMD_LANDSCAPE_CLEAR>::Do(tile);
}

/* static */ SQInteger ScriptAirport::GetNumHangars(TileIndex tile)
{
	EnforceDeityOrCompanyModeValid(-1);
	if (!::IsValidTile(tile)) return -1;
	if (!::IsTileType(tile, TileType::Station)) return -1;

	const Station *st = ::Station::GetByTile(tile);
	if (st->owner != ScriptObject::GetCompany() && ScriptCompanyMode::IsValid()) return -1;
	if (!st->facilities.Test(StationFacility::Airport)) return -1;

	return st->airport.GetNumHangars();
}

/* static */ TileIndex ScriptAirport::GetHangarOfAirport(TileIndex tile)
{
	EnforceDeityOrCompanyModeValid(INVALID_TILE);
	if (!::IsValidTile(tile)) return INVALID_TILE;
	if (!::IsTileType(tile, TileType::Station)) return INVALID_TILE;
	if (GetNumHangars(tile) < 1) return INVALID_TILE;

	const Station *st = ::Station::GetByTile(tile);
	if (st->owner != ScriptObject::GetCompany() && ScriptCompanyMode::IsValid()) return INVALID_TILE;
	if (!st->facilities.Test(StationFacility::Airport)) return INVALID_TILE;

	return st->airport.GetHangarTile(0);
}

/* static */ ScriptAirport::AirportType ScriptAirport::GetAirportType(TileIndex tile)
{
	if (!ScriptTile::IsStationTile(tile)) return AT_INVALID;

	StationID station_id = ::GetStationIndex(tile);

	if (!ScriptStation::HasStationType(station_id, ScriptStation::STATION_AIRPORT)) return AT_INVALID;

	return (AirportType)::Station::Get(station_id)->airport.type;
}


/* static */ SQInteger ScriptAirport::GetNoiseLevelIncrease(TileIndex tile, AirportType type)
{
	if (!::IsValidTile(tile)) return -1;
	if (!IsAirportInformationAvailable(type)) return -1;

	const AirportSpec *as = ::AirportSpec::Get(type);
	if (!as->IsWithinMapBounds(0, tile)) return -1;

	if (_settings_game.economy.station_noise_level) {
		uint dist;
		const auto &layout = as->layouts[0];
		AirportGetNearestTown(as, layout.rotation, tile, AirportTileTableIterator(layout.tiles, tile), dist);
		return GetAirportNoiseLevelForDistance(as, dist);
	}

	return 1;
}

/* static */ TownID ScriptAirport::GetNearestTown(TileIndex tile, AirportType type)
{
	if (!::IsValidTile(tile)) return TownID::Invalid();
	if (!IsAirportInformationAvailable(type)) return TownID::Invalid();

	const AirportSpec *as = AirportSpec::Get(type);
	if (!as->IsWithinMapBounds(0, tile)) return TownID::Invalid();

	uint dist;
	const auto &layout = as->layouts[0];
	return AirportGetNearestTown(as, layout.rotation, tile, AirportTileTableIterator(layout.tiles, tile), dist)->index;
}

/* static */ SQInteger ScriptAirport::GetMaintenanceCostFactor(AirportType type)
{
	if (!IsAirportInformationAvailable(type)) return 0;

	return AirportSpec::Get(type)->maintenance_cost;
}

/* static */ Money ScriptAirport::GetMonthlyMaintenanceCost(AirportType type)
{
	if (!IsAirportInformationAvailable(type)) return -1;

	return ScaleAirportMaintenanceCost(_price[Price::InfrastructureAirport], GetMaintenanceCostFactor(type) * 8);
}

/* static */ SQInteger ScriptAirport::GetAirportNumHelipads(AirportType type)
{
	if (!IsAirportInformationAvailable(type)) return -1;

	return ::AirportSpec::Get(type)->fsm->num_helipads;
}

/**
 * The graphic each named modular piece places. The table is the single source of
 * truth for both directions of the mapping, so a piece cannot be built as one
 * graphic and read back as another.
 */
static const std::pair<ScriptAirport::ModularPiece, uint8_t> _modular_piece_gfx[] = {
	{ScriptAirport::MP_APRON,                 APT_APRON},
	{ScriptAirport::MP_STAND,                 APT_STAND},
	{ScriptAirport::MP_RUNWAY,                APT_RUNWAY_5},
	{ScriptAirport::MP_RUNWAY_END,            APT_RUNWAY_END},
	{ScriptAirport::MP_RUNWAY_SMALL_MIDDLE,   APT_RUNWAY_SMALL_MIDDLE},
	{ScriptAirport::MP_RUNWAY_SMALL_NEAR_END, APT_RUNWAY_SMALL_NEAR_END},
	{ScriptAirport::MP_RUNWAY_SMALL_FAR_END,  APT_RUNWAY_SMALL_FAR_END},
	{ScriptAirport::MP_HANGAR,                APT_DEPOT_SE},
	{ScriptAirport::MP_SMALL_HANGAR,          APT_SMALL_DEPOT_SE},
	{ScriptAirport::MP_HELIPAD,               APT_HELIPAD_2},
	{ScriptAirport::MP_HELIPAD_PLAIN,         APT_HELIPAD_3_FENCE_NW},
	{ScriptAirport::MP_HELIPORT,              APT_HELIPORT},
	{ScriptAirport::MP_TERMINAL,              APT_BUILDING_1},
	{ScriptAirport::MP_TERMINAL_ALT,          APT_BUILDING_2},
	{ScriptAirport::MP_TERMINAL_OTHER,        APT_BUILDING_3},
	{ScriptAirport::MP_TERMINAL_ROUND,        APT_ROUND_TERMINAL},
	{ScriptAirport::MP_LOW_TERMINAL,          APT_LOW_BUILDING},
	{ScriptAirport::MP_SMALL_TERMINAL_3,      APT_SMALL_BUILDING_2},
	{ScriptAirport::MP_TOWER,                 APT_TOWER},
	{ScriptAirport::MP_RADIO_TOWER,           APT_RADIO_TOWER_FENCE_NE},
	{ScriptAirport::MP_RADAR,                 APT_RADAR_FENCE_NE},
	{ScriptAirport::MP_RADAR_GRASS,           APT_RADAR_GRASS_FENCE_SW},
	{ScriptAirport::MP_FLAG_GRASS,            APT_GRASS_FENCE_NE_FLAG_2},
	{ScriptAirport::MP_GRASS,                 APT_GRASS_1},
	{ScriptAirport::MP_EMPTY,                 APT_EMPTY},
};

/**
 * Get the graphic a named modular piece places.
 *
 * Deliberately not static: the modular airport tests hold this mapping against
 * the builder's own piece tables, which is what keeps a script's vocabulary and
 * a player's identical.
 * @param piece The piece to look up.
 * @return The graphic, or UINT8_MAX when the piece is not a valid ModularPiece.
 */
uint8_t GetGfxForModularPiece(ScriptAirport::ModularPiece piece)
{
	for (const auto &[named, gfx] : _modular_piece_gfx) {
		if (named == piece) return gfx;
	}
	return UINT8_MAX;
}

/**
 * Get the named piece for a graphic actually on the map.
 *
 * Beyond the exact graphics this API places, a modular airport can hold decorative
 * variants (fenced aprons, the other hangar rotations) and the graphics a converted
 * stock airport brings with it — including stands that are not placeable, such as
 * the jetway-bearing pair from the stock city airport. Those are reported as the
 * family they belong to, so a script inspecting an airport it did not build still
 * gets a useful answer, and so a script can never learn a piece name it is not
 * allowed to build.
 *
 * Deliberately not static; see GetGfxForModularPiece above.
 * @param gfx The graphic to look up.
 * @return The piece, or MP_INVALID when nothing sensible names it.
 */
ScriptAirport::ModularPiece GetModularPieceForGfx(uint8_t gfx)
{
	for (const auto &[named, named_gfx] : _modular_piece_gfx) {
		if (named_gfx == gfx) return named;
	}

	/* Not one of the canonical graphics; fall back to the family. */
	if (::IsModularRunwayPiece(gfx)) {
		if (::IsLegacySmallRunwayPiece(gfx)) return ScriptAirport::MP_RUNWAY_SMALL_MIDDLE;
		return gfx == APT_RUNWAY_END ? ScriptAirport::MP_RUNWAY_END : ScriptAirport::MP_RUNWAY;
	}
	if (::IsModularHangarPiece(gfx)) {
		return ::IsLegacySmallHangarPiece(gfx) ? ScriptAirport::MP_SMALL_HANGAR : ScriptAirport::MP_HANGAR;
	}
	if (::IsModularHelipadPiece(gfx)) return ScriptAirport::MP_HELIPAD;
	if (::IsModularStandPiece(gfx)) return ScriptAirport::MP_STAND;
	/* The other two thirds of the small terminal: reading any of its tiles names
	 * the whole piece, which is the only thing a script can build or reason about. */
	if (gfx == APT_SMALL_BUILDING_1 || gfx == APT_SMALL_BUILDING_3) return ScriptAirport::MP_SMALL_TERMINAL_3;
	if (::IsApronOrTaxiwayPiece(gfx)) return ScriptAirport::MP_APRON;

	return ScriptAirport::MP_INVALID;
}

/**
 * Get the modular tile data of a tile, if it has any.
 * @param tile The tile to look up.
 * @return The tile data, or nullptr when the tile is not part of a modular airport.
 */
static const ModularAirportTileData *GetScriptModularTileData(TileIndex tile)
{
	if (!::IsValidTile(tile)) return nullptr;
	if (!::IsTileType(tile, TileType::Station) || !::IsAirport(tile)) return nullptr;

	const Station *st = ::Station::GetByTile(tile);
	if (st == nullptr || !st->airport.blocks.Test(AirportBlock::Modular)) return nullptr;

	return st->airport.GetModularTileData(tile);
}

/**
 * Convert a flat script layout array into the placement tiles the command takes.
 * @param layout The flat array, MLF_STRIDE values per tile.
 * @param[out] tiles The converted tiles.
 * @return True when the layout is well-formed.
 */
static bool ParseModularLayout(const Array<SQInteger> &layout, std::vector<ModularTemplatePlacementTile> &tiles)
{
	if (layout.empty()) return false;
	if ((layout.size() % ScriptAirport::MLF_STRIDE) != 0) return false;

	tiles.clear();
	tiles.reserve(layout.size() / ScriptAirport::MLF_STRIDE);

	for (size_t i = 0; i < layout.size(); i += ScriptAirport::MLF_STRIDE) {
		const SQInteger dx = layout[i + ScriptAirport::MLF_DX];
		const SQInteger dy = layout[i + ScriptAirport::MLF_DY];
		const SQInteger piece = layout[i + ScriptAirport::MLF_PIECE];
		const SQInteger rotation = layout[i + ScriptAirport::MLF_ROTATION];
		const SQInteger runway_flags = layout[i + ScriptAirport::MLF_RUNWAY_FLAGS];
		const SQInteger taxi_dir_mask = layout[i + ScriptAirport::MLF_TAXI_DIR_MASK];
		const SQInteger edge_fence_mask = layout[i + ScriptAirport::MLF_EDGE_FENCE_MASK];

		if (dx < 0 || dx > UINT16_MAX || dy < 0 || dy > UINT16_MAX) return false;
		if (rotation < 0 || rotation > 3) return false;
		if (runway_flags < 0 || runway_flags > 0x0F) return false;
		if (taxi_dir_mask < 0 || taxi_dir_mask > 0x0F) return false;
		if (edge_fence_mask < 0 || edge_fence_mask > 0x0F) return false;

		const uint8_t gfx = GetGfxForModularPiece(static_cast<ScriptAirport::ModularPiece>(piece));
		if (gfx == UINT8_MAX) return false;

		ModularTemplatePlacementTile tile{};
		tile.dx = static_cast<uint16_t>(dx);
		tile.dy = static_cast<uint16_t>(dy);
		tile.piece_type = gfx;
		tile.rotation = static_cast<uint8_t>(rotation);
		tile.runway_flags = ::IsModularRunwayPiece(gfx) ?
				::NormalizeModularRunwayFlags(static_cast<uint8_t>(runway_flags)) :
				static_cast<uint8_t>(runway_flags);
		tile.one_way_taxi = layout[i + ScriptAirport::MLF_ONE_WAY_TAXI] != 0;
		tile.user_taxi_dir_mask = static_cast<uint8_t>(taxi_dir_mask);
		tile.edge_block_mask = static_cast<uint8_t>(edge_fence_mask);

		/* A compound piece is one entry to the script and several tiles on the
		 * ground. Expanding here means every caller — placement, and all the
		 * layout-derived queries that go through this parser — sees the tiles the
		 * layout will really occupy, so a script can cost and size a layout
		 * containing one without knowing its footprint. */
		const std::span<const ModularCompoundPieceTile> compound = GetModularCompoundPieceTiles(gfx);
		if (compound.empty()) {
			tiles.push_back(tile);
			continue;
		}

		/* Each tile of a compound has its own graphic and they join up one way
		 * only, so there is nothing sensible to do with a rotation. */
		if (rotation != 0) return false;
		for (const ModularCompoundPieceTile &ct : compound) {
			ModularTemplatePlacementTile part = tile;
			part.dx = static_cast<uint16_t>(dx + ct.dx);
			part.dy = static_cast<uint16_t>(dy + ct.dy);
			part.piece_type = ct.gfx;
			tiles.push_back(part);
		}
	}

	return true;
}

/**
 * Convert a flat script layout array into the abstract-grid pieces the
 * layout-derived property functions take.
 * @param layout The flat array, MLF_STRIDE values per tile.
 * @param[out] pieces The converted pieces.
 * @return True when the layout is well-formed.
 */
static bool ParseModularLayoutPieces(const Array<SQInteger> &layout, std::vector<ModularCatchmentPiece> &pieces)
{
	std::vector<ModularTemplatePlacementTile> tiles;
	if (!ParseModularLayout(layout, tiles)) return false;

	pieces.clear();
	pieces.reserve(tiles.size());
	for (const ModularTemplatePlacementTile &tile : tiles) {
		pieces.push_back({static_cast<int>(tile.dx), static_cast<int>(tile.dy), tile.piece_type, tile.rotation, tile.runway_flags});
	}
	return true;
}

/* static */ bool ScriptAirport::IsModularAirportTile(TileIndex tile)
{
	return GetScriptModularTileData(tile) != nullptr;
}

/* static */ ScriptAirport::ModularPiece ScriptAirport::GetModularPiece(TileIndex tile)
{
	const ModularAirportTileData *data = GetScriptModularTileData(tile);
	if (data == nullptr) return MP_INVALID;

	return GetModularPieceForGfx(data->piece_type);
}

/* static */ SQInteger ScriptAirport::GetModularPieceRotation(TileIndex tile)
{
	const ModularAirportTileData *data = GetScriptModularTileData(tile);
	if (data == nullptr) return -1;

	return data->rotation;
}

/* static */ SQInteger ScriptAirport::GetModularRunwayFlags(TileIndex tile)
{
	const ModularAirportTileData *data = GetScriptModularTileData(tile);
	if (data == nullptr || !::IsModularRunwayPiece(data->piece_type)) return -1;

	return data->runway_flags;
}

/* static */ bool ScriptAirport::IsModularPieceAvailable(ModularPiece piece)
{
	const uint8_t gfx = GetGfxForModularPiece(piece);
	if (gfx == UINT8_MAX) return false;

	return !::IsModernModularPiece(gfx) || TimerGameCalendar::year >= ::GetModularPieceMinYear(gfx);
}

/* static */ SQInteger ScriptAirport::GetModularPieceMinYear(ModularPiece piece)
{
	const uint8_t gfx = GetGfxForModularPiece(piece);
	if (gfx == UINT8_MAX) return -1;

	return ::GetModularPieceMinYear(gfx).base();
}

/* static */ SQInteger ScriptAirport::GetModularAirportSafety(TileIndex tile)
{
	if (!IsModularAirportTile(tile)) return -1;

	return ::GetModularAirportSafetyStatus(::Station::GetByTile(tile));
}

/* static */ bool ScriptAirport::BuildModularAirportTile(TileIndex tile, ModularPiece piece, SQInteger rotation, StationID station_id)
{
	EnforceCompanyModeValid(false);
	EnforcePrecondition(false, ::IsValidTile(tile));
	EnforcePrecondition(false, rotation >= 0 && rotation <= 3);
	EnforcePrecondition(false, IsModularPieceAvailable(piece));
	EnforcePrecondition(false, station_id == ScriptStation::STATION_NEW || station_id == ScriptStation::STATION_JOIN_ADJACENT || ScriptStation::IsValidStation(station_id));

	const uint8_t gfx = GetGfxForModularPiece(piece);

	/* A compound piece covers several tiles, so it goes through the template
	 * command the same way the builder's own click does — one atomic placement,
	 * never a half-built building. */
	if (const std::span<const ModularCompoundPieceTile> compound = GetModularCompoundPieceTiles(gfx); !compound.empty()) {
		EnforcePrecondition(false, rotation == 0);
		const Dimension size = GetModularCompoundPieceSize(gfx);
		ModularTemplatePlacementData data;
		data.width = static_cast<uint16_t>(size.width);
		data.height = static_cast<uint16_t>(size.height);
		data.rotation = 0;
		for (const ModularCompoundPieceTile &ct : compound) {
			data.tiles.push_back({static_cast<uint16_t>(ct.dx), static_cast<uint16_t>(ct.dy), ct.gfx, 0, 0, false, 0x0F, 0});
		}
		return ScriptObject::Command<CMD_PLACE_MODULAR_AIRPORT_TEMPLATE>::Do(tile,
				(ScriptStation::IsValidStation(station_id) ? station_id : StationID::Invalid()),
				station_id != ScriptStation::STATION_JOIN_ADJACENT, data);
	}

	/* Taxi directions and one-way are left at their defaults; SetModularTaxiwayFlags
	 * changes them afterwards. Runway orientation comes from the rotation the caller
	 * gave rather than from the neighbouring tiles, so a script gets what it asked for. */
	return ScriptObject::Command<CMD_BUILD_MODULAR_AIRPORT_TILE>::Do(tile, gfx,
			(ScriptStation::IsValidStation(station_id) ? station_id : StationID::Invalid()),
			station_id != ScriptStation::STATION_JOIN_ADJACENT, static_cast<uint8_t>(rotation),
			static_cast<uint8_t>(0x0F), false, false);
}

/* static */ bool ScriptAirport::SetModularRunwayFlags(TileIndex tile, SQInteger flags)
{
	EnforceCompanyModeValid(false);
	EnforcePrecondition(false, IsModularAirportTile(tile));
	EnforcePrecondition(false, flags >= 0 && flags <= (MRF_LANDING | MRF_TAKEOFF | MRF_DIR_LOW | MRF_DIR_HIGH));
	/* Mirrors what the command itself accepts, so a bad combination is reported as
	 * a precondition rather than as an opaque command failure. */
	EnforcePrecondition(false, (flags & (MRF_LANDING | MRF_TAKEOFF)) != 0);
	const SQInteger dir_flags = flags & (MRF_DIR_LOW | MRF_DIR_HIGH);
	EnforcePrecondition(false, dir_flags == MRF_DIR_LOW || dir_flags == MRF_DIR_HIGH);

	return ScriptObject::Command<CMD_SET_RUNWAY_FLAGS>::Do(tile, static_cast<uint8_t>(flags));
}

/* static */ bool ScriptAirport::SetModularTaxiwayFlags(TileIndex tile, SQInteger dir_mask, bool one_way)
{
	EnforceCompanyModeValid(false);
	EnforcePrecondition(false, IsModularAirportTile(tile));
	EnforcePrecondition(false, dir_mask >= 0 && dir_mask <= 0x0F);
	/* A one-way taxiway runs in one direction, so exactly one bit is meaningful. */
	EnforcePrecondition(false, !one_way || HasExactlyOneBit(static_cast<uint8_t>(dir_mask)));

	return ScriptObject::Command<CMD_SET_TAXIWAY_FLAGS>::Do(tile, static_cast<uint8_t>(dir_mask), one_way);
}

/* static */ bool ScriptAirport::PlaceModularAirportLayout(TileIndex tile, StationID station_id, SQInteger rotation, SQInteger width, SQInteger height, Array<SQInteger> &&layout)
{
	EnforceCompanyModeValid(false);
	EnforcePrecondition(false, ::IsValidTile(tile));
	EnforcePrecondition(false, rotation >= 0 && rotation <= 3);
	EnforcePrecondition(false, width > 0 && width <= UINT16_MAX);
	EnforcePrecondition(false, height > 0 && height <= UINT16_MAX);
	EnforcePrecondition(false, station_id == ScriptStation::STATION_NEW || station_id == ScriptStation::STATION_JOIN_ADJACENT || ScriptStation::IsValidStation(station_id));

	ModularTemplatePlacementData data;
	EnforcePrecondition(false, ParseModularLayout(layout, data.tiles));
	/* Checked here rather than in the parser: previewing a layout the current year
	 * cannot build yet is a legitimate thing for a script to want. */
	for (const ModularTemplatePlacementTile &t : data.tiles) {
		EnforcePrecondition(false, IsModularPieceAvailable(GetModularPieceForGfx(t.piece_type)));
	}
	data.width = static_cast<uint16_t>(width);
	data.height = static_cast<uint16_t>(height);
	data.rotation = static_cast<uint8_t>(rotation);

	return ScriptObject::Command<CMD_PLACE_MODULAR_AIRPORT_TEMPLATE>::Do(tile,
			(ScriptStation::IsValidStation(station_id) ? station_id : StationID::Invalid()),
			station_id != ScriptStation::STATION_JOIN_ADJACENT, data);
}

/* static */ SQInteger ScriptAirport::GetModularLayoutNoiseLevel(Array<SQInteger> &&layout)
{
	std::vector<ModularTemplatePlacementTile> tiles;
	if (!ParseModularLayout(layout, tiles)) return -1;

	std::vector<uint8_t> piece_types;
	piece_types.reserve(tiles.size());
	for (const ModularTemplatePlacementTile &t : tiles) piece_types.push_back(t.piece_type);

	return ::GetModularAirportNoiseLevelFromPieces(piece_types);
}

/* static */ SQInteger ScriptAirport::GetModularLayoutCatchmentRadius(Array<SQInteger> &&layout)
{
	std::vector<ModularCatchmentPiece> pieces;
	if (!ParseModularLayoutPieces(layout, pieces)) return -1;

	return ::GetModularAirportCatchmentRadiusFromPieces(pieces);
}

/* static */ Money ScriptAirport::GetModularLayoutMonthlyMaintenanceCost(Array<SQInteger> &&layout)
{
	std::vector<ModularTemplatePlacementTile> tiles;
	if (!ParseModularLayout(layout, tiles)) return -1;

	std::vector<uint8_t> piece_types;
	piece_types.reserve(tiles.size());
	for (const ModularTemplatePlacementTile &t : tiles) piece_types.push_back(t.piece_type);

	/* Modular maintenance points are already in the eighths that the stock
	 * maintenance factor is scaled into, so this is comparable with
	 * GetMonthlyMaintenanceCost() for a stock airport type. */
	return ScaleAirportMaintenanceCost(_price[Price::InfrastructureAirport], ::GetModularAirportMaintenancePointsFromPieces(piece_types));
}

/* static */ bool ScriptAirport::GetModularLayoutAcceptsPlanes(Array<SQInteger> &&layout)
{
	std::vector<ModularTemplatePlacementTile> tiles;
	if (!ParseModularLayout(layout, tiles)) return false;

	std::vector<ModularAirportCapabilityPiece> pieces;
	pieces.reserve(tiles.size());
	for (const ModularTemplatePlacementTile &t : tiles) pieces.push_back({t.piece_type, t.runway_flags});

	return ::ModularAirportAcceptsPlanesFromPieces(pieces);
}

/* static */ bool ScriptAirport::GetModularLayoutHasHelipad(Array<SQInteger> &&layout)
{
	std::vector<ModularTemplatePlacementTile> tiles;
	if (!ParseModularLayout(layout, tiles)) return false;

	/* Deliberately only the topological question. Whether a helipad-less airport
	 * ends up taking helicopters on its apron depends on the tiles it is built on,
	 * so it cannot be answered from the layout alone. */
	return std::any_of(tiles.begin(), tiles.end(),
			[](const ModularTemplatePlacementTile &t) { return ::IsModularHelipadPiece(t.piece_type); });
}

/* static */ SQInteger ScriptAirport::GetModularLayoutSafety(Array<SQInteger> &&layout)
{
	std::vector<ModularCatchmentPiece> pieces;
	if (!ParseModularLayoutPieces(layout, pieces)) return -1;

	return ::GetModularAirportSafetyStatusFromPieces(pieces);
}
