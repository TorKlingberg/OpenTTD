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
#include "order_backup.h"
#include "order_func.h"
#include "station_cmd.h"
#include "station_map.h"
#include "timer/timer_game_calendar.h"
#include "town.h"
#include "vehicle_func.h"
#include "viewport_func.h"
#include "window_func.h"

#include "table/airporttile_ids.h"
#include "table/strings.h"

static void InitializeNewModularAirport(Airport &airport)
{
	airport.type = AT_MODULAR;
	airport.layout = 0;
	airport.blocks = {};
	airport.blocks.Set(AirportBlock::Modular);
	airport.rotation = Direction::N;
}

static void CollectRunwayFamilySegment(Station *st, TileIndex start, TileIndexDiff diff, bool horizontal, bool family_large, std::vector<TileIndex> &tiles)
{
	TileIndex cur = start;
	while (true) {
		ModularAirportTileData *data = st->airport.GetModularTileData(cur);
		if (!IsRunwayPieceOnAxis(data, horizontal)) break;
		bool is_large = IsLargeRunwayFamily(data->piece_type);
		bool is_small = IsLegacySmallRunwayPiece(data->piece_type);
		if (family_large && !is_large) break;
		if (!family_large && !is_small) break;
		tiles.push_back(cur);
		cur = cur + diff;
	}
}

static void NormalizeRunwaySegmentVisuals(Station *st, TileIndex changed_tile, bool horizontal)
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
	bool retyped_any = false;
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
				retyped_any = true;
			}
		}

		cur = seg.back() + diff;
	}

	/* Retyping a tile is a layout change in its own right: which tiles are runway
	 * *ends* decides whether the airport has a large-safe runway, among other cached
	 * answers. Callers already mark the layout dirty for the add/remove that brought
	 * us here, but they do so before this runs, so a read in between would cache a
	 * pre-normalization answer and never be invalidated again. */
	if (retyped_any) st->airport.MarkLayoutDirty();
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
			return APT_HELIPORT;
		case APT_HELIPAD_1:
		case APT_HELIPAD_2_FENCE_NW:
		case APT_HELIPAD_2:
		case APT_HELIPAD_2_FENCE_NE_SE:
			return APT_HELIPAD_2;
		case APT_HELIPAD_3_FENCE_SE_SW:
		case APT_HELIPAD_3_FENCE_NW_SW:
		case APT_HELIPAD_3_FENCE_NW:
			return APT_HELIPAD_3_FENCE_NW;
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

uint8_t ApplyStockTileOverride(uint8_t airport_type, int dx, int dy, uint8_t piece_type)
{
	if (airport_type == AT_SMALL && dy == 1) {
		if (dx == 0 || dx == 3) return APT_APRON;
		if (dx == 1 || dx == 2) return APT_STAND;
	}
	return piece_type;
}

static Money ScaleModularAirportCost(Money base, uint16_t percent)
{
	return static_cast<Money>((static_cast<int64_t>(base) * percent + 50) / 100);
}

static Money GetModularAirportPieceBuildCost(uint8_t piece_type)
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

ModularAirportNoiseSnapshot GetModularAirportNoiseSnapshot(std::span<const ModularAirportNoisePiece> pieces)
{
	ModularAirportNoiseSnapshot result;
	if (pieces.empty()) return result;

	std::vector<TileIndex> tiles;
	std::vector<uint8_t> piece_types;
	tiles.reserve(pieces.size());
	piece_types.reserve(pieces.size());
	for (const ModularAirportNoisePiece &piece : pieces) {
		tiles.push_back(piece.tile);
		piece_types.push_back(piece.piece_type);
	}

	uint distance;
	result.town = AirportGetNearestTown(tiles, distance);
	if (result.town != nullptr) {
		result.level = GetAirportNoiseLevelForDistance(GetModularAirportNoiseLevelFromPieces(piece_types), distance);
	}
	return result;
}

ModularAirportNoiseSnapshot GetModularAirportNoiseSnapshot(const Station *st)
{
	if (!st->facilities.Test(StationFacility::Airport) || st->airport.tile == INVALID_TILE ||
			st->airport.modular_tile_data == nullptr || st->airport.modular_tile_data->empty()) {
		return {};
	}

	uint distance;
	Town *town = AirportGetNearestTown(st, distance);
	return {town, static_cast<uint8_t>(town != nullptr ? GetAirportNoiseLevelForDistance(st, distance) : 0)};
}

