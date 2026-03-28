/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file modular_airport_build.cpp Helpers for modular airport build/remove commands. */

#include "stdafx.h"

#include "modular_airport_build.h"

#include "aircraft.h"
#include "airport.h"
#include "airport_pathfinder.h"
#include "animated_tile_func.h"
#include "clear_func.h"
#include "command_func.h"
#include "company_base.h"
#include "company_func.h"
#include "economy_func.h"
#include "gfx_func.h"
#include "landscape.h"
#include "landscape_cmd.h"
#include "modular_airport_cmd.h"
#include "modular_airport_gui.h"
#include "newgrf_airport.h"
#include "newgrf_airporttiles.h"
#include "newgrf_debug.h"
#include "station_cmd.h"
#include "station_map.h"
#include "timer/timer_game_calendar.h"
#include "town.h"
#include "vehicle_func.h"
#include "viewport_func.h"
#include "window_func.h"

#include "table/airporttile_ids.h"
#include "table/strings.h"

static bool IsSmallRunwayFamily(uint8_t piece_type)
{
	switch (piece_type) {
		case APT_RUNWAY_SMALL_NEAR_END:
		case APT_RUNWAY_SMALL_MIDDLE:
		case APT_RUNWAY_SMALL_FAR_END:
			return true;
		default:
			return false;
	}
}

static void CollectRunwayFamilySegment(Station *st, TileIndex start, TileIndexDiff diff, bool horizontal, bool family_large, std::vector<TileIndex> &tiles)
{
	TileIndex cur = start;
	while (true) {
		ModularAirportTileData *data = st->airport.GetModularTileData(cur);
		if (!IsRunwayPieceOnAxis(data, horizontal)) break;
		bool is_large = IsLargeRunwayFamily(data->piece_type);
		bool is_small = IsSmallRunwayFamily(data->piece_type);
		if (family_large && !is_large) break;
		if (!family_large && !is_small) break;
		tiles.push_back(cur);
		cur = cur + diff;
	}
}

void NormalizeRunwaySegmentVisuals(Station *st, TileIndex changed_tile, bool horizontal)
{
	TileIndexDiff diff = horizontal ? TileDiffXY(1, 0) : TileDiffXY(0, 1);

	/* Walk to the low end of the entire contiguous runway (both families). */
	TileIndex first = changed_tile;
	while (true) {
		TileIndex prev = first - diff;
		ModularAirportTileData *prev_data = st->airport.GetModularTileData(prev);
		if (!IsRunwayPieceOnAxis(prev_data, horizontal)) break;
		first = prev;
	}

	/* Walk from low end to high end, splitting into family sub-segments. */
	TileIndex cur = first;
	while (true) {
		ModularAirportTileData *data = st->airport.GetModularTileData(cur);
		if (!IsRunwayPieceOnAxis(data, horizontal)) break;

		bool is_large = IsLargeRunwayFamily(data->piece_type);
		std::vector<TileIndex> seg;
		CollectRunwayFamilySegment(st, cur, diff, horizontal, is_large, seg);
		if (seg.empty()) break;

		for (size_t i = 0; i < seg.size(); i++) {
			ModularAirportTileData *td = st->airport.GetModularTileData(seg[i]);
			if (td == nullptr) continue;

			uint8_t new_type = GetCanonicalRunwaySegmentPiece(is_large, seg.size(), i);

			if (td->piece_type != new_type) {
				td->piece_type = new_type;
				SetStationGfx(Tile(seg[i]), new_type);
				MarkTileDirtyByTile(seg[i]);
			}
		}

		cur = seg.back() + diff;
	}
}

uint8_t GetStockFenceEdgeMask(uint8_t stock_gfx)
{
	switch (stock_gfx) {
		case APT_APRON_FENCE_NW: return 0x01;
		case APT_APRON_FENCE_SW: return 0x02;
		case APT_APRON_FENCE_SE: return 0x04;
		case APT_APRON_FENCE_NE: return 0x08;
		case APT_APRON_FENCE_NE_SW: return 0x08 | 0x02;
		case APT_APRON_FENCE_SE_SW: return 0x04 | 0x02;
		case APT_APRON_FENCE_NE_SE: return 0x08 | 0x04;
		case APT_APRON_N_FENCE_SW: return 0x02;
		case APT_RUNWAY_END_FENCE_SE: return 0x04;
		case APT_RUNWAY_END_FENCE_NW: return 0x01;
		case APT_RUNWAY_END_FENCE_NW_SW: return 0x01 | 0x02;
		case APT_RUNWAY_END_FENCE_SE_SW: return 0x04 | 0x02;
		case APT_RUNWAY_END_FENCE_NE_NW: return 0x08 | 0x01;
		case APT_RUNWAY_END_FENCE_NE_SE: return 0x08 | 0x04;
		case APT_RUNWAY_FENCE_NW: return 0x01;
		case APT_HELIPAD_2_FENCE_NW: return 0x01;
		case APT_HELIPAD_2_FENCE_NE_SE: return 0x08 | 0x04;
		case APT_HELIPAD_3_FENCE_NW: return 0x01;
		case APT_HELIPAD_3_FENCE_NW_SW: return 0x01 | 0x02;
		case APT_HELIPAD_3_FENCE_SE_SW: return 0x04 | 0x02;
		case APT_TOWER_FENCE_SW: return 0x02;
		case APT_LOW_BUILDING_FENCE_N: return 0x01;
		case APT_LOW_BUILDING_FENCE_NW: return 0x01;
		case APT_RADAR_FENCE_SW: return 0x02;
		case APT_RADAR_FENCE_NE: return 0x08;
		case APT_RADAR_GRASS_FENCE_SW: return 0x02;
		case APT_RADIO_TOWER_FENCE_NE: return 0x08;
		case APT_GRASS_FENCE_SW: return 0x02;
		case APT_GRASS_FENCE_NE_FLAG: return 0x08;
		case APT_EMPTY_FENCE_NE: return 0x08;
		default: return 0;
	}
}

