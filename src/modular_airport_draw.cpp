/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file modular_airport_draw.cpp Drawing helpers for modular airport tiles. */

#include "stdafx.h"

#include "modular_airport_draw.h"

#include "bridge_map.h"
#include "airport_pathfinder.h"
#include "landscape.h"
#include "modular_airport_cmd.h"
#include "modular_airport_gui.h"
#include "newgrf_airporttiles.h"
#include "station_base.h"
#include "station_map.h"
#include "viewport_func.h"

#include "table/airporttile_ids.h"

static uint8_t GetModularTileFenceOpenMask(uint8_t piece_type, uint8_t rotation)
{
	switch (piece_type) {
		case APT_RUNWAY_1: case APT_RUNWAY_2: case APT_RUNWAY_3:
		case APT_RUNWAY_4: case APT_RUNWAY_5: case APT_RUNWAY_END:
		case APT_RUNWAY_SMALL_NEAR_END: case APT_RUNWAY_SMALL_MIDDLE:
		case APT_RUNWAY_SMALL_FAR_END:
			return (rotation % 2 == 0) ? 0x0A : 0x05;
		case APT_DEPOT_SE: case APT_DEPOT_SW: case APT_DEPOT_NW: case APT_DEPOT_NE:
		case APT_SMALL_DEPOT_SE: case APT_SMALL_DEPOT_SW:
		case APT_SMALL_DEPOT_NW: case APT_SMALL_DEPOT_NE:
			return CalculateAutoTaxiDirectionsForGfx(piece_type, rotation);
		case APT_APRON_FENCE_NE: case APT_APRON_FENCE_SE:
		case APT_APRON_FENCE_SW: case APT_APRON_FENCE_NW:
			return 0x0F;
		case APT_BUILDING_1: case APT_BUILDING_2: case APT_BUILDING_3:
		case APT_ROUND_TERMINAL:
		case APT_LOW_BUILDING: case APT_LOW_BUILDING_FENCE_N: case APT_LOW_BUILDING_FENCE_NW:
		case APT_SMALL_BUILDING_1: case APT_SMALL_BUILDING_2: case APT_SMALL_BUILDING_3:
		case APT_TOWER: case APT_TOWER_FENCE_SW:
			return 0x0F;
		default:
			return 0x00;
	}
}

void DrawModularAirportPerimeterFences(const TileInfo *ti, PaletteID palette)
{
	if (!IsAirport(ti->tile)) return;

	const Station *fence_st = Station::GetByTile(ti->tile);
	if (fence_st == nullptr || !fence_st->airport.blocks.Test(AirportBlock::Modular)) return;

	const ModularAirportTileData *fence_md = fence_st->airport.GetModularTileData(ti->tile);
	if (fence_md == nullptr) return;

	const uint8_t open_mask = GetModularTileFenceOpenMask(fence_md->piece_type, fence_md->rotation);

	static constexpr struct {
		int8_t dx, dy;
		uint8_t dir_bit;
		SpriteID spr;
		int8_t fx, fy;
	} kEdges[] = {
		{  0, -1, 0x01, SPR_AIRPORT_FENCE_X,  0,  0 },
		{ +1,  0, 0x02, SPR_AIRPORT_FENCE_Y, 15,  0 },
		{  0, +1, 0x04, SPR_AIRPORT_FENCE_X,  0, 15 },
		{ -1,  0, 0x08, SPR_AIRPORT_FENCE_Y,  0,  0 },
	};

	for (const auto &e : kEdges) {
		if (open_mask & e.dir_bit) continue;
		bool explicit_fence = (fence_md->edge_block_mask & e.dir_bit) != 0;
		TileIndex nb = TileAddXY(ti->tile, e.dx, e.dy);
		bool perimeter = !(IsValidTile(nb) && fence_st->TileBelongsToAirport(nb));
		if (!explicit_fence && !perimeter) continue;
		DrawGroundSpriteAt(e.spr | (1U << PALETTE_MODIFIER_COLOUR), palette, e.fx, e.fy, GetPartialPixelZ(e.fx, e.fy, ti->tileh));
	}
}

void DrawModularAirportDirectionOverlays(const TileInfo *ti)
{
	if (!_show_runway_direction_overlay || !IsAirport(ti->tile)) return;

	Station *station = Station::GetByTile(ti->tile);
	if (station == nullptr || !station->airport.blocks.Test(AirportBlock::Modular)) return;

	const ModularAirportTileData *tile_data = station->airport.GetModularTileData(ti->tile);
	if (tile_data == nullptr) return;

	if (IsModularRunwayPiece(tile_data->piece_type)) {
		uint8_t flags = tile_data->runway_flags;
		bool horizontal = (tile_data->rotation % 2) == 0;
		SpriteID base = SPR_ONEWAY_BASE;

		bool dir_low = (flags & RUF_DIR_LOW) != 0;
		bool dir_high = (flags & RUF_DIR_HIGH) != 0;
		bool can_land = (flags & RUF_LANDING) != 0;
		bool can_takeoff = (flags & RUF_TAKEOFF) != 0;

		if (can_land || can_takeoff) {
			SpriteID sprite;
			PaletteID pal_overlay;

			if (dir_low && dir_high) {
				sprite = base + (horizontal ? 2 : 5);
			} else if (dir_low) {
				sprite = base + (horizontal ? 1 : 3);
			} else {
				sprite = base + (horizontal ? 0 : 4);
			}

			if (can_land && can_takeoff) {
				pal_overlay = PAL_NONE;
			} else if (can_land) {
				pal_overlay = PALETTE_SEL_TILE_BLUE;
			} else {
				pal_overlay = PALETTE_SEL_TILE_RED;
			}

			DrawGroundSpriteAt(sprite, PAL_NONE, 8, 8, GetPartialPixelZ(8, 8, ti->tileh));
			if (pal_overlay != PAL_NONE) DrawGroundSpriteAt(SPR_SELECT_TILE + SlopeToSpriteOffset(ti->tileh), pal_overlay, 0, 0, 7);
		}
	} else if (IsTaxiwayPiece(tile_data->piece_type) && tile_data->one_way_taxi && HasExactlyOneBit(tile_data->user_taxi_dir_mask)) {
		SpriteID sprite = 0;
		SpriteID base = SPR_ONEWAY_BASE;
		const uint8_t dir = tile_data->user_taxi_dir_mask & 0x0F;

		if (dir == 0x01) {
			sprite = base + 3;
		} else if (dir == 0x02) {
			sprite = base + 0;
		} else if (dir == 0x04) {
			sprite = base + 4;
		} else if (dir == 0x08) {
			sprite = base + 1;
		}

		if (sprite != 0) DrawGroundSpriteAt(sprite, PAL_NONE, 8, 8, GetPartialPixelZ(8, 8, ti->tileh));
	}
}