CommandCost CheckModularAirportNoiseChange(const ModularAirportNoiseSnapshot &before, const ModularAirportNoiseSnapshot &after)
{
	if (!_settings_game.economy.station_noise_level || after.town == nullptr) return CommandCost();

	uint projected = after.town->noise_reached + after.level;
	if (before.town == after.town) projected -= before.level;
	if (projected > after.town->MaxTownNoise()) {
		return CommandCostWithParam(STR_ERROR_LOCAL_AUTHORITY_REFUSES_NOISE, after.town->index);
	}
	return CommandCost();
}

/**
 * Apply one modular-airport command's town-noise delta after its final layout exists.
 *
 * Capture @p before before the first mutation and before MarkLayoutDirty invalidates
 * the old cached level. Call this once per affected station, after all tiles in the
 * command have been added, removed, replaced or upgraded. Both the nearest town and
 * the distance-reduced total can change, so intermediate per-tile deltas are invalid.
 */
void ApplyModularAirportNoiseChange(const Station *st, const ModularAirportNoiseSnapshot &before)
{
	const ModularAirportNoiseSnapshot after = GetModularAirportNoiseSnapshot(st);
	if (before.town != nullptr) before.town->noise_reached -= before.level;
	if (after.town != nullptr) after.town->noise_reached += after.level;

	if (_settings_game.economy.station_noise_level) {
		if (before.town != nullptr) SetWindowDirty(WindowClass::TownView, before.town->index);
		if (after.town != nullptr && after.town != before.town) SetWindowDirty(WindowClass::TownView, after.town->index);
	}
}

/**
 * Cancel every aircraft hangar order aimed at @p st once its last hangar is gone.
 *
 * Stock does this from UpdateAirplanesOnNewStation, which runs when an airport is rebuilt
 * as one without a hangar. A modular airport instead loses its hangar a tile at a time, so
 * the same cleanup has to hang off the tile mutation paths. Without it an order issued
 * while the hangar still stood keeps sending the aircraft to an airport that can no longer
 * serve it -- it lands, finds no hangar, and leaves again on a loop.
 *
 * Call this once per command, on the finished layout -- never from inside a per-tile loop.
 * Building is not monotonic: a template that replaces the old hangar early and lays its own
 * down later passes through a hangarless moment that says nothing about the result, and
 * purging there would drop orders the finished airport can still serve. (Removal alone
 * would be safe, since hangars only ever disappear, but there is no reason to special-case
 * it.) A no-op while any hangar remains.
 */
