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
#include <vector>

struct Town;
class AirportTileTableIterator;

enum StationNaming : uint8_t {
	STATIONNAMING_RAIL,
	STATIONNAMING_ROAD,
	STATIONNAMING_AIRPORT,
	STATIONNAMING_OILRIG,
	STATIONNAMING_DOCK,
	STATIONNAMING_HELIPORT,
};

CommandCost GetStationAroundModular(TileArea ta, StationID closest_station, CompanyID company, struct Station **st);
CommandCost CheckBuildableTile(TileIndex tile, DiagDirections invalid_dirs, int &allowed_z, bool allow_steep, bool check_bridge = true);
CommandCost FindJoiningStation(StationID existing_station, StationID station_to_join, bool adjacent, TileArea ta, Station **st);
CommandCost BuildStationPart(Station **st, DoCommandFlags flags, bool reuse, TileArea area, StationNaming name_class);
CommandCost CheckFlatLandAirport(AirportTileTableIterator tile_iter, DoCommandFlags flags);

enum StationClassID : uint16_t;
enum RoadStopClassID : uint16_t;

struct ModularTemplatePlacementTile {
	uint16_t dx = 0;
	uint16_t dy = 0;
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
extern uint8_t GetAirportNoiseLevelForDistance(const struct AirportSpec *as, uint distance);

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

DEF_CMD_TRAIT(CMD_BUILD_AIRPORT,            CmdBuildAirport,          CommandFlags({CommandFlag::Auto, CommandFlag::NoWater}), CommandType::LandscapeConstruction)
DEF_CMD_TRAIT(CMD_BUILD_MODULAR_AIRPORT_TILE, CmdBuildModularAirportTile, CommandFlags({CommandFlag::Auto, CommandFlag::NoWater}), CommandType::LandscapeConstruction)
DEF_CMD_TRAIT(CMD_SET_RUNWAY_FLAGS,          CmdSetRunwayFlags,          {}, CommandType::LandscapeConstruction)
DEF_CMD_TRAIT(CMD_SET_TAXIWAY_FLAGS,         CmdSetTaxiwayFlags,         {}, CommandType::LandscapeConstruction)
DEF_CMD_TRAIT(CMD_BUILD_MODULAR_AIRPORT_FROM_STOCK, CmdBuildModularAirportFromStock, CommandFlags({CommandFlag::Auto, CommandFlag::NoWater}), CommandType::LandscapeConstruction)
DEF_CMD_TRAIT(CMD_SET_MODULAR_AIRPORT_EDGE_FENCE, CmdSetModularAirportEdgeFence, {}, CommandType::LandscapeConstruction)
DEF_CMD_TRAIT(CMD_PLACE_MODULAR_AIRPORT_TEMPLATE, CmdPlaceModularAirportTemplate, CommandFlags({CommandFlag::Auto, CommandFlag::NoTest, CommandFlag::NoWater}), CommandType::LandscapeConstruction)
DEF_CMD_TRAIT(CMD_UPGRADE_MODULAR_AIRPORT_TILE, CmdUpgradeModularAirportTile, {}, CommandType::LandscapeConstruction)
DEF_CMD_TRAIT(CMD_BUILD_DOCK,               CmdBuildDock,             CommandFlag::Auto,                CommandType::LandscapeConstruction)
DEF_CMD_TRAIT(CMD_BUILD_RAIL_STATION,       CmdBuildRailStation,      CommandFlags({CommandFlag::Auto, CommandFlag::NoWater}), CommandType::LandscapeConstruction)
DEF_CMD_TRAIT(CMD_REMOVE_FROM_RAIL_STATION, CmdRemoveFromRailStation, {},                       CommandType::LandscapeConstruction)
DEF_CMD_TRAIT(CMD_BUILD_ROAD_STOP,          CmdBuildRoadStop,         CommandFlags({CommandFlag::Auto, CommandFlag::NoWater}), CommandType::LandscapeConstruction)
DEF_CMD_TRAIT(CMD_REMOVE_ROAD_STOP,         CmdRemoveRoadStop,        {},                       CommandType::LandscapeConstruction)
DEF_CMD_TRAIT(CMD_RENAME_STATION,           CmdRenameStation,         {},                       CommandType::OtherManagement)
DEF_CMD_TRAIT(CMD_MOVE_STATION_NAME,        CmdMoveStationName,       {},                       CommandType::OtherManagement)
DEF_CMD_TRAIT(CMD_OPEN_CLOSE_AIRPORT,       CmdOpenCloseAirport,      {},                       CommandType::RouteManagement)

void CcMoveStationName(Commands cmd, const CommandCost &result, StationID station_id);

template <typename Tcont, typename Titer>
inline EndianBufferWriter<Tcont, Titer> &operator <<(EndianBufferWriter<Tcont, Titer> &buffer, const ModularTemplatePlacementTile &tile)
{
	return buffer << tile.dx << tile.dy << tile.piece_type << tile.rotation
	              << tile.runway_flags << tile.one_way_taxi << tile.user_taxi_dir_mask << tile.edge_block_mask;
}

inline EndianBufferReader &operator >>(EndianBufferReader &buffer, ModularTemplatePlacementTile &tile)
{
	return buffer >> tile.dx >> tile.dy >> tile.piece_type >> tile.rotation
	              >> tile.runway_flags >> tile.one_way_taxi >> tile.user_taxi_dir_mask >> tile.edge_block_mask;
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
