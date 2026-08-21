/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file station_cmd.h Command definitions related to stations. */

#ifndef STATION_CMD_H
#define STATION_CMD_H

#include "command_type.h"
#include "misc/endian_buffer.hpp"
#include "rail_type.h"
#include "road_type.h"
#include "station_type.h"
#include "newgrf_roadstop.h"
#include "newgrf_station.h"

struct Town;
class AirportTileTableIterator;

/** Station types a station could be named after. */
enum class StationNaming : uint8_t {
	Rail, ///< Railway station.
	Road, ///< Truck or bus stop.
	Airport, ///< Airport for fixed wing aircraft.
	Oilrig, ///< Heliport of an oilrig.
	Dock, ///< Ship dock.
	Heliport, ///< Standalone heliport.
};

CommandCost GetStationAroundModular(TileArea ta, StationID closest_station, CompanyID company, struct Station **st);
CommandCost CheckBuildableTile(TileIndex tile, DiagDirections invalid_dirs, int &allowed_z, bool allow_steep, bool check_bridge = true);
CommandCost FindJoiningStation(StationID existing_station, StationID station_to_join, bool adjacent, TileArea ta, Station **st);
CommandCost BuildStationPart(Station **st, DoCommandFlags flags, bool reuse, TileArea area, StationNaming name_class);
Station *GetClosestDeletedStationForArea(const TileArea &area);
CommandCost CheckFlatLandAirport(AirportTileTableIterator tile_iter, DoCommandFlags flags);

struct ModularTemplatePlacementTile {
	/* One byte each: these are offsets inside the layout's bounding box, and a
	 * station's bounding box is limited to station_spread, whose own maximum is
	 * far below 256. Keeping them narrow is what lets a full 64x64 layout fit in
	 * a single command payload; see modular_airport_template_cmd.cpp. */
	uint8_t dx = 0;
	uint8_t dy = 0;
	uint8_t piece_type = 0;
	uint8_t rotation = 0;
	uint8_t runway_flags = 0;
	bool one_way_taxi = false;
	uint8_t user_taxi_dir_mask = 0x0F;
	uint8_t edge_block_mask = 0;
};

struct ModularTemplatePlacementData {
	uint16_t width = 0;
	uint16_t height = 0;
	uint8_t rotation = 0; // 0..3, clockwise
	std::vector<ModularTemplatePlacementTile> tiles;
};

extern Town *AirportGetNearestTown(const struct AirportSpec *as, Direction rotation, TileIndex tile, TileIterator &&it, uint &mindist);
extern Town *AirportGetNearestTown(const struct Station *st, uint &mindist);
extern Town *AirportGetNearestTown(std::span<const TileIndex> tiles, uint &mindist);
extern uint8_t GetAirportNoiseLevelForDistance(uint8_t noise_level, uint distance);
extern uint8_t GetAirportNoiseLevelForDistance(const struct AirportSpec *as, uint distance);
extern uint8_t GetAirportNoiseLevelForDistance(const struct Station *st, uint distance);