void CancelModularHangarOrdersIfNoneLeft(const Station *st)
{
	if (!st->airport.blocks.Test(AirportBlock::Modular)) return;
	if (st->airport.HasHangar()) return;
	RemoveOrderFromAllVehicles(OT_GOTO_DEPOT, st->index, true);
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

	CommandCost cost(ExpensesType::Construction);
	cost.AddCost(_price[Price::ClearStationAirport] * static_cast<uint>(tiles_to_remove.size()));

	if (flags.Test(DoCommandFlag::Execute)) {
		const ModularAirportNoiseSnapshot noise_before = GetModularAirportNoiseSnapshot(st);
		std::vector<std::pair<TileIndex, uint8_t>> removed_runway_tiles;

		for (TileIndex t : tiles_to_remove) {
			const ModularAirportTileData *md = (st->airport.modular_tile_data != nullptr) ? st->airport.GetModularTileData(t) : nullptr;
			if (md != nullptr) {
				if (IsModularRunwayPiece(md->piece_type)) {
					removed_runway_tiles.push_back({t, md->rotation});
				}
				if (IsModularHangarPiece(md->piece_type)) {
					OrderBackup::Reset(t, false);
					CloseWindowById(WindowClass::VehicleDepot, t);
				}
			}
			DoClearSquare(t);
			DeleteNewGRFInspectWindow(GrfSpecFeature::AirportTiles, t.base());
			st->rect.AfterRemoveTile(st, t);
		}

		if (st->airport.modular_tile_data != nullptr) {
			auto &tile_data_vec = *st->airport.modular_tile_data;
			tile_data_vec.erase(std::remove_if(tile_data_vec.begin(), tile_data_vec.end(),
				[&](const ModularAirportTileData &data) {
					return std::find(tiles_to_remove.begin(), tiles_to_remove.end(), data.tile) != tiles_to_remove.end();
				}), tile_data_vec.end());
			st->airport.modular_tile_index_dirty = true;
			st->airport.MarkLayoutDirty();
			CancelModularHangarOrdersIfNoneLeft(st);
			/* Tiles that left the layout classify differently from the pieces they were. */
			RefreshModularAircraftPathSegments(st);
			if (_show_holding_overlay) MarkWholeScreenDirty();

			for (const auto &[removed_tile, removed_rotation] : removed_runway_tiles) {
				const bool horizontal = (removed_rotation % 2) == 0;
				const TileIndexDiff diff = horizontal ? TileDiffXY(1, 0) : TileDiffXY(0, 1);
				const TileIndex neighbor_lo = removed_tile - diff;
				const TileIndex neighbor_hi = removed_tile + diff;
				if (st->airport.GetModularTileData(neighbor_lo) != nullptr) {
					NormalizeRunwaySegmentVisuals(st, neighbor_lo, horizontal);
				}
				if (st->airport.GetModularTileData(neighbor_hi) != nullptr) {
					NormalizeRunwaySegmentVisuals(st, neighbor_hi, horizontal);
				}
			}
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
			st->airport.MarkLayoutDirty();
			if (_show_holding_overlay) MarkWholeScreenDirty();
			st->airport.Clear();
			st->facilities.Reset(StationFacility::Airport);
			SetWindowClassesDirty(WindowClass::VehicleOrders);
			Company::Get(st->owner)->infrastructure.airport--;
			st->AfterStationTileSetChange(false, StationType::Airport);
			DeleteNewGRFInspectWindow(GrfSpecFeature::Airports, st->index);
		}
		ApplyModularAirportNoiseChange(st, noise_before);

		InvalidateWindowData(WindowClass::StationView, st->index, -1);
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

	struct UpgradeTarget {
		TileIndex tile;
		Station *station;
		ModularAirportTileData *data;
		uint8_t new_piece;
	};

	CommandCost cost(ExpensesType::Construction);
	std::vector<UpgradeTarget> targets;
	std::set<StationID> affected_stations;
	std::map<StationID, ModularAirportNoiseSnapshot> noise_before;

	/* Preflight the complete area before changing any tile. Besides making the
	 * command safe when called directly in tests or by another command, this is
	 * what guarantees a runway is never left half upgraded when an aircraft is
	 * standing on a later tile. */
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

		/* Cost = removal + build of new piece (no discount). */
		cost.AddCost(_price[Price::ClearStationAirport]);
		cost.AddCost(GetModularAirportPieceBuildCost(new_piece));
		targets.push_back({t, st, md, new_piece});
	}

	if (targets.empty()) return CommandCost(STR_ERROR_NOTHING_TO_UPGRADE);

	if (flags.Test(DoCommandFlag::Execute)) {
		for (const UpgradeTarget &target : targets) {
			Station *st = target.station;
			ModularAirportTileData *md = target.data;
			const TileIndex t = target.tile;
			noise_before.try_emplace(st->index, GetModularAirportNoiseSnapshot(st));
			/* Update map tile gfx and modular metadata. */
			uint8_t old_rotation = md->rotation;
			SetStationGfx(Tile(t), target.new_piece);
			md->piece_type = target.new_piece;
			md->auto_taxi_dir_mask = CalculateAutoTaxiDirectionsForGfx(target.new_piece, old_rotation);

			/* Normalize may further adjust gfx for the segment context. */
			if (IsModularRunwayPiece(target.new_piece)) {
				NormalizeRunwaySegmentVisuals(st, t, (old_rotation % 2) == 0);
			}

			st->airport.modular_tile_index_dirty = true;
			st->airport.MarkLayoutDirty();

			MarkTileDirtyByTile(t, 0, 8);
			if (TileX(t) > 0 && TileY(t) > 0) MarkTileDirtyByTile(t - TileDiffXY(1, 1));
			if (TileX(t) > 0) MarkTileDirtyByTile(t - TileDiffXY(1, 0));
			if (TileY(t) > 0) MarkTileDirtyByTile(t - TileDiffXY(0, 1));

			affected_stations.insert(st->index);
		}

		/* Batch station updates after all tiles are upgraded. */
		for (StationID sid : affected_stations) {
			Station *st = Station::GetIfValid(sid);
			if (st == nullptr) continue;
			/* An upgrade can have retyped the last hangar into something else. */
			CancelModularHangarOrdersIfNoneLeft(st);
			/* EnsureNoVehicleOnGround only clears the upgraded tiles themselves, so an
			 * aircraft elsewhere on the airport can hold a path across one of them whose
			 * cached segment types the retype has just invalidated. */
			RefreshModularAircraftPathSegments(st);
			ApplyModularAirportNoiseChange(st, noise_before.at(sid));
			st->AfterStationTileSetChange(true, StationType::Airport);
			InvalidateWindowData(WindowClass::StationView, st->index, -1);
		}
	}

	return cost;
}

