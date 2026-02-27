/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file modular_airport_template_cmd.cpp Commands for saved modular airport template placement/metadata. */

#include "stdafx.h"

#include "station_cmd.h"
#include "command_func.h"
#include "company_func.h"
#include "modular_airport_cmd.h"
#include "modular_airport_gui.h"
#include "airport_pathfinder.h"
#include "station_base.h"
#include "station_map.h"
#include "tile_map.h"
#include "viewport_func.h"

#include "table/strings.h"

#include <array>

static constexpr uint16_t MAX_TEMPLATE_TILES = 128;
static constexpr std::array<uint8_t, 4> kFenceEdgeBits = {0x01, 0x02, 0x04, 0x08};


static void RotateTemplateTile(ModularTemplatePlacementTile &tile, uint8_t r, uint16_t width, uint16_t height)
{
	r &= 3;
	if (r == 0) return;

	uint16_t ox = tile.dx;
	uint16_t oy = tile.dy;
	uint8_t old_rotation = tile.rotation;

	switch (r) {
		case 1:
			tile.dx = height - 1 - oy;
			tile.dy = ox;
			break;
		case 2:
			tile.dx = width - 1 - ox;
			tile.dy = height - 1 - oy;
			break;
		case 3:
			tile.dx = oy;
			tile.dy = width - 1 - ox;
			break;
		default: NOT_REACHED();
	}

	tile.rotation = (old_rotation + r) & 3;

	SwapBuildingPieceForRotation(tile.piece_type, r);

	auto rotate_mask = [r](uint8_t mask) -> uint8_t {
		uint8_t out = 0;
		for (uint8_t i = 0; i < 4; i++) {
			if ((mask & (1 << i)) != 0) out |= (1 << ((i + r) & 3));
		}
		return out;
	};

	tile.user_taxi_dir_mask = rotate_mask(tile.user_taxi_dir_mask);
	tile.edge_block_mask = rotate_mask(tile.edge_block_mask);

	/* Swap low/high when coordinate order along the original axis reverses. */
	bool original_x_axis = (old_rotation % 2) == 0;
	bool reverse = false;
	if (original_x_axis) {
		reverse = (r == 2 || r == 3);
	} else {
		reverse = (r == 1 || r == 2);
	}
	if (reverse) {
		uint8_t flags = tile.runway_flags;
		uint8_t low = flags & RUF_DIR_LOW;
		uint8_t high = flags & RUF_DIR_HIGH;
		flags &= ~(RUF_DIR_LOW | RUF_DIR_HIGH);
		if (low != 0) flags |= RUF_DIR_HIGH;
		if (high != 0) flags |= RUF_DIR_LOW;
		tile.runway_flags = flags;
	}
}

static void GetRotatedTemplateDimensions(uint16_t width, uint16_t height, uint8_t rotation, uint16_t &out_w, uint16_t &out_h)
{
	if ((rotation & 1) != 0) {
		out_w = height;
		out_h = width;
	} else {
		out_w = width;
		out_h = height;
	}
}

static uint8_t NormalizeTemplateRunwayFlags(uint8_t flags)
{
	const uint8_t mode_bits = flags & (RUF_LANDING | RUF_TAKEOFF);
	const uint8_t dir_bits = flags & (RUF_DIR_LOW | RUF_DIR_HIGH);

	uint8_t normalized = flags;
	if (mode_bits == 0) normalized |= (RUF_LANDING | RUF_TAKEOFF);
	if (dir_bits != RUF_DIR_LOW && dir_bits != RUF_DIR_HIGH) {
		normalized &= ~(RUF_DIR_LOW | RUF_DIR_HIGH);
		normalized |= RUF_DIR_LOW;
	}
	return normalized;
}