CommandCost CmdBuildAirport(DoCommandFlags flags, TileIndex tile, uint8_t airport_type, uint8_t layout, StationID station_to_join, bool allow_adjacent);
CommandCost CmdBuildModularAirportTile(DoCommandFlags flags, TileIndex tile, uint16_t gfx, StationID station_to_join, bool allow_adjacent, uint8_t rotation, uint8_t taxi_dir_mask, bool one_way_taxi, bool auto_rotate_runway);
CommandCost CmdSetRunwayFlags(DoCommandFlags flags, TileIndex tile, uint8_t runway_flags);
CommandCost CmdSetTaxiwayFlags(DoCommandFlags flags, TileIndex tile, uint8_t taxi_dir_mask, bool one_way_taxi);
CommandCost CmdBuildModularAirportFromStock(DoCommandFlags flags, TileIndex tile, uint8_t airport_type, uint8_t layout, StationID station_to_join, bool allow_adjacent);
CommandCost CmdSetModularAirportEdgeFence(DoCommandFlags flags, TileIndex tile, uint8_t edge_bit, bool set);
CommandCost CmdPlaceModularAirportTemplate(DoCommandFlags flags, TileIndex tile, StationID station_to_join, bool allow_adjacent, const ModularTemplatePlacementData &data);
CommandCost CmdUpgradeModularAirportTile(DoCommandFlags flags, TileIndex tile, TileIndex area_start);
CommandCost CmdBuildDock(DoCommandFlags flags, TileIndex tile, StationID station_to_join, bool adjacent);
CommandCost CmdBuildRailStation(DoCommandFlags flags, TileIndex tile_org, RailType rt, Axis axis, uint8_t numtracks, uint8_t plat_len, StationClassID spec_class, uint16_t spec_index, StationID station_to_join, bool adjacent);
CommandCost CmdRemoveFromRailStation(DoCommandFlags flags, TileIndex start, TileIndex end, bool keep_rail);
CommandCost CmdBuildRoadStop(DoCommandFlags flags, TileIndex tile, uint8_t width, uint8_t length, RoadStopType stop_type, bool is_drive_through, DiagDirection ddir, RoadType rt, RoadStopClassID spec_class, uint16_t spec_index, StationID station_to_join, bool adjacent);
CommandCost CmdRemoveRoadStop(DoCommandFlags flags, TileIndex tile, uint8_t width, uint8_t height, RoadStopType stop_type, bool remove_road);
CommandCost CmdRenameStation(DoCommandFlags flags, StationID station_id, const std::string &text);
std::tuple<CommandCost, StationID> CmdMoveStationName(DoCommandFlags flags, StationID station_id, TileIndex tile);
CommandCost CmdOpenCloseAirport(DoCommandFlags flags, StationID station_id);

DEF_CMD_TRAIT(Commands::BuildAirport, CmdBuildAirport, CommandFlags({CommandFlag::Auto, CommandFlag::NoWater}), CommandType::LandscapeConstruction)
DEF_CMD_TRAIT(Commands::BuildModularAirportTile, CmdBuildModularAirportTile, CommandFlags({CommandFlag::Auto, CommandFlag::NoWater}), CommandType::LandscapeConstruction)
DEF_CMD_TRAIT(Commands::SetRunwayFlags, CmdSetRunwayFlags, {}, CommandType::LandscapeConstruction)
DEF_CMD_TRAIT(Commands::SetTaxiwayFlags, CmdSetTaxiwayFlags, {}, CommandType::LandscapeConstruction)
DEF_CMD_TRAIT(Commands::BuildModularAirportFromStock, CmdBuildModularAirportFromStock, CommandFlags({CommandFlag::Auto, CommandFlag::NoWater}), CommandType::LandscapeConstruction)
DEF_CMD_TRAIT(Commands::SetModularAirportEdgeFence, CmdSetModularAirportEdgeFence, {}, CommandType::LandscapeConstruction)
DEF_CMD_TRAIT(Commands::PlaceModularAirportTemplate, CmdPlaceModularAirportTemplate, CommandFlags({CommandFlag::Auto, CommandFlag::NoTest, CommandFlag::NoWater}), CommandType::LandscapeConstruction)
DEF_CMD_TRAIT(Commands::UpgradeModularAirportTile, CmdUpgradeModularAirportTile, {}, CommandType::LandscapeConstruction)
DEF_CMD_TRAIT(Commands::BuildDock, CmdBuildDock, CommandFlag::Auto, CommandType::LandscapeConstruction)
DEF_CMD_TRAIT(Commands::BuildRailStation, CmdBuildRailStation, CommandFlags({CommandFlag::Auto, CommandFlag::NoWater}), CommandType::LandscapeConstruction)
DEF_CMD_TRAIT(Commands::RemoveFromRailStation, CmdRemoveFromRailStation, {}, CommandType::LandscapeConstruction)
DEF_CMD_TRAIT(Commands::BuildRoadStop, CmdBuildRoadStop, CommandFlags({CommandFlag::Auto, CommandFlag::NoWater}), CommandType::LandscapeConstruction)
DEF_CMD_TRAIT(Commands::RemoveRoadStop, CmdRemoveRoadStop, {}, CommandType::LandscapeConstruction)
DEF_CMD_TRAIT(Commands::RenameStation, CmdRenameStation, {}, CommandType::OtherManagement)
DEF_CMD_TRAIT(Commands::MoveStationName, CmdMoveStationName, {}, CommandType::OtherManagement)
DEF_CMD_TRAIT(Commands::OpenCloseAirport, CmdOpenCloseAirport, {}, CommandType::RouteManagement)

void CcMoveStationName(Commands, const CommandCost &result, StationID station_id);

