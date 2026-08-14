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
#include "modular_airport_build.h"
#include "airport_pathfinder.h"
#include "station_base.h"
#include "station_map.h"
#include "tile_map.h"
#include "town.h"
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

CommandCost SetRunwayFlags_Check(TileIndex tile, uint8_t runway_flags, Station *st)
{
	/* Validate flags: at least one operation and exactly one direction must be set */
	if ((runway_flags & (RUF_LANDING | RUF_TAKEOFF)) == 0) return CMD_ERROR;
	uint8_t dir_flags = runway_flags & (RUF_DIR_LOW | RUF_DIR_HIGH);
	if (dir_flags != RUF_DIR_LOW && dir_flags != RUF_DIR_HIGH) return CMD_ERROR;

	/* Greenfield template test pass: station hasn't been allocated yet. The caller's
	 * BuildModularAirportTile_Check has already validated the placement. */
	if (st == nullptr) return CommandCost();

	CommandCost ret = CheckOwnership(st->owner);
	if (ret.Failed()) return ret;

	if (!st->airport.blocks.Test(AirportBlock::Modular)) return CMD_ERROR;

	ModularAirportTileData *data = st->airport.GetModularTileData(tile);
	if (data != nullptr && !IsModularRunwayPiece(data->piece_type)) return CMD_ERROR;

	return CommandCost();
}

void SetRunwayFlags_Apply(TileIndex tile, uint8_t runway_flags, Station *st)
{
	ModularAirportTileData *data = st->airport.GetModularTileData(tile);
	if (data == nullptr) return;

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
	/* Landing/takeoff flags gate the large-aircraft-safe catchment tier, so the
	 * catchment radius may change; MarkLayoutDirty invalidates it and we recompute. */
	st->airport.MarkLayoutDirty();
	st->RecomputeCatchment();
	if (_show_holding_overlay) MarkWholeScreenDirty();
}

CommandCost CmdSetRunwayFlags(DoCommandFlags flags, TileIndex tile, uint8_t runway_flags)
{
	if (!IsValidTile(tile)) return CMD_ERROR;

	/* For GUI direct callers, ensure it's an existing airport tile first.
	 * The template command bypasses this by calling _Check directly. */
	if (!IsTileType(tile, TileType::Station) || !IsAirport(tile)) return CMD_ERROR;

	Station *st = Station::GetByTile(tile);
	CommandCost ret = SetRunwayFlags_Check(tile, runway_flags, st);
	if (ret.Failed()) return ret;

	if (flags.Test(DoCommandFlag::Execute)) {
		SetRunwayFlags_Apply(tile, runway_flags, st);
	}

	return CommandCost();
}

CommandCost SetTaxiwayFlags_Check(TileIndex tile, uint8_t taxi_dir_mask, bool one_way_taxi, Station *st, uint8_t piece_type = 0, uint8_t rotation = 0)
{
	/* Greenfield template test pass: station hasn't been allocated yet. The caller's
	 * BuildModularAirportTile_Check has already validated the placement. The taxi-direction
	 * checks below still run against the supplied piece_type/rotation. */
	ModularAirportTileData *data = (st != nullptr) ? st->airport.GetModularTileData(tile) : nullptr;
	if (st != nullptr) {
		CommandCost ret = CheckOwnership(st->owner);
		if (ret.Failed()) return ret;

		if (!st->airport.blocks.Test(AirportBlock::Modular)) return CMD_ERROR;
	}

	uint8_t current_piece_type = (data != nullptr) ? data->piece_type : piece_type;
	uint8_t current_rotation = (data != nullptr) ? data->rotation : rotation;

	if (current_piece_type != 0) {
		if (!IsTaxiwayPiece(current_piece_type)) return CMD_ERROR;
		const uint8_t auto_dirs = CalculateAutoTaxiDirectionsForGfx(current_piece_type, current_rotation);
		if (one_way_taxi) {
			if (!HasExactlyOneBit(taxi_dir_mask)) return CMD_ERROR;
			if ((auto_dirs & (taxi_dir_mask & 0x0F)) == 0) return CMD_ERROR;
		}
	} else if (one_way_taxi) {
		/* Fallback for when we don't even have a piece type to check against. */
		if (!HasExactlyOneBit(taxi_dir_mask)) return CMD_ERROR;
	}

	return CommandCost();
}