/**
 * Check if a modular airport tile can be built.
 * @param flags Command flags.
 * @param tile Tile to build on.
 * @param gfx Piece type.
 * @param station_to_join Station to join, or INVALID_STATION.
 * @param allow_adjacent Whether to allow adjacent stations.
 * @param st [in/out] Reference to station pointer.
 * @param is_modular_replace [out] Reference to replacement flag.
 * @param cost [in/out] Accumulated construction cost.
 * @return Success or error code.
 */
CommandCost BuildModularAirportTile_Check(DoCommandFlags flags, TileIndex tile, uint16_t gfx, StationID station_to_join, bool allow_adjacent, Station *&st, bool &is_modular_replace, CommandCost &cost, bool check_noise)
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
		const Station *st_local = Station::GetByTile(t);
		if (st_local == nullptr || !st_local->airport.blocks.Test(AirportBlock::Modular)) return false;
		const ModularAirportTileData *md = st_local->airport.GetModularTileData(t);
		if (md == nullptr) return false;

		if (md->piece_type == APT_GRASS_1 || md->piece_type == APT_EMPTY) return true;

		const bool existing_hangar = IsHangarPiece(md->piece_type);
		const bool new_hangar = IsHangarPiece(new_piece_type);
		return existing_hangar && new_hangar;
	};
	is_modular_replace = IsReplaceableTile(tile, static_cast<uint8_t>(gfx));
	StationID existing_at_tile = is_modular_replace ? Station::GetByTile(tile)->index : StationID::Invalid();

	/* Replacing a piece within an existing modular airport takes no new land,
	 * so the town authority has no say; only builds onto new tiles are gated. */
	CommandCost ret;
	if (!is_modular_replace) {
		ret = CheckIfAuthorityAllowsNewStation(tile, flags);
		if (ret.Failed()) return ret;
	}

	if (is_modular_replace) {
		ret = EnsureNoVehicleOnGround(tile);
		if (ret.Failed()) return ret;
		allowed_z = GetTileMaxZ(tile);
	} else {
		ret = CheckBuildableTile(tile, {}, allowed_z, true);
		if (ret.Failed()) return ret;
		cost.AddCost(ret.GetCost());

		/* Always test landscape clear in Check phase. Apply path will do the real clear. */
		ret = Command<Commands::LandscapeClear>::Do(DoCommandFlags{flags}.Reset(DoCommandFlag::Execute), tile);
		if (ret.Failed()) return ret;
		cost.AddCost(ret.GetCost());
	}

	TileArea airport_area(tile, 1, 1);

	if (st == nullptr) {
		ret = FindJoiningStation(existing_at_tile, station_to_join, allow_adjacent, airport_area, &st);
		if (ret.Failed()) return ret;

		/* Distant join */
		if (st == nullptr && distant_join) st = Station::GetIfValid(station_to_join);
	}

	if (st != nullptr && st->facilities.Test(StationFacility::Airport) && !st->airport.blocks.Test(AirportBlock::Modular)) {
		return CommandCost(STR_ERROR_TOO_CLOSE_TO_ANOTHER_AIRPORT);
	}

	const bool new_facility = st == nullptr || !st->facilities.Test(StationFacility::Airport);

	/* Only worth building when we are going to test it. Template placement calls this
	 * once per tile with check_noise false, and each snapshot walks the whole layout
	 * looking for the nearest town -- computing them anyway makes a placement quadratic
	 * in its own tile count for an answer that is thrown away. */
	if (check_noise) {
		std::vector<ModularAirportNoisePiece> future_pieces;
		if (!new_facility && st->airport.modular_tile_data != nullptr) {
			future_pieces.reserve(st->airport.modular_tile_data->size() + 1);
			for (const ModularAirportTileData &data : *st->airport.modular_tile_data) {
				if (data.tile != tile) future_pieces.push_back({data.tile, data.piece_type});
			}
		}
		future_pieces.push_back({tile, static_cast<uint8_t>(gfx)});
		const ModularAirportNoiseSnapshot noise_before = new_facility ? ModularAirportNoiseSnapshot{} : GetModularAirportNoiseSnapshot(st);
		const ModularAirportNoiseSnapshot noise_after = GetModularAirportNoiseSnapshot(future_pieces);
		ret = CheckModularAirportNoiseChange(noise_before, noise_after);
		if (ret.Failed()) return ret;
	}

	if (new_facility && !_settings_game.economy.station_noise_level &&
			_settings_game.difficulty.town_council_tolerance != TOWN_COUNCIL_PERMISSIVE) {
		Town *t = ClosestTownFromTile(tile, UINT_MAX);
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
		int new_z = GetTileMaxZ(tile);
		if (new_z != existing_z) {
			return CommandCost(STR_ERROR_FLAT_LAND_REQUIRED);
		}
	}

	/* The name is chosen once, when the first tile creates the station, so this path
	 * has to answer "airport or heliport?" from a single piece -- the template and
	 * from-stock paths derive it from their finished layout, which does not exist yet
	 * here.
	 *
	 * A helipad is the one piece that says something: nothing but a helicopter uses
	 * one, and a player laying a pad down first is building a heliport. Every other
	 * piece -- apron, stand, hangar, runway, building -- appears in both kinds of
	 * airport, so it gets the generic name. Asking instead whether this one tile is a
	 * runway (or whether the one-tile layout accepts planes) names an airport begun
	 * with an apron "Heliport" for the rest of the game.
	 *
	 * Order dependence is not fully removable here: a large airport whose first tile
	 * happens to be a helipad still comes out "Heliport". What this does buy is the
	 * case that matters -- a hand-built heliport now gets the same name as the stock
	 * heliport built as modular. */
	const StationNaming naming = IsModularHelipadPiece(static_cast<uint8_t>(gfx)) ? StationNaming::Heliport : StationNaming::Airport;
	ret = BuildStationPart(&st, flags, reuse, airport_area, naming);
	if (ret.Failed()) return ret;

	cost.AddCost(GetModularAirportPieceBuildCost(static_cast<uint8_t>(gfx)));

	return CommandCost();
}