/**
 * Bit layout of the two packed flag bytes of a serialised
 * ModularTemplatePlacementTile. Every one of these fields is narrower than a
 * byte, and packing them is what keeps a full 64x64 layout inside a single
 * command payload. CmdPlaceModularAirportTemplate rejects any tile whose fields
 * do not fit these widths, so the packing is lossless: a locally executed
 * command and one that made the round trip over the network see the same tile.
 */
enum ModularTemplatePlacementTilePacking : uint8_t {
	MTPP_ROTATION_SHIFT      = 0, ///< Rotation, 0..3.
	MTPP_ROTATION_MASK       = 0x03,
	MTPP_RUNWAY_FLAGS_SHIFT  = 2, ///< RUF_* flags, four bits.
	MTPP_RUNWAY_FLAGS_MASK   = 0x0F,
	MTPP_ONE_WAY_TAXI_BIT    = 6, ///< One-way taxiway, one bit.

	MTPP_TAXI_DIR_MASK_SHIFT = 0, ///< Allowed taxi directions, four bits.
	MTPP_TAXI_DIR_MASK_MASK  = 0x0F,
	MTPP_EDGE_BLOCK_SHIFT    = 4, ///< Fenced edges, four bits.
	MTPP_EDGE_BLOCK_MASK     = 0x0F,
};

template <typename Tcont, typename Titer>
inline EndianBufferWriter<Tcont, Titer> &operator <<(EndianBufferWriter<Tcont, Titer> &buffer, const ModularTemplatePlacementTile &tile)
{
	uint8_t packed_flags = static_cast<uint8_t>(
			((tile.rotation & MTPP_ROTATION_MASK) << MTPP_ROTATION_SHIFT) |
			((tile.runway_flags & MTPP_RUNWAY_FLAGS_MASK) << MTPP_RUNWAY_FLAGS_SHIFT) |
			(tile.one_way_taxi ? (1 << MTPP_ONE_WAY_TAXI_BIT) : 0));
	uint8_t packed_masks = static_cast<uint8_t>(
			((tile.user_taxi_dir_mask & MTPP_TAXI_DIR_MASK_MASK) << MTPP_TAXI_DIR_MASK_SHIFT) |
			((tile.edge_block_mask & MTPP_EDGE_BLOCK_MASK) << MTPP_EDGE_BLOCK_SHIFT));
	return buffer << tile.dx << tile.dy << tile.piece_type << packed_flags << packed_masks;
}

inline EndianBufferReader &operator >>(EndianBufferReader &buffer, ModularTemplatePlacementTile &tile)
{
	uint8_t packed_flags = 0;
	uint8_t packed_masks = 0;
	buffer >> tile.dx >> tile.dy >> tile.piece_type >> packed_flags >> packed_masks;

	tile.rotation = (packed_flags >> MTPP_ROTATION_SHIFT) & MTPP_ROTATION_MASK;
	tile.runway_flags = (packed_flags >> MTPP_RUNWAY_FLAGS_SHIFT) & MTPP_RUNWAY_FLAGS_MASK;
	tile.one_way_taxi = HasBit(packed_flags, MTPP_ONE_WAY_TAXI_BIT);
	tile.user_taxi_dir_mask = (packed_masks >> MTPP_TAXI_DIR_MASK_SHIFT) & MTPP_TAXI_DIR_MASK_MASK;
	tile.edge_block_mask = (packed_masks >> MTPP_EDGE_BLOCK_SHIFT) & MTPP_EDGE_BLOCK_MASK;
	return buffer;
}

template <typename Tcont, typename Titer>
inline EndianBufferWriter<Tcont, Titer> &operator <<(EndianBufferWriter<Tcont, Titer> &buffer, const ModularTemplatePlacementData &data)
{
	buffer << data.width << data.height << data.rotation;
	uint16_t count = ClampTo<uint16_t>(data.tiles.size());
	buffer << count;
	for (uint16_t i = 0; i < count; i++) buffer << data.tiles[i];
	return buffer;
}

inline EndianBufferReader &operator >>(EndianBufferReader &buffer, ModularTemplatePlacementData &data)
{
	buffer >> data.width >> data.height >> data.rotation;
	uint16_t count = 0;
	buffer >> count;
	data.tiles.resize(count);
	for (uint16_t i = 0; i < count; i++) buffer >> data.tiles[i];
	return buffer;
}

#endif /* STATION_CMD_H */
