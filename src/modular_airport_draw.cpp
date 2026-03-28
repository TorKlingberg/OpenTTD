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
#include "station_func.h"
#include "station_map.h"
#include "tile_map.h"
#include "viewport_func.h"

#include "table/airporttile_ids.h"
#include "table/station_land.h"

/* Modular hangar sprite layouts — apron ground with hangar child sprites. */
static const DrawTileSpriteSpan _station_display_modular_hangar_se(
	PalSpriteID{SPR_AIRPORT_APRON, PAL_NONE}, _station_display_hangar_se);

static const DrawTileSpriteSpan _station_display_modular_hangar_sw(
	PalSpriteID{SPR_AIRPORT_APRON, PAL_NONE}, _station_display_hangar_sw);

static const DrawTileSpriteSpan _station_display_modular_hangar_nw(
	PalSpriteID{SPR_AIRPORT_APRON, PAL_NONE}, _station_display_hangar_nw);

static const DrawTileSpriteSpan _station_display_modular_hangar_ne(
	PalSpriteID{SPR_AIRPORT_APRON, PAL_NONE}, _station_display_hangar_ne);

static const DrawTileSpriteSpan _station_display_modular_small_hangar_se(
	PalSpriteID{SPR_AIRPORT_APRON, PAL_NONE}, _station_display_small_depot_se);

static const DrawTileSpriteSpan _station_display_modular_newhelipad(
	PalSpriteID{SPR_AIRPORT_APRON, PAL_NONE}, _station_display_newhelipad);

/* Auto-jetway stand layout without the baked-in fence from stock city airport. */
static const DrawTileSeqStruct _station_display_jetway_1_nofence[] = {
	{ 7, 11,  0,  3,  3, 14, {SPR_AIRPORT_JETWAY_1 | (1U << PALETTE_MODIFIER_COLOUR), PAL_NONE} },
};
static const DrawTileSpriteSpan _station_display_modular_jetway_1(
	PalSpriteID{SPR_AIRPORT_AIRCRAFT_STAND, PAL_NONE}, _station_display_jetway_1_nofence);

/* NS (NW-SE on screen) runway sprites for modular airports. */
static const DrawTileSpriteSpan _station_display_modular_ns_runway_1(PalSpriteID{SPR_NSRUNWAY1, PAL_NONE});
static const DrawTileSpriteSpan _station_display_modular_ns_runway_2(PalSpriteID{SPR_NSRUNWAY2, PAL_NONE});
static const DrawTileSpriteSpan _station_display_modular_ns_runway_3(PalSpriteID{SPR_NSRUNWAY3, PAL_NONE});
static const DrawTileSpriteSpan _station_display_modular_ns_runway_4(PalSpriteID{SPR_NSRUNWAY4, PAL_NONE});
static const DrawTileSpriteSpan _station_display_modular_ns_runway_end(PalSpriteID{SPR_NSRUNWAY_END, PAL_NONE});

/* Legacy small runway fence-free overrides. */
static const DrawTileSpriteSpan _station_display_modular_old_runway_near_end(PalSpriteID{SPR_AIRFIELD_RUNWAY_NEAR_END, PAL_NONE});
static const DrawTileSpriteSpan _station_display_modular_old_runway_middle(PalSpriteID{SPR_AIRFIELD_RUNWAY_MIDDLE, PAL_NONE});
static const DrawTileSpriteSpan _station_display_modular_old_runway_far_end(PalSpriteID{SPR_AIRFIELD_RUNWAY_FAR_END, PAL_NONE});

const DrawTileSprites *GetModularHangarTileLayout(uint8_t rotation, bool small_hangar)
{
	if (small_hangar) return &_station_display_modular_small_hangar_se;
	switch (rotation) {
		case 1: return &_station_display_modular_hangar_ne;
		case 2: return &_station_display_modular_hangar_nw;
		case 3: return &_station_display_modular_hangar_sw;
		default: return &_station_display_modular_hangar_se;
	}
}

const DrawTileSprites *GetModularHangarTileLayoutByPiece(uint8_t piece_type, uint8_t rotation)
{
	const bool is_large_hangar =
			piece_type == APT_DEPOT_SE || piece_type == APT_DEPOT_SW ||
			piece_type == APT_DEPOT_NW || piece_type == APT_DEPOT_NE;
	const bool is_small_hangar =
			piece_type == APT_SMALL_DEPOT_SE || piece_type == APT_SMALL_DEPOT_SW ||
			piece_type == APT_SMALL_DEPOT_NW || piece_type == APT_SMALL_DEPOT_NE;

	if (!is_large_hangar && !is_small_hangar) return nullptr;

	uint8_t visual_rot = rotation % 4;

	/* Compatibility for saves written when directional hangars were encoded in piece_type. */
	/* Important: piece_type directional variants use 0=SE,1=NE,2=NW,3=SW.
	 * So SW maps to rot=3 and NE maps to rot=1. This is easy to invert by mistake.
	 * Keep in sync with SwapBuildingPieceForRotation() and airport_pathfinder.cpp. */
	switch (piece_type) {
		case APT_DEPOT_SW:
		case APT_SMALL_DEPOT_SW: visual_rot = 3; break;
		case APT_DEPOT_NW:
		case APT_SMALL_DEPOT_NW: visual_rot = 2; break;
		case APT_DEPOT_NE:
		case APT_SMALL_DEPOT_NE: visual_rot = 1; break;
		default: break;
	}

	return GetModularHangarTileLayout(visual_rot, is_small_hangar);
}

