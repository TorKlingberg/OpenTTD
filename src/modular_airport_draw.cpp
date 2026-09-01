/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file modular_airport_draw.cpp Drawing helpers for modular airport tiles. */

#include "stdafx.h"

#include "modular_airport_draw.h"

#include "gfx_func.h"
#include "landscape.h"
#include "modular_airport_cmd.h"
#include "modular_airport_gui.h"
#include "sprite.h"
#include "spritecache.h"
#include "station_base.h"
#include "station_func.h"
#include "station_map.h"
#include "tile_map.h"
#include "viewport_func.h"
#include "zoom_func.h"

#include "bridge_map.h"
#include "table/airporttile_ids.h"
#include "table/station_land.h"

/* Modular hangar sprite layouts -- apron ground with hangar child sprites. */
static const DrawTileSpriteSpan _station_display_modular_hangar_se(
	PalSpriteID{SPR_AIRPORT_APRON, PAL_NONE}, _station_display_hangar_se);

/* The three rotated hangars are rebuilt from the loaded sprites by
 * InitModularAirportHangarLayouts(); see the comment there for why. These are the
 * upstream _station_display_hangar_{sw,nw,ne} values, which are what that ends up
 * with for openttd.grf and OpenGFX. */
static DrawTileSeqStruct _modular_hangar_seq_sw[] = {
	{14,  0,  0,  2, 17, 28, {SPR_NEWHANGAR_W | (1U << PALETTE_MODIFIER_COLOUR), PAL_NONE} },
	{ 0,  0,  0,  2, 17, 28, {SPR_NEWHANGAR_W_WALL | (1U << PALETTE_MODIFIER_COLOUR), PAL_NONE} },
};
static DrawTileSeqStruct _modular_hangar_seq_nw[] = {
	{14,  0,  0,  2, 16, 28, {SPR_NEWHANGAR_N | (1U << PALETTE_MODIFIER_COLOUR), PAL_NONE} },
};
static DrawTileSeqStruct _modular_hangar_seq_ne[] = {
	{14,  0,  0,  2, 16, 28, {SPR_NEWHANGAR_E | (1U << PALETTE_MODIFIER_COLOUR), PAL_NONE} },
};

static const DrawTileSpriteSpan _station_display_modular_hangar_sw(
	PalSpriteID{SPR_AIRPORT_APRON, PAL_NONE}, _modular_hangar_seq_sw);

static const DrawTileSpriteSpan _station_display_modular_hangar_nw(
	PalSpriteID{SPR_AIRPORT_APRON, PAL_NONE}, _modular_hangar_seq_nw);

static const DrawTileSpriteSpan _station_display_modular_hangar_ne(
	PalSpriteID{SPR_AIRPORT_APRON, PAL_NONE}, _modular_hangar_seq_ne);

static const DrawTileSpriteSpan _station_display_modular_small_hangar_se(
	PalSpriteID{SPR_AIRPORT_APRON, PAL_NONE}, _station_display_small_depot_se);

/* The stock small-hangar composition splits the body from the near doorway. The
 * opposite direction uses the body alone, with the doorway on the hidden far face;
 * mirroring the selected base set's pixels supplies the other axis. */
static const DrawTileSeqStruct _station_display_modular_small_hangar_sw_seq[] = {
	{ 0, 14, 0, 17,  2, 28, {SPR_MIRROR_AIRFIELD_HANGAR_FRONT | (1U << PALETTE_MODIFIER_COLOUR), PAL_NONE}},
	{ 0,  0, 0, 17,  2, 28, {SPR_MIRROR_AIRFIELD_HANGAR_REAR  | (1U << PALETTE_MODIFIER_COLOUR), PAL_NONE}},
};
static const DrawTileSpriteSpan _station_display_modular_small_hangar_sw(
	PalSpriteID{SPR_AIRPORT_APRON, PAL_NONE}, _station_display_modular_small_hangar_sw_seq);

static const DrawTileSeqStruct _station_display_modular_small_hangar_nw_seq[] = {
	{14, 0, 0, 2, 16, 28, {SPR_AIRFIELD_HANGAR_FRONT | (1U << PALETTE_MODIFIER_COLOUR), PAL_NONE}},
};
static const DrawTileSpriteSpan _station_display_modular_small_hangar_nw(
	PalSpriteID{SPR_AIRPORT_APRON, PAL_NONE}, _station_display_modular_small_hangar_nw_seq);