/**
 * Apply the mutation for a modular airport tile.
 * @param tile Tile to build on.
 * @param gfx Piece type.
 * @param st Station to add tile to.
 * @param is_modular_replace Whether an existing tile is being replaced.
 * @param rotation Rotation of the piece.
 * @param taxi_dir_mask User taxi direction mask.
 * @param one_way_taxi Whether taxi direction is one-way.
 * @param auto_rotate_runway Whether to automatically rotate runways based on neighbors.
 */
void BuildModularAirportTile_Apply(TileIndex tile, uint16_t gfx, Station *st, bool is_modular_replace, uint8_t rotation, uint8_t taxi_dir_mask, bool one_way_taxi, bool auto_rotate_runway)
{
	const bool new_facility = !st->facilities.Test(StationFacility::Airport);

	if (!is_modular_replace) {
		CommandCost ret = Command<Commands::LandscapeClear>::Do(DoCommandFlag::Execute, tile);
		assert(ret.Succeeded());
	}

	st->AddFacility(StationFacility::Airport, tile);
	if (new_facility) {
		InitializeNewModularAirport(st->airport);
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
	 * Do NOT use SwapBuildingPieceForRotation here -- it would double-rotate template tiles
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
	/* Auto-detect axis: if the current rotation doesn't match any adjacent
	 * runway but the perpendicular axis does, flip to extend that runway.
	 * Only when auto_rotate_runway is set (single-click placement). */
	if (auto_rotate_runway && IsModularRunwayPiece(tile_data.piece_type)) {
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
	if (IsModularRunwayPiece(tile_data.piece_type)) {
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
	st->airport.MarkLayoutDirty();
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
	InvalidateWindowData(WindowClass::StationView, st->index, -1);
}

CommandCost CmdBuildModularAirportTile(DoCommandFlags flags, TileIndex tile, uint16_t gfx, StationID station_to_join, bool allow_adjacent, uint8_t rotation, uint8_t taxi_dir_mask, bool one_way_taxi, bool auto_rotate_runway)
{
	Station *st = nullptr;
	bool is_modular_replace = false;
	CommandCost cost(ExpensesType::Construction);

	CommandCost ret = BuildModularAirportTile_Check(flags, tile, gfx, station_to_join, allow_adjacent, st, is_modular_replace, cost);
	if (ret.Failed()) return ret;

	if (flags.Test(DoCommandFlag::Execute)) {
		const ModularAirportNoiseSnapshot noise_before = GetModularAirportNoiseSnapshot(st);
		BuildModularAirportTile_Apply(tile, gfx, st, is_modular_replace, rotation, taxi_dir_mask, one_way_taxi, auto_rotate_runway);
		ApplyModularAirportNoiseChange(st, noise_before);
		/* A replace can have overwritten the last hangar. */
		CancelModularHangarOrdersIfNoneLeft(st);
		/* ...and can have retyped a tile some other aircraft's path runs across. */
		RefreshModularAircraftPathSegments(st);
	}

	return cost;
}

/** Per-airport runway configuration: which Y-row gets which flags. */
struct StockRunwayConfig {
	int y_row;
	uint8_t runway_flags;
};

static constexpr StockRunwayConfig _country_runways[] = {
	{2, RUF_LANDING | RUF_TAKEOFF | RUF_DIR_LOW},
};
static constexpr StockRunwayConfig _commuter_runways[] = {
	{3, RUF_LANDING | RUF_TAKEOFF | RUF_DIR_LOW},
};
static constexpr StockRunwayConfig _city_runways[] = {
	{5, RUF_LANDING | RUF_TAKEOFF | RUF_DIR_LOW},
};
static constexpr StockRunwayConfig _metropolitan_runways[] = {
	{4, RUF_TAKEOFF | RUF_DIR_LOW},
	{5, RUF_LANDING | RUF_DIR_LOW},
};
static constexpr StockRunwayConfig _international_runways[] = {
	{0, RUF_TAKEOFF | RUF_DIR_HIGH},
	{6, RUF_LANDING | RUF_DIR_LOW},
};
static constexpr StockRunwayConfig _intercontinental_runways[] = {
	{0, RUF_LANDING | RUF_DIR_HIGH},
	{1, RUF_TAKEOFF | RUF_DIR_HIGH},
	{9, RUF_TAKEOFF | RUF_DIR_LOW},
	{10, RUF_LANDING | RUF_DIR_LOW},
};

static std::span<const StockRunwayConfig> GetStockRunwayConfigs(uint8_t airport_type)
{
	switch (airport_type) {
		case AT_SMALL: return _country_runways;
		case AT_COMMUTER: return _commuter_runways;
		case AT_LARGE: return _city_runways;
		case AT_METROPOLITAN: return _metropolitan_runways;
		case AT_INTERNATIONAL: return _international_runways;
		case AT_INTERCON: return _intercontinental_runways;
		default: return {};
	}
}

static uint8_t GetStockModularRunwayFlags(std::span<const StockRunwayConfig> configs, uint8_t piece_type, int dy)
{
	if (!IsModularRunwayPiece(piece_type)) return RUF_DEFAULT;
	for (const StockRunwayConfig &config : configs) {
		if (config.y_row == dy) return config.runway_flags;
	}
	return RUF_DEFAULT;
}

std::vector<ModularAirportTileData> ConvertStockAirportLayoutToModular(uint8_t airport_type, uint8_t layout, TileIndex base_tile)
{
	const AirportSpec *as = AirportSpec::Get(airport_type);
	const std::span<const StockRunwayConfig> runway_configs = GetStockRunwayConfigs(airport_type);
	std::vector<ModularAirportTileData> result;

	for (AirportTileTableIterator iter(as->layouts[layout].tiles, base_tile); iter != INVALID_TILE; ++iter) {
		const TileIndex tile = iter;
		const int dx = TileX(tile) - TileX(base_tile);
		const int dy = TileY(tile) - TileY(base_tile);
		const StationGfx stock_gfx = iter.GetStationGfx();
		const uint8_t piece_type = ApplyStockTileOverride(airport_type, dx, dy, MapStockGfxToModularPiece(stock_gfx));

		ModularAirportTileData data;
		data.tile = tile;
		data.piece_type = piece_type;
		data.rotation = 0;
		data.auto_taxi_dir_mask = CalculateAutoTaxiDirectionsForGfx(piece_type, 0);
		data.one_way_taxi = false;
		data.user_taxi_dir_mask = 0x0F;
		data.runway_flags = GetStockModularRunwayFlags(runway_configs, piece_type, dy);
		data.edge_block_mask = GetStockFenceEdgeMask(stock_gfx);
		result.push_back(data);
	}

	/* Tile-by-tile construction canonicalizes each completed runway to its
	 * family-specific end/middle pieces. Do the same before from-stock state is
	 * installed so identical layouts do not retain different saved piece IDs. */
	auto find_data = [&](TileIndex tile) -> ModularAirportTileData * {
		auto it = std::find_if(result.begin(), result.end(),
				[=](const ModularAirportTileData &candidate) { return candidate.tile == tile; });
		return it != result.end() ? &*it : nullptr;
	};
	for (ModularAirportTileData &data : result) {
		if (!IsModularRunwayPiece(data.piece_type)) continue;
		const bool large_family = IsLargeRunwayFamily(data.piece_type);
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

	/* GetStockFenceEdgeMask() reproduces whatever edges the stock gfx variant baked a
	 * fence line into, which is either a genuine internal partition between two stock
	 * tiles (rare, e.g. an apron divider) or, far more commonly, just the airport's
	 * outer boundary at conversion time. Modular airports already draw an automatic
	 * fence along any edge that isn't shared with another airport tile (the "perimeter"
	 * check in DrawModularAirportPerimeterFences), and that check keeps working
	 * correctly as tiles are added or removed later. A perimeter-only bit baked into
	 * edge_block_mask is therefore both redundant at conversion time and wrong once the
	 * airport grows past it -- it would keep drawing (and taxi-blocking) a fence deep
	 * inside the layout. Keep the bit only where the neighbour is also part of this
	 * layout, i.e. a genuine internal partition; mirror it onto that neighbour so both
	 * sides agree, matching how the in-game fence tool stores a partition. Drop it
	 * everywhere else. */
	static constexpr struct { int8_t dx, dy; uint8_t bit, opposite; } fence_edges[] = {
		{  0, -1, 0x01, 0x04},
		{ +1,  0, 0x02, 0x08},
		{  0, +1, 0x04, 0x01},
		{ -1,  0, 0x08, 0x02},
	};
	for (ModularAirportTileData &data : result) {
		const uint8_t original_mask = data.edge_block_mask;
		for (const auto &edge : fence_edges) {
			if ((original_mask & edge.bit) == 0) continue;
			const TileIndex neighbour = TileAddXY(data.tile, edge.dx, edge.dy);
			auto it = std::find_if(result.begin(), result.end(), [=](const ModularAirportTileData &candidate) { return candidate.tile == neighbour; });
			if (it != result.end()) {
				it->edge_block_mask |= edge.opposite;
			} else {
				data.edge_block_mask &= ~edge.bit;
			}
		}
	}

	return result;
}

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

	/* NewGRF layouts can contain rotated and GRF-defined tiles whose modular
	 * movement semantics cannot currently be inferred from their FSM. */
	if (airport_type >= NEW_AIRPORT_OFFSET) return CMD_ERROR;

	CommandCost ret = CheckIfAuthorityAllowsNewStation(tile, flags);
	if (ret.Failed()) return ret;

	const AirportSpec *as = AirportSpec::Get(airport_type);
	if (!as->IsAvailable() || layout >= as->layouts.size()) return CMD_ERROR;
	if (!as->IsWithinMapBounds(layout, tile)) return CMD_ERROR;

	Direction rotation = as->layouts[layout].rotation;
	int w = as->size_x;
	int h = as->size_y;
	if (rotation == Direction::E || rotation == Direction::W) std::swap(w, h);
	TileArea airport_area = TileArea(tile, w, h);

	if (w > _settings_game.station.station_spread || h > _settings_game.station.station_spread) {
		return CommandCost(STR_ERROR_STATION_TOO_SPREAD_OUT);
	}

	AirportTileTableIterator tile_iter(as->layouts[layout].tiles, tile);
	CommandCost cost = CheckFlatLandAirport(tile_iter, flags);
	if (cost.Failed()) return cost;

	const std::vector<ModularAirportTileData> converted_data = ConvertStockAirportLayoutToModular(airport_type, layout, tile);
	std::vector<ModularAirportNoisePiece> future_noise_pieces;
	std::vector<ModularAirportCapabilityPiece> future_capability_pieces;
	for (const ModularAirportTileData &data : converted_data) {
		future_noise_pieces.push_back({data.tile, data.piece_type});
		future_capability_pieces.push_back({data.piece_type, data.runway_flags});
	}
	const ModularAirportNoiseSnapshot noise_after = GetModularAirportNoiseSnapshot(future_noise_pieces);

	StringID authority_refuse_message = STR_NULL;
	Town *authority_refuse_town = nullptr;

	if (_settings_game.economy.station_noise_level) {
		ret = CheckModularAirportNoiseChange({}, noise_after);
		if (ret.Failed()) return ret;
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

	/* Rebuilding where an airport was just demolished takes its station back. Ask over the
	 * whole footprint rather than leaving it to BuildStationPart, which measures from the
	 * northern corner only -- see GetClosestDeletedStationForArea. */
	if (st == nullptr && reuse) st = GetClosestDeletedStationForArea(airport_area);

	const StationNaming naming = ModularAirportAcceptsPlanesFromPieces(future_capability_pieces) ? StationNaming::Airport : StationNaming::Heliport;
	ret = BuildStationPart(&st, flags, reuse, airport_area, naming);
	if (ret.Failed()) return ret;

	if (st != nullptr && st->airport.tile != INVALID_TILE) {
		return CommandCost(STR_ERROR_TOO_CLOSE_TO_ANOTHER_AIRPORT);
	}

	for (const ModularAirportTileData &data : converted_data) cost.AddCost(GetModularAirportPieceBuildCost(data.piece_type));

	if (flags.Test(DoCommandFlag::Execute)) {
		st->AddFacility(StationFacility::Airport, tile);
		InitializeNewModularAirport(st->airport);

		st->rect.BeforeAddRect(tile, w, h, StationRect::ADD_TRY);

		st->airport.EnsureModularDataExists();
		auto &tile_data_vec = *st->airport.modular_tile_data;

		for (const ModularAirportTileData &data : converted_data) {
			Tile t(data.tile);
			MakeAirport(t, st->owner, st->index, data.piece_type, WaterClass::Invalid);
			SetStationTileRandomBits(t, GB(Random(), 0, 4));
			st->airport.Add(data.tile);

			if (AirportTileSpec::Get(GetTranslatedAirportTileID(data.piece_type))->animation.status != AnimationStatus::NoAnimation) AddAnimatedTile(t);
			tile_data_vec.push_back(data);
		}

		st->airport.modular_tile_index_dirty = true;
		st->airport.MarkLayoutDirty();

		/* Trigger animations */
		for (const ModularAirportTileData &data : converted_data) TriggerAirportTileAnimation(st, data.tile, AirportAnimationTrigger::Built);

		UpdateAirplanesOnNewStation(st);

		Company::Get(st->owner)->infrastructure.airport++;

		st->AfterStationTileSetChange(true, StationType::Airport);
		InvalidateWindowData(WindowClass::StationView, st->index, -1);
		ApplyModularAirportNoiseChange(st, {});

		if (_show_holding_overlay) MarkWholeScreenDirty();
	}

	return cost;
}