uint8_t MapStockGfxToModularPiece(uint8_t stock_gfx)
{
	switch (stock_gfx) {
		case APT_RUNWAY_1:
		case APT_RUNWAY_2:
		case APT_RUNWAY_3:
		case APT_RUNWAY_4:
		case APT_RUNWAY_5:
		case APT_RUNWAY_END:
		case APT_RUNWAY_SMALL_NEAR_END:
		case APT_RUNWAY_SMALL_MIDDLE:
		case APT_RUNWAY_SMALL_FAR_END:
			return stock_gfx;
		case APT_RUNWAY_END_FENCE_SE:
		case APT_RUNWAY_END_FENCE_NW:
		case APT_RUNWAY_END_FENCE_NW_SW:
		case APT_RUNWAY_END_FENCE_SE_SW:
		case APT_RUNWAY_END_FENCE_NE_NW:
		case APT_RUNWAY_END_FENCE_NE_SE:
			return APT_RUNWAY_END;
		case APT_RUNWAY_FENCE_NW:
			return APT_RUNWAY_5;
		case APT_APRON:
		case APT_APRON_FENCE_NW:
		case APT_APRON_FENCE_SW:
		case APT_APRON_FENCE_NE:
		case APT_APRON_FENCE_SE:
		case APT_APRON_FENCE_NE_SW:
		case APT_APRON_FENCE_SE_SW:
		case APT_APRON_FENCE_NE_SE:
		case APT_APRON_W:
		case APT_APRON_S:
		case APT_APRON_E:
		case APT_APRON_N:
		case APT_APRON_HOR:
		case APT_APRON_N_FENCE_SW:
		case APT_APRON_VER_CROSSING_S:
		case APT_APRON_HOR_CROSSING_W:
		case APT_APRON_VER_CROSSING_N:
		case APT_APRON_HOR_CROSSING_E:
		case APT_APRON_HALF_EAST:
		case APT_APRON_HALF_WEST:
			return APT_APRON;
		case APT_STAND:
		case APT_STAND_1:
		case APT_STAND_PIER_NE:
			return APT_STAND;
		case APT_DEPOT_SE:
		case APT_DEPOT_SW:
		case APT_DEPOT_NW:
		case APT_DEPOT_NE:
		case APT_SMALL_DEPOT_SE:
		case APT_SMALL_DEPOT_SW:
		case APT_SMALL_DEPOT_NW:
		case APT_SMALL_DEPOT_NE:
			return stock_gfx;
		case APT_HELIPORT:
		case APT_HELIPAD_1:
		case APT_HELIPAD_2_FENCE_NW:
		case APT_HELIPAD_2:
		case APT_HELIPAD_2_FENCE_NE_SE:
		case APT_HELIPAD_3_FENCE_SE_SW:
		case APT_HELIPAD_3_FENCE_NW_SW:
		case APT_HELIPAD_3_FENCE_NW:
			return APT_HELIPAD_2;
		case APT_TOWER:
		case APT_TOWER_FENCE_SW:
			return APT_TOWER;
		case APT_GRASS_1:
		case APT_GRASS_2:
		case APT_GRASS_FENCE_SW:
		case APT_GRASS_FENCE_NE_FLAG:
			return APT_GRASS_1;
		case APT_GRASS_FENCE_NE_FLAG_2:
			return APT_GRASS_FENCE_NE_FLAG_2;
		case APT_EMPTY:
		case APT_EMPTY_FENCE_NE:
			return APT_EMPTY;
		case APT_BUILDING_1:
			return APT_BUILDING_1;
		case APT_BUILDING_2:
			return APT_BUILDING_2;
		case APT_BUILDING_3:
			return APT_BUILDING_3;
		case APT_ROUND_TERMINAL:
			return APT_ROUND_TERMINAL;
		case APT_LOW_BUILDING:
		case APT_LOW_BUILDING_FENCE_N:
		case APT_LOW_BUILDING_FENCE_NW:
			return APT_LOW_BUILDING;
		case APT_RADAR_FENCE_SW:
		case APT_RADAR_FENCE_NE:
		case APT_RADAR_GRASS_FENCE_SW:
			return APT_RADAR_FENCE_NE;
		case APT_RADIO_TOWER_FENCE_NE:
			return APT_RADIO_TOWER_FENCE_NE;
		case APT_SMALL_BUILDING_1:
		case APT_SMALL_BUILDING_2:
		case APT_SMALL_BUILDING_3:
			return stock_gfx;
		case APT_PIER:
		case APT_PIER_NW_NE:
			return APT_APRON;
		default:
			return APT_BUILDING_1;
	}
}

struct StockTileOverride {
	int x;
	int y;
	uint8_t piece_type;
};

static constexpr StockTileOverride _country_stock_to_modular_overrides[] = {
	{0, 1, APT_APRON}, {1, 1, APT_STAND}, {2, 1, APT_STAND}, {3, 1, APT_APRON},
};

uint8_t ApplyStockTileOverride(uint8_t airport_type, int dx, int dy, uint8_t piece_type)
{
	std::span<const StockTileOverride> tile_overrides;
	switch (airport_type) {
		case AT_SMALL:
			tile_overrides = _country_stock_to_modular_overrides;
			break;
		default:
			break;
	}

	for (const auto &ovr : tile_overrides) {
		if (ovr.x == dx && ovr.y == dy) return ovr.piece_type;
	}
	return piece_type;
}

static Money ScaleModularAirportCost(Money base, uint16_t percent)
{
	return static_cast<Money>((static_cast<int64_t>(base) * percent + 50) / 100);
}

