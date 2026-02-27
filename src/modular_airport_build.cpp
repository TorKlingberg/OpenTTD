/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file modular_airport_build.cpp Helpers for modular airport build/remove commands. */

#include "stdafx.h"

#include "modular_airport_build.h"

#include "clear_func.h"
#include "company_base.h"
#include "company_func.h"
#include "economy_func.h"
#include "landscape.h"
#include "modular_airport_cmd.h"
#include "modular_airport_gui.h"
#include "newgrf_debug.h"
#include "station_map.h"
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
		case APT_ARPON_N:
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
			return APT_LOW_BUILDING;
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
	{0, 0, APT_GRASS_1}, {2, 0, APT_GRASS_1},
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
		case APT_ARPON_N:
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