static const DrawTileSeqStruct _station_display_modular_small_hangar_ne_seq[] = {
	{0, 14, 0, 16, 2, 28, {SPR_MIRROR_AIRFIELD_HANGAR_FRONT | (1U << PALETTE_MODIFIER_COLOUR), PAL_NONE}},
};
static const DrawTileSpriteSpan _station_display_modular_small_hangar_ne(
	PalSpriteID{SPR_AIRPORT_APRON, PAL_NONE}, _station_display_modular_small_hangar_ne_seq);

static const DrawTileSpriteSpan _station_display_modular_newhelipad(
	PalSpriteID{SPR_AIRPORT_APRON, PAL_NONE}, _station_display_newhelipad);

/* Auto-jetway stand layout without the baked-in fence from stock city airport. */
static const DrawTileSeqStruct _station_display_jetway_1_nofence[] = {
	{ 7, 11,  0,  3,  3, 14, {SPR_AIRPORT_JETWAY_1 | (1U << PALETTE_MODIFIER_COLOUR), PAL_NONE} },
};
static const DrawTileSpriteSpan _station_display_modular_jetway_1(
	PalSpriteID{SPR_AIRPORT_AIRCRAFT_STAND, PAL_NONE}, _station_display_jetway_1_nofence);

/* A stand with nothing on it. Every modular stand starts from this; the jetway
 * is added back only where a round terminal is actually adjacent. */
static const DrawTileSpriteSpan _station_display_modular_stand(
	PalSpriteID{SPR_AIRPORT_AIRCRAFT_STAND, PAL_NONE});

/* Metadata-only one-tile airport decorations. The map keeps a canonical apron
 * or grass graphic; these layouts supply the dedicated object sprite. */
static const DrawTileSeqStruct _station_display_modular_fire_station_seq[] = {
	{0, 0, 0, 16, 16, 36, {SPR_AIRPORT_FIRE_STATION, PAL_NONE}},
};
static const DrawTileSpriteSpan _station_display_modular_fire_station(
	PalSpriteID{SPR_AIRPORT_APRON, PAL_NONE}, _station_display_modular_fire_station_seq);
static const DrawTileSeqStruct _station_display_modular_fire_station_other_seq[] = {
	{0, 0, 0, 16, 16, 36, {SPR_AIRPORT_FIRE_STATION_OTHER, PAL_NONE}},
};
static const DrawTileSpriteSpan _station_display_modular_fire_station_other(
	PalSpriteID{SPR_AIRPORT_APRON, PAL_NONE}, _station_display_modular_fire_station_other_seq);

static const DrawTileSeqStruct _station_display_modular_cargo_terminal_seq[] = {
	{0, 0, 0, 16, 16, 30, {SPR_AIRPORT_CARGO_TERMINAL, PAL_NONE}},
};
static const DrawTileSpriteSpan _station_display_modular_cargo_terminal(
	PalSpriteID{SPR_AIRPORT_APRON, PAL_NONE}, _station_display_modular_cargo_terminal_seq);

static const DrawTileSeqStruct _station_display_modular_fuel_farm_seq[] = {
	{0, 0, 0, 16, 16, 24, {SPR_AIRPORT_FUEL_FARM, PAL_NONE}},
};
static const DrawTileSpriteSpan _station_display_modular_fuel_farm(
	PalSpriteID{SPR_AIRPORT_APRON, PAL_NONE}, _station_display_modular_fuel_farm_seq);

static const DrawTileSeqStruct _station_display_modular_car_park_seq[] = {
	{0, 0, 0, 16, 16, 36, {SPR_AIRPORT_CAR_PARK, PAL_NONE}},
};
static const DrawTileSpriteSpan _station_display_modular_car_park(
	PalSpriteID{SPR_AIRPORT_APRON, PAL_NONE}, _station_display_modular_car_park_seq);
static const DrawTileSeqStruct _station_display_modular_car_park_other_seq[] = {
	{0, 0, 0, 16, 16, 36, {SPR_AIRPORT_CAR_PARK_OTHER, PAL_NONE}},
};
static const DrawTileSpriteSpan _station_display_modular_car_park_other(
	PalSpriteID{SPR_AIRPORT_APRON, PAL_NONE}, _station_display_modular_car_park_other_seq);

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

/* The same strip on the other axis. The base sets draw the small airfield's runway for one
 * axis only, so the other one is their own sprite mirrored -- see SetupMirroredSprites(). */