CommandCost CmdSetRunwayFlags(DoCommandFlags flags, TileIndex tile, uint8_t runway_flags)
{
	if (!IsValidTile(tile)) return CMD_ERROR;

	/* Validate flags: at least one operation and exactly one direction must be set */
	if ((runway_flags & (RUF_LANDING | RUF_TAKEOFF)) == 0) return CMD_ERROR;
	uint8_t dir_flags = runway_flags & (RUF_DIR_LOW | RUF_DIR_HIGH);
	if (dir_flags != RUF_DIR_LOW && dir_flags != RUF_DIR_HIGH) return CMD_ERROR;

	/* If we're just testing, we might be calling this for a tile that is about to be built. */
	if (!flags.Test(DoCommandFlag::Execute) && (!IsTileType(tile, TileType::Station) || !IsAirport(tile))) {
		return CommandCost();
	}

	if (!IsTileType(tile, TileType::Station)) return CMD_ERROR;
	Station *st = Station::GetByTile(tile);
	if (st == nullptr) return CMD_ERROR;

	CommandCost ret = CheckOwnership(st->owner);
	if (ret.Failed()) return ret;

	if (!st->airport.blocks.Test(AirportBlock::Modular)) return CMD_ERROR;

	ModularAirportTileData *data = st->airport.GetModularTileData(tile);
	if (data == nullptr) return CMD_ERROR;
	if (!IsModularRunwayPiece(data->piece_type)) return CMD_ERROR;

	if (flags.Test(DoCommandFlag::Execute)) {
		/* Set flags on this tile and all contiguous runway tiles */
		bool horizontal = (data->rotation % 2) == 0;
		TileIndexDiff diff = horizontal ? TileDiffXY(1, 0) : TileDiffXY(0, 1);

		/* Walk to the low end first */
		TileIndex first = tile;
		while (true) {
			TileIndex prev = first - diff;
			ModularAirportTileData *prev_data = st->airport.GetModularTileData(prev);
			if (!IsRunwayPieceOnAxis(prev_data, horizontal)) break;
			first = prev;
		}

		/* Walk from low end to high end, setting flags on each tile */
		TileIndex current = first;
		while (true) {
			ModularAirportTileData *cur_data = st->airport.GetModularTileData(current);
			if (!IsRunwayPieceOnAxis(cur_data, horizontal)) break;

			cur_data->runway_flags = runway_flags;
			MarkTileDirtyByTile(current);

			TileIndex next = current + diff;
			if (next == current) break; /* Shouldn't happen, but safety */
			current = next;
		}
		st->airport.modular_holding_loop_dirty = true;
		if (_show_holding_overlay) MarkWholeScreenDirty();
	}

	return CommandCost();
}

CommandCost CmdSetTaxiwayFlags(DoCommandFlags flags, TileIndex tile, uint8_t taxi_dir_mask, bool one_way_taxi)
{
	if (!IsValidTile(tile)) return CMD_ERROR;

	/* If we're just testing, we might be calling this for a tile that is about to be built. */
	if (!flags.Test(DoCommandFlag::Execute) && (!IsTileType(tile, TileType::Station) || !IsAirport(tile))) {
		return CommandCost();
	}

	if (!IsTileType(tile, TileType::Station)) return CMD_ERROR;

	Station *st = Station::GetByTile(tile);
	if (st == nullptr) return CMD_ERROR;

	CommandCost ret = CheckOwnership(st->owner);
	if (ret.Failed()) return ret;

	if (!st->airport.blocks.Test(AirportBlock::Modular)) return CMD_ERROR;

	ModularAirportTileData *data = st->airport.GetModularTileData(tile);
	if (data == nullptr || !IsTaxiwayPiece(data->piece_type)) return CMD_ERROR;

	const uint8_t auto_dirs = CalculateAutoTaxiDirectionsForGfx(data->piece_type, data->rotation);
	taxi_dir_mask &= 0x0F;

	if (one_way_taxi) {
		if (!HasExactlyOneBit(taxi_dir_mask)) return CMD_ERROR;
		if ((auto_dirs & taxi_dir_mask) == 0) return CMD_ERROR;
	}

	if (flags.Test(DoCommandFlag::Execute)) {
		data->one_way_taxi = one_way_taxi;
		data->user_taxi_dir_mask = one_way_taxi ? taxi_dir_mask : 0x0F;
		MarkTileDirtyByTile(tile);
	}

	return CommandCost();
}