Money GetModularAirportPieceBuildCost(uint8_t piece_type)
{
	const Money base = _price[Price::BuildStationAirport];

	switch (piece_type) {
		case APT_RUNWAY_1:
		case APT_RUNWAY_2:
		case APT_RUNWAY_3:
		case APT_RUNWAY_4:
		case APT_RUNWAY_5:
		case APT_RUNWAY_END:
			return ScaleModularAirportCost(base, 155);
		case APT_RUNWAY_SMALL_NEAR_END:
		case APT_RUNWAY_SMALL_MIDDLE:
		case APT_RUNWAY_SMALL_FAR_END:
			return ScaleModularAirportCost(base, 125);
		case APT_STAND:
		case APT_STAND_1:
		case APT_STAND_PIER_NE:
			return ScaleModularAirportCost(base, 135);
		case APT_APRON:
		case APT_APRON_FENCE_NW:
		case APT_APRON_FENCE_SW:
		case APT_APRON_W:
		case APT_APRON_S:
		case APT_APRON_VER_CROSSING_S:
		case APT_APRON_HOR_CROSSING_W:
		case APT_APRON_VER_CROSSING_N:
		case APT_APRON_HOR_CROSSING_E:
		case APT_APRON_E:
		case APT_APRON_N:
		case APT_APRON_HOR:
		case APT_APRON_N_FENCE_SW:
		case APT_PIER_NW_NE:
		case APT_PIER:
		case APT_APRON_FENCE_NE:
		case APT_APRON_FENCE_NE_SW:
		case APT_APRON_FENCE_SE_SW:
		case APT_APRON_FENCE_SE:
		case APT_APRON_FENCE_NE_SE:
		case APT_APRON_HALF_EAST:
		case APT_APRON_HALF_WEST:
			return ScaleModularAirportCost(base, 90);
		case APT_DEPOT_SE:
		case APT_DEPOT_SW:
		case APT_DEPOT_NW:
		case APT_DEPOT_NE:
			return ScaleModularAirportCost(base, 170);
		case APT_SMALL_DEPOT_SE:
		case APT_SMALL_DEPOT_SW:
		case APT_SMALL_DEPOT_NW:
		case APT_SMALL_DEPOT_NE:
			return ScaleModularAirportCost(base, 145);
		case APT_HELIPORT:
		case APT_HELIPAD_1:
		case APT_HELIPAD_2_FENCE_NW:
		case APT_HELIPAD_2:
		case APT_HELIPAD_2_FENCE_NE_SE:
		case APT_HELIPAD_3_FENCE_SE_SW:
		case APT_HELIPAD_3_FENCE_NW_SW:
		case APT_HELIPAD_3_FENCE_NW:
			return ScaleModularAirportCost(base, 145);
		case APT_BUILDING_1:
		case APT_BUILDING_2:
		case APT_BUILDING_3:
		case APT_ROUND_TERMINAL:
			return ScaleModularAirportCost(base, 30);
		case APT_LOW_BUILDING:
		case APT_LOW_BUILDING_FENCE_N:
		case APT_LOW_BUILDING_FENCE_NW:
			return ScaleModularAirportCost(base, 18);
		case APT_TOWER:
		case APT_TOWER_FENCE_SW:
		case APT_RADAR_GRASS_FENCE_SW:
		case APT_RADAR_FENCE_SW:
		case APT_RADAR_FENCE_NE:
		case APT_RADIO_TOWER_FENCE_NE:
		case APT_GRASS_FENCE_NE_FLAG_2:
			return ScaleModularAirportCost(base, 24);
		case APT_EMPTY:
		case APT_EMPTY_FENCE_NE:
		case APT_GRASS_FENCE_SW:
		case APT_GRASS_2:
		case APT_GRASS_1:
		case APT_GRASS_FENCE_NE_FLAG:
			return ScaleModularAirportCost(base, 8);
		default:
			return base;
	}
}

CommandCost RemoveModularAirportTile(TileIndex tile, DoCommandFlags flags)
{
	Station *st = Station::GetByTile(tile);

	if (_current_company != OWNER_WATER) {
		CommandCost ret = CheckOwnership(st->owner);
		if (ret.Failed()) return ret;
	}

	auto is_small_terminal_piece = [](uint8_t piece_type) {
		return piece_type == APT_SMALL_BUILDING_1 || piece_type == APT_SMALL_BUILDING_2 || piece_type == APT_SMALL_BUILDING_3;
	};

	auto find_small_terminal_demolition_tiles = [&](TileIndex seed) {
		std::vector<TileIndex> tiles;
		tiles.push_back(seed);

		const ModularAirportTileData *seed_md = st->airport.GetModularTileData(seed);
		if (seed_md == nullptr || !is_small_terminal_piece(seed_md->piece_type)) return tiles;

		auto get_terminal_piece = [&](TileIndex t) -> uint8_t {
			const ModularAirportTileData *md = st->airport.GetModularTileData(t);
			return md != nullptr ? md->piece_type : 0xFF;
		};

		TileIndex middle = INVALID_TILE;
		if (seed_md->piece_type == APT_SMALL_BUILDING_2) {
			middle = seed;
		} else {
			const TileIndexDiff kN4[] = { TileDiffXY(1, 0), TileDiffXY(-1, 0), TileDiffXY(0, 1), TileDiffXY(0, -1) };
			for (TileIndexDiff d : kN4) {
				TileIndex n = seed + d;
				if (get_terminal_piece(n) == APT_SMALL_BUILDING_2) {
					middle = n;
					break;
				}
			}
		}
		if (middle == INVALID_TILE) return tiles;

		const TileIndexDiff kAxis[] = { TileDiffXY(1, 0), TileDiffXY(0, 1) };
		for (TileIndexDiff d : kAxis) {
			TileIndex a = middle + d;
			TileIndex b = middle - d;
			uint8_t pa = get_terminal_piece(a);
			uint8_t pb = get_terminal_piece(b);
			if ((pa == APT_SMALL_BUILDING_1 && pb == APT_SMALL_BUILDING_3) ||
					(pa == APT_SMALL_BUILDING_3 && pb == APT_SMALL_BUILDING_1)) {
				tiles.clear();
				tiles.push_back(middle);
				tiles.push_back(a);
				tiles.push_back(b);
				break;
			}
		}

		return tiles;
	};

	std::vector<TileIndex> tiles_to_remove = find_small_terminal_demolition_tiles(tile);

	for (TileIndex t : tiles_to_remove) {
		if (!TeleportAircraftOnModularTile(t, st, flags.Test(DoCommandFlag::Execute))) {
			return CommandCost(STR_ERROR_AIRCRAFT_IN_THE_WAY);
		}
	}

	CommandCost cost(EXPENSES_CONSTRUCTION);
	for (size_t i = 0; i < tiles_to_remove.size(); ++i) {
		cost.AddCost(_price[Price::ClearStationAirport]);
	}

	if (flags.Test(DoCommandFlag::Execute)) {
		std::vector<std::pair<TileIndex, uint8_t>> removed_runway_tiles;
		if (st->airport.modular_tile_data != nullptr) {
			for (TileIndex t : tiles_to_remove) {
				const ModularAirportTileData *md = st->airport.GetModularTileData(t);
				if (md != nullptr && IsModularRunwayPiece(md->piece_type)) {
					removed_runway_tiles.push_back({t, md->rotation});
				}
			}
		}

		for (TileIndex t : tiles_to_remove) {
			DoClearSquare(t);
			DeleteNewGRFInspectWindow(GSF_AIRPORTTILES, t.base());
		}

		if (st->airport.modular_tile_data != nullptr) {
			auto &tile_data_vec = *st->airport.modular_tile_data;
			tile_data_vec.erase(std::remove_if(tile_data_vec.begin(), tile_data_vec.end(),
				[&](const ModularAirportTileData &data) {
					return std::find(tiles_to_remove.begin(), tiles_to_remove.end(), data.tile) != tiles_to_remove.end();
				}), tile_data_vec.end());
			st->airport.modular_tile_index_dirty = true;
			st->airport.modular_holding_loop_dirty = true;
			if (_show_holding_overlay) MarkWholeScreenDirty();

			for (const auto &[removed_tile, removed_rotation] : removed_runway_tiles) {
				bool horizontal = (removed_rotation % 2) == 0;
				TileIndexDiff diff = horizontal ? TileDiffXY(1, 0) : TileDiffXY(0, 1);
				TileIndex neighbor_lo = removed_tile - diff;
				TileIndex neighbor_hi = removed_tile + diff;
				if (st->airport.GetModularTileData(neighbor_lo) != nullptr) {
					NormalizeRunwaySegmentVisuals(st, neighbor_lo, horizontal);
				}
				if (st->airport.GetModularTileData(neighbor_hi) != nullptr) {
					NormalizeRunwaySegmentVisuals(st, neighbor_hi, horizontal);
				}
			}
		}

		for (TileIndex t : tiles_to_remove) {
			st->rect.AfterRemoveTile(st, t);
		}

		bool any_tiles = false;
		OrthogonalTileArea new_area;
		new_area.Clear();
		for (TileIndex cur_tile : st->airport) {
			if (!st->TileBelongsToAirport(cur_tile)) continue;
			any_tiles = true;
			new_area.Add(cur_tile);
		}

		if (any_tiles) {
			st->airport.tile = new_area.tile;
			st->airport.w = new_area.w;
			st->airport.h = new_area.h;
			st->AfterStationTileSetChange(false, StationType::Airport);
		} else {
			delete st->airport.psa;
			delete st->airport.modular_tile_data;
			st->airport.modular_tile_data = nullptr;
			delete st->airport.modular_tile_index;
			st->airport.modular_tile_index = nullptr;
			delete st->airport.modular_holding_loop;
			st->airport.modular_holding_loop = nullptr;
			st->airport.modular_holding_loop_dirty = true;
			if (_show_holding_overlay) MarkWholeScreenDirty();
			st->airport.Clear();
			st->facilities.Reset(StationFacility::Airport);
			SetWindowClassesDirty(WC_VEHICLE_ORDERS);
			Company::Get(st->owner)->infrastructure.airport--;
			st->AfterStationTileSetChange(false, StationType::Airport);
			DeleteNewGRFInspectWindow(GSF_AIRPORTS, st->index);
		}

		InvalidateWindowData(WC_STATION_VIEW, st->index, -1);
	}

	return cost;
}