const DrawTileSprites *GetModularNSRunwayLayout(uint8_t piece_type)
{
	switch (piece_type) {
		case APT_RUNWAY_1:           return &_station_display_modular_ns_runway_1;
		case APT_RUNWAY_2:
		case APT_RUNWAY_5:           return &_station_display_modular_ns_runway_2;
		case APT_RUNWAY_3:           return &_station_display_modular_ns_runway_3;
		case APT_RUNWAY_4:           return &_station_display_modular_ns_runway_4;
		case APT_RUNWAY_END:         return &_station_display_modular_ns_runway_end;
		default:                     return nullptr;
	}
}

const DrawTileSprites *GetAirportTileLayoutWithModularOverrides(uint8_t gfx, uint8_t modular_piece_type, uint8_t modular_rotation, uint8_t animation_frame)
{
	const DrawTileSprites *t = nullptr;

	if (const DrawTileSprites *hangar_layout = GetModularHangarTileLayoutByPiece(modular_piece_type, modular_rotation); hangar_layout != nullptr) {
		t = hangar_layout;
	}

	/* NS runway sprite override: rotation%2==1 means Y-axis (NW-SE) runway. */
	if ((modular_rotation % 2) == 1 && t == nullptr) {
		t = GetModularNSRunwayLayout(modular_piece_type);
	}

	/* Legacy small runway sprites include a baked SE fence in stock layouts.
	 * In modular mode fence rendering should come from edge fences only. */
	switch (modular_piece_type) {
		case APT_RUNWAY_SMALL_NEAR_END: t = &_station_display_modular_old_runway_near_end; break;
		case APT_RUNWAY_SMALL_MIDDLE:   t = &_station_display_modular_old_runway_middle; break;
		case APT_RUNWAY_SMALL_FAR_END:  t = &_station_display_modular_old_runway_far_end; break;
		default: break;
	}

	/* Modular windsock: draw without the built-in NE fence. */
	if (modular_piece_type == APT_GRASS_FENCE_NE_FLAG_2) {
		t = &_station_display_datas_airport_flag_grass[animation_frame % lengthof(_station_display_datas_airport_flag_grass)];
	}

	/* Helistation-style H pad: use no-fence variant in modular mode. */
	if (modular_piece_type == APT_HELIPAD_3_FENCE_NW) {
		t = &_station_display_modular_newhelipad;
	}

	if (t == nullptr) switch (gfx) {
		case APT_RADAR_GRASS_FENCE_SW:
			t = &_station_display_datas_airport_radar_grass_fence_sw[animation_frame % lengthof(_station_display_datas_airport_radar_grass_fence_sw)];
			break;
		case APT_GRASS_FENCE_NE_FLAG:
			t = &_station_display_datas_airport_flag_grass_fence_ne[animation_frame % lengthof(_station_display_datas_airport_flag_grass_fence_ne)];
			break;
		case APT_RADAR_FENCE_SW:
			t = &_station_display_datas_airport_radar_fence_sw[animation_frame % lengthof(_station_display_datas_airport_radar_fence_sw)];
			break;
		case APT_RADAR_FENCE_NE:
			t = &_station_display_datas_airport_radar_fence_ne[animation_frame % lengthof(_station_display_datas_airport_radar_fence_ne)];
			break;
		case APT_GRASS_FENCE_NE_FLAG_2:
			t = &_station_display_datas_airport_flag_grass_fence_ne_2[animation_frame % lengthof(_station_display_datas_airport_flag_grass_fence_ne_2)];
			break;
	}

	if (t == nullptr) t = GetStationTileLayout(StationType::Airport, gfx);
	return t;
}

void ApplyModularAirportTileLayoutOverrides(const TileInfo *ti, StationGfx &gfx, const DrawTileSprites *&t)
{
	if (!IsAirport(ti->tile)) return;

	const Station *airport_st = Station::GetByTile(ti->tile);
	if (airport_st == nullptr || !airport_st->airport.blocks.Test(AirportBlock::Modular)) return;

	const ModularAirportTileData *md = airport_st->airport.GetModularTileData(ti->tile);
	if (md == nullptr) return;

	t = GetAirportTileLayoutWithModularOverrides(gfx, md->piece_type, md->rotation, GetAnimationFrame(ti->tile));

	const StationGfx original_gfx = gfx;

	/* Auto-jetway: a plain stand adjacent to a round terminal gets a jetway sprite.
	 * jetway_1 (APT_STAND_1)      -- terminal is one tile to the south (dy=+1)
	 * jetway_2 (APT_STAND_PIER_NE) -- terminal is one tile to the west  (dx=-1) */
	if (md->piece_type == APT_STAND) {
		auto NeighborPiece = [&](int dx, int dy) -> uint8_t {
			TileIndex nb = TileAddXY(ti->tile, dx, dy);
			if (!IsValidTile(nb)) return 0xFF;
			const ModularAirportTileData *nb_md = airport_st->airport.GetModularTileData(nb);
			return nb_md != nullptr ? nb_md->piece_type : 0xFF;
		};
		if (NeighborPiece(0, +1) == APT_ROUND_TERMINAL) {
			gfx = APT_STAND_1;
		} else if (NeighborPiece(-1, 0) == APT_ROUND_TERMINAL) {
			gfx = APT_STAND_PIER_NE;
		}
	}

	/* Auto-jetway intentionally overrides the default stand layout returned by
	 * GetAirportTileLayoutWithModularOverrides when a round terminal is adjacent.
	 * Use fence-free variant for jetway_1 since the stock layout has a baked-in
	 * north fence that doesn't belong in modular airports. */
	if (gfx == APT_STAND_1) {
		t = &_station_display_modular_jetway_1;
	} else if (gfx != original_gfx) {
		t = GetStationTileLayout(StationType::Airport, gfx);
	}
}

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