static const DrawTileSpriteSpan _station_display_modular_old_runway_near_end_mirror(PalSpriteID{SPR_MIRROR_AIRFIELD_RUNWAY_NEAR_END, PAL_NONE});
static const DrawTileSpriteSpan _station_display_modular_old_runway_middle_mirror(PalSpriteID{SPR_MIRROR_AIRFIELD_RUNWAY_MIDDLE, PAL_NONE});
static const DrawTileSpriteSpan _station_display_modular_old_runway_far_end_mirror(PalSpriteID{SPR_MIRROR_AIRFIELD_RUNWAY_FAR_END, PAL_NONE});

/* The small terminal's three pieces, mirrored onto the other axis the same way. The stock
 * layouts of APT_SMALL_BUILDING_1..3 are reproduced here with the mirrored sprites; only
 * the third piece has a building of its own on top of the ground, and its bounding box is
 * square, so mirroring leaves the box where it is. */
static const DrawTileSpriteSpan _station_display_modular_small_terminal_a_mirror(PalSpriteID{SPR_MIRROR_AIRFIELD_TERM_A, PAL_NONE});
static const DrawTileSpriteSpan _station_display_modular_small_terminal_b_mirror(PalSpriteID{SPR_MIRROR_AIRFIELD_TERM_B, PAL_NONE});
static const DrawTileSeqStruct _station_display_modular_small_terminal_c_mirror_seq[] = {
	{0, 0, 0, 15, 15, 30, {SPR_MIRROR_AIRFIELD_TERM_C_BUILD | (1U << PALETTE_MODIFIER_COLOUR), PAL_NONE}},
};
static const DrawTileSpriteSpan _station_display_modular_small_terminal_c_mirror(
	PalSpriteID{SPR_MIRROR_AIRFIELD_TERM_C_GROUND | (1U << PALETTE_MODIFIER_COLOUR), PAL_NONE},
	_station_display_modular_small_terminal_c_mirror_seq);

/**
 * Screen position, relative to the tile origin, of a correctly placed hangar building:
 * its left edge, and the ground line its foot stands on.
 *
 * openttd.grf anchors its hangar building sprites at (-2,-38) and the upstream layout
 * draws them from tile origin (14,0,0), which RemapCoords turns into (-28,+14). Those
 * sprites are 64 by 55, so they span -30 to +34 across and -24 to +31 down.
 *
 * The foot is the half to pin, not the top: base sets do not agree on how tall a hangar
 * is (aBase draws 51 rows where openttd.grf draws 55), and pinning the top of a shorter
 * building leaves it hanging in the air. Width they do agree on -- all three of the base
 * sets checked draw a full tile width -- so the left edge is safe to pin as it is.
 */
static constexpr int HANGAR_BUILDING_SCREEN_X = -30;
static constexpr int HANGAR_BUILDING_SCREEN_BOTTOM = 31;

/** Tile origin the upstream hangar layouts give their building sprite. */
static constexpr int HANGAR_BUILDING_ORIGIN_X = 14;
static constexpr int HANGAR_BUILDING_ORIGIN_Y = 0;

/**
 * Find the sprite offset that draws a hangar building at the canonical position.
 *
 * @param sprite Building sprite to place.
 * @param[out] offset_x Offset along the X axis, relative to the canonical origin.
 * @param[out] offset_y Offset along the Y axis, relative to the canonical origin.
 * @return Whether an offset was found; the outputs are untouched if not.
 */
static bool SolveHangarBuildingOffset(SpriteID sprite, int &offset_x, int &offset_y)
{
	Point offset;
	const Dimension size = GetSpriteSize(sprite, &offset, ZoomLevel::Normal);
	const int height = static_cast<int>(size.height) - offset.y;

	/* RemapCoords(x, y, 0) is ((y - x) * 2, y + x), so the position follows from the sum
	 * and the difference of its two components. The sum is pinned exactly; the
	 * difference only moves the sprite in steps of two pixels and has to have the same
	 * parity as the sum. */
	const int sum = HANGAR_BUILDING_SCREEN_BOTTOM - height - offset.y;
	const int wanted_screen_x = HANGAR_BUILDING_SCREEN_X - offset.x;
	const int center_diff = wanted_screen_x / 2;

	bool found = false;
	int best_error = 0;
	for (int diff = center_diff - 2; diff <= center_diff + 2; diff++) {
		if (((diff + sum) % 2) != 0) continue;
		const int x = (sum - diff) / 2 - HANGAR_BUILDING_ORIGIN_X;
		const int y = (sum + diff) / 2 - HANGAR_BUILDING_ORIGIN_Y;
		if (x < INT8_MIN || x > INT8_MAX || y < INT8_MIN || y > INT8_MAX) continue;

		const int error = abs(diff * 2 - wanted_screen_x);
		if (found && error >= best_error) continue;
		found = true;
		best_error = error;
		offset_x = x;
		offset_y = y;
	}
	return found;
}