/**
 * Get the upgraded piece type for a modular airport tile.
 * @param piece_type The current piece type.
 * @return The upgraded piece type, or 0xFF if no upgrade is available.
 */
static uint8_t GetUpgradedPieceType(uint8_t piece_type)
{
	switch (piece_type) {
		case APT_RUNWAY_SMALL_NEAR_END: return APT_RUNWAY_END;
		case APT_RUNWAY_SMALL_MIDDLE:   return APT_RUNWAY_5;
		case APT_RUNWAY_SMALL_FAR_END:  return APT_RUNWAY_END;
		case APT_SMALL_DEPOT_SE:        return APT_DEPOT_SE;
		case APT_SMALL_DEPOT_SW:        return APT_DEPOT_SW;
		case APT_SMALL_DEPOT_NW:        return APT_DEPOT_NW;
		case APT_SMALL_DEPOT_NE:        return APT_DEPOT_NE;
		case APT_GRASS_1:               return APT_APRON;
		default:                        return 0xFF;
	}
}

/**
 * Upgrade old modular airport tiles to modern variants in an area.
 * @param flags Command flags.
 * @param tile One corner of the area.
 * @param area_start Other corner of the area.
 * @return The cost of this operation or an error.
 */
CommandCost CmdUpgradeModularAirportTile(DoCommandFlags flags, TileIndex tile, TileIndex area_start)
{
	if (tile >= Map::Size() || area_start >= Map::Size()) return CMD_ERROR;

	CommandCost cost(EXPENSES_CONSTRUCTION);
	bool found_upgradeable = false;
	std::set<StationID> affected_stations;

	TileArea ta(tile, area_start);
	for (TileIndex t : ta) {
		if (!IsTileType(t, TileType::Station) || !IsAirport(t)) continue;

		Station *st = Station::GetByTile(t);
		if (st == nullptr || st->owner != _current_company) continue;
		if (!st->airport.blocks.Test(AirportBlock::Modular)) continue;

		ModularAirportTileData *md = st->airport.GetModularTileData(t);
		if (md == nullptr) continue;

		uint8_t new_piece = GetUpgradedPieceType(md->piece_type);
		if (new_piece == 0xFF) continue;

		/* Year-gate: modern pieces may not be available yet. */
		if (IsModernModularPiece(new_piece) &&
				TimerGameCalendar::year < GetModularPieceMinYear(new_piece)) {
			continue;
		}

		CommandCost ret = EnsureNoVehicleOnGround(t);
		if (ret.Failed()) return ret;

		found_upgradeable = true;

		/* Cost = removal + build of new piece (no discount). */
		cost.AddCost(_price[Price::ClearStationAirport]);
		cost.AddCost(GetModularAirportPieceBuildCost(new_piece));

		if (flags.Test(DoCommandFlag::Execute)) {
			/* Update map tile gfx and modular metadata. */
			uint8_t old_rotation = md->rotation;
			SetStationGfx(Tile(t), new_piece);
			md->piece_type = new_piece;
			md->auto_taxi_dir_mask = CalculateAutoTaxiDirectionsForGfx(new_piece, old_rotation);

			/* Normalize may further adjust gfx for the segment context. */
			if (IsModularRunwayPiece(new_piece)) {
				NormalizeRunwaySegmentVisuals(st, t, (old_rotation % 2) == 0);
			}

			st->airport.modular_tile_index_dirty = true;
			st->airport.modular_holding_loop_dirty = true;

			MarkTileDirtyByTile(t, 0, 8);
			if (TileX(t) > 0 && TileY(t) > 0) MarkTileDirtyByTile(t - TileDiffXY(1, 1));
			if (TileX(t) > 0) MarkTileDirtyByTile(t - TileDiffXY(1, 0));
			if (TileY(t) > 0) MarkTileDirtyByTile(t - TileDiffXY(0, 1));

			affected_stations.insert(st->index);
		}
	}

	if (!found_upgradeable) return CommandCost(STR_ERROR_NOTHING_TO_UPGRADE);

	/* Batch station updates after all tiles are upgraded. */
	if (flags.Test(DoCommandFlag::Execute)) {
		for (StationID sid : affected_stations) {
			Station *st = Station::GetIfValid(sid);
			if (st == nullptr) continue;
			st->AfterStationTileSetChange(true, StationType::Airport);
			InvalidateWindowData(WC_STATION_VIEW, st->index, -1);
		}
	}

	return cost;
}