void SetTaxiwayFlags_Apply(TileIndex tile, uint8_t taxi_dir_mask, bool one_way_taxi, Station *st)
{
	ModularAirportTileData *data = st->airport.GetModularTileData(tile);
	if (data == nullptr) return;

	data->one_way_taxi = one_way_taxi;
	data->user_taxi_dir_mask = one_way_taxi ? (taxi_dir_mask & 0x0F) : 0x0F;
	MarkTileDirtyByTile(tile);
	/* One-way flags feed the layout-derived caches: ComputeModularHeliTiles refuses to
	 * put a helicopter pad on a one-way tile, so a tile turned one-way after the pad was
	 * computed leaves the cache pointing at a tile that is now illegal. */
	st->airport.MarkLayoutDirty();
}

CommandCost CmdSetTaxiwayFlags(DoCommandFlags flags, TileIndex tile, uint8_t taxi_dir_mask, bool one_way_taxi)
{
	if (!IsValidTile(tile)) return CMD_ERROR;

	if (!IsTileType(tile, TileType::Station) || !IsAirport(tile)) return CMD_ERROR;

	Station *st = Station::GetByTile(tile);
	CommandCost ret = SetTaxiwayFlags_Check(tile, taxi_dir_mask, one_way_taxi, st);
	if (ret.Failed()) return ret;

	if (flags.Test(DoCommandFlag::Execute)) {
		SetTaxiwayFlags_Apply(tile, taxi_dir_mask, one_way_taxi, st);
	}

	return CommandCost();
}

CommandCost SetEdgeFence_Check(TileIndex tile [[maybe_unused]], uint8_t edge_bit, Station *st)
{
	/* Validate: exactly one edge bit. */
	if (edge_bit != 0x01 && edge_bit != 0x02 && edge_bit != 0x04 && edge_bit != 0x08) return CMD_ERROR;

	/* Greenfield template test pass: station hasn't been allocated yet. The caller's
	 * BuildModularAirportTile_Check has already validated the placement. */
	if (st == nullptr) return CommandCost();

	CommandCost ret = CheckOwnership(st->owner);
	if (ret.Failed()) return ret;

	if (!st->airport.blocks.Test(AirportBlock::Modular)) return CMD_ERROR;

	return CommandCost();
}

void SetEdgeFence_Apply(TileIndex tile, uint8_t edge_bit, bool set, Station *st)
{
	ModularAirportTileData *data = st->airport.GetModularTileData(tile);
	if (data == nullptr) return;

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

	/* Edge fences change which tiles can reach which, so every layout-derived cache
	 * (heli pads, holding loop, catchment) may now be wrong. */
	st->airport.MarkLayoutDirty();
}