/**
 * Height of a sprite in its own pixels, or 0 if it isn't a drawable sprite.
 * Used to tell a hangar building from the wall piece that goes with it.
 */
static uint GetHangarSpriteHeight(SpriteID sprite)
{
	if (!SpriteExists(sprite) || GetSpriteType(sprite) != SpriteType::Normal) return 0;
	return GetSprite(sprite, SpriteType::Normal)->height;
}

/**
 * Point a hangar layout entry at @a sprite, offset so the building lands on its tile.
 * @param dtss Layout entry to fill in.
 * @param extent_y Depth of the upstream bounding box for this layout.
 * @param sprite Building sprite to draw.
 */
static void SetHangarBuildingSprite(DrawTileSeqStruct &dtss, uint8_t extent_y, SpriteID sprite)
{
	dtss = DrawTileSeqStruct(HANGAR_BUILDING_ORIGIN_X, HANGAR_BUILDING_ORIGIN_Y, 0, 2, extent_y, 28,
			PalSpriteID{sprite | (1U << PALETTE_MODIFIER_COLOUR), PAL_NONE});

	int offset_x, offset_y;
	if (GetHangarSpriteHeight(sprite) != 0 && SolveHangarBuildingOffset(sprite, offset_x, offset_y)) {
		dtss.offset = {static_cast<int8_t>(offset_x), static_cast<int8_t>(offset_y), 0};
	}
}

/**
 * Rebuild the rotated hangar layouts from the sprites the base set actually provides.
 *
 * The three rotated hangars are drawn from the six-sprite hangar run in the base set's
 * AIRPORTX block (SPR_NEWHANGAR_*). Nothing in vanilla OpenTTD draws any of them -- the
 * upstream _station_display_hangar_{sw,nw,ne} tables are unreferenced -- so base sets have
 * been free to fill that run however they liked, and there are two conventions in the wild:
 *
 *   openttd.grf, OpenGFX  {S, S_WALL} {W, W_WALL} {N, E}: building first in each pair,
 *                         all four buildings anchored at sprite offset (-2,-38), and
 *                         E facing NE with N facing NW.
 *   OpenGFX2 Classic      every pair the other way round: the wall piece first, the west
 *                         and north buildings anchored half a tile further left at
 *                         (-61,-39), and E facing NW with N facing NE.
 *
 * Hard-coding either one gets the other wrong. Under OpenGFX2 Classic the upstream tables
 * draw the NW and SW hangars half a tile off their tile, and hand the NE and NW hangars
 * each other's doors. So work both out from the sprites themselves: the W pair settles
 * which order the run is in, because a wall piece is a fraction of a building's height,
 * and that in turn settles the N/E pair, where both entries are same-sized buildings with
 * nothing to tell them apart. Each building's origin then follows from its own offsets.
 *
 * Called whenever sprites are (re)loaded, since a NewGRF can replace this block too.
 */
void InitModularAirportHangarLayouts()
{
	/* Wall piece ahead of its building means the whole run is in OpenGFX2 Classic's order. */
	const bool pairs_swapped = GetHangarSpriteHeight(SPR_NEWHANGAR_W) < GetHangarSpriteHeight(SPR_NEWHANGAR_W_WALL);

	SetHangarBuildingSprite(_modular_hangar_seq_sw[0], 17, pairs_swapped ? SPR_NEWHANGAR_W_WALL : SPR_NEWHANGAR_W);
	_modular_hangar_seq_sw[1] = DrawTileSeqStruct(0, 0, 0, 2, 17, 28,
			PalSpriteID{(pairs_swapped ? SPR_NEWHANGAR_W : SPR_NEWHANGAR_W_WALL) | (1U << PALETTE_MODIFIER_COLOUR), PAL_NONE});

	SetHangarBuildingSprite(_modular_hangar_seq_nw[0], 16, pairs_swapped ? SPR_NEWHANGAR_E : SPR_NEWHANGAR_N);
	SetHangarBuildingSprite(_modular_hangar_seq_ne[0], 16, pairs_swapped ? SPR_NEWHANGAR_N : SPR_NEWHANGAR_E);
}