CommandCost CmdSetModularAirportEdgeFence(DoCommandFlags flags, TileIndex tile, uint8_t edge_bit, bool set)
{
	if (!IsValidTile(tile)) return CMD_ERROR;

	/* If we're just testing, we might be calling this for a tile that is about to be built. */
	if (!flags.Test(DoCommandFlag::Execute) && (!IsTileType(tile, TileType::Station) || !IsAirport(tile))) {
		if (edge_bit != 0x01 && edge_bit != 0x02 && edge_bit != 0x04 && edge_bit != 0x08) return CMD_ERROR;
		return CommandCost();
	}

	if (!IsTileType(tile, TileType::Station)) return CMD_ERROR;

	Station *st = Station::GetByTile(tile);
	if (st == nullptr) return CMD_ERROR;

	CommandCost ret = CheckOwnership(st->owner);
	if (ret.Failed()) return ret;

	if (!st->airport.blocks.Test(AirportBlock::Modular)) return CMD_ERROR;

	/* Validate: exactly one edge bit. */
	if (edge_bit != 0x01 && edge_bit != 0x02 && edge_bit != 0x04 && edge_bit != 0x08) return CMD_ERROR;

	ModularAirportTileData *data = st->airport.GetModularTileData(tile);
	if (data == nullptr) return CMD_ERROR;

	if (flags.Test(DoCommandFlag::Execute)) {
		if (set) {
			data->edge_block_mask |= edge_bit;
		} else {
			data->edge_block_mask &= ~edge_bit;
		}
		MarkTileDirtyByTile(tile);

		/* Mirror to the neighbor tile's opposite edge. */
		static const int dx[] = { 0, 1, 0, -1}; /* N, E, S, W */
		static const int dy[] = {-1, 0, 1,  0};
		static const uint8_t opposite[] = {0x04, 0x08, 0x01, 0x02}; /* S, W, N, E */
		int edge_idx = (edge_bit == 0x01) ? 0 : (edge_bit == 0x02) ? 1 : (edge_bit == 0x04) ? 2 : 3;
		TileIndex nb = TileAddXY(tile, dx[edge_idx], dy[edge_idx]);
		if (IsValidTile(nb)) {
			ModularAirportTileData *nb_data = st->airport.GetModularTileData(nb);
			if (nb_data != nullptr) {
				if (set) {
					nb_data->edge_block_mask |= opposite[edge_idx];
				} else {
					nb_data->edge_block_mask &= ~opposite[edge_idx];
				}
				MarkTileDirtyByTile(nb);
			}
		}
	}

	return CommandCost();
}