CommandCost CmdSetModularAirportEdgeFence(DoCommandFlags flags, TileIndex tile, uint8_t edge_bit, bool set)
{
	if (!IsValidTile(tile)) return CMD_ERROR;

	if (!IsTileType(tile, TileType::Station) || !IsAirport(tile)) return CMD_ERROR;

	Station *st = Station::GetByTile(tile);
	CommandCost ret = SetEdgeFence_Check(tile, edge_bit, st);
	if (ret.Failed()) return ret;

	if (flags.Test(DoCommandFlag::Execute)) {
		SetEdgeFence_Apply(tile, edge_bit, set, st);
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
	CommandCost ret;
	std::vector<TileIndex> abs_tiles;
	abs_tiles.reserve(rotated_tiles.size());
	int common_z = -1;
	uint min_x = UINT_MAX, min_y = UINT_MAX, max_x = 0, max_y = 0;
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

		min_x = std::min<uint>(min_x, TileX(t));
		min_y = std::min<uint>(min_y, TileY(t));
		max_x = std::max<uint>(max_x, TileX(t));
		max_y = std::max<uint>(max_y, TileY(t));
	}
	/* union_area is only used for noise/town iterators where a bounding box is acceptable. */
	TileArea union_area(TileXY(min_x, min_y), max_x - min_x + 1, max_y - min_y + 1);

	/* Step 3.A: Whole-template validation */
	/* Identify the station to join by scanning each template tile. */
	Station *st = nullptr;
	for (TileIndex t : abs_tiles) {
		Station *around = nullptr;
		ret = FindJoiningStation(StationID::Invalid(), station_to_join, allow_adjacent, TileArea(t, 1, 1), &around);
		if (ret.Failed()) return ret;
		if (around != nullptr) {
			if (st != nullptr && st != around) return CommandCost(STR_ERROR_ADJOINS_MORE_THAN_ONE_EXISTING);
			st = around;
		}
	}

	if (st == nullptr && distant_join) st = Station::GetIfValid(station_to_join);

	const bool will_create_airport_facility = st == nullptr || !st->facilities.Test(StationFacility::Airport);
	const ModularAirportNoiseSnapshot noise_before = will_create_airport_facility ? ModularAirportNoiseSnapshot{} : GetModularAirportNoiseSnapshot(st);
	std::vector<ModularAirportNoisePiece> future_noise_pieces;
	std::vector<ModularAirportCapabilityPiece> future_capability_pieces;
	if (!will_create_airport_facility && st->airport.modular_tile_data != nullptr) {
		future_noise_pieces.reserve(st->airport.modular_tile_data->size() + rotated_tiles.size());
		future_capability_pieces.reserve(st->airport.modular_tile_data->size() + rotated_tiles.size());
		for (const ModularAirportTileData &data : *st->airport.modular_tile_data) {
			if (std::find(abs_tiles.begin(), abs_tiles.end(), data.tile) == abs_tiles.end()) {
				future_noise_pieces.push_back({data.tile, data.piece_type});
				future_capability_pieces.push_back({data.piece_type, data.runway_flags});
			}
		}
	}
	for (size_t i = 0; i < rotated_tiles.size(); i++) {
		future_noise_pieces.push_back({abs_tiles[i], rotated_tiles[i].piece_type});
		future_capability_pieces.push_back({rotated_tiles[i].piece_type,
				IsModularRunwayPiece(rotated_tiles[i].piece_type) ? NormalizeTemplateRunwayFlags(rotated_tiles[i].runway_flags) : RUF_DEFAULT});
	}
	const ModularAirportNoiseSnapshot noise_after = GetModularAirportNoiseSnapshot(future_noise_pieces);
	const StationNaming naming = ModularAirportAcceptsPlanesFromPieces(future_capability_pieces) ? STATIONNAMING_AIRPORT : STATIONNAMING_HELIPORT;
	ret = CheckModularAirportNoiseChange(noise_before, noise_after);
	if (ret.Failed()) return ret;

	if (will_create_airport_facility && !_settings_game.economy.station_noise_level &&
			_settings_game.difficulty.town_council_tolerance != TOWN_COUNCIL_PERMISSIVE) {
		Town *t = ClosestTownFromTile(abs_tiles[0], UINT_MAX);
		uint num = 0;
		for (const Station *other : Station::Iterate()) {
			if (other->town == t && other->facilities.Test(StationFacility::Airport) && other->airport.type != AT_OILRIG) num++;
		}
		if (num >= 2) {
			return CommandCostWithParam(STR_ERROR_LOCAL_AUTHORITY_REFUSES_AIRPORT, t->index);
		}
	}

	/* Enforce same height level across the entire modular airport. */
	if (st != nullptr && st->airport.blocks.Test(AirportBlock::Modular) &&
			st->airport.modular_tile_data != nullptr && !st->airport.modular_tile_data->empty()) {
		int existing_z = GetTileMaxZ(st->airport.modular_tile_data->front().tile);
		if (existing_z != common_z) {
			return CommandCost(STR_ERROR_FLAT_LAND_REQUIRED);
		}
	}

	/* Test station building/joining. Move execution to after the per-tile validation loop. */
	ret = BuildStationPart(&st, DoCommandFlags{flags}.Reset(DoCommandFlag::Execute), reuse, union_area, naming);
	if (ret.Failed()) return ret;

	/* Compute placement order. */
	StationID join_id = (st != nullptr) ? st->index : StationID::Invalid();
	std::vector<size_t> placement_order;
	placement_order.reserve(rotated_tiles.size());
	if (!distant_join && !_settings_game.station.distant_join_stations) {
		std::vector<bool> added(rotated_tiles.size(), false);

		auto add_index = [&](size_t idx) {
			if (added[idx]) return;
			added[idx] = true;
			placement_order.push_back(idx);
		};
		auto adjacent_tiles = [&](TileIndex a, TileIndex b) {
			const int dx = abs(static_cast<int>(TileX(a)) - static_cast<int>(TileX(b)));
			const int dy = abs(static_cast<int>(TileY(a)) - static_cast<int>(TileY(b)));
			return dx + dy == 1;
		};
		auto adjacent_to_join_station = [&](TileIndex t) {
			if (join_id == StationID::Invalid()) return false;
			Station *around = nullptr;
			return GetStationAroundModular(TileArea(t, 1, 1), StationID::Invalid(), _current_company, &around).Succeeded() &&
					around != nullptr && around->index == join_id;
		};

		for (size_t i = 0; i < abs_tiles.size(); i++) {
			if (adjacent_to_join_station(abs_tiles[i])) add_index(i);
		}
		if (placement_order.empty() && join_id == StationID::Invalid()) add_index(0);

		for (size_t pos = 0; pos < placement_order.size(); pos++) {
			const TileIndex base = abs_tiles[placement_order[pos]];
			for (size_t i = 0; i < abs_tiles.size(); i++) {
				if (!added[i] && adjacent_tiles(base, abs_tiles[i])) add_index(i);
			}
		}

		if (placement_order.size() != rotated_tiles.size()) return CMD_ERROR;
	} else {
		for (size_t i = 0; i < rotated_tiles.size(); i++) placement_order.push_back(i);
	}

	/* Step 3.B: Per-tile validation loop. Noise was checked once above against
	 * the finished layout; intermediate per-tile layouts are not meaningful. */
	struct TileValidationResult {
		bool is_replace;
	};
	std::vector<TileValidationResult> validation_results(rotated_tiles.size());
	for (size_t i : placement_order) {
		const ModularTemplatePlacementTile &rt = rotated_tiles[i];
		TileIndex t = abs_tiles[i];

		Station *tile_st = st;
		TileValidationResult &res = validation_results[i];

		ret = BuildModularAirportTile_Check(DoCommandFlags{flags}.Reset(DoCommandFlag::Execute), t, rt.piece_type, station_to_join, allow_adjacent, tile_st, res.is_replace, total, false);
		if (ret.Failed()) return ret;

		if (IsModularRunwayPiece(rt.piece_type)) {
			uint8_t runway_flags = NormalizeTemplateRunwayFlags(rt.runway_flags);
			ret = SetRunwayFlags_Check(t, runway_flags, st);
			if (ret.Failed()) return ret;
		}

		if (IsTaxiwayPiece(rt.piece_type)) {
			ret = SetTaxiwayFlags_Check(t, rt.user_taxi_dir_mask, rt.one_way_taxi, st, rt.piece_type, rt.rotation);
			if (ret.Failed()) return ret;
		}

		for (uint8_t edge_bit : kFenceEdgeBits) {
			if ((rt.edge_block_mask & edge_bit) != 0) {
				ret = SetEdgeFence_Check(t, edge_bit, st);
				if (ret.Failed()) return ret;
			}
		}
	}

	if (flags.Test(DoCommandFlag::Execute)) {
		const ModularAirportNoiseSnapshot execute_noise_before = st != nullptr ? GetModularAirportNoiseSnapshot(st) : ModularAirportNoiseSnapshot{};
		/* Apply station creation/joining now that validation is complete. */
		ret = BuildStationPart(&st, flags, reuse, union_area, naming);
		assert(ret.Succeeded());

		/* Step 3.C: Single execute block. */
		for (size_t i : placement_order) {
			const ModularTemplatePlacementTile &rt = rotated_tiles[i];
			TileIndex t = abs_tiles[i];
			const TileValidationResult &res = validation_results[i];

			BuildModularAirportTile_Apply(t, rt.piece_type, st, res.is_replace, rt.rotation, rt.user_taxi_dir_mask, rt.one_way_taxi, false);

			if (IsModularRunwayPiece(rt.piece_type)) {
				uint8_t runway_flags = NormalizeTemplateRunwayFlags(rt.runway_flags);
				SetRunwayFlags_Apply(t, runway_flags, st);
			}

			if (IsTaxiwayPiece(rt.piece_type)) {
				SetTaxiwayFlags_Apply(t, rt.user_taxi_dir_mask, rt.one_way_taxi, st);
			}

			for (uint8_t edge_bit : kFenceEdgeBits) {
				if ((rt.edge_block_mask & edge_bit) != 0) {
					SetEdgeFence_Apply(t, edge_bit, true, st);
				}
			}
		}

		/* Once, on the finished layout. A template overbuilding an existing airport can
		 * replace its hangar early and lay its own down later in placement_order, so the
		 * intermediate states say nothing about the result. */
		CancelModularHangarOrdersIfNoneLeft(st);
		ApplyModularAirportNoiseChange(st, execute_noise_before);
	}

	return total;
}