/**
 * Draw one modular airport tile's sprite layout into the GUI at (@a x, @a y).
 *
 * DrawCommonTileSeqInGUI() positions each parent sprite from its bounding box origin
 * alone, in layout order. Neither is enough here.
 *
 * The rotated hangars keep the stock bounding box so the world view sorts them against
 * neighbouring buildings correctly, and carry their base set specific position correction
 * in the sprite offset instead, so the offset has to be added.
 *
 * And layout order is not painting order. A two-piece hangar lists the near half first,
 * which the world view is free to do because it sorts by bounding box before painting;
 * a GUI painter that follows the layout draws the far wall over the near building and
 * shows the inside of the hangar. How badly that reads depends on how much the two
 * sprites overlap, which is a base set's choice: openttd.grf and OpenGFX keep the far
 * piece to a narrow wall, while aBase draws both halves full tile width. So sort back to
 * front first, the way the viewport would.
 *
 * Every entry in these layouts is a parent sprite, so there are no child offsets to
 * accumulate.
 *
 * @param x Left edge of the tile to draw at.
 * @param y Top edge of the tile to draw at.
 * @param dts Tile layout, e.g. from GetModularHangarTileLayout().
 * @param default_palette Company palette to recolour with.
 * @param zoom Zoom level to draw the sprites at.
 */
void DrawModularTileSeqInGUI(int x, int y, const DrawTileSprites *dts, PaletteID default_palette, ZoomLevel zoom)
{
	std::array<const DrawTileSeqStruct *, 8> order;
	size_t count = 0;
	for (const DrawTileSeqStruct &dtss : dts->GetSequence()) {
		const SpriteID image = dtss.image.sprite;

		/* TTD sprite 0 means no sprite. */
		if (GB(image, 0, SPRITE_WIDTH) == 0 && !HasBit(image, SPRITE_MODIFIER_CUSTOM_SPRITE)) continue;
		if (!dtss.IsParentSprite()) continue;

		if (count < order.size()) order[count++] = &dtss;
	}

	/* The viewport counts a sprite as being in front when its box starts beyond another's
	 * along any axis; these layouts sit on one tile, so ordering on the sum agrees. */
	std::stable_sort(order.begin(), order.begin() + count, [](const DrawTileSeqStruct *a, const DrawTileSeqStruct *b) {
		return a->origin.x + a->origin.y + a->origin.z < b->origin.x + b->origin.y + b->origin.z;
	});

	for (size_t i = 0; i < count; ++i) {
		const DrawTileSeqStruct *dtss = order[i];
		const SpriteID image = dtss->image.sprite;
		const PaletteID pal = SpriteLayoutPaletteTransform(image, dtss->image.pal, default_palette);
		const Point pt = RemapCoords(dtss->origin.x + dtss->offset.x, dtss->origin.y + dtss->offset.y, dtss->origin.z + dtss->offset.z);
		DrawSprite(image, pal, x + UnScaleByZoom(pt.x, zoom), y + UnScaleByZoom(pt.y, zoom), nullptr, zoom);
	}
}

const DrawTileSprites *GetModularHangarTileLayout(uint8_t rotation, bool small_hangar)
{
	if (small_hangar) {
		switch (rotation) {
			case 1: return &_station_display_modular_small_hangar_ne;
			case 2: return &_station_display_modular_small_hangar_nw;
			case 3: return &_station_display_modular_small_hangar_sw;
			default: return &_station_display_modular_small_hangar_se;
		}
	}
	switch (rotation) {
		case 1: return &_station_display_modular_hangar_ne;
		case 2: return &_station_display_modular_hangar_nw;
		case 3: return &_station_display_modular_hangar_sw;
		default: return &_station_display_modular_hangar_se;
	}
}

static const DrawTileSprites *GetModularHangarTileLayoutByPiece(ModularAirportPieceID piece_type, uint8_t rotation)
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

static const DrawTileSprites *GetModularNSRunwayLayout(ModularAirportPieceID piece_type)
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