CommandCost CmdPlaceModularAirportTemplate(DoCommandFlags flags, TileIndex tile, StationID station_to_join, bool allow_adjacent, const ModularTemplatePlacementData &data)
{
	bool reuse = (station_to_join != NEW_STATION);
	if (!reuse) station_to_join = StationID::Invalid();
	bool distant_join = (station_to_join != StationID::Invalid());
	if (distant_join && (!_settings_game.station.distant_join_stations || !Station::IsValidID(station_to_join))) return CMD_ERROR;

	if (!IsValidTile(tile)) return CMD_ERROR;
	if (data.width == 0 || data.height == 0) return CMD_ERROR;
	if (data.rotation > 3) return CMD_ERROR;

	/* Templates containing non-rotatable compound pieces (e.g. 3-tile small terminal)
	 * must be placed without rotation. */
	if (data.rotation != 0) {
		for (const auto &t : data.tiles) {
			if (t.piece_type == APT_SMALL_BUILDING_1 || t.piece_type == APT_SMALL_BUILDING_2 || t.piece_type == APT_SMALL_BUILDING_3 ||
					IsLegacySmallHangarPiece(t.piece_type)) {
				return CommandCost(STR_ERROR_TEMPLATE_CONTAINS_NON_ROTATABLE);
			}
		}
	}

	/* Legacy (small) runway pieces are axis-locked and only support 0/180 rotation. */
	if ((data.rotation & 1) != 0) {
		for (const auto &t : data.tiles) {
			if (IsLegacySmallRunwayPiece(t.piece_type)) return CMD_ERROR;
		}
	}
	if (data.tiles.empty() || data.tiles.size() > MAX_TEMPLATE_TILES) {
		return CommandCost(STR_ERROR_AIRPORT_TEMPLATE_TOO_LARGE);
	}

	uint16_t rotated_w = 0, rotated_h = 0;
	GetRotatedTemplateDimensions(data.width, data.height, data.rotation, rotated_w, rotated_h);

	std::vector<ModularTemplatePlacementTile> rotated_tiles;
	rotated_tiles.reserve(data.tiles.size());

	for (const ModularTemplatePlacementTile &src_tile : data.tiles) {
		ModularTemplatePlacementTile rt = src_tile;
		RotateTemplateTile(rt, data.rotation, data.width, data.height);
		if (rt.dx >= rotated_w || rt.dy >= rotated_h) return CMD_ERROR;
		rotated_tiles.push_back(rt);
	}

	std::sort(rotated_tiles.begin(), rotated_tiles.end(), [](const ModularTemplatePlacementTile &a, const ModularTemplatePlacementTile &b) {
		if (a.dy != b.dy) return a.dy < b.dy;
		return a.dx < b.dx;
	});

	for (size_t i = 1; i < rotated_tiles.size(); i++) {
		if (rotated_tiles[i - 1].dx == rotated_tiles[i].dx && rotated_tiles[i - 1].dy == rotated_tiles[i].dy) return CMD_ERROR;
	}

	CommandCost total(EXPENSES_CONSTRUCTION);
	std::vector<TileIndex> abs_tiles;
	abs_tiles.reserve(rotated_tiles.size());
	int common_z = -1;
	for (const ModularTemplatePlacementTile &rt : rotated_tiles) {
		TileIndex t = TileAddXY(tile, rt.dx, rt.dy);
		if (!IsValidTile(t)) return CMD_ERROR;
		abs_tiles.push_back(t);

		/* Enforce common height for all tiles in the template. */
		int tile_z = GetTileMaxZ(t);
		if (common_z == -1) {
			common_z = tile_z;
		} else if (common_z != tile_z) {
			return CommandCost(STR_ERROR_FLAT_LAND_REQUIRED);
		}
	}

	/* Pass 1: Test all tile placements and metadata.
	 * Identify the station to join if not explicitly provided. */
	StationID join_id = station_to_join;
	for (size_t i = 0; i < rotated_tiles.size(); i++) {
		const ModularTemplatePlacementTile &rt = rotated_tiles[i];
		TileIndex t = abs_tiles[i];

		/* If we don't have a station to join yet, see if this tile is adjacent to an existing one. */
		if (join_id == StationID::Invalid()) {
			TileArea ta(t, 1, 1);
			Station *st = nullptr;
			if (GetStationAroundModular(ta, StationID::Invalid(), _current_company, &st).Succeeded() && st != nullptr) {
				join_id = st->index;
			}
		}

		CommandCost ret = Command<CMD_BUILD_MODULAR_AIRPORT_TILE>::Do(DoCommandFlags{flags}.Reset(DoCommandFlag::Execute), t, rt.piece_type, join_id, allow_adjacent, rt.rotation, rt.user_taxi_dir_mask, rt.one_way_taxi);
		if (ret.Failed()) return ret;
		total.AddCost(ret.GetCost());

		/* Test metadata. */
		if (IsModularRunwayPiece(rt.piece_type)) {
			uint8_t runway_flags = NormalizeTemplateRunwayFlags(rt.runway_flags);
			ret = Command<CMD_SET_RUNWAY_FLAGS>::Do(DoCommandFlags{flags}.Reset(DoCommandFlag::Execute), t, runway_flags);
			if (ret.Failed()) return ret;
			total.AddCost(ret.GetCost());
		}

		if (IsTaxiwayPiece(rt.piece_type)) {
			uint8_t taxi_dir_mask = rt.user_taxi_dir_mask & 0x0F;
			if (rt.one_way_taxi) {
				if (!HasExactlyOneBit(taxi_dir_mask)) return CMD_ERROR;
				const uint8_t auto_dirs = CalculateAutoTaxiDirectionsForGfx(rt.piece_type, rt.rotation);
				if ((auto_dirs & taxi_dir_mask) == 0) return CMD_ERROR;
			}

			ret = Command<CMD_SET_TAXIWAY_FLAGS>::Do(DoCommandFlags{flags}.Reset(DoCommandFlag::Execute), t, rt.user_taxi_dir_mask, rt.one_way_taxi);
			if (ret.Failed()) return ret;
			total.AddCost(ret.GetCost());
		}

		for (uint8_t edge_bit : kFenceEdgeBits) {
			if ((rt.edge_block_mask & edge_bit) == 0) continue;
			ret = Command<CMD_SET_MODULAR_AIRPORT_EDGE_FENCE>::Do(DoCommandFlags{flags}.Reset(DoCommandFlag::Execute), t, edge_bit, true);
			if (ret.Failed()) return ret;
			total.AddCost(ret.GetCost());
		}
	}

	if (flags.Test(DoCommandFlag::Execute)) {
		/* Pass 2: Execute tile placements. */
		StationID final_join_id = join_id;
		for (size_t i = 0; i < rotated_tiles.size(); i++) {
			const ModularTemplatePlacementTile &rt = rotated_tiles[i];
			TileIndex t = abs_tiles[i];

			CommandCost ret = Command<CMD_BUILD_MODULAR_AIRPORT_TILE>::Do(flags, t, rt.piece_type, final_join_id, allow_adjacent, rt.rotation, rt.user_taxi_dir_mask, rt.one_way_taxi);
			if (ret.Failed()) return ret; // Should not fail due to Pass 1.

			/* If we started with no station and this was the first tile, it created one.
			 * Use it for all subsequent tiles to ensure they join the same station. */
			if (final_join_id == StationID::Invalid()) {
				final_join_id = GetStationIndex(t);
			}
		}

		/* Pass 3: Execute metadata. */
		for (size_t i = 0; i < rotated_tiles.size(); i++) {
			const ModularTemplatePlacementTile &rt = rotated_tiles[i];
			TileIndex t = abs_tiles[i];

			if (IsModularRunwayPiece(rt.piece_type)) {
				uint8_t runway_flags = NormalizeTemplateRunwayFlags(rt.runway_flags);
				CommandCost ret = Command<CMD_SET_RUNWAY_FLAGS>::Do(flags, t, runway_flags);
				if (ret.Failed()) return ret;
			}

			if (IsTaxiwayPiece(rt.piece_type)) {
				CommandCost ret = Command<CMD_SET_TAXIWAY_FLAGS>::Do(flags, t, rt.user_taxi_dir_mask, rt.one_way_taxi);
				if (ret.Failed()) return ret;
			}

			for (uint8_t edge_bit : kFenceEdgeBits) {
				if ((rt.edge_block_mask & edge_bit) == 0) continue;
				CommandCost ret = Command<CMD_SET_MODULAR_AIRPORT_EDGE_FENCE>::Do(flags, t, edge_bit, true);
				if (ret.Failed()) return ret;
			}
		}
	}

	return total;
}