CommandCost CmdBuildModularAirportTile(DoCommandFlags flags, TileIndex tile, uint16_t gfx, StationID station_to_join, bool allow_adjacent, uint8_t rotation, uint8_t taxi_dir_mask, bool one_way_taxi, bool auto_rotate_runway)
{
	bool reuse = (station_to_join != NEW_STATION);
	if (!reuse) station_to_join = StationID::Invalid();
	bool distant_join = (station_to_join != StationID::Invalid());

	if (distant_join && (!_settings_game.station.distant_join_stations || !Station::IsValidID(station_to_join))) return CMD_ERROR;

	if (gfx >= NUM_AIRPORTTILES) return CMD_ERROR;

	/* Modern pieces are unavailable before the city airport introduction year. */
	if (IsModernModularPiece(static_cast<uint8_t>(gfx)) &&
			TimerGameCalendar::year < GetModularPieceMinYear(static_cast<uint8_t>(gfx))) {
		return CommandCost(STR_ERROR_MODULAR_PIECE_NOT_YET_AVAILABLE);
	}

	CommandCost ret = CheckIfAuthorityAllowsNewStation(tile, flags);
	if (ret.Failed()) return ret;

	CommandCost cost(EXPENSES_CONSTRUCTION);
	int allowed_z = -1;

	/* Check if we're replacing an allowed modular airport tile.
	 * In that case, skip the landscape clear (which would fail with Auto flag or
	 * destroy station state) and just check for vehicles on the tile. */
	auto IsHangarPiece = [](uint8_t piece_type) {
		return piece_type == APT_DEPOT_SE || piece_type == APT_DEPOT_SW ||
				piece_type == APT_DEPOT_NW || piece_type == APT_DEPOT_NE ||
				piece_type == APT_SMALL_DEPOT_SE || piece_type == APT_SMALL_DEPOT_SW ||
				piece_type == APT_SMALL_DEPOT_NW || piece_type == APT_SMALL_DEPOT_NE;
	};
	auto IsReplaceableTile = [&](TileIndex t, uint8_t new_piece_type) {
		if (!IsTileType(t, TileType::Station) || !IsAirport(t)) return false;
		const Station *st = Station::GetByTile(t);
		if (st == nullptr || !st->airport.blocks.Test(AirportBlock::Modular)) return false;
		const ModularAirportTileData *md = st->airport.GetModularTileData(t);
		if (md == nullptr) return false;

		if (md->piece_type == APT_GRASS_1 || md->piece_type == APT_EMPTY) return true;

		const bool existing_hangar = IsHangarPiece(md->piece_type);
		const bool new_hangar = IsHangarPiece(new_piece_type);
		return existing_hangar && new_hangar;
	};
	bool is_modular_replace = IsReplaceableTile(tile, static_cast<uint8_t>(gfx));
	StationID existing_at_tile = is_modular_replace ? Station::GetByTile(tile)->index : StationID::Invalid();

	if (is_modular_replace) {
		ret = EnsureNoVehicleOnGround(tile);
		if (ret.Failed()) return ret;
		allowed_z = GetTileMaxZ(tile);
	} else {
		ret = CheckBuildableTile(tile, {}, allowed_z, true);
		if (ret.Failed()) return ret;
		cost.AddCost(ret.GetCost());

		ret = Command<CMD_LANDSCAPE_CLEAR>::Do(flags, tile);
		if (ret.Failed()) return ret;
		cost.AddCost(ret.GetCost());
	}

	TileArea airport_area(tile, 1, 1);

	Station *st = nullptr;
	ret = FindJoiningStation(existing_at_tile, station_to_join, allow_adjacent, airport_area, &st);
	if (ret.Failed()) return ret;

	/* Distant join */
	if (st == nullptr && distant_join) st = Station::GetIfValid(station_to_join);

	if (st != nullptr && st->facilities.Test(StationFacility::Airport) && !st->airport.blocks.Test(AirportBlock::Modular)) {
		return CommandCost(STR_ERROR_TOO_CLOSE_TO_ANOTHER_AIRPORT);
	}

	/* Enforce same height level across the entire modular airport. */
	if (st != nullptr && st->airport.blocks.Test(AirportBlock::Modular) &&
			st->airport.modular_tile_data != nullptr && !st->airport.modular_tile_data->empty()) {
		int existing_z = GetTileMaxZ(st->airport.modular_tile_data->front().tile);
		int new_z = GetTileMaxZ(tile);
		if (new_z != existing_z) {
			return CommandCost(STR_ERROR_FLAT_LAND_REQUIRED);
		}
	}

	ret = BuildStationPart(&st, flags, reuse, airport_area, STATIONNAMING_AIRPORT);
	if (ret.Failed()) return ret;

	cost.AddCost(GetModularAirportPieceBuildCost(static_cast<uint8_t>(gfx)));

	if (flags.Test(DoCommandFlag::Execute)) {
		bool new_facility = !st->facilities.Test(StationFacility::Airport);

		st->AddFacility(StationFacility::Airport, tile);
		if (new_facility) {
			st->airport.type = AT_SMALL;
			st->airport.layout = 0;
			st->airport.blocks = {};
			st->airport.blocks.Set(AirportBlock::Modular);
			st->airport.rotation = DIR_N;
			Company::Get(st->owner)->infrastructure.airport++;
		}
		st->airport.blocks.Set(AirportBlock::Modular);

		st->rect.BeforeAddTile(tile, StationRect::ADD_TRY);

		Tile t(tile);
		MakeAirport(t, st->owner, st->index, static_cast<uint8_t>(gfx), WaterClass::Invalid);
		SetStationTileRandomBits(t, GB(Random(), 0, 4));
		st->airport.Add(tile);

		if (AirportTileSpec::Get(GetTranslatedAirportTileID(static_cast<uint8_t>(gfx)))->animation.status != AnimationStatus::NoAnimation) AddAnimatedTile(t);
		TriggerAirportTileAnimation(st, tile, AirportAnimationTrigger::Built);

		/* Store modular airport tile data */
		st->airport.EnsureModularDataExists();

		/* Remove any existing data for this tile (in case of replacement) */
		auto &tile_data_vec = *st->airport.modular_tile_data;
		tile_data_vec.erase(
			std::remove_if(tile_data_vec.begin(), tile_data_vec.end(),
				[tile](const ModularAirportTileData &data) { return data.tile == tile; }),
			tile_data_vec.end()
		);
		st->airport.modular_tile_index_dirty = true;

		/* Create and store new tile data */
		ModularAirportTileData tile_data;
		tile_data.tile = tile;
		/* Convert canonical SE hangars to directional variants based on rotation.
		 * Only canonical forms (APT_DEPOT_SE / APT_SMALL_DEPOT_SE) are rotated here;
		 * template tiles arrive pre-rotated via RotateTemplateTile and pass through unchanged.
		 * Do NOT use SwapBuildingPieceForRotation here — it would double-rotate template tiles
		 * (which also apply it) and double-swap APT_BUILDING_1/2 and runway near/far ends. */
		uint8_t directional_piece = static_cast<uint8_t>(gfx);
		if (directional_piece == APT_DEPOT_SE) {
			static constexpr uint8_t kLargeByRot[] = {APT_DEPOT_SE, APT_DEPOT_NE, APT_DEPOT_NW, APT_DEPOT_SW};
			directional_piece = kLargeByRot[rotation % 4];
		} else if (directional_piece == APT_SMALL_DEPOT_SE) {
			static constexpr uint8_t kSmallByRot[] = {APT_SMALL_DEPOT_SE, APT_SMALL_DEPOT_NE, APT_SMALL_DEPOT_NW, APT_SMALL_DEPOT_SW};
			directional_piece = kSmallByRot[rotation % 4];
		}
		tile_data.piece_type = directional_piece;
		tile_data.rotation = rotation;
		tile_data.auto_taxi_dir_mask = CalculateAutoTaxiDirectionsForGfx(tile_data.piece_type, rotation);
		taxi_dir_mask &= 0x0F;

		/* One-way taxi only applies to taxiway/apron surface tiles and requires exactly one valid direction. */
		if (IsTaxiwayPiece(tile_data.piece_type) && one_way_taxi && HasExactlyOneBit(taxi_dir_mask) && (tile_data.auto_taxi_dir_mask & taxi_dir_mask) != 0) {
			tile_data.one_way_taxi = true;
			tile_data.user_taxi_dir_mask = taxi_dir_mask;
		} else {
			tile_data.one_way_taxi = false;
			tile_data.user_taxi_dir_mask = 0x0F;
		}
		auto is_runway_piece = [](uint8_t piece_type) {
			switch (piece_type) {
				case APT_RUNWAY_1:
				case APT_RUNWAY_2:
				case APT_RUNWAY_3:
				case APT_RUNWAY_4:
				case APT_RUNWAY_5:
				case APT_RUNWAY_END:
				case APT_RUNWAY_SMALL_NEAR_END:
				case APT_RUNWAY_SMALL_MIDDLE:
				case APT_RUNWAY_SMALL_FAR_END:
					return true;
				default:
					return false;
			}
		};

		/* Auto-detect axis: if the current rotation doesn't match any adjacent
		 * runway but the perpendicular axis does, flip to extend that runway.
		 * Only when auto_rotate_runway is set (single-click placement). */
		if (auto_rotate_runway && is_runway_piece(tile_data.piece_type)) {
			const bool horizontal = (rotation % 2) == 0;
			const TileIndexDiff same_diff = horizontal ? TileDiffXY(1, 0) : TileDiffXY(0, 1);
			bool has_same_axis = IsRunwayPieceOnAxis(st->airport.GetModularTileData(tile - same_diff), horizontal)
			                  || IsRunwayPieceOnAxis(st->airport.GetModularTileData(tile + same_diff), horizontal);
			if (!has_same_axis) {
				const TileIndexDiff perp_diff = horizontal ? TileDiffXY(0, 1) : TileDiffXY(1, 0);
				bool has_perp_axis = IsRunwayPieceOnAxis(st->airport.GetModularTileData(tile - perp_diff), !horizontal)
				                  || IsRunwayPieceOnAxis(st->airport.GetModularTileData(tile + perp_diff), !horizontal);
				if (has_perp_axis) {
					rotation ^= 1;
					tile_data.rotation = rotation;
					tile_data.auto_taxi_dir_mask = CalculateAutoTaxiDirectionsForGfx(tile_data.piece_type, rotation);
				}
			}
		}

		/* If this extends an existing runway, inherit its direction/usage flags. */
		if (is_runway_piece(tile_data.piece_type)) {
			const bool horizontal = (rotation % 2) == 0;
			const TileIndexDiff diff = horizontal ? TileDiffXY(1, 0) : TileDiffXY(0, 1);
			bool inherited_runway_flags = false;

			const ModularAirportTileData *prev = st->airport.GetModularTileData(tile - diff);
			if (IsRunwayPieceOnAxis(prev, horizontal)) {
				tile_data.runway_flags = prev->runway_flags;
				inherited_runway_flags = true;
			} else {
				const ModularAirportTileData *next = st->airport.GetModularTileData(tile + diff);
				if (IsRunwayPieceOnAxis(next, horizontal)) {
					tile_data.runway_flags = next->runway_flags;
					inherited_runway_flags = true;
				}
			}

			if (!inherited_runway_flags) {
				/* Default new isolated runways to one-way up-screen landing. */
				tile_data.runway_flags = RUF_LANDING | RUF_TAKEOFF | RUF_DIR_LOW;
			}
		}

		tile_data_vec.push_back(tile_data);
		st->airport.modular_tile_index_dirty = true;
		st->airport.modular_holding_loop_dirty = true;
		if (_show_holding_overlay) MarkWholeScreenDirty();

		/* Normalize runway end/middle visuals for the segment this tile belongs to. */
		if (IsModularRunwayPiece(tile_data.piece_type)) {
			NormalizeRunwaySegmentVisuals(st, tile, (tile_data.rotation % 2) == 0);
		}

		/* Mark tile and neighbors dirty to ensure tall building sprites
		 * (terminals, towers, radar) that extend beyond tile bounds are fully redrawn. */
		MarkTileDirtyByTile(tile, 0, 8);
		if (TileX(tile) > 0 && TileY(tile) > 0) MarkTileDirtyByTile(tile - TileDiffXY(1, 1));
		if (TileX(tile) > 0) MarkTileDirtyByTile(tile - TileDiffXY(1, 0));
		if (TileY(tile) > 0) MarkTileDirtyByTile(tile - TileDiffXY(0, 1));

		st->AfterStationTileSetChange(true, StationType::Airport);
		InvalidateWindowData(WC_STATION_VIEW, st->index, -1);
	}

	return cost;
}