const DrawTileSprites *GetAirportTileLayoutWithModularOverrides(uint8_t gfx, ModularAirportPieceID modular_piece_type, uint8_t modular_rotation, uint8_t animation_frame)
{
	const DrawTileSprites *t = nullptr;

	/* An odd rotation is the mirrored variant of a piece that has one. */
	const bool mirrored = (modular_rotation % 2) == 1;

	switch (modular_piece_type) {
		case APT_MODULAR_FIRE_STATION:
			t = mirrored ? &_station_display_modular_fire_station_other : &_station_display_modular_fire_station;
			break;
		case APT_MODULAR_CARGO_TERMINAL:  t = &_station_display_modular_cargo_terminal; break;
		case APT_MODULAR_FUEL_FARM:       t = &_station_display_modular_fuel_farm; break;
		case APT_MODULAR_CAR_PARK:
			t = mirrored ? &_station_display_modular_car_park_other : &_station_display_modular_car_park;
			break;
		default: break;
	}

	if (t == nullptr) {
		if (const DrawTileSprites *hangar_layout = GetModularHangarTileLayoutByPiece(modular_piece_type, modular_rotation); hangar_layout != nullptr) {
			t = hangar_layout;
		}
	}

	/* NS runway sprite override: rotation%2==1 means Y-axis (NW-SE) runway. */
	if (mirrored && t == nullptr) {
		t = GetModularNSRunwayLayout(modular_piece_type);
	}

	/* Legacy small runway sprites include a baked SE fence in stock layouts.
	 * In modular mode fence rendering should come from edge fences only. The base sets
	 * only draw this strip along the X axis, so a Y-axis one uses the mirrored sprites. */
	switch (modular_piece_type) {
		case APT_RUNWAY_SMALL_NEAR_END:
			t = mirrored ? &_station_display_modular_old_runway_near_end_mirror : &_station_display_modular_old_runway_near_end;
			break;
		case APT_RUNWAY_SMALL_MIDDLE:
			t = mirrored ? &_station_display_modular_old_runway_middle_mirror : &_station_display_modular_old_runway_middle;
			break;
		case APT_RUNWAY_SMALL_FAR_END:
			t = mirrored ? &_station_display_modular_old_runway_far_end_mirror : &_station_display_modular_old_runway_far_end;
			break;
		/* The small terminal's pieces likewise face along the X axis only. */
		case APT_SMALL_BUILDING_1:
			if (mirrored) t = &_station_display_modular_small_terminal_c_mirror;
			break;
		case APT_SMALL_BUILDING_2:
			if (mirrored) t = &_station_display_modular_small_terminal_b_mirror;
			break;
		case APT_SMALL_BUILDING_3:
			if (mirrored) t = &_station_display_modular_small_terminal_a_mirror;
			break;
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

	/* APT_STAND_1 and APT_STAND_PIER_NE carry a jetway in their stock layouts,
	 * because in the stock city airport they are only ever the stands beside the
	 * round terminal. Plain stand is the default here;
	 * ApplyModularAirportTileLayoutOverrides puts the jetway back when a round
	 * terminal really is next door. */
	if (modular_piece_type == APT_STAND_1 || modular_piece_type == APT_STAND_PIER_NE) {
		t = &_station_display_modular_stand;
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
		default:
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

	/* Auto-jetway: if this stand tile sits next to a round terminal, replace the
	 * stand sprite with one that carries the matching jetway.
	 *   jetway_1 (APT_STAND_1)       -- terminal one tile to the SE (dy=+1)
	 *   jetway_2 (APT_STAND_PIER_NE) -- terminal one tile to the NE (dx=-1)
	 */
	if (IsModularStandPiece(md->piece_type)) {
		auto NeighborPiece = [&](TileIndexDiff diff) -> ModularAirportPieceID {
			TileIndex nb = ti->tile + diff;
			if (!IsValidTile(nb) || !airport_st->TileBelongsToAirport(nb)) return APT_EMPTY;
			const ModularAirportTileData *nd = airport_st->airport.GetModularTileData(nb);
			return nd != nullptr ? nd->piece_type : APT_EMPTY;
		};

		if (NeighborPiece(TileDiffXY(0, 1)) == APT_ROUND_TERMINAL) {
			gfx = APT_STAND_1;
			t = &_station_display_modular_jetway_1;
			return;
		}
		if (NeighborPiece(TileDiffXY(-1, 0)) == APT_ROUND_TERMINAL) {
			gfx = APT_STAND_PIER_NE;
			t = GetStationTileLayout(StationType::Airport, gfx);
			return;
		}
	}

	t = GetAirportTileLayoutWithModularOverrides(gfx, md->piece_type, md->rotation, GetAnimationFrame(ti->tile));
}

/**
 * Edges along which a piece never draws a perimeter fence.
 * @param piece_type Modular piece type.
 * @param rotation Piece rotation.
 * @return Mask of edge bits (N/E/S/W = 0x01/0x02/0x04/0x08) that stay fence-free.
 */
uint8_t GetModularTileFenceOpenMask(ModularAirportPieceID piece_type, uint8_t rotation)
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
			/* Hangar sprites already depict a fully-walled building (door aside),
			 * so the generic perimeter fence overlay would be redundant/overlapping.
			 * Door-direction handling for taxi/pathfinding purposes is unaffected;
			 * that still goes through CalculateAutoTaxiDirectionsForGfx elsewhere. */
			return 0x0F;
		case APT_APRON_FENCE_NE: case APT_APRON_FENCE_SE:
		case APT_APRON_FENCE_SW: case APT_APRON_FENCE_NW:
			return 0x0F;
		case APT_BUILDING_1: case APT_BUILDING_2: case APT_BUILDING_3:
		case APT_ROUND_TERMINAL:
		case APT_LOW_BUILDING: case APT_LOW_BUILDING_FENCE_N: case APT_LOW_BUILDING_FENCE_NW:
		case APT_SMALL_BUILDING_1: case APT_SMALL_BUILDING_2: case APT_SMALL_BUILDING_3:
		case APT_TOWER: case APT_TOWER_FENCE_SW:
		case APT_MODULAR_FIRE_STATION: case APT_MODULAR_CARGO_TERMINAL:
		case APT_MODULAR_FUEL_FARM: case APT_MODULAR_CAR_PARK:
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

	const Station *station = Station::GetByTile(ti->tile);
	if (station == nullptr || !station->airport.blocks.Test(AirportBlock::Modular)) return;

	const ModularAirportTileData *tile_data = station->airport.GetModularTileData(ti->tile);
	if (tile_data == nullptr) return;

	if (IsModularRunwayPiece(tile_data->piece_type)) {
		const uint8_t flags = tile_data->runway_flags;
		const bool horizontal = (tile_data->rotation % 2) == 0;
		const SpriteID base = SPR_ONEWAY_BASE;

		const bool dir_low = (flags & RUF_DIR_LOW) != 0;
		const bool dir_high = (flags & RUF_DIR_HIGH) != 0;
		const bool can_land = (flags & RUF_LANDING) != 0;
		const bool can_takeoff = (flags & RUF_TAKEOFF) != 0;

		if (can_land || can_takeoff) {
			SpriteID sprite;
			if (dir_low && dir_high) {
				sprite = base + (horizontal ? 2 : 5);
			} else if (dir_low) {
				sprite = base + (horizontal ? 1 : 3);
			} else {
				sprite = base + (horizontal ? 0 : 4);
			}

			PaletteID pal_overlay = PAL_NONE;
			if (!can_land || !can_takeoff) {
				pal_overlay = can_land ? PALETTE_SEL_TILE_BLUE : PALETTE_SEL_TILE_RED;
			}

			DrawGroundSpriteAt(sprite, PAL_NONE, 8, 8, GetPartialPixelZ(8, 8, ti->tileh));
			if (pal_overlay != PAL_NONE) DrawGroundSpriteAt(SPR_SELECT_TILE + SlopeToSpriteOffset(ti->tileh), pal_overlay, 0, 0, 7);
		}
	} else if (IsTaxiwayPiece(tile_data->piece_type) && tile_data->one_way_taxi && HasExactlyOneBit(tile_data->user_taxi_dir_mask)) {
		static constexpr uint8_t kDirOffsets[] = {3, 0, 4, 1}; // 0x01, 0x02, 0x04, 0x08 -> N, E, S, W
		const uint8_t bit = FindFirstBit(static_cast<uint32_t>(tile_data->user_taxi_dir_mask & 0x0F));
		if (bit < lengthof(kDirOffsets)) {
			DrawGroundSpriteAt(SPR_ONEWAY_BASE + kDirOffsets[bit], PAL_NONE, 8, 8, GetPartialPixelZ(8, 8, ti->tileh));
		}
	}
}