/** Per-airport runway configuration: which Y-row gets which flags. */
struct StockRunwayConfig {
	int y_row;
	uint8_t runway_flags;
};

/**
 * Build a stock airport layout as a modular airport.
 * @param flags Command flags.
 * @param tile Top-left tile of the airport.
 * @param airport_type Type of airport (AT_SMALL, AT_LARGE, etc.).
 * @param layout Layout index.
 * @param station_to_join Station to join, or NEW_STATION.
 * @param allow_adjacent Whether to allow adjacent stations.
 * @return The cost or an error.
 */
CommandCost CmdBuildModularAirportFromStock(DoCommandFlags flags, TileIndex tile, uint8_t airport_type, uint8_t layout, StationID station_to_join, bool allow_adjacent)
{
	bool reuse = (station_to_join != NEW_STATION);
	if (!reuse) station_to_join = StationID::Invalid();
	bool distant_join = (station_to_join != StationID::Invalid());

	if (distant_join && (!_settings_game.station.distant_join_stations || !Station::IsValidID(station_to_join))) return CMD_ERROR;

	if (airport_type >= NUM_AIRPORTS) return CMD_ERROR;

	CommandCost ret = CheckIfAuthorityAllowsNewStation(tile, flags);
	if (ret.Failed()) return ret;

	const AirportSpec *as = AirportSpec::Get(airport_type);
	if (!as->IsAvailable() || layout >= as->layouts.size()) return CMD_ERROR;
	if (!as->IsWithinMapBounds(layout, tile)) return CMD_ERROR;

	Direction rotation = as->layouts[layout].rotation;
	int w = as->size_x;
	int h = as->size_y;
	if (rotation == DIR_E || rotation == DIR_W) std::swap(w, h);
	TileArea airport_area = TileArea(tile, w, h);

	if (w > _settings_game.station.station_spread || h > _settings_game.station.station_spread) {
		return CommandCost(STR_ERROR_STATION_TOO_SPREAD_OUT);
	}

	AirportTileTableIterator tile_iter(as->layouts[layout].tiles, tile);
	CommandCost cost = CheckFlatLandAirport(tile_iter, flags);
	if (cost.Failed()) return cost;

	/* Noise checks */
	uint dist;
	Town *nearest = AirportGetNearestTown(as, rotation, tile, std::move(tile_iter), dist);
	uint newnoise_level = GetAirportNoiseLevelForDistance(as, dist);

	StringID authority_refuse_message = STR_NULL;
	Town *authority_refuse_town = nullptr;

	if (_settings_game.economy.station_noise_level) {
		if ((nearest->noise_reached + newnoise_level) > nearest->MaxTownNoise()) {
			authority_refuse_message = STR_ERROR_LOCAL_AUTHORITY_REFUSES_NOISE;
			authority_refuse_town = nearest;
		}
	} else if (_settings_game.difficulty.town_council_tolerance != TOWN_COUNCIL_PERMISSIVE) {
		Town *t = ClosestTownFromTile(tile, UINT_MAX);
		uint num = 0;
		for (const Station *st : Station::Iterate()) {
			if (st->town == t && st->facilities.Test(StationFacility::Airport) && st->airport.type != AT_OILRIG) num++;
		}
		if (num >= 2) {
			authority_refuse_message = STR_ERROR_LOCAL_AUTHORITY_REFUSES_AIRPORT;
			authority_refuse_town = t;
		}
	}

	if (authority_refuse_message != STR_NULL) {
		return CommandCostWithParam(authority_refuse_message, authority_refuse_town->index);
	}

	Station *st = nullptr;
	ret = FindJoiningStation(StationID::Invalid(), station_to_join, allow_adjacent, airport_area, &st);
	if (ret.Failed()) return ret;

	if (st == nullptr && distant_join) st = Station::GetIfValid(station_to_join);

	ret = BuildStationPart(&st, flags, reuse, airport_area, GetAirport(airport_type)->flags.Test(AirportFTAClass::Flag::Airplanes) ? STATIONNAMING_AIRPORT : STATIONNAMING_HELIPORT);
	if (ret.Failed()) return ret;

	if (st != nullptr && st->airport.tile != INVALID_TILE) {
		return CommandCost(STR_ERROR_TOO_CLOSE_TO_ANOTHER_AIRPORT);
	}

	for (AirportTileTableIterator iter(as->layouts[layout].tiles, tile); iter != INVALID_TILE; ++iter) {
		TileIndex cur_tile = iter;
		int dx = TileX(cur_tile) - TileX(tile);
		int dy = TileY(cur_tile) - TileY(tile);
		uint8_t piece_type = MapStockGfxToModularPiece(iter.GetStationGfx());
		piece_type = ApplyStockTileOverride(airport_type, dx, dy, piece_type);
		cost.AddCost(GetModularAirportPieceBuildCost(piece_type));
	}

	if (flags.Test(DoCommandFlag::Execute)) {
		/* Per-airport runway configs */
		static const StockRunwayConfig country_runways[] = {
			{2, RUF_LANDING | RUF_TAKEOFF | RUF_DIR_LOW},
		};
		static const StockRunwayConfig commuter_runways[] = {
			{3, RUF_LANDING | RUF_TAKEOFF | RUF_DIR_LOW},
		};
		static const StockRunwayConfig city_runways[] = {
			{5, RUF_LANDING | RUF_TAKEOFF | RUF_DIR_LOW},
		};
		static const StockRunwayConfig metropolitan_runways[] = {
			{4, RUF_TAKEOFF | RUF_DIR_LOW},
			{5, RUF_LANDING | RUF_DIR_LOW},
		};
		static const StockRunwayConfig international_runways[] = {
			{0, RUF_TAKEOFF | RUF_DIR_HIGH},
			{6, RUF_LANDING | RUF_DIR_LOW},
		};
		static const StockRunwayConfig intercontinental_runways[] = {
			{0, RUF_LANDING | RUF_DIR_HIGH},
			{1, RUF_TAKEOFF | RUF_DIR_HIGH},
			{9, RUF_TAKEOFF | RUF_DIR_LOW},
			{10, RUF_LANDING | RUF_DIR_LOW},
		};

		/* Select configs based on airport type */
		std::span<const StockRunwayConfig> runway_configs;

		switch (airport_type) {
			case AT_SMALL:
				runway_configs = country_runways;
				break;
			case AT_COMMUTER:
				runway_configs = commuter_runways;
				break;
			case AT_LARGE:
				runway_configs = city_runways;
				break;
			case AT_METROPOLITAN:
				runway_configs = metropolitan_runways;
				break;
			case AT_INTERNATIONAL:
				runway_configs = international_runways;
				break;
			case AT_INTERCON:
				runway_configs = intercontinental_runways;
				break;
			default:
				break;
		}

		nearest->noise_reached += newnoise_level;

		st->AddFacility(StationFacility::Airport, tile);
		st->airport.type = airport_type;
		st->airport.layout = layout;
		st->airport.blocks = {};
		st->airport.blocks.Set(AirportBlock::Modular);
		st->airport.rotation = rotation;

		st->rect.BeforeAddRect(tile, w, h, StationRect::ADD_TRY);

		st->airport.EnsureModularDataExists();
		auto &tile_data_vec = *st->airport.modular_tile_data;

		for (AirportTileTableIterator iter(as->layouts[layout].tiles, tile); iter != INVALID_TILE; ++iter) {
			TileIndex cur_tile = iter;
			StationGfx stock_gfx = iter.GetStationGfx();

			/* Compute (dx, dy) offset from base tile */
			int dx = TileX(cur_tile) - TileX(tile);
			int dy = TileY(cur_tile) - TileY(tile);

			uint8_t piece_type = MapStockGfxToModularPiece(stock_gfx);
			piece_type = ApplyStockTileOverride(airport_type, dx, dy, piece_type);

			Tile t(cur_tile);
			MakeAirport(t, st->owner, st->index, piece_type, WaterClass::Invalid);
			SetStationTileRandomBits(t, GB(Random(), 0, 4));
			st->airport.Add(cur_tile);

			if (AirportTileSpec::Get(GetTranslatedAirportTileID(piece_type))->animation.status != AnimationStatus::NoAnimation) AddAnimatedTile(t);

			ModularAirportTileData tile_data;
			tile_data.tile = cur_tile;
			tile_data.piece_type = piece_type;
			tile_data.rotation = 0; /* Stock layout 0 = DIR_N; all horizontal runways */
			tile_data.auto_taxi_dir_mask = CalculateAutoTaxiDirectionsForGfx(piece_type, 0);
			tile_data.one_way_taxi = false;
			tile_data.user_taxi_dir_mask = 0x0F;
			tile_data.runway_flags = 0;
			tile_data.edge_block_mask = GetStockFenceEdgeMask(stock_gfx);

			tile_data_vec.push_back(tile_data);
		}

		/* Runway flags post-pass: set flags on all runway tiles in the configured Y-rows */
		for (const auto &rc : runway_configs) {
			for (auto &td : tile_data_vec) {
				if (!IsModularRunwayPiece(td.piece_type)) continue;
				/* Compute Y offset of this tile from base */
				int td_dy = TileY(td.tile) - TileY(tile);
				if (td_dy == rc.y_row) {
					td.runway_flags = rc.runway_flags;
				}
			}
		}

		/* Fence edge mirroring post-pass: for each edge fence, set the opposite
		 * edge on the neighbor tile so the pathfinder sees fences from both sides. */
		static constexpr struct { int8_t dx, dy; uint8_t bit, opposite; } kFenceEdges[] = {
			{  0, -1, 0x01, 0x04 }, // NW edge → neighbor's SE
			{ +1,  0, 0x02, 0x08 }, // SW edge → neighbor's NE
			{  0, +1, 0x04, 0x01 }, // SE edge → neighbor's NW
			{ -1,  0, 0x08, 0x02 }, // NE edge → neighbor's SW
		};
		for (const auto &td : tile_data_vec) {
			if (td.edge_block_mask == 0) continue;
			for (const auto &e : kFenceEdges) {
				if ((td.edge_block_mask & e.bit) == 0) continue;
				TileIndex nb = TileAddXY(td.tile, e.dx, e.dy);
				for (auto &nb_td : tile_data_vec) {
					if (nb_td.tile == nb) {
						nb_td.edge_block_mask |= e.opposite;
						break;
					}
				}
			}
		}

		st->airport.modular_tile_index_dirty = true;
		st->airport.modular_holding_loop_dirty = true;

		/* Trigger animations */
		for (AirportTileTableIterator iter(as->layouts[layout].tiles, tile); iter != INVALID_TILE; ++iter) {
			TriggerAirportTileAnimation(st, iter, AirportAnimationTrigger::Built);
		}

		UpdateAirplanesOnNewStation(st);

		Company::Get(st->owner)->infrastructure.airport++;

		st->AfterStationTileSetChange(true, StationType::Airport);
		InvalidateWindowData(WC_STATION_VIEW, st->index, -1);

		if (_settings_game.economy.station_noise_level) {
			SetWindowDirty(WC_TOWN_VIEW, nearest->index);
		}

		if (_show_holding_overlay) MarkWholeScreenDirty();
	}

	return cost;
}
