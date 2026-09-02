/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file modular_airport_gui.cpp Modular airport build GUI windows and controls. */

#include "stdafx.h"
#include "economy_func.h"
#include "window_gui.h"
#include "station_gui.h"
#include "station_func.h"
#include "sprite.h"
#include "terraform_gui.h"
#include "sound_func.h"
#include "window_func.h"
#include "strings_func.h"
#include "viewport_func.h"
#include "company_func.h"
#include "tilehighlight_func.h"
#include "company_base.h"
#include "station_type.h"
#include "station_base.h"
#include "station_map.h"
#include "newgrf_airport.h"
#include "newgrf_badge_gui.h"
#include "newgrf_callbacks.h"
#include "dropdown_type.h"
#include "dropdown_func.h"
#include "core/geometry_func.hpp"
#include "hotkeys.h"
#include "vehicle_func.h"
#include "aircraft.h"
#include "gui.h"
#include "command_func.h"
#include "airport_cmd.h"
#include "station_cmd.h"
#include "airport_pathfinder.h"
#include "airport_ground_pathfinder.h"
#include "landscape_cmd.h"
#include "landscape.h"
#include "zoom_func.h"
#include "map_func.h"
#include "direction_func.h"
#include "tilearea_type.h"
#include "error.h"
#include "debug.h"
#include "tile_map.h"
#include "timer/timer.h"
#include "timer/timer_game_calendar.h"
#include "palette_func.h"
#include "gfx_func.h"
#include "modular_airport_cmd.h"
#include "modular_airport_draw.h"
#include "modular_airport_gui.h"
#include "airport_template_gui.h"
#include "newgrf_airporttiles.h"

#include "widgets/airport_widget.h"

#include "table/airporttile_ids.h"
#include "table/strings.h"

#include <cctype>
#include <map>
#include <unordered_set>

#include "safeguards.h"

StationID _last_modular_airport_station = StationID::Invalid();
static uint8_t _modular_hangar_rotation = 0;   ///< 0=SE, 1=NE, 2=NW, 3=SW
static uint8_t _modular_cosmetic_piece = 0;    ///< Selected cosmetic piece in _cosmetic_pieces.
static uint8_t _modular_helipad_piece = 0;     ///< Selected helipad look in _helipad_pieces.

bool _show_runway_direction_overlay = false; ///< Show runway direction/usage arrows in viewport
bool _show_holding_overlay = false;          ///< Show holding loop overlay in viewport
bool _show_taxi_reservation_overlay = false; ///< Show per-aircraft modular reservation chains in viewport

struct ReservationOverlayBounds {
	int left;
	int top;
	int right;
	int bottom;
};

static void MarkReservationOverlayBoundsDirty(const ReservationOverlayBounds &bounds)
{
	MarkAllViewportsDirty(bounds.left, bounds.top, bounds.right, bounds.bottom);
}

static bool IsTileReservedBy(TileIndex tile, VehicleID vid)
{
	if (!IsValidTile(tile)) return false;
	Tile t(tile);
	return IsAirportTile(t) && IsModularAirportTileReservedBy(tile, vid);
}

static bool GetReservationOverlayBoundsForAircraft(const Aircraft *v, ReservationOverlayBounds *out_bounds)
{
	if (v == nullptr || out_bounds == nullptr || !v->IsNormalAircraft()) return false;

	bool has_reserved = false;
	bool has_point = false;
	int left = 0, top = 0, right = 0, bottom = 0;
	const auto include_world = [&](int wx, int wy, int wz) {
		Point p = RemapCoords(wx, wy, wz);
		if (!has_point) {
			has_point = true;
			left = right = p.x;
			top = bottom = p.y;
			return;
		}
		left = std::min(left, p.x);
		top = std::min(top, p.y);
		right = std::max(right, p.x);
		bottom = std::max(bottom, p.y);
	};
	const auto include_tile_if_reserved = [&](TileIndex tile) {
		if (!IsTileReservedBy(tile, v->index)) return;
		has_reserved = true;
		const int wx = TileX(tile) * TILE_SIZE + TILE_SIZE / 2;
		const int wy = TileY(tile) * TILE_SIZE + TILE_SIZE / 2;
		include_world(wx, wy, GetSlopePixelZ(wx, wy) + 4);
	};

	/* Match draw ordering: landing anchor first, then path/taxi/runway chain tiles. */
	const bool landing_phase = v->state == LANDING || v->state == ENDLANDING || v->state == HELILANDING || v->state == HELIENDLANDING;
	if (landing_phase && IsValidTile(v->modular_landing_tile)) include_tile_if_reserved(v->modular_landing_tile);
	if (v->taxi_path != nullptr) {
		for (TileIndex tile : v->taxi_path->tiles) include_tile_if_reserved(tile);
	}
	for (TileIndex tile : v->taxi_reserved_tiles) include_tile_if_reserved(tile);
	for (TileIndex tile : v->modular_runway_reservation) include_tile_if_reserved(tile);
	if (!has_reserved) return false;

	/* Include the aircraft origin because every chain is drawn from current position. */
	include_world(v->x_pos, v->y_pos, v->z_pos + 4);

	static constexpr int OVERLAY_MARGIN = 10;
	out_bounds->left = left - OVERLAY_MARGIN;
	out_bounds->top = top - OVERLAY_MARGIN;
	out_bounds->right = right + OVERLAY_MARGIN;
	out_bounds->bottom = bottom + OVERLAY_MARGIN;
	return true;
}

static const WindowNumber WN_BUILD_MODULAR_AIRPORT = WindowNumber{TransportType::Air};
struct ModularAirportPiece {
	StringID name;    ///< Full name (used as tooltip)
	SpriteID icon;    ///< Toolbar button icon sprite
	PixelColour colour;
	bool ground_tile = false; ///< Icon is a whole ground tile, so draw it anchored on the tile.
};

struct CosmeticPiece {
	StringID name;
	SpriteID icon;    ///< Small icon at Out2x zoom (for toolbar button)
	SpriteID ground;  ///< Optional ground sprite drawn behind icon (0 = none)
	ModularAirportPieceID apt_gfx; ///< AirportTiles/NewGRF ID or metadata-only piece for placement and preview.
	int8_t preview_y_offset; ///< Vertical bias in picker preview; positive moves down. Unused by a multi-tile piece, whose preview is measured.
	bool is_multi_tile = false; ///< True if this piece places multiple tiles at once.
	bool use_layout_preview = false; ///< Draw the complete modular tile layout instead of a bare icon.
	uint8_t rotation = 0; ///< Fixed orientation used by the picker and placement.
};

struct HelipadPiece {
	StringID name;
	SpriteID icon;
	uint8_t apt_gfx;  ///< AirportTiles value for placement and picker preview.
	int8_t preview_y_offset; ///< Vertical bias in picker preview; positive moves down.
};

static constexpr CosmeticPiece _cosmetic_pieces[] = {
	{STR_STATION_BUILD_MODULAR_AIRPORT_PIECE_TERMINAL,         SPR_AIRPORT_TERMINAL_C,       0,                    APT_BUILDING_1,          3},
	{STR_STATION_BUILD_MODULAR_AIRPORT_PIECE_TERMINAL_ALT,     SPR_AIRPORT_TERMINAL_A,       0,                    APT_BUILDING_2,          3},
	{STR_STATION_BUILD_MODULAR_AIRPORT_PIECE_TERMINAL_OTHER,   SPR_AIRPORT_TERMINAL_B,       0,                    APT_BUILDING_3,          3},
	{STR_STATION_BUILD_MODULAR_AIRPORT_PIECE_TERMINAL_ROUND,   SPR_AIRPORT_CONCOURSE,        0,                    APT_ROUND_TERMINAL,      3},
	{STR_STATION_BUILD_MODULAR_AIRPORT_PIECE_LOW_TERMINAL,     SPR_AIRPORT_HELIDEPOT_OFFICE, 0,                    APT_LOW_BUILDING,        0},
	{STR_STATION_BUILD_MODULAR_AIRPORT_PIECE_TOWER,            SPR_AIRPORT_TOWER,            0,                    APT_TOWER,               3},
	{STR_STATION_BUILD_MODULAR_AIRPORT_PIECE_RADIO_TOWER,      SPR_TRANSMITTER,              0,                    APT_RADIO_TOWER_FENCE_NE, 14},
	{STR_STATION_BUILD_MODULAR_AIRPORT_PIECE_FLAG_GRASS,       SPR_AIRFIELD_WIND_1,          SPR_FLAT_GRASS_TILE,  APT_GRASS_FENCE_NE_FLAG_2, 0},
	{STR_STATION_BUILD_MODULAR_AIRPORT_PIECE_RADAR,            SPR_AIRPORT_RADAR_5,          0,                    APT_RADAR_FENCE_NE,      0},
	{STR_STATION_BUILD_MODULAR_AIRPORT_PIECE_RADAR_GRASS,      SPR_AIRPORT_RADAR_5,          SPR_FLAT_GRASS_TILE,  APT_RADAR_GRASS_FENCE_SW, 0},
	{STR_STATION_BUILD_MODULAR_AIRPORT_PIECE_SMALL_TERMINAL_3, SPR_AIRFIELD_TERM_B,          0,                    APT_SMALL_BUILDING_2,     0, true,  false, 0},
	{STR_STATION_BUILD_MODULAR_AIRPORT_PIECE_FIRE_STATION,     SPR_AIRPORT_FIRE_STATION_OTHER, 0,                  APT_MODULAR_FIRE_STATION, 0, false, true,  1},
	{STR_STATION_BUILD_MODULAR_AIRPORT_PIECE_FIRE_STATION,     SPR_AIRPORT_FIRE_STATION,     0,                    APT_MODULAR_FIRE_STATION, 0, false, true,  0},
	{STR_STATION_BUILD_MODULAR_AIRPORT_PIECE_SMALL_TERMINAL_3, SPR_MIRROR_AIRFIELD_TERM_B,   0,                    APT_SMALL_BUILDING_2,     0, true,  false, 1},
	{STR_STATION_BUILD_MODULAR_AIRPORT_PIECE_CARGO_TERMINAL,   SPR_AIRPORT_CARGO_TERMINAL,   0,                    APT_MODULAR_CARGO_TERMINAL, 0, false, true},
	{STR_STATION_BUILD_MODULAR_AIRPORT_PIECE_FUEL_FARM,        SPR_AIRPORT_FUEL_FARM,        0,                    APT_MODULAR_FUEL_FARM, 0, false, true},
	{STR_STATION_BUILD_MODULAR_AIRPORT_PIECE_CAR_PARK,         SPR_AIRPORT_CAR_PARK,         0,                    APT_MODULAR_CAR_PARK, 0, false, true, 0},
	{STR_STATION_BUILD_MODULAR_AIRPORT_PIECE_CAR_PARK,         SPR_AIRPORT_CAR_PARK_OTHER,   0,                    APT_MODULAR_CAR_PARK, 0, false, true, 1},
};

static constexpr HelipadPiece _helipad_pieces[] = {
	{STR_STATION_BUILD_MODULAR_AIRPORT_PIECE_HELIPAD, SPR_AIRPORT_HELIPAD, APT_HELIPAD_2,          0},
	{STR_STATION_BUILD_MODULAR_AIRPORT_PIECE_PLAIN_H, SPR_NEWHELIPAD,       APT_HELIPAD_3_FENCE_NW, 0},
	{STR_AIRPORT_HELIPORT,                            SPR_HELIPORT,         APT_HELIPORT,           10},
};

static_assert(lengthof(_cosmetic_pieces) == WID_MACP_PIECE_LAST - WID_MACP_PIECE_FIRST + 1);
static_assert(lengthof(_helipad_pieces) == WID_MAHPAD_PIECE_LAST - WID_MAHPAD_PIECE_FIRST + 1);

/**
 * Whether a piece cannot be built right now, so its button is shown disabled.
 *
 * Two gates gather here: a modern piece is unavailable until the year the large
 * airport arrives, and a piece backed by a stored openttd.grf bitmap is
 * unavailable while the setting for those is off.
 *
 * @param gfx Piece to check.
 * @param rotation Rotation the button places the piece in, which is what tells a
 *                 mirrored piece from the base set's own one.
 * @return True if the piece may not be placed now.
 */
static bool IsModularPieceLocked(ModularAirportPieceID gfx, uint8_t rotation = 0)
{
	if (IsNewAirportGraphicsPiece(gfx, rotation) && !AreNewAirportGraphicsAvailable()) return true;
	return IsModernModularPiece(gfx) && TimerGameCalendar::year < GetModularPieceMinYear(gfx);
}

/**
 * Match the tile highlight to the footprint of the selected cosmetic piece.
 *
 * A lock can change that selection under the player -- from the three-tile small
 * terminal to a one-tile piece, say -- so this runs wherever the selection moves,
 * not only where the player moves it.
 */
static void UpdateModularCosmeticSelectSize()
{
	const CosmeticPiece &piece = _cosmetic_pieces[std::min<uint8_t>(_modular_cosmetic_piece, lengthof(_cosmetic_pieces) - 1)];
	const Dimension size = GetModularCompoundPieceSize(piece.apt_gfx, piece.rotation);
	SetTileSelectSize(size.width, size.height);
}

static constexpr ModularAirportPiece _modular_airport_pieces[] = {
	{STR_STATION_BUILD_MODULAR_AIRPORT_PIECE_RUNWAY,           SPR_AIRPORT_RUNWAY_EXIT_B,  PC_DARK_GREY,     true},  // 0
	{STR_STATION_BUILD_MODULAR_AIRPORT_PIECE_RUNWAY_END,       SPR_NSRUNWAY_END,           PC_DARK_GREY,     true},  // 1
	{STR_STATION_BUILD_MODULAR_AIRPORT_PIECE_RUNWAY_SMALL_MID, SPR_AIRFIELD_RUNWAY_MIDDLE, PC_DARK_GREY,     true},  // 2  (smart-drag: auto-adds near/far ends)
	{STR_STATION_BUILD_MODULAR_AIRPORT_PIECE_COSMETIC,         SPR_AIRPORT_CONCOURSE,      PC_ORANGE},               // 3 (cosmetic picker)
	{STR_STATION_BUILD_MODULAR_AIRPORT_PIECE_HANGAR,           SPR_AIRPORT_HANGAR_FRONT,   PC_DARK_RED},             // 4
	{STR_STATION_BUILD_MODULAR_AIRPORT_PIECE_SMALL_HANGAR,     SPR_AIRFIELD_HANGAR_FRONT,  PC_DARK_RED},             // 5
	{STR_STATION_BUILD_MODULAR_AIRPORT_PIECE_HELIPAD,          SPR_NEWHELIPAD,             PC_LIGHT_YELLOW},         // 6 (an overlay, not a whole tile)
	{STR_STATION_BUILD_MODULAR_AIRPORT_PIECE_STAND,            SPR_AIRPORT_AIRCRAFT_STAND, PC_YELLOW,        true},  // 7
	{STR_STATION_BUILD_MODULAR_AIRPORT_PIECE_APRON,            SPR_AIRPORT_APRON,          PC_GREY,          true},  // 8
	{STR_STATION_BUILD_MODULAR_AIRPORT_PIECE_GRASS,            SPR_AIRFIELD_APRON_C,       PC_GREEN,         true},  // 9
	{STR_STATION_BUILD_MODULAR_AIRPORT_PIECE_EMPTY,            SPR_FLAT_GRASS_TILE,        PC_WHITE,         true},  // 10
	{STR_STATION_BUILD_MODULAR_AIRPORT_PIECE_ERASE,            SPR_IMG_DYNAMITE,           PC_WHITE},                // 11
};

static constexpr int MODULAR_AIRPORT_PIECE_ERASE_INDEX = lengthof(_modular_airport_pieces) - 1;

static ModularAirportPieceID GetModularAirportPieceGfx(uint8_t piece)
{
	switch (piece) {
		case 0:  return APT_RUNWAY_5;
		case 1:  return APT_RUNWAY_END;
		case 2:  return APT_RUNWAY_SMALL_MIDDLE;
		case 3:  return _cosmetic_pieces[std::min<uint8_t>(_modular_cosmetic_piece, lengthof(_cosmetic_pieces) - 1)].apt_gfx;
		case 4:  return APT_DEPOT_SE;
		case 5:  return APT_SMALL_DEPOT_SE;
		case 6:  return _helipad_pieces[std::min<uint8_t>(_modular_helipad_piece, lengthof(_helipad_pieces) - 1)].apt_gfx;
		case 7:  return APT_STAND;
		case 8:  return APT_APRON;
		case 9:  return APT_GRASS_1;
		case 10: return APT_EMPTY;
		default: return APT_APRON;
	}
}

/**
 * Graphics the builder places that no toolbar button selects directly.
 *
 * The small runway tool drags a strip of APT_RUNWAY_SMALL_MIDDLE and caps it
 * with these two, so they are part of the builder's vocabulary even though they
 * have no button of their own.
 */
static constexpr uint8_t _modular_airport_implicit_gfx[] = {
	APT_RUNWAY_SMALL_NEAR_END,
	APT_RUNWAY_SMALL_FAR_END,
};

/**
 * Every airport graphic the modular builder can place on the map.
 *
 * The tables above are the authority on what a modular airport is built from:
 * the toolbar, the cosmetic picker and the helipad picker are between them its
 * whole vocabulary. AirportTiles holds a good deal more, but the rest belongs to
 * stock airports and reaches a modular airport only through conversion -- those
 * graphics must not become buildable by any other route, the script API
 * included. See ModularAirportBuilderVocabulary in the tests, which holds that
 * line.
 *
 * A compound piece is listed once, under the graphic that names it in the picker
 * table; GetModularCompoundPieceTiles says what it puts on the ground.
 */
std::vector<ModularAirportPieceID> GetModularAirportBuilderPieceGfx()
{
	std::vector<ModularAirportPieceID> out;

	for (uint8_t i = 0; i < lengthof(_modular_airport_pieces); i++) {
		/* The two picker buttons resolve to whatever the picker currently has
		 * selected; their graphics come from the picker tables below. The last
		 * entry is the eraser and places nothing. */
		if (i == 3 || i == 6 || i == MODULAR_AIRPORT_PIECE_ERASE_INDEX) continue;
		out.push_back(GetModularAirportPieceGfx(i));
	}
	for (const CosmeticPiece &p : _cosmetic_pieces) {
		out.push_back(p.apt_gfx);
	}
	for (const HelipadPiece &p : _helipad_pieces) {
		out.push_back(p.apt_gfx);
	}
	for (uint8_t gfx : _modular_airport_implicit_gfx) {
		out.push_back(gfx);
	}

	std::sort(out.begin(), out.end());
	out.erase(std::unique(out.begin(), out.end()), out.end());
	return out;
}

/**
 * The three tiles of the small terminal.
 *
 * Each has its own graphic and they only join up along one axis, so this is the
 * whole of the piece's geometry. Anything that places it -- the picker below, the
 * script API -- works from this one definition.
 */
static constexpr ModularCompoundPieceTile _small_terminal_3_tiles[] = {
	{0, 0, APT_SMALL_BUILDING_1},
	{1, 0, APT_SMALL_BUILDING_2},
	{2, 0, APT_SMALL_BUILDING_3},
};

/** The same three tiles laid along the other axis, drawn from the mirrored sprites. */
static constexpr ModularCompoundPieceTile _small_terminal_3_tiles_mirror[] = {
	{0, 0, APT_SMALL_BUILDING_1},
	{0, 1, APT_SMALL_BUILDING_2},
	{0, 2, APT_SMALL_BUILDING_3},
};

std::span<const ModularCompoundPieceTile> GetModularCompoundPieceTiles(ModularAirportPieceID gfx, uint8_t rotation)
{
	if (gfx == APT_SMALL_BUILDING_2) {
		return (rotation % 2) == 1 ? _small_terminal_3_tiles_mirror : _small_terminal_3_tiles;
	}
	return {};
}

Dimension GetModularCompoundPieceSize(ModularAirportPieceID gfx, uint8_t rotation)
{
	Dimension size{1, 1};
	for (const ModularCompoundPieceTile &ct : GetModularCompoundPieceTiles(gfx, rotation)) {
		size.width = std::max<uint>(size.width, ct.dx + 1);
		size.height = std::max<uint>(size.height, ct.dy + 1);
	}
	return size;
}

/** Screen box a modular piece's preview covers. Right and bottom edges are exclusive. */
struct ModularPiecePreviewBox {
	int left = 0;   ///< Leftmost pixel, relative to the drawing origin of the piece.
	int top = 0;    ///< Topmost pixel, relative to that same origin.
	int right = 0;  ///< One past the rightmost pixel.
	int bottom = 0; ///< One past the bottom pixel.

	int Width() const { return this->right - this->left; }
	int Height() const { return this->bottom - this->top; }
};

/** Measure one tile layout relative to its drawing origin. */
static ModularPiecePreviewBox GetModularTilePreviewBox(const DrawTileSprites *t, ZoomLevel zoom)
{
	ModularPiecePreviewBox box{INT_MAX, INT_MAX, INT_MIN, INT_MIN};

	auto include = [&](SpriteID sprite, int x, int y) {
		/* TTD sprite 0 means no sprite. */
		if (GB(sprite, 0, SPRITE_WIDTH) == 0 && !HasBit(sprite, SPRITE_MODIFIER_CUSTOM_SPRITE)) return;

		Point offset;
		const Dimension d = GetSpriteSize(sprite & SPRITE_MASK, &offset, zoom);
		box.left   = std::min(box.left,   x + offset.x);
		box.top    = std::min(box.top,    y + offset.y);
		box.right  = std::max(box.right,  x + static_cast<int>(d.width));
		box.bottom = std::max(box.bottom, y + static_cast<int>(d.height));
	};

	include(t->ground.sprite, 0, 0);
	for (const DrawTileSeqStruct &dtss : t->GetSequence()) {
		if (!dtss.IsParentSprite()) continue;
		const Point pt = RemapCoords(dtss.origin.x + dtss.offset.x, dtss.origin.y + dtss.offset.y, dtss.origin.z + dtss.offset.z);
		include(dtss.image.sprite, UnScaleByZoom(pt.x, zoom), UnScaleByZoom(pt.y, zoom));
	}

	if (box.left > box.right) return {};
	return box;
}

/**
 * Where one tile of a compound piece is drawn, relative to the piece's first tile.
 *
 * A compound piece's tiles sit on the map, so their preview follows the map's projection:
 * a step along +X goes down-left on screen and a step along +Y down-right. Reading the
 * step off a tile sprite's size instead would follow the base set, which is free to draw
 * a tile sprite taller than the tile it covers -- aBase's terminal reaches above its tile,
 * and half of that overshoot went into the step and pulled the row apart into a staircase.
 *
 * @param ct Tile of the compound piece.
 * @param zoom Zoom level the preview is drawn at.
 * @return Offset in pixels from the first tile's drawing origin.
 */
static Point GetModularCompoundPieceTileOffset(const ModularCompoundPieceTile &ct, ZoomLevel zoom)
{
	const Point pt = RemapCoords(ct.dx * TILE_SIZE, ct.dy * TILE_SIZE, 0);
	return {UnScaleByZoom(pt.x, zoom), UnScaleByZoom(pt.y, zoom)};
}

/** The tiles of a compound piece, back to front: the viewport paints a tile in front of
 * another when it starts beyond it along an axis, which on a flat piece is their sum. */
static std::vector<const ModularCompoundPieceTile *> GetModularCompoundPieceTilesBackToFront(ModularAirportPieceID gfx, uint8_t rotation)
{
	std::vector<const ModularCompoundPieceTile *> order;
	for (const ModularCompoundPieceTile &ct : GetModularCompoundPieceTiles(gfx, rotation)) order.push_back(&ct);
	std::stable_sort(order.begin(), order.end(), [](const ModularCompoundPieceTile *a, const ModularCompoundPieceTile *b) {
		return a->dx + a->dy < b->dx + b->dy;
	});
	return order;
}

/**
 * Measure what a compound piece's preview covers, so the button can be sized to hold it
 * and the preview placed inside without guessing at either.
 *
 * Every sprite the preview draws is measured, buildings included: how far a piece reaches
 * above its tiles is the base set's business, and a fixed allowance either crops aBase's
 * terminal or leaves the others sitting in empty space.
 *
 * @param gfx Graphic naming the compound piece.
 * @param rotation Rotation the piece is previewed in.
 * @param zoom Zoom level the preview is drawn at.
 * @return The box covered, or an empty box if @a gfx is not a compound piece.
 */
static ModularPiecePreviewBox GetModularCompoundPiecePreviewBox(ModularAirportPieceID gfx, uint8_t rotation, ZoomLevel zoom)
{
	ModularPiecePreviewBox box{INT_MAX, INT_MAX, INT_MIN, INT_MIN};

	for (const ModularCompoundPieceTile *ct : GetModularCompoundPieceTilesBackToFront(gfx, rotation)) {
		const Point tile = GetModularCompoundPieceTileOffset(*ct, zoom);
		const DrawTileSprites *t = GetAirportTileLayoutWithModularOverrides(ct->gfx, ct->gfx, rotation);
		const ModularPiecePreviewBox tile_box = GetModularTilePreviewBox(t, zoom);
		/* A layout that paints nothing measures as an empty box at the origin. Folding
		 * that into the union would stretch the piece's box to the tile's own origin,
		 * which is not a pixel anything draws on, and shift the centring. */
		if (tile_box.Width() == 0 && tile_box.Height() == 0) continue;
		box.left   = std::min(box.left,   tile.x + tile_box.left);
		box.top    = std::min(box.top,    tile.y + tile_box.top);
		box.right  = std::max(box.right,  tile.x + tile_box.right);
		box.bottom = std::max(box.bottom, tile.y + tile_box.bottom);
	}

	if (box.left > box.right) return {};
	return box;
}

/**
 * Draw a compound piece's preview, one tile at a time and each from its own layout, so
 * that what the button shows is what the piece puts on the ground.
 *
 * @param x Drawing origin of the piece's first tile.
 * @param y Drawing origin of the piece's first tile.
 * @param gfx Graphic naming the compound piece.
 * @param rotation Rotation the piece is previewed in.
 * @param pal Company palette to recolour with.
 * @param zoom Zoom level to draw at.
 */
static void DrawModularCompoundPiecePreview(int x, int y, ModularAirportPieceID gfx, uint8_t rotation, PaletteID pal, ZoomLevel zoom)
{
	for (const ModularCompoundPieceTile *ct : GetModularCompoundPieceTilesBackToFront(gfx, rotation)) {
		const Point tile = GetModularCompoundPieceTileOffset(*ct, zoom);
		const DrawTileSprites *t = GetAirportTileLayoutWithModularOverrides(ct->gfx, ct->gfx, rotation);
		const SpriteID ground = t->ground.sprite;
		DrawSprite(ground, HasBit(ground, PALETTE_MODIFIER_COLOUR) ? pal : PAL_NONE, x + tile.x, y + tile.y, nullptr, zoom);
		DrawModularTileSeqInGUI(x + tile.x, y + tile.y, t, pal, zoom);
	}
}

static void ShowModularHangarPicker(Window *parent, bool large_hangar);
static void ShowModularCosmeticPicker(Window *parent);
static void ShowModularHelipadPicker(Window *parent);
static void ShowModularInfoOverlayWindow(Window *parent);

class BuildModularInfoOverlayWindow;

static Point GetModularAirportChildWindowPosition(const Window *parent, int16_t sm_width, int16_t sm_height, bool align_right)
{
	if (parent == nullptr) return GetToolbarAlignedWindowPosition(sm_width);

	int x = align_right ? parent->left + parent->width - sm_width : parent->left;
	int y = parent->top + parent->height;

	x = SoftClamp(x, 0, _screen.width - sm_width);
	if (y + sm_height > _screen.height) y = std::max(0, parent->top - sm_height);
	y = SoftClamp(y, 0, _screen.height - sm_height);

	return {x, y};
}

class BuildModularAirportWindow : public PickerWindowBase {
	static constexpr int PIECE_COUNT = lengthof(_modular_airport_pieces);
	friend class BuildModularInfoOverlayWindow;

	uint8_t selected_piece = static_cast<uint8_t>(PIECE_COUNT);
	bool show_taxi_arrows = true;
	bool show_holding_loop = false;
	bool show_taxi_reservations = false;
	std::map<VehicleID, ReservationOverlayBounds> reservation_overlay_bounds;
	bool updating_cursor = false; ///< True while UpdatePlacementCursor is running (suppresses abort side-effects).
	bool fence_tool_active = false; ///< When true, clicks toggle edge fences instead of placing tiles.
	bool upgrade_tool_active = false; ///< When true, clicks upgrade old tiles to modern variants.
	TimerGameCalendar::Year cached_year = CalendarTime::MIN_YEAR;
	const IntervalTimer<TimerGameCalendar> yearly_interval = {{TimerGameCalendar::Trigger::Year, TimerGameCalendar::Priority::None}, [this](auto) {
		this->RefreshPieceGating();
	}};

public:
	BuildModularAirportWindow(WindowDesc &desc, Window *parent) : PickerWindowBase(desc, parent)
	{
		this->InitNested(WN_BUILD_MODULAR_AIRPORT);
		this->SetWidgetLoweredState(WID_MA_TEMPLATE_MANAGER, false);
		this->SetWidgetLoweredState(WID_MA_INFO_OVERLAY, false);
		this->UpdatePieceGating();
		this->cached_year = TimerGameCalendar::year;
		this->UpdatePlacementCursor();
		_show_runway_direction_overlay = this->show_taxi_arrows;
		_show_holding_overlay = this->show_holding_loop;
		_show_taxi_reservation_overlay = this->show_taxi_reservations;
		MarkWholeScreenDirty();
		if (_settings_client.gui.link_terraform_toolbar) ShowTerraformToolbar(this);
	}

	void StopPlacementFromClosedPicker(uint8_t picker_piece)
	{
		if (this->selected_piece != picker_piece) return;
		this->RaiseWidget(WID_MA_PIECE_0 + this->selected_piece);
		this->selected_piece = static_cast<uint8_t>(PIECE_COUNT);
		this->UpdatePlacementCursor();
		this->SetDirty();
	}

	static uint8_t FirstAvailableCosmeticPiece()
	{
		for (uint8_t i = 0; i < lengthof(_cosmetic_pieces); i++) {
			if (!IsModularPieceLocked(_cosmetic_pieces[i].apt_gfx, _cosmetic_pieces[i].rotation)) return i;
		}
		return 0;
	}

	static uint8_t FirstAvailableHelipadPiece()
	{
		for (uint8_t i = 0; i < lengthof(_helipad_pieces); i++) {
			if (!IsModularPieceLocked(_helipad_pieces[i].apt_gfx)) return i;
		}
		return 0;
	}

	void NormalizePickerSelections()
	{
		if (_modular_cosmetic_piece >= lengthof(_cosmetic_pieces) ||
				IsModularPieceLocked(_cosmetic_pieces[_modular_cosmetic_piece].apt_gfx, _cosmetic_pieces[_modular_cosmetic_piece].rotation)) {
			_modular_cosmetic_piece = FirstAvailableCosmeticPiece();
			/* The cosmetic tool may be placing right now, and the piece it places has
			 * just changed size under it. */
			if (this->selected_piece == 3) UpdateModularCosmeticSelectSize();
		}
		if (_modular_helipad_piece >= lengthof(_helipad_pieces) || IsModularPieceLocked(_helipad_pieces[_modular_helipad_piece].apt_gfx)) {
			_modular_helipad_piece = FirstAvailableHelipadPiece();
		}
	}

	bool IsPieceButtonLocked(uint8_t piece) const
	{
		if (piece == MODULAR_AIRPORT_PIECE_ERASE_INDEX) return false;
		if (piece == 3) {
			for (const CosmeticPiece &cp : _cosmetic_pieces) {
				if (!IsModularPieceLocked(cp.apt_gfx, cp.rotation)) return false;
			}
			return true;
		}
		if (piece == 6) {
			for (const HelipadPiece &hp : _helipad_pieces) {
				if (!IsModularPieceLocked(hp.apt_gfx)) return false;
			}
			return true;
		}
		return IsModularPieceLocked(GetModularAirportPieceGfx(piece));
	}

	/** Disable piece buttons that are gated behind a future year or a switched-off graphics setting. */
	void UpdatePieceGating()
	{
		this->NormalizePickerSelections();
		for (int i = 0; i < PIECE_COUNT; i++) {
			this->SetWidgetDisabledState(WID_MA_PIECE_0 + i, this->IsPieceButtonLocked(static_cast<uint8_t>(i)));
		}
		if (this->selected_piece < PIECE_COUNT && this->IsWidgetDisabled(WID_MA_PIECE_0 + this->selected_piece)) {
			this->RaiseWidget(WID_MA_PIECE_0 + this->selected_piece);
			this->selected_piece = static_cast<uint8_t>(PIECE_COUNT);
			/* The selection is gone, so its picker must go too, or it stands for no tool. */
			CloseWindowByClass(WindowClass::BuildDepot);
			this->UpdatePlacementCursor();
		}
	}

	void RefreshPieceGating()
	{
		this->UpdatePieceGating();
		InvalidateWindowClassesData(WindowClass::BuildDepot, 0);
		this->SetDirty();
	}

	Point OnInitialPosition(int16_t sm_width, [[maybe_unused]] int16_t sm_height, [[maybe_unused]] int window_number) override
	{
		/* Place it like the other construction toolbars: under the main toolbar. */
		return AlignInitialConstructionToolbar(sm_width);
	}

	void Close([[maybe_unused]] int data = 0) override
	{
		_show_runway_direction_overlay = false;
		_show_holding_overlay = false;
		_show_taxi_reservation_overlay = false;
		this->reservation_overlay_bounds.clear();
		MarkWholeScreenDirty();
		if (_thd.window_class == this->window_class && _thd.window_number == this->window_number) {
			ResetObjectToPlace();
		}
		if (_settings_client.gui.link_terraform_toolbar) CloseWindowById(WindowClass::ScenarioGenerateLandscape, 0, false);
		CloseWindowById(WindowClass::AirportTemplateManager, 0);
		CloseWindowById(WindowClass::ModularAirportInfoOverlay, 0);
		CloseWindowByClass(WindowClass::BuildDepot);
		CloseWindowById(WindowClass::JoinStation, 0);
		/* Use Window::Close() instead of PickerWindowBase::Close() to avoid
		 * an unconditional ResetObjectToPlace() -- the guard above already
		 * handles our own cursor, and we must not reset another window's cursor
		 * (e.g. the stock airport builder toolbar). */
		this->Window::Close();
	}

	void UpdateWidgetSize(WidgetID widget, Dimension &size, [[maybe_unused]] const Dimension &padding, [[maybe_unused]] Dimension &fill, [[maybe_unused]] Dimension &resize) override
	{
		if (widget == WID_MA_FENCE_TOOL) {
			/* Keep the fence tool the same fixed width as other toolbar buttons, regardless of sprite width. */
			size.width = ScaleGUITrad(22);
			size.height = std::max<uint>(size.height, ScaleGUITrad(22));
			return;
		}
		if (widget < WID_MA_PIECE_FIRST || widget > WID_MA_PIECE_LAST) return;
		/* Keep piece buttons the same size as standard construction toolbar buttons. */
		size.width = std::max<uint>(size.width, ScaleGUITrad(22));
		size.height = std::max<uint>(size.height, ScaleGUITrad(22));
	}

	void DrawWidget(const Rect &r, WidgetID widget) const override
	{
		if (widget == WID_MA_FENCE_TOOL) {
			DrawPixelInfo tmp_dpi;
			Rect ir = r.Shrink(WidgetDimensions::scaled.bevel);
			if (!FillDrawPixelInfo(&tmp_dpi, ir)) return;
			AutoRestoreBackup dpi_backup(_cur_dpi, &tmp_dpi);
			Point offset;
			ZoomLevel icon_zoom = _gui_zoom;
			if (icon_zoom < ZoomLevel::Max) ++icon_zoom;
			Dimension d = GetSpriteSize(SPR_AIRPORT_FENCE_Y, &offset, icon_zoom);
			d.width -= offset.x;
			d.height -= offset.y;
			int x = (ir.Width() - static_cast<int>(d.width)) / 2;
			int y = (ir.Height() - static_cast<int>(d.height)) / 2;
			DrawSprite(SPR_AIRPORT_FENCE_Y, PAL_NONE, x - offset.x, y - offset.y, nullptr, icon_zoom);
			return;
		}
		if (widget < WID_MA_PIECE_FIRST || widget > WID_MA_PIECE_LAST) return;
		const auto &piece = _modular_airport_pieces[widget - WID_MA_PIECE_FIRST];
		DrawPixelInfo tmp_dpi;
		Rect ir = r.Shrink(WidgetDimensions::scaled.bevel);
		if (!FillDrawPixelInfo(&tmp_dpi, ir)) return;
		AutoRestoreBackup dpi_backup(_cur_dpi, &tmp_dpi);
		Point offset;
		PaletteID pal = GetCompanyPalette(_local_company);
		if (widget == WID_MA_PIECE_LAST) {
			/* Demolish: draw at full scale to match other toolbar demolish buttons. */
			Dimension d = GetSpriteSize(piece.icon, &offset);
			d.width  -= offset.x;
			d.height -= offset.y;
			int x = (ir.Width()  - static_cast<int>(d.width))  / 2;
			int y = (ir.Height() - static_cast<int>(d.height)) / 2;
			DrawSprite(piece.icon, pal, x - offset.x, y - offset.y);
		} else if (widget == WID_MA_PIECE_4 || widget == WID_MA_PIECE_5) {
			/* Match the richer hangar preview style from the direction picker (ground + wall),
			 * but draw one zoom step smaller so it fits the toolbar button. */
			ZoomLevel icon_zoom = _gui_zoom;
			if (icon_zoom < ZoomLevel::Max) ++icon_zoom;

			int tile_w = UnScaleByZoom(64 * ZOOM_BASE, icon_zoom);
			int tile_h = UnScaleByZoom(48 * ZOOM_BASE, icon_zoom);
			int anchor = UnScaleByZoom(31 * ZOOM_BASE, icon_zoom);
			int x = (ir.Width()  - tile_w) / 2 + anchor;
			int y = (ir.Height() + tile_h) / 2 - anchor;

			const DrawTileSprites *t = GetModularHangarTileLayout(0, widget == WID_MA_PIECE_5);
			DrawSprite(t->ground.sprite, HasBit(t->ground.sprite, PALETTE_MODIFIER_COLOUR) ? pal : PAL_NONE, x, y, nullptr, icon_zoom);
			DrawModularTileSeqInGUI(x, y, t, pal, icon_zoom);
		} else {
			SpriteID icon = piece.icon;
			ZoomLevel icon_zoom = _gui_zoom;
			if (icon_zoom < ZoomLevel::Max) ++icon_zoom;
			if (piece.ground_tile) {
				/* A ground tile sprite is anchored on the tile's northern corner, so place
				 * that corner instead of centring the sprite's bounding box. A base set may
				 * extend a tile sprite well above the tile it covers -- aBase's runway and
				 * stand tiles reach 96 and 48 pixels up -- and centring the box then slides
				 * the tile itself off the bottom of the button. */
				int tile_w = UnScaleByZoom(64 * ZOOM_BASE, icon_zoom);
				int tile_h = UnScaleByZoom(32 * ZOOM_BASE, icon_zoom);
				int anchor = UnScaleByZoom(31 * ZOOM_BASE, icon_zoom);
				DrawSprite(icon, pal, (ir.Width() - tile_w) / 2 + anchor, (ir.Height() - tile_h) / 2, nullptr, icon_zoom);
			} else {
				Dimension d = GetSpriteSize(icon, &offset, icon_zoom);
				d.width  -= offset.x;
				d.height -= offset.y;
				int x = (ir.Width()  - static_cast<int>(d.width))  / 2;
				int y = (ir.Height() - static_cast<int>(d.height)) / 2;
				DrawSprite(icon, pal, x - offset.x, y - offset.y, nullptr, icon_zoom);
			}
		}
	}

	void OnClick([[maybe_unused]] Point pt, WidgetID widget, [[maybe_unused]] int click_count) override
	{
		if (widget >= WID_MA_PIECE_FIRST && widget <= WID_MA_PIECE_LAST) {
			uint8_t new_piece = static_cast<uint8_t>(widget - WID_MA_PIECE_FIRST);
			bool already_selected = (new_piece == this->selected_piece);
			bool wants_picker = (new_piece == 3 || new_piece == 4 || new_piece == 5 || new_piece == 6);

			/* Deactivate fence/upgrade tools when selecting a piece. */
			if (this->fence_tool_active) {
				this->fence_tool_active = false;
				this->SetWidgetLoweredState(WID_MA_FENCE_TOOL, false);
			}
			if (this->upgrade_tool_active) {
				this->upgrade_tool_active = false;
				this->SetWidgetLoweredState(WID_MA_UPGRADE_TOOL, false);
			}
			/* Raise the previously selected piece button. */
			if (this->selected_piece < PIECE_COUNT) this->RaiseWidget(WID_MA_PIECE_0 + this->selected_piece);

			/* Close any open sub-picker and update the cursor. Both can trigger
			 * OnPlaceObjectAbort on this window; the updating_cursor guard in
			 * UpdatePlacementCursor prevents that from clearing our state. */
			CloseWindowByClass(WindowClass::BuildDepot);

			/* Update selection state. */
			if (already_selected) {
				/* Toggle off: clicking the same piece deselects it. */
				this->selected_piece = static_cast<uint8_t>(PIECE_COUNT);
			} else {
				this->selected_piece = new_piece;
				this->LowerWidget(WID_MA_PIECE_0 + this->selected_piece);
			}

			/* Update the placement cursor. */
			this->UpdatePlacementCursor();
			this->SetDirty();

			/* Open picker for pieces that need one (unless we just toggled off). */
			if (wants_picker && !already_selected) {
				if (new_piece == 4 || new_piece == 5) {
					ShowModularHangarPicker(this, new_piece == 4);
				} else if (new_piece == 3) {
					ShowModularCosmeticPicker(this);
				} else {
					ShowModularHelipadPicker(this);
				}
			}
			return;
		}

		switch (widget) {
			case WID_MA_FENCE_TOOL:
				this->fence_tool_active = !this->fence_tool_active;
				this->SetWidgetLoweredState(WID_MA_FENCE_TOOL, this->fence_tool_active);
				if (this->upgrade_tool_active) {
					this->upgrade_tool_active = false;
					this->SetWidgetLoweredState(WID_MA_UPGRADE_TOOL, false);
				}
				if (this->fence_tool_active) {
					/* Deselect any piece button so fence works standalone. */
					if (this->selected_piece < PIECE_COUNT) {
						this->RaiseWidget(WID_MA_PIECE_0 + this->selected_piece);
						this->selected_piece = static_cast<uint8_t>(PIECE_COUNT);
					}
					/* Guard: CloseWindowByClass and SetObjectToPlace both trigger
					 * OnPlaceObjectAbort on us; the guard prevents that from
					 * clearing our fence state. */
					this->updating_cursor = true;
					CloseWindowByClass(WindowClass::BuildDepot);
					SetObjectToPlace(SPR_CURSOR_AIRPORT, PAL_NONE, HT_RECT, this->window_class, this->window_number);
					this->updating_cursor = false;
				} else {
					this->UpdatePlacementCursor();
				}
				this->SetDirty();
				break;

			case WID_MA_UPGRADE_TOOL:
				this->upgrade_tool_active = !this->upgrade_tool_active;
				this->SetWidgetLoweredState(WID_MA_UPGRADE_TOOL, this->upgrade_tool_active);
				if (this->upgrade_tool_active) {
					/* Deselect any piece button and fence tool. */
					if (this->selected_piece < PIECE_COUNT) {
						this->RaiseWidget(WID_MA_PIECE_0 + this->selected_piece);
						this->selected_piece = static_cast<uint8_t>(PIECE_COUNT);
					}
					if (this->fence_tool_active) {
						this->fence_tool_active = false;
						this->SetWidgetLoweredState(WID_MA_FENCE_TOOL, false);
					}
					this->updating_cursor = true;
					CloseWindowByClass(WindowClass::BuildDepot);
					SetObjectToPlace(SPR_CURSOR_AIRPORT, PAL_NONE, HT_RECT, this->window_class, this->window_number);
					this->updating_cursor = false;
				} else {
					this->UpdatePlacementCursor();
				}
				this->SetDirty();
				break;

			case WID_MA_TEMPLATE_MANAGER: {
				Window *w = FindWindowById(WindowClass::AirportTemplateManager, 0);
				if (w != nullptr) {
					w->Close();
				} else {
					this->fence_tool_active = false;
					this->SetWidgetLoweredState(WID_MA_FENCE_TOOL, false);
					this->upgrade_tool_active = false;
					this->SetWidgetLoweredState(WID_MA_UPGRADE_TOOL, false);
					if (this->selected_piece < PIECE_COUNT) {
						this->RaiseWidget(WID_MA_PIECE_0 + this->selected_piece);
						this->selected_piece = static_cast<uint8_t>(PIECE_COUNT);
					}
					/* The piece pickers belong to the selection we just dropped; leaving one
					 * open next to the template manager would show a picker for no tool. */
					CloseWindowByClass(WindowClass::BuildDepot);
					this->UpdatePlacementCursor();
					ShowBuildAirportTemplateManagerWindow(this);
				}
				this->SetWidgetLoweredState(WID_MA_TEMPLATE_MANAGER, FindWindowById(WindowClass::AirportTemplateManager, 0) != nullptr);
				this->SetDirty();
				break;
			}

			case WID_MA_INFO_OVERLAY: {
				Window *w = FindWindowById(WindowClass::ModularAirportInfoOverlay, 0);
				if (w != nullptr) {
					w->Close();
				} else {
					ShowModularInfoOverlayWindow(this);
				}
				this->SetWidgetLoweredState(WID_MA_INFO_OVERLAY, FindWindowById(WindowClass::ModularAirportInfoOverlay, 0) != nullptr);
				this->SetDirty();
				break;
			}

			default: break;
		}
	}

	/**
	 * Cycle runway flags when overlay is active and user clicks a runway tile.
	 * Left-click cycles direction, Ctrl+click cycles usage (landing/takeoff).
	 * @return true if the click was handled as a runway flag edit.
	 */
	bool TryEditRunwayFlags(TileIndex tile)
	{
		if (!_show_runway_direction_overlay) return false;
		if (!IsValidTile(tile) || !IsTileType(tile, TileType::Station)) return false;

		Station *st = Station::GetByTile(tile);
		if (st == nullptr || !st->airport.blocks.Test(AirportBlock::Modular)) return false;

		const ModularAirportTileData *data = st->airport.GetModularTileData(tile);
		if (data == nullptr) return false;

		if (!IsModularRunwayPiece(data->piece_type)) return false;

		uint8_t flags = data->runway_flags;
		/* Runway direction is one-way only. Normalize invalid/legacy masks. */
		uint8_t dirs = flags & (RUF_DIR_LOW | RUF_DIR_HIGH);
		if (dirs != RUF_DIR_LOW && dirs != RUF_DIR_HIGH) {
			flags = (flags & ~(RUF_DIR_LOW | RUF_DIR_HIGH)) | RUF_DIR_LOW;
		}

		if (_ctrl_pressed) {
			/* Ctrl+click: cycle usage (both -> landing only -> takeoff only -> both) */
			uint8_t usage = flags & (RUF_LANDING | RUF_TAKEOFF);
			if (usage == (RUF_LANDING | RUF_TAKEOFF)) {
				flags = (flags & ~(RUF_LANDING | RUF_TAKEOFF)) | RUF_LANDING;
			} else if (usage == RUF_LANDING) {
				flags = (flags & ~(RUF_LANDING | RUF_TAKEOFF)) | RUF_TAKEOFF;
			} else {
				flags = (flags & ~(RUF_LANDING | RUF_TAKEOFF)) | RUF_LANDING | RUF_TAKEOFF;
			}
		} else {
			/* Left-click: toggle one-way direction (low <-> high). */
			dirs = flags & (RUF_DIR_LOW | RUF_DIR_HIGH);
			flags = (flags & ~(RUF_DIR_LOW | RUF_DIR_HIGH)) | (dirs == RUF_DIR_LOW ? RUF_DIR_HIGH : RUF_DIR_LOW);
		}

		Command<Commands::SetRunwayFlags>::Post(tile, flags);
		return true;
	}

	/**
	 * Cycle taxiway one-way flags when overlay is active and user clicks a taxiway tile.
	 * Cycle order: unrestricted -> N -> E -> S -> W -> unrestricted.
	 * @return true if the click was handled as a taxiway flag edit.
	 */
	bool TryEditTaxiwayFlags(TileIndex tile)
	{
		if (!_show_runway_direction_overlay) return false;
		if (!IsValidTile(tile) || !IsTileType(tile, TileType::Station)) return false;

		Station *st = Station::GetByTile(tile);
		if (st == nullptr || !st->airport.blocks.Test(AirportBlock::Modular)) return false;

		const ModularAirportTileData *data = st->airport.GetModularTileData(tile);
		if (data == nullptr || !IsTaxiwayPiece(data->piece_type)) return false;

		bool next_one_way = true;
		uint8_t next_mask = 0x01; // North

		if (!data->one_way_taxi) {
			next_one_way = true;
			next_mask = 0x01; // N
		} else if (data->user_taxi_dir_mask == 0x01) {
			next_mask = 0x02; // E
		} else if (data->user_taxi_dir_mask == 0x02) {
			next_mask = 0x04; // S
		} else if (data->user_taxi_dir_mask == 0x04) {
			next_mask = 0x08; // W
		} else {
			next_one_way = false;
			next_mask = 0x0F; // unrestricted
		}

		Command<Commands::SetTaxiwayFlags>::Post(tile, next_mask, next_one_way);
		return true;
	}

	void OnPlaceObject([[maybe_unused]] Point pt, TileIndex tile) override
	{
		if (this->selected_piece == MODULAR_AIRPORT_PIECE_ERASE_INDEX) {
			VpStartPlaceSizing(tile, VPM_X_AND_Y, DDSP_DEMOLISH_AREA);
			return;
		}

		/* Upgrade tool: start area drag for upgrade command. */
		if (this->upgrade_tool_active) {
			VpStartPlaceSizing(tile, VPM_X_AND_Y, DDSP_UPGRADE_AIRPORT);
			return;
		}

		/* Fence tool: determine closest edge from click position and toggle fence.
		 * Uses _tile_fract_coords (0-15 sub-tile position in world X/Y) set by
		 * the viewport system on each click -- same mechanism as the autoroad tool. */
		if (this->fence_tool_active) {
			if (!IsTileType(tile, TileType::Station) || !IsAirport(tile)) return;
			Station *st = Station::GetByTile(tile);
			if (st == nullptr || !st->airport.blocks.Test(AirportBlock::Modular)) return;
			const ModularAirportTileData *md = st->airport.GetModularTileData(tile);
			if (md == nullptr) return;

			int fx = _tile_fract_coords.x; /* 0..15, 0 = W edge, 15 = E edge */
			int fy = _tile_fract_coords.y; /* 0..15, 0 = N edge, 15 = S edge */

			/* Distance from each edge. */
			int dist_n = fy;           /* N edge: neighbor dy=-1 */
			int dist_s = 15 - fy;     /* S edge: neighbor dy=+1 */
			int dist_e = 15 - fx;     /* E edge: neighbor dx=+1 */
			int dist_w = fx;           /* W edge: neighbor dx=-1 */

			uint8_t edge_bit;
			int min_dist = dist_n;
			edge_bit = 0x01; /* N */
			if (dist_s < min_dist) { min_dist = dist_s; edge_bit = 0x04; } /* S */
			if (dist_e < min_dist) { min_dist = dist_e; edge_bit = 0x02; } /* E */
			if (dist_w < min_dist) { min_dist = dist_w; edge_bit = 0x08; } /* W */

			bool currently_set = (md->edge_block_mask & edge_bit) != 0;
			Command<Commands::SetModularAirportEdgeFence>::Post(tile, edge_bit, !currently_set);
			return;
		}

		/* When overlay is active, clicking on runway tiles edits their flags */
		if (this->TryEditRunwayFlags(tile)) return;
		/* When overlay is active, clicking on taxiway tiles edits one-way exit direction */
		if (this->TryEditTaxiwayFlags(tile)) return;

		/* Determine if this piece type supports drag-building */
		bool is_runway = (this->selected_piece <= 2);  // Pieces 0-2: Runways
		bool is_apron  = (this->selected_piece == 8);                                // Piece 8: Apron
		bool is_grass  = (this->selected_piece == 9);                                // Piece 9: Grass
		bool is_empty  = (this->selected_piece == 10);                               // Piece 10: Empty

		bool supports_drag = is_runway || is_apron || is_grass || is_empty;

		if (supports_drag) {
			/* Enable drag-building */
			if (is_runway) {
				/* Linear pieces: allow drag in X or Y direction only. The legacy runway's
				 * other axis is produced by runtime mirrors of the selected base set. */
				VpStartPlaceSizing(tile, VPM_X_OR_Y, DDSP_BUILD_STATION);
			} else {
				/* Rectangular pieces: allow drag in both X and Y */
				VpStartPlaceSizing(tile, VPM_X_AND_Y, DDSP_BUILD_STATION);
			}
		} else {
			/* Single tile placement */
			this->PlaceSingleTile(tile);
		}
	}

	void OnPlaceDrag(ViewportPlaceMethod select_method, [[maybe_unused]] ViewportDragDropSelectionProcess select_proc, [[maybe_unused]] Point pt) override
	{
		VpSelectTilesWithMethod(pt.x, pt.y, select_method);
	}

	void OnPlaceMouseUp([[maybe_unused]] ViewportPlaceMethod select_method, ViewportDragDropSelectionProcess select_proc, [[maybe_unused]] Point pt, TileIndex start_tile, TileIndex end_tile) override
	{
		/* Mirror other build toolbars: ignore canceled mouse-up events. */
		if (pt.x == -1) return;
		/* Guard against out-of-bounds drag endpoints before constructing TileArea. */
		if (start_tile >= Map::Size() || end_tile >= Map::Size()) return;

		if (select_proc == DDSP_UPGRADE_AIRPORT) {
			Command<Commands::UpgradeModularAirportTile>::Post(STR_ERROR_CAN_T_UPGRADE_AIRPORT, CcBuildAirport, end_tile, start_tile);
			return;
		}

		if (select_proc == DDSP_DEMOLISH_AREA) {
			/* Erase mode */
			if (start_tile != end_tile) {
				/* Drag-erase area */
				GUIPlaceProcDragXY(select_proc, start_tile, end_tile);
			} else {
				/* Single tile erase */
				Command<Commands::LandscapeClear>::Post(STR_ERROR_CAN_T_CLEAR_THIS_AREA, CcBuildAirport, start_tile);
			}
			return;
		}

		if (select_proc != DDSP_BUILD_STATION) return;

		/* Build modular airport pieces in the selected area */
		TileArea ta(start_tile, end_tile);

		/* Check if this is a single tile or drag operation */
		bool is_single_tile = (start_tile == end_tile);

		/* Only validate for drag operations, not single tile placement */
		if (!is_single_tile && !this->ValidateDragBuild(ta)) {
			return;
		}

		if (is_single_tile) {
			/* Single tile placement - use station selection dialog */
			this->PlaceSingleTileWithDialog(start_tile);
			return;
		}

		/* Build the complete drag through the atomic template-placement command. This
		 * preflights every tile, resolves one station for the whole footprint, and
		 * checks authority noise against the finished layout before changing the map. */
		ModularTemplatePlacementData data;
		data.width = static_cast<uint16_t>(ta.w);
		data.height = static_cast<uint16_t>(ta.h);
		data.is_drag_build = true;
		data.tiles.reserve(static_cast<size_t>(ta.w) * ta.h);

		auto add_tile = [&](TileIndex tile, ModularAirportPieceID gfx, uint8_t rotation) {
			ModularTemplatePlacementTile placement;
			placement.dx = static_cast<uint8_t>(TileX(tile) - TileX(ta.tile));
			placement.dy = static_cast<uint8_t>(TileY(tile) - TileY(ta.tile));
			placement.piece_type = gfx;
			placement.rotation = rotation;
			data.tiles.push_back(placement);
		};

		/* Smart runway building: auto-add end pieces when dragging runway pieces (pieces 0 and 2) */
		bool is_main_runway  = (this->selected_piece == 0);
		bool is_small_runway = (this->selected_piece == 2);
		bool should_auto_end = (is_main_runway || is_small_runway) && (ta.w > 2 || ta.h > 2);

		if (should_auto_end) {
			/* Build runway with automatic end pieces */
			bool is_horizontal = (ta.w > ta.h);
			/* Derive rotation from drag direction, ignoring the rotation widget. */
			uint8_t drag_rotation = is_horizontal ? 0 : 1;

			std::vector<TileIndex> ordered_tiles;
			if (is_horizontal) {
				for (uint x = 0; x < ta.w; x++) {
					ordered_tiles.push_back(TileAddXY(ta.tile, x, 0));
				}
			} else {
				for (uint y = 0; y < ta.h; y++) {
					ordered_tiles.push_back(TileAddXY(ta.tile, 0, y));
				}
			}

			Debug(misc, 3, "[Airport] Drag-building runway atomically: {} tiles, first={}", ordered_tiles.size(), ordered_tiles.front().base());

			if (is_main_runway) {
				/* Place large runway end pieces at both ends */
				add_tile(ordered_tiles.front(), GetModularAirportPieceGfx(1), drag_rotation);
				for (size_t i = 1; i < ordered_tiles.size() - 1; i++) {
					add_tile(ordered_tiles[i], GetModularAirportPieceGfx(this->selected_piece), drag_rotation);
				}
				if (ordered_tiles.size() > 1) {
					add_tile(ordered_tiles.back(), GetModularAirportPieceGfx(1), drag_rotation);
				}
			} else {
				add_tile(ordered_tiles.front(), APT_RUNWAY_SMALL_NEAR_END, drag_rotation);
				for (size_t i = 1; i < ordered_tiles.size() - 1; i++) {
					add_tile(ordered_tiles[i], GetModularAirportPieceGfx(this->selected_piece), drag_rotation);
				}
				if (ordered_tiles.size() > 1) {
					add_tile(ordered_tiles.back(), APT_RUNWAY_SMALL_FAR_END, drag_rotation);
				}
			}
		} else {
			/* Normal multi-tile drag */
			bool is_runway = (this->selected_piece == 0 || this->selected_piece == 1 || this->selected_piece == 2);
			for (TileIndex tile : ta) {
				if (is_runway) {
					uint8_t drag_rotation = (ta.w >= ta.h) ? 0 : 1;
					add_tile(tile, GetModularAirportPieceGfx(this->selected_piece), drag_rotation);
				} else {
					const uint8_t rotation = this->selected_piece == 3 ?
							_cosmetic_pieces[std::min<uint8_t>(_modular_cosmetic_piece, lengthof(_cosmetic_pieces) - 1)].rotation : 0;
					add_tile(tile, GetModularAirportPieceGfx(this->selected_piece), rotation);
				}
			}
		}

		Command<Commands::PlaceModularAirportTemplate>::Post(STR_ERROR_CAN_T_BUILD_AIRPORT_HERE, CcBuildAirport,
			ta.tile, StationID::Invalid(), false, data);
	}

private:
	/**
	 * Find an existing station owned by the local company that is adjacent to
	 * or within the given tile area. Used to pre-determine which station drag-built
	 * tiles should join, so all tiles in a drag join the same station.
	 * @param ta The tile area to check around
	 * @return The station ID if found, or StationID::Invalid() if no station nearby
	 */
	static StationID FindNearbyStation(TileArea ta)
	{
		ta.Expand(1);
		for (TileIndex tile : ta) {
			if (IsValidTile(tile) && IsTileType(tile, TileType::Station)) {
				StationID sid = GetStationIndex(tile);
				Station *st = Station::GetIfValid(sid);
				if (st != nullptr && st->owner == _local_company) return sid;
			}
		}
		return StationID::Invalid();
	}

	/**
	 * Place a single airport piece tile
	 * @param tile The tile to place the piece on
	 */
	void PlaceSingleTile(TileIndex tile)
	{
		if (this->selected_piece == 3) { // Cosmetic picker selected
			const CosmeticPiece &piece = _cosmetic_pieces[std::min<uint8_t>(_modular_cosmetic_piece, lengthof(_cosmetic_pieces) - 1)];
			if (piece.is_multi_tile) {
				this->PlaceMultiTileCosmetic(tile, piece);
				return;
			}
		}
		this->PlaceSingleTileWithDialog(tile);
	}

	/**
	 * Place a multi-tile cosmetic piece (like the 3-tile terminal).
	 */
	void PlaceMultiTileCosmetic(TileIndex tile, const CosmeticPiece &piece)
	{
		const std::span<const ModularCompoundPieceTile> compound = GetModularCompoundPieceTiles(piece.apt_gfx, piece.rotation);
		if (!compound.empty()) {
			const Dimension size = GetModularCompoundPieceSize(piece.apt_gfx, piece.rotation);
			ModularTemplatePlacementData data;
			data.width = static_cast<uint16_t>(size.width);
			data.height = static_cast<uint16_t>(size.height);
			data.rotation = 0;
			for (const ModularCompoundPieceTile &ct : compound) {
				data.tiles.push_back({static_cast<uint8_t>(ct.dx), static_cast<uint8_t>(ct.dy), ct.gfx, piece.rotation, 0, false, 0x0F, 0});
			}

			auto proc = [=](bool test, StationID to_join) -> bool {
				if (test) {
					return Command<Commands::PlaceModularAirportTemplate>::Do(CommandFlagsToDCFlags(GetCommandFlags<Commands::PlaceModularAirportTemplate>()),
							tile, StationID::Invalid(), _ctrl_pressed, data).Succeeded();
				} else {
					return Command<Commands::PlaceModularAirportTemplate>::Post(STR_ERROR_CAN_T_BUILD_AIRPORT_HERE, CcBuildAirport,
							tile, to_join, _ctrl_pressed, data);
				}
			};

			ShowSelectStationIfNeeded(TileArea(tile, size.width, size.height), proc);
		}
	}

	/**
	 * Place a single tile with station selection dialog (for non-drag placement).
	 */
	void PlaceSingleTileWithDialog(TileIndex tile)
	{
		ModularAirportPieceID gfx = GetModularAirportPieceGfx(this->selected_piece);
		bool adjacent = _ctrl_pressed;
		uint8_t rot = 0;
		if (this->selected_piece == 4 || this->selected_piece == 5) {
			rot = _modular_hangar_rotation;
		} else if (this->selected_piece == 3) {
			rot = _cosmetic_pieces[std::min<uint8_t>(_modular_cosmetic_piece, lengthof(_cosmetic_pieces) - 1)].rotation;
		}

		auto proc = [=](bool test, StationID to_join) -> bool {
			if (test) {
				return Command<Commands::BuildModularAirportTile>::Do(CommandFlagsToDCFlags(GetCommandFlags<Commands::BuildModularAirportTile>()),
						tile, gfx, StationID::Invalid(), adjacent, rot, (uint8_t)0x0F, false, false).Succeeded();
			} else {
				return Command<Commands::BuildModularAirportTile>::Post(STR_ERROR_CAN_T_BUILD_AIRPORT_HERE, CcBuildAirport,
						tile, gfx, to_join, adjacent, rot, (uint8_t)0x0F, false, true);
			}
		};

		ShowSelectStationIfNeeded(TileArea(tile, 1, 1), proc);
	}

	/**
	 * Validate drag-build operation
	 * @param ta The tile area being built
	 * @return True if valid, false otherwise
	 */
	bool ValidateDragBuild(const TileArea &ta)
	{
		bool is_runway = (this->selected_piece <= 2);

		/* For runways, validate linear alignment */
		if (is_runway) {
			DiagDirection dir = DiagdirBetweenTiles(ta.tile, TileAddXY(ta.tile, ta.w - 1, ta.h - 1));
			if (dir == DiagDirection::Invalid && (ta.w > 1 || ta.h > 1)) {
				/* Not a straight line */
				ShowErrorMessage(GetEncodedString(STR_ERROR_AIRPORT_MUST_BE_STRAIGHT_LINE), {}, WarningLevel::Info);
				return false;
			}
		}

		/* Minimum runway length check */
		if (is_runway) {
			uint piece_count = std::max(ta.w, ta.h);
			if (piece_count < 3) {
				ShowErrorMessage(GetEncodedString(STR_ERROR_AIRPORT_RUNWAY_TOO_SHORT), {}, WarningLevel::Info);
				return false;
			}
		}

		/* All tiles in the drag must be at the same height level. */
		int required_z = -1;
		for (TileIndex tile : ta) {
			if (!IsValidTile(tile)) continue;
			int z = GetTileMaxZ(tile);
			if (required_z < 0) {
				required_z = z;
			} else if (z != required_z) {
				ShowErrorMessage(GetEncodedString(STR_ERROR_FLAT_LAND_REQUIRED), {}, WarningLevel::Info);
				return false;
			}
		}

		/* If joining an existing modular airport, the drag must match its height. */
		StationID nearby = FindNearbyStation(ta);
		if (nearby != StationID::Invalid()) {
			Station *st = Station::GetIfValid(nearby);
			if (st != nullptr && st->airport.blocks.Test(AirportBlock::Modular) &&
					st->airport.modular_tile_data != nullptr && !st->airport.modular_tile_data->empty()) {
				int existing_z = GetTileMaxZ(st->airport.modular_tile_data->front().tile);
				if (required_z >= 0 && required_z != existing_z) {
					ShowErrorMessage(GetEncodedString(STR_ERROR_FLAT_LAND_REQUIRED), {}, WarningLevel::Info);
					return false;
				}
			}
		}

		return true;
	}

	void OnPlaceObjectAbort() override
	{
		/* Before the guard: a pending join prompt belongs to the placement we are leaving,
		 * whoever takes the cursor next -- including ourselves on a tool switch, which is
		 * where the stock toolbars close it too (their HandlePlacePushButton reset is
		 * unguarded, so their abort handler runs). It holds _thd.freeze while open, so
		 * leaving it behind freezes the tile highlight for the next tool. */
		CloseWindowById(WindowClass::JoinStation, 0);

		if (this->updating_cursor) return; // We're re-setting our own cursor; ignore.

		/* External window stole the cursor -- deselect and raise all buttons. */
		this->selected_piece = static_cast<uint8_t>(PIECE_COUNT);
		for (WidgetID w = WID_MA_PIECE_FIRST; w <= WID_MA_PIECE_LAST; w++) {
			this->RaiseWidget(w);
		}
		this->fence_tool_active = false;
		this->SetWidgetLoweredState(WID_MA_FENCE_TOOL, false);
		this->upgrade_tool_active = false;
		this->SetWidgetLoweredState(WID_MA_UPGRADE_TOOL, false);

		/* Dismiss the piece pickers, like the rail/road/dock toolbars do: they belong to a
		 * tool that is no longer active. selected_piece is already cleared above, so the
		 * pickers' StopPlacementFromClosedPicker() is a no-op and cannot re-enter the cursor
		 * code. The template manager and info overlay are toggles rather than placement
		 * pickers, so they stay open and only their button state is re-synced. */
		CloseWindowByClass(WindowClass::BuildDepot);

		this->SetWidgetLoweredState(WID_MA_TEMPLATE_MANAGER, FindWindowById(WindowClass::AirportTemplateManager, 0) != nullptr);
		this->SetWidgetLoweredState(WID_MA_INFO_OVERLAY, FindWindowById(WindowClass::ModularAirportInfoOverlay, 0) != nullptr);
		this->SetDirty();
	}

	void OnInvalidateData([[maybe_unused]] int data = 0, [[maybe_unused]] bool gui_scope = true) override
	{
		if (!gui_scope) return;
		/* The airport dropdown only offers the builder while these hold; close if they stop
		 * holding while we are open. Both gates invalidate (WindowClass::BuildToolbar, TransportType::Air):
		 * the modular_airports setting from its post_cb, aircraft availability from engine.cpp.
		 * Window::Close() only marks the window for deletion, so closing ourselves here is safe. */
		if (!_settings_game.station.modular_airports || !CanBuildVehicleInfrastructure(VehicleType::Aircraft)) {
			this->Close();
			return;
		}
		this->cached_year = TimerGameCalendar::year;
		this->UpdatePieceGating();
		this->SetWidgetLoweredState(WID_MA_TEMPLATE_MANAGER, FindWindowById(WindowClass::AirportTemplateManager, 0) != nullptr);
		this->SetWidgetLoweredState(WID_MA_INFO_OVERLAY, FindWindowById(WindowClass::ModularAirportInfoOverlay, 0) != nullptr);
		this->SetDirty();
	}

	void OnGameTick() override
	{
		if (this->cached_year != TimerGameCalendar::year) {
			this->cached_year = TimerGameCalendar::year;
			this->RefreshPieceGating();
		}

		if (!this->show_taxi_reservations) return;

		/* Refresh at a modest cadence; aircraft movement updates in game ticks, and this
		 * keeps the overlay smooth without forcing full-screen redraw every realtime frame. */
		if ((TimerGameTick::counter & 0x03) != 0) return;

		std::map<VehicleID, ReservationOverlayBounds> current_bounds;
		for (const Aircraft *v : Aircraft::Iterate()) {
			ReservationOverlayBounds bounds;
			if (!GetReservationOverlayBoundsForAircraft(v, &bounds)) continue;
			current_bounds.emplace(v->index, bounds);
			MarkReservationOverlayBoundsDirty(bounds);
		}

		/* Clean up stale lines where a chain moved or disappeared. */
		for (const auto &[vid, old_bounds] : this->reservation_overlay_bounds) {
			if (!current_bounds.contains(vid)) {
				MarkReservationOverlayBoundsDirty(old_bounds);
			}
		}

		this->reservation_overlay_bounds = std::move(current_bounds);
	}

	void OnRealtimeTick([[maybe_unused]] uint delta_ms) override
	{
		if (this->cached_year == TimerGameCalendar::year) return;
		this->cached_year = TimerGameCalendar::year;
		this->RefreshPieceGating();
	}

private:
	void UpdatePlacementCursor()
	{
		this->updating_cursor = true;
		SetTileSelectSize(1, 1);
		if (this->selected_piece >= PIECE_COUNT) {
			ResetObjectToPlace();
		} else if (this->selected_piece == MODULAR_AIRPORT_PIECE_ERASE_INDEX) {
			SetObjectToPlace(ANIMCURSOR_DEMOLISH, PAL_NONE, HT_RECT | HT_DIAGONAL, this->window_class, this->window_number);
		} else {
			SetObjectToPlace(SPR_CURSOR_AIRPORT, PAL_NONE, HT_RECT, this->window_class, this->window_number);
			/* Show multi-tile footprint for compound cosmetic pieces. */
			if (this->selected_piece == 3) { // Cosmetic picker
				const CosmeticPiece &piece = _cosmetic_pieces[std::min<uint8_t>(_modular_cosmetic_piece, lengthof(_cosmetic_pieces) - 1)];
				const Dimension size = GetModularCompoundPieceSize(piece.apt_gfx, piece.rotation);
				SetTileSelectSize(size.width, size.height);
			}
		}
		this->updating_cursor = false;
	}
};

/** Hangar direction picker window. */
class BuildModularHangarPickerWindow : public PickerWindowBase {
	bool large_hangar; ///< Whether the picker previews the large or small hangar.

	/** Widget-to-rotation mapping: NW=2, NE=1, SW=3, SE=0 */
	static constexpr uint8_t _widget_to_rot[4] = {2, 1, 3, 0}; // indexed by (widget - WID_MAHP_DIR_NW)

	void RefreshAvailability()
	{
		const ModularAirportPieceID gfx = this->large_hangar ? APT_DEPOT_SE : APT_SMALL_DEPOT_SE;
		if (IsModularPieceLocked(gfx, _modular_hangar_rotation)) _modular_hangar_rotation = 0;

		/* The closed-back small hangars are stored openttd.grf bitmaps. Hide their
		 * whole row when disabled; the open-front stock view and runtime mirror remain. */
		const bool show_bitmap_row = this->large_hangar || AreNewAirportGraphicsAvailable();
		if (this->GetWidget<NWidgetStacked>(WID_MAHP_BITMAP_ROW)->SetDisplayedPlane(show_bitmap_row ? 0 : SZSP_NONE)) {
			this->ReInit();
		}
		for (WidgetID w = WID_MAHP_DIR_NW; w <= WID_MAHP_DIR_SE; w++) {
			const uint8_t rotation = _widget_to_rot[w - WID_MAHP_DIR_NW];
			if (!show_bitmap_row && (rotation == 1 || rotation == 2)) continue;
			this->SetWidgetDisabledState(w, IsModularPieceLocked(gfx, rotation));
			this->SetWidgetLoweredState(w, rotation == _modular_hangar_rotation);
		}
	}

public:
	BuildModularHangarPickerWindow(WindowDesc &desc, Window *parent, bool large_hangar)
		: PickerWindowBase(desc, parent), large_hangar(large_hangar)
	{
		this->InitNested(0);
		this->RefreshAvailability();
	}

	void Close([[maybe_unused]] int data = 0) override
	{
		if (this->parent != nullptr &&
				this->parent->window_class == WindowClass::BuildToolbar &&
				this->parent->window_number == WN_BUILD_MODULAR_AIRPORT) {
			static_cast<BuildModularAirportWindow *>(this->parent)->StopPlacementFromClosedPicker(this->large_hangar ? 4 : 5);
		}
		/* Skip PickerWindowBase::Close() which calls ResetObjectToPlace() --
		 * we're a child picker and must not steal the parent's cursor. */
		this->Window::Close();
	}

	Point OnInitialPosition(int16_t sm_width, int16_t sm_height, [[maybe_unused]] int window_number) override
	{
		return GetModularAirportChildWindowPosition(this->parent, sm_width, sm_height, false);
	}

	void OnInvalidateData([[maybe_unused]] int data = 0, [[maybe_unused]] bool gui_scope = true) override
	{
		this->RefreshAvailability();
		this->SetDirty();
	}

	void UpdateWidgetSize(WidgetID widget, Dimension &size, [[maybe_unused]] const Dimension &padding, [[maybe_unused]] Dimension &fill, [[maybe_unused]] Dimension &resize) override
	{
		if (widget < WID_MAHP_DIR_NW || widget > WID_MAHP_DIR_SE) return;
		size.width  = ScaleGUITrad(64) + WidgetDimensions::scaled.fullbevel.Horizontal();
		size.height = ScaleGUITrad(48) + WidgetDimensions::scaled.fullbevel.Vertical();
	}

	void DrawWidget(const Rect &r, WidgetID widget) const override
	{
		if (widget < WID_MAHP_DIR_NW || widget > WID_MAHP_DIR_SE) return;

		DrawPixelInfo tmp_dpi;
		Rect ir = r.Shrink(WidgetDimensions::scaled.bevel);
		if (FillDrawPixelInfo(&tmp_dpi, ir)) {
			AutoRestoreBackup dpi_backup(_cur_dpi, &tmp_dpi);
			int x = (ir.Width()  - ScaleSpriteTrad(64)) / 2 + ScaleSpriteTrad(31);
			int y = (ir.Height() + ScaleSpriteTrad(48)) / 2 - ScaleSpriteTrad(31);
			uint8_t rot = _widget_to_rot[widget - WID_MAHP_DIR_NW];
			/* Use the modular hangar layout directly -- StationPickerDrawSprite can't handle
			 * the high gfx indices (APT_DEPOT_NW/NE/SW = 88-90) which are beyond the
			 * airport tile layout table size and get clamped to index 0 (apron). */
			const DrawTileSprites *t = GetModularHangarTileLayout(rot, !this->large_hangar);
			PaletteID pal = GetCompanyPalette(_local_company);
			DrawSprite(t->ground.sprite, HasBit(t->ground.sprite, PALETTE_MODIFIER_COLOUR) ? pal : PAL_NONE, x, y);
			DrawModularTileSeqInGUI(x, y, t, pal);
		}
	}

	void OnClick([[maybe_unused]] Point pt, WidgetID widget, [[maybe_unused]] int click_count) override
	{
		if (widget < WID_MAHP_DIR_NW || widget > WID_MAHP_DIR_SE) return;
		if (this->IsWidgetDisabled(widget)) return;

		/* Raise old selection */
		for (WidgetID w = WID_MAHP_DIR_NW; w <= WID_MAHP_DIR_SE; w++) {
			if (_widget_to_rot[w - WID_MAHP_DIR_NW] == _modular_hangar_rotation) {
				this->RaiseWidget(w);
				break;
			}
		}
		_modular_hangar_rotation = _widget_to_rot[widget - WID_MAHP_DIR_NW];
		this->LowerWidget(widget);
		SndClickBeep();
		this->SetDirty();
	}
};

static constexpr std::initializer_list<NWidgetPart> _nested_build_modular_hangar_picker_widgets = {
	NWidget(NWID_HORIZONTAL),
		NWidget(WWT_CLOSEBOX, Colours::DarkGreen),
		NWidget(WWT_CAPTION, Colours::DarkGreen, WID_MAHP_CAPTION),
			SetStringTip(STR_STATION_BUILD_MODULAR_AIRPORT_HANGAR_PICKER_CAPTION, STR_TOOLTIP_WINDOW_TITLE_DRAG_THIS),
	EndContainer(),
	NWidget(WWT_PANEL, Colours::DarkGreen),
		NWidget(NWID_VERTICAL), SetPIP(0, WidgetDimensions::unscaled.vsep_normal, 0), SetPIPRatio(1, 0, 1), SetPadding(WidgetDimensions::unscaled.picker),
			NWidget(NWID_SELECTION, Colours::Invalid, WID_MAHP_BITMAP_ROW),
				NWidget(NWID_HORIZONTAL_LTR), SetPIP(0, WidgetDimensions::unscaled.hsep_normal, 0), SetPIPRatio(1, 0, 1),
				NWidget(WWT_TEXTBTN, Colours::Grey, WID_MAHP_DIR_NW), SetFill(0, 0), SetToolTip(STR_STATION_BUILD_MODULAR_AIRPORT_PIECE_HANGAR),
				NWidget(WWT_TEXTBTN, Colours::Grey, WID_MAHP_DIR_NE), SetFill(0, 0), SetToolTip(STR_STATION_BUILD_MODULAR_AIRPORT_PIECE_HANGAR),
				EndContainer(),
			EndContainer(),
			NWidget(NWID_HORIZONTAL_LTR), SetPIP(0, WidgetDimensions::unscaled.hsep_normal, 0), SetPIPRatio(1, 0, 1),
				NWidget(WWT_TEXTBTN, Colours::Grey, WID_MAHP_DIR_SW), SetFill(0, 0), SetToolTip(STR_STATION_BUILD_MODULAR_AIRPORT_PIECE_HANGAR),
				NWidget(WWT_TEXTBTN, Colours::Grey, WID_MAHP_DIR_SE), SetFill(0, 0), SetToolTip(STR_STATION_BUILD_MODULAR_AIRPORT_PIECE_HANGAR),
			EndContainer(),
		EndContainer(),
	EndContainer(),
};

static WindowDesc _build_modular_hangar_picker_desc(
	WindowPosition::Automatic, {}, 0, 0,
	WindowClass::BuildDepot, WindowClass::BuildToolbar,
	WindowDefaultFlag::Construction,
	_nested_build_modular_hangar_picker_widgets
);

static void ShowModularHangarPicker(Window *parent, bool large_hangar)
{
	CloseWindowByClass(WindowClass::BuildDepot);
	new BuildModularHangarPickerWindow(_build_modular_hangar_picker_desc, parent, large_hangar);
}

/** Cosmetic tile picker window (opened when clicking the Cosmetic piece button). */
class BuildModularCosmeticPickerWindow : public PickerWindowBase {
public:
	void RefreshAvailability()
	{
		/* Bitmap-backed decorations disappear entirely with their setting. Keep the
		 * fire-station group's two-button footprint as an empty plane so the mirrored
		 * small terminal stays directly below its unmirrored counterpart; the final
		 * bitmap-only row still disappears so the window contracts vertically. */
		const bool show_bitmap_pieces = AreNewAirportGraphicsAvailable();
		bool reinit = this->GetWidget<NWidgetStacked>(WID_MACP_BITMAP_FIRE_GROUP)->SetDisplayedPlane(show_bitmap_pieces ? 0 : 1);
		reinit |= this->GetWidget<NWidgetStacked>(WID_MACP_BITMAP_ROW)->SetDisplayedPlane(show_bitmap_pieces ? 0 : SZSP_NONE);
		if (reinit) this->ReInit();

		/* Disable visible pieces gated behind a future year. */
		for (uint i = 0; i < lengthof(_cosmetic_pieces); i++) {
			if (!show_bitmap_pieces && IsNewAirportGraphicsPiece(_cosmetic_pieces[i].apt_gfx, _cosmetic_pieces[i].rotation)) continue;
			this->SetWidgetDisabledState(WID_MACP_PIECE_0 + i,
					IsModularPieceLocked(_cosmetic_pieces[i].apt_gfx, _cosmetic_pieces[i].rotation));
		}
		const CosmeticPiece &selected = _cosmetic_pieces[_modular_cosmetic_piece];
		if ((!show_bitmap_pieces && IsNewAirportGraphicsPiece(selected.apt_gfx, selected.rotation)) ||
				this->IsWidgetDisabled(WID_MACP_PIECE_0 + _modular_cosmetic_piece)) {
			/* Selected piece is hidden or locked; pick the first visible, available one. */
			for (uint i = 0; i < lengthof(_cosmetic_pieces); i++) {
				if (!show_bitmap_pieces && IsNewAirportGraphicsPiece(_cosmetic_pieces[i].apt_gfx, _cosmetic_pieces[i].rotation)) continue;
				if (!this->IsWidgetDisabled(WID_MACP_PIECE_0 + i)) {
					_modular_cosmetic_piece = static_cast<uint8_t>(i);
					break;
				}
			}
			/* This window is only open while the cosmetic tool is the active one. */
			UpdateModularCosmeticSelectSize();
		}
		for (uint i = 0; i < lengthof(_cosmetic_pieces); i++) {
			if (!show_bitmap_pieces && IsNewAirportGraphicsPiece(_cosmetic_pieces[i].apt_gfx, _cosmetic_pieces[i].rotation)) continue;
			this->RaiseWidget(WID_MACP_PIECE_0 + i);
		}
		this->LowerWidget(WID_MACP_PIECE_0 + _modular_cosmetic_piece);
	}

	BuildModularCosmeticPickerWindow(WindowDesc &desc, Window *parent)
		: PickerWindowBase(desc, parent)
	{
		if (_modular_cosmetic_piece >= lengthof(_cosmetic_pieces)) _modular_cosmetic_piece = 0;
		this->InitNested(0);
		this->RefreshAvailability();
	}

	void Close([[maybe_unused]] int data = 0) override
	{
		if (this->parent != nullptr &&
				this->parent->window_class == WindowClass::BuildToolbar &&
				this->parent->window_number == WN_BUILD_MODULAR_AIRPORT) {
			static_cast<BuildModularAirportWindow *>(this->parent)->StopPlacementFromClosedPicker(3);
		}
		this->Window::Close();
	}

	Point OnInitialPosition(int16_t sm_width, int16_t sm_height, [[maybe_unused]] int window_number) override
	{
		return GetModularAirportChildWindowPosition(this->parent, sm_width, sm_height, false);
	}

	void OnInvalidateData([[maybe_unused]] int data = 0, [[maybe_unused]] bool gui_scope = true) override
	{
		this->RefreshAvailability();
		this->SetDirty();
	}

	void UpdateWidgetSize(WidgetID widget, Dimension &size,
	                      [[maybe_unused]] const Dimension &padding,
	                      [[maybe_unused]] Dimension &fill,
	                      [[maybe_unused]] Dimension &resize) override
	{
		if (widget < WID_MACP_PIECE_FIRST || widget > WID_MACP_PIECE_LAST) return;
		/* Fixed DPI-stable size matching the hangar picker (full tile view). */
		size.width  = ScaleGUITrad(64) + WidgetDimensions::scaled.fullbevel.Horizontal();
		size.height = ScaleGUITrad(48) + WidgetDimensions::scaled.fullbevel.Vertical();
		/* A compound piece shows its whole footprint, so it needs a button that holds it. */
		const CosmeticPiece &piece = _cosmetic_pieces[widget - WID_MACP_PIECE_FIRST];
		if (piece.is_multi_tile) {
			const ModularPiecePreviewBox box = GetModularCompoundPiecePreviewBox(piece.apt_gfx, piece.rotation, _gui_zoom);
			size.width  = std::max<uint>(size.width,  box.Width()  + WidgetDimensions::scaled.fullbevel.Horizontal() + ScaleGUITrad(4));
			size.height = std::max<uint>(size.height, box.Height() + WidgetDimensions::scaled.fullbevel.Vertical() + ScaleGUITrad(4));
		} else if (piece.use_layout_preview) {
			const DrawTileSprites *t = GetAirportTileLayoutWithModularOverrides(
					GetModularAirportMapGfx(piece.apt_gfx), piece.apt_gfx, piece.rotation);
			const ModularPiecePreviewBox box = GetModularTilePreviewBox(t, _gui_zoom);
			size.width  = std::max<uint>(size.width,  box.Width()  + WidgetDimensions::scaled.fullbevel.Horizontal() + ScaleGUITrad(4));
			size.height = std::max<uint>(size.height, box.Height() + WidgetDimensions::scaled.fullbevel.Vertical() + ScaleGUITrad(4));
		}
	}

	void DrawWidget(const Rect &r, WidgetID widget) const override
	{
		if (widget < WID_MACP_PIECE_FIRST || widget > WID_MACP_PIECE_LAST) return;
		uint8_t piece_idx = static_cast<uint8_t>(widget - WID_MACP_PIECE_FIRST);
		const CosmeticPiece &piece = _cosmetic_pieces[piece_idx];
		DrawPixelInfo tmp_dpi;
		Rect ir = r.Shrink(WidgetDimensions::scaled.bevel);
		if (!FillDrawPixelInfo(&tmp_dpi, ir)) return;
		AutoRestoreBackup dpi_backup(_cur_dpi, &tmp_dpi);
		ZoomLevel icon_zoom = _gui_zoom;
		PaletteID pal = GetCompanyPalette(_local_company);

		/* A compound piece is previewed as the tiles it places, centred on what they cover. */
		if (piece.is_multi_tile) {
			const ModularPiecePreviewBox box = GetModularCompoundPiecePreviewBox(piece.apt_gfx, piece.rotation, icon_zoom);
			DrawModularCompoundPiecePreview((ir.Width() - box.Width()) / 2 - box.left,
					(ir.Height() - box.Height()) / 2 - box.top, piece.apt_gfx, piece.rotation, pal, icon_zoom);
			return;
		}

		if (piece.use_layout_preview) {
			const DrawTileSprites *t = GetAirportTileLayoutWithModularOverrides(
					GetModularAirportMapGfx(piece.apt_gfx), piece.apt_gfx, piece.rotation);
			const ModularPiecePreviewBox box = GetModularTilePreviewBox(t, icon_zoom);
			const int x = (ir.Width() - box.Width()) / 2 - box.left;
			const int y = (ir.Height() - box.Height()) / 2 - box.top;
			const SpriteID ground = t->ground.sprite;
			DrawSprite(ground, HasBit(ground, PALETTE_MODIFIER_COLOUR) ? pal : PAL_NONE, x, y, nullptr, icon_zoom);
			DrawModularTileSeqInGUI(x, y, t, pal, icon_zoom);
			return;
		}

		Point offset;
		Dimension d = GetSpriteSize(piece.icon, &offset, icon_zoom);
		d.width  -= offset.x;
		d.height -= offset.y;
		int x = (ir.Width()  - static_cast<int>(d.width))  / 2;
		int y = (ir.Height() - static_cast<int>(d.height)) / 2;
		y += ScaleSpriteTrad(piece.preview_y_offset);
		if (piece.ground != 0) {
			/* Draw ground tile centred behind the icon. */
			Point go;
			Dimension gd = GetSpriteSize(piece.ground, &go, icon_zoom);
			gd.width  -= go.x;
			gd.height -= go.y;
			int gx = (ir.Width()  - static_cast<int>(gd.width))  / 2;
			int gy = (ir.Height() - static_cast<int>(gd.height)) / 2;
			DrawSprite(piece.ground, pal, gx - go.x, gy - go.y, nullptr, icon_zoom);
		}
		DrawSprite(piece.icon, pal, x - offset.x, y - offset.y, nullptr, icon_zoom);
	}

	void OnClick([[maybe_unused]] Point pt, WidgetID widget,
	             [[maybe_unused]] int click_count) override
	{
		if (widget < WID_MACP_PIECE_FIRST || widget > WID_MACP_PIECE_LAST) return;
		this->RaiseWidget(WID_MACP_PIECE_0 + _modular_cosmetic_piece);
		_modular_cosmetic_piece = static_cast<uint8_t>(widget - WID_MACP_PIECE_FIRST);
		this->LowerWidget(WID_MACP_PIECE_0 + _modular_cosmetic_piece);
		SndClickBeep();
		this->SetDirty();

		/* Update cursor footprint for multi-tile pieces. */
		UpdateModularCosmeticSelectSize();
	}
};

static constexpr std::initializer_list<NWidgetPart> _nested_build_modular_cosmetic_picker_widgets = {
	NWidget(NWID_HORIZONTAL),
		NWidget(WWT_CLOSEBOX, Colours::DarkGreen),
		NWidget(WWT_CAPTION, Colours::DarkGreen, WID_MACP_CAPTION),
			SetStringTip(STR_STATION_BUILD_MODULAR_AIRPORT_COSMETIC_PICKER_CAPTION,
			             STR_TOOLTIP_WINDOW_TITLE_DRAG_THIS),
	EndContainer(),
	NWidget(WWT_PANEL, Colours::DarkGreen),
		NWidget(NWID_VERTICAL), SetPIP(0, WidgetDimensions::unscaled.vsep_normal, 0),
		                          SetPIPRatio(1, 0, 1), SetPadding(WidgetDimensions::unscaled.picker),
			NWidget(NWID_HORIZONTAL), SetPIP(0, WidgetDimensions::unscaled.hsep_normal, 0), SetPIPRatio(1, 0, 1),
				NWidget(WWT_TEXTBTN, Colours::Grey, WID_MACP_PIECE_0), SetFill(0, 0),
					SetToolTip(STR_STATION_BUILD_MODULAR_AIRPORT_PIECE_TERMINAL),
				NWidget(WWT_TEXTBTN, Colours::Grey, WID_MACP_PIECE_1), SetFill(0, 0),
					SetToolTip(STR_STATION_BUILD_MODULAR_AIRPORT_PIECE_TERMINAL_ALT),
				NWidget(WWT_TEXTBTN, Colours::Grey, WID_MACP_PIECE_2), SetFill(0, 0),
					SetToolTip(STR_STATION_BUILD_MODULAR_AIRPORT_PIECE_TERMINAL_OTHER),
				NWidget(WWT_TEXTBTN, Colours::Grey, WID_MACP_PIECE_3), SetFill(0, 0),
					SetToolTip(STR_STATION_BUILD_MODULAR_AIRPORT_PIECE_TERMINAL_ROUND),
			EndContainer(),
			NWidget(NWID_HORIZONTAL), SetPIP(0, WidgetDimensions::unscaled.hsep_normal, 0), SetPIPRatio(1, 0, 1),
				NWidget(WWT_TEXTBTN, Colours::Grey, WID_MACP_PIECE_4), SetFill(0, 0),
					SetToolTip(STR_STATION_BUILD_MODULAR_AIRPORT_PIECE_LOW_TERMINAL),
				NWidget(WWT_TEXTBTN, Colours::Grey, WID_MACP_PIECE_5), SetFill(0, 0),
					SetToolTip(STR_STATION_BUILD_MODULAR_AIRPORT_PIECE_TOWER),
				NWidget(WWT_TEXTBTN, Colours::Grey, WID_MACP_PIECE_6), SetFill(0, 0),
					SetToolTip(STR_STATION_BUILD_MODULAR_AIRPORT_PIECE_RADIO_TOWER),
				NWidget(WWT_TEXTBTN, Colours::Grey, WID_MACP_PIECE_7), SetFill(0, 0),
					SetToolTip(STR_STATION_BUILD_MODULAR_AIRPORT_PIECE_FLAG_GRASS),
			EndContainer(),
			NWidget(NWID_HORIZONTAL), SetPIP(0, WidgetDimensions::unscaled.hsep_normal, 0), SetPIPRatio(1, 0, 1),
				NWidget(WWT_TEXTBTN, Colours::Grey, WID_MACP_PIECE_8), SetFill(0, 0),
					SetToolTip(STR_STATION_BUILD_MODULAR_AIRPORT_PIECE_RADAR),
				NWidget(WWT_TEXTBTN, Colours::Grey, WID_MACP_PIECE_9), SetFill(0, 0),
					SetToolTip(STR_STATION_BUILD_MODULAR_AIRPORT_PIECE_RADAR_GRASS),
				NWidget(WWT_TEXTBTN, Colours::Grey, WID_MACP_PIECE_10), SetFill(1, 0), SetMinimalSize(120, 0),
					SetToolTip(STR_STATION_BUILD_MODULAR_AIRPORT_PIECE_SMALL_TERMINAL_3),
			EndContainer(),
			/* Four pieces per row, with each three-tile small terminal counting as two.
			 * The stored-bitmap fire stations disappear as a group when their setting is
			 * off, leaving the runtime-mirrored terminal centred on this row. */
			NWidget(NWID_HORIZONTAL), SetPIP(0, WidgetDimensions::unscaled.hsep_normal, 0), SetPIPRatio(1, 0, 1),
				NWidget(NWID_SELECTION, Colours::Invalid, WID_MACP_BITMAP_FIRE_GROUP),
					NWidget(NWID_HORIZONTAL), SetPIP(0, WidgetDimensions::unscaled.hsep_normal, 0),
						NWidget(WWT_TEXTBTN, Colours::Grey, WID_MACP_PIECE_11), SetFill(0, 0),
							SetToolTip(STR_STATION_BUILD_MODULAR_AIRPORT_PIECE_FIRE_STATION),
						NWidget(WWT_TEXTBTN, Colours::Grey, WID_MACP_PIECE_12), SetFill(0, 0),
							SetToolTip(STR_STATION_BUILD_MODULAR_AIRPORT_PIECE_FIRE_STATION),
					EndContainer(),
					NWidget(NWID_SPACER),
				EndContainer(),
				NWidget(WWT_TEXTBTN, Colours::Grey, WID_MACP_PIECE_13), SetFill(1, 0), SetMinimalSize(120, 0),
					SetToolTip(STR_STATION_BUILD_MODULAR_AIRPORT_PIECE_SMALL_TERMINAL_3),
			EndContainer(),
			NWidget(NWID_SELECTION, Colours::Invalid, WID_MACP_BITMAP_ROW),
				NWidget(NWID_HORIZONTAL), SetPIP(0, WidgetDimensions::unscaled.hsep_normal, 0), SetPIPRatio(1, 0, 1),
					NWidget(WWT_TEXTBTN, Colours::Grey, WID_MACP_PIECE_14), SetFill(0, 0),
						SetToolTip(STR_STATION_BUILD_MODULAR_AIRPORT_PIECE_CARGO_TERMINAL),
					NWidget(WWT_TEXTBTN, Colours::Grey, WID_MACP_PIECE_15), SetFill(0, 0),
						SetToolTip(STR_STATION_BUILD_MODULAR_AIRPORT_PIECE_FUEL_FARM),
					NWidget(WWT_TEXTBTN, Colours::Grey, WID_MACP_PIECE_16), SetFill(0, 0),
						SetToolTip(STR_STATION_BUILD_MODULAR_AIRPORT_PIECE_CAR_PARK),
					NWidget(WWT_TEXTBTN, Colours::Grey, WID_MACP_PIECE_17), SetFill(0, 0),
						SetToolTip(STR_STATION_BUILD_MODULAR_AIRPORT_PIECE_CAR_PARK),
				EndContainer(),
			EndContainer(),
		EndContainer(),
	EndContainer(),
};

static WindowDesc _build_modular_cosmetic_picker_desc(
	WindowPosition::Automatic, {}, 0, 0,
	WindowClass::BuildDepot, WindowClass::BuildToolbar,
	WindowDefaultFlag::Construction,
	_nested_build_modular_cosmetic_picker_widgets
);

static void ShowModularCosmeticPicker(Window *parent)
{
	CloseWindowByClass(WindowClass::BuildDepot);
	new BuildModularCosmeticPickerWindow(_build_modular_cosmetic_picker_desc, parent);
}

/** Helipad tile picker window (opened when clicking the Helipad piece button). */
class BuildModularHelipadPickerWindow : public PickerWindowBase {
public:
	BuildModularHelipadPickerWindow(WindowDesc &desc, Window *parent)
		: PickerWindowBase(desc, parent)
	{
		if (_modular_helipad_piece >= lengthof(_helipad_pieces)) _modular_helipad_piece = 0;
		this->InitNested(0);
		/* Disable helipad pieces gated behind a future year. */
		for (uint i = 0; i < lengthof(_helipad_pieces); i++) {
			this->SetWidgetDisabledState(WID_MAHPAD_PIECE_0 + i, IsModularPieceLocked(_helipad_pieces[i].apt_gfx));
		}
		if (this->IsWidgetDisabled(WID_MAHPAD_PIECE_0 + _modular_helipad_piece)) {
			for (uint i = 0; i < lengthof(_helipad_pieces); i++) {
				if (!this->IsWidgetDisabled(WID_MAHPAD_PIECE_0 + i)) {
					_modular_helipad_piece = static_cast<uint8_t>(i);
					break;
				}
			}
		}
		this->LowerWidget(WID_MAHPAD_PIECE_0 + _modular_helipad_piece);
	}

	void Close([[maybe_unused]] int data = 0) override
	{
		if (this->parent != nullptr &&
				this->parent->window_class == WindowClass::BuildToolbar &&
				this->parent->window_number == WN_BUILD_MODULAR_AIRPORT) {
			static_cast<BuildModularAirportWindow *>(this->parent)->StopPlacementFromClosedPicker(6);
		}
		this->Window::Close();
	}

	Point OnInitialPosition(int16_t sm_width, int16_t sm_height, [[maybe_unused]] int window_number) override
	{
		return GetModularAirportChildWindowPosition(this->parent, sm_width, sm_height, false);
	}

	void UpdateWidgetSize(WidgetID widget, Dimension &size,
	                      [[maybe_unused]] const Dimension &padding,
	                      [[maybe_unused]] Dimension &fill,
	                      [[maybe_unused]] Dimension &resize) override
	{
		if (widget < WID_MAHPAD_PIECE_FIRST || widget > WID_MAHPAD_PIECE_LAST) return;
		/* Fixed picker-style preview size to show each variant clearly. */
		size.width  = ScaleGUITrad(64) + WidgetDimensions::scaled.fullbevel.Horizontal();
		size.height = ScaleGUITrad(48) + WidgetDimensions::scaled.fullbevel.Vertical();
	}

	void DrawWidget(const Rect &r, WidgetID widget) const override
	{
		if (widget < WID_MAHPAD_PIECE_FIRST || widget > WID_MAHPAD_PIECE_LAST) return;
		uint8_t piece_idx = static_cast<uint8_t>(widget - WID_MAHPAD_PIECE_FIRST);
		const HelipadPiece &piece = _helipad_pieces[piece_idx];

		DrawPixelInfo tmp_dpi;
		Rect ir = r.Shrink(WidgetDimensions::scaled.bevel);
		if (!FillDrawPixelInfo(&tmp_dpi, ir)) return;
		AutoRestoreBackup dpi_backup(_cur_dpi, &tmp_dpi);
		ZoomLevel icon_zoom = _gui_zoom;
		PaletteID pal = GetCompanyPalette(_local_company);

		Point offset;
		Dimension d = GetSpriteSize(piece.icon, &offset, icon_zoom);
		d.width  -= offset.x;
		d.height -= offset.y;
		int x = (ir.Width()  - static_cast<int>(d.width))  / 2;
		int y = (ir.Height() - static_cast<int>(d.height)) / 2;
		y += ScaleSpriteTrad(piece.preview_y_offset);
		DrawSprite(piece.icon, pal, x - offset.x, y - offset.y, nullptr, icon_zoom);
	}

	void OnClick([[maybe_unused]] Point pt, WidgetID widget,
	             [[maybe_unused]] int click_count) override
	{
		if (widget < WID_MAHPAD_PIECE_FIRST || widget > WID_MAHPAD_PIECE_LAST) return;
		this->RaiseWidget(WID_MAHPAD_PIECE_0 + _modular_helipad_piece);
		_modular_helipad_piece = static_cast<uint8_t>(widget - WID_MAHPAD_PIECE_FIRST);
		this->LowerWidget(WID_MAHPAD_PIECE_0 + _modular_helipad_piece);
		SndClickBeep();
		this->SetDirty();

	}
};

static constexpr std::initializer_list<NWidgetPart> _nested_build_modular_helipad_picker_widgets = {
	NWidget(NWID_HORIZONTAL),
		NWidget(WWT_CLOSEBOX, Colours::DarkGreen),
		NWidget(WWT_CAPTION, Colours::DarkGreen, WID_MAHPAD_CAPTION),
			SetStringTip(STR_STATION_BUILD_MODULAR_AIRPORT_HELIPAD_PICKER_CAPTION, STR_TOOLTIP_WINDOW_TITLE_DRAG_THIS),
	EndContainer(),
	NWidget(WWT_PANEL, Colours::DarkGreen),
		NWidget(NWID_HORIZONTAL_LTR), SetPIP(0, WidgetDimensions::unscaled.hsep_normal, 0),
		                              SetPIPRatio(1, 0, 1), SetPadding(WidgetDimensions::unscaled.picker),
			NWidget(WWT_TEXTBTN, Colours::Grey, WID_MAHPAD_PIECE_0), SetFill(0, 0),
				SetToolTip(STR_STATION_BUILD_MODULAR_AIRPORT_PIECE_HELIPAD),
			NWidget(WWT_TEXTBTN, Colours::Grey, WID_MAHPAD_PIECE_1), SetFill(0, 0),
				SetToolTip(STR_STATION_BUILD_MODULAR_AIRPORT_PIECE_PLAIN_H),
			NWidget(WWT_TEXTBTN, Colours::Grey, WID_MAHPAD_PIECE_2), SetFill(0, 0),
				SetToolTip(STR_AIRPORT_HELIPORT),
		EndContainer(),
	EndContainer(),
};

static WindowDesc _build_modular_helipad_picker_desc(
	WindowPosition::Automatic, {}, 0, 0,
	WindowClass::BuildDepot, WindowClass::BuildToolbar,
	WindowDefaultFlag::Construction,
	_nested_build_modular_helipad_picker_widgets
);

static void ShowModularHelipadPicker(Window *parent)
{
	CloseWindowByClass(WindowClass::BuildDepot);
	new BuildModularHelipadPickerWindow(_build_modular_helipad_picker_desc, parent);
}

class BuildModularInfoOverlayWindow : public Window {
public:
	BuildModularInfoOverlayWindow(WindowDesc &desc, Window *parent) : Window(desc)
	{
		this->parent = parent;
		this->InitNested(0);
		this->UpdateButtonStates();
	}

	void Close([[maybe_unused]] int data = 0) override
	{
		Window *parent = this->parent;
		this->Window::Close();
		if (parent != nullptr) parent->InvalidateData();
	}

	Point OnInitialPosition(int16_t sm_width, int16_t sm_height, [[maybe_unused]] int window_number) override
	{
		return GetModularAirportChildWindowPosition(this->parent, sm_width, sm_height, true);
	}

	void OnClick([[maybe_unused]] Point pt, WidgetID widget, [[maybe_unused]] int click_count) override
	{
		auto *builder = static_cast<BuildModularAirportWindow *>(this->parent);
		if (builder == nullptr) return;

		switch (widget) {
			case WID_MAIO_ARROWS_OFF:
				builder->show_taxi_arrows = false;
				break;

			case WID_MAIO_ARROWS_ON:
				builder->show_taxi_arrows = true;
				break;

			case WID_MAIO_HOLDING_OFF:
				builder->show_holding_loop = false;
				break;

			case WID_MAIO_HOLDING_ON:
				builder->show_holding_loop = true;
				break;

			case WID_MAIO_RESERVATIONS_OFF:
				builder->show_taxi_reservations = false;
				builder->reservation_overlay_bounds.clear();
				break;

			case WID_MAIO_RESERVATIONS_ON:
				builder->show_taxi_reservations = true;
				break;

			default:
				return;
		}

		_show_runway_direction_overlay = builder->show_taxi_arrows;
		_show_holding_overlay = builder->show_holding_loop;
		_show_taxi_reservation_overlay = builder->show_taxi_reservations;
		this->UpdateButtonStates();
		MarkWholeScreenDirty();
	}

private:
	void UpdateButtonStates()
	{
		auto *builder = static_cast<BuildModularAirportWindow *>(this->parent);
		if (builder == nullptr) return;

		this->SetWidgetLoweredState(WID_MAIO_ARROWS_OFF, !builder->show_taxi_arrows);
		this->SetWidgetLoweredState(WID_MAIO_ARROWS_ON, builder->show_taxi_arrows);
		this->SetWidgetLoweredState(WID_MAIO_HOLDING_OFF, !builder->show_holding_loop);
		this->SetWidgetLoweredState(WID_MAIO_HOLDING_ON, builder->show_holding_loop);
		this->SetWidgetLoweredState(WID_MAIO_RESERVATIONS_OFF, !builder->show_taxi_reservations);
		this->SetWidgetLoweredState(WID_MAIO_RESERVATIONS_ON, builder->show_taxi_reservations);
		this->SetDirty();
	}
};

static constexpr std::initializer_list<NWidgetPart> _nested_build_modular_info_overlay_widgets = {
	NWidget(NWID_HORIZONTAL),
		NWidget(WWT_CLOSEBOX, Colours::DarkGreen),
		NWidget(WWT_CAPTION, Colours::DarkGreen),
			SetStringTip(STR_STATION_BUILD_MODULAR_AIRPORT_INFO_OVERLAY_CAPTION, STR_TOOLTIP_WINDOW_TITLE_DRAG_THIS),
	EndContainer(),
	NWidget(WWT_PANEL, Colours::DarkGreen),
		NWidget(NWID_VERTICAL), SetPIP(0, WidgetDimensions::unscaled.vsep_normal, 0), SetPadding(WidgetDimensions::unscaled.picker),
			NWidget(WWT_LABEL, Colours::Invalid), SetStringTip(STR_STATION_BUILD_MODULAR_AIRPORT_TOGGLE_SHOW_ARROWS), SetFill(1, 0),
			NWidget(NWID_HORIZONTAL), SetPIP(14, 0, 14), SetPIPRatio(1, 0, 1),
				NWidget(NWID_HORIZONTAL, NWidContainerFlag::EqualSize),
					NWidget(WWT_TEXTBTN, Colours::Grey, WID_MAIO_ARROWS_OFF), SetMinimalSize(60, 12), SetFill(1, 0),
						SetStringTip(STR_STATION_BUILD_COVERAGE_OFF, STR_NULL),
					NWidget(WWT_TEXTBTN, Colours::Grey, WID_MAIO_ARROWS_ON), SetMinimalSize(60, 12), SetFill(1, 0),
						SetStringTip(STR_STATION_BUILD_COVERAGE_ON, STR_NULL),
				EndContainer(),
			EndContainer(),
			NWidget(WWT_LABEL, Colours::Invalid), SetStringTip(STR_STATION_BUILD_MODULAR_AIRPORT_TOGGLE_SHOW_HOLDING), SetFill(1, 0),
			NWidget(NWID_HORIZONTAL), SetPIP(14, 0, 14), SetPIPRatio(1, 0, 1),
				NWidget(NWID_HORIZONTAL, NWidContainerFlag::EqualSize),
					NWidget(WWT_TEXTBTN, Colours::Grey, WID_MAIO_HOLDING_OFF), SetMinimalSize(60, 12), SetFill(1, 0),
						SetStringTip(STR_STATION_BUILD_COVERAGE_OFF, STR_NULL),
					NWidget(WWT_TEXTBTN, Colours::Grey, WID_MAIO_HOLDING_ON), SetMinimalSize(60, 12), SetFill(1, 0),
						SetStringTip(STR_STATION_BUILD_COVERAGE_ON, STR_NULL),
				EndContainer(),
			EndContainer(),
			NWidget(WWT_LABEL, Colours::Invalid), SetStringTip(STR_STATION_BUILD_MODULAR_AIRPORT_TOGGLE_SHOW_RESERVATIONS), SetFill(1, 0),
			NWidget(NWID_HORIZONTAL), SetPIP(14, 0, 14), SetPIPRatio(1, 0, 1),
				NWidget(NWID_HORIZONTAL, NWidContainerFlag::EqualSize),
					NWidget(WWT_TEXTBTN, Colours::Grey, WID_MAIO_RESERVATIONS_OFF), SetMinimalSize(60, 12), SetFill(1, 0),
						SetStringTip(STR_STATION_BUILD_COVERAGE_OFF, STR_NULL),
					NWidget(WWT_TEXTBTN, Colours::Grey, WID_MAIO_RESERVATIONS_ON), SetMinimalSize(60, 12), SetFill(1, 0),
						SetStringTip(STR_STATION_BUILD_COVERAGE_ON, STR_NULL),
				EndContainer(),
			EndContainer(),
		EndContainer(),
	EndContainer(),
};

static WindowDesc _build_modular_info_overlay_desc(
	WindowPosition::Automatic, {}, 0, 0,
	WindowClass::ModularAirportInfoOverlay, WindowClass::BuildToolbar,
	WindowDefaultFlag::Construction,
	_nested_build_modular_info_overlay_widgets
);

static void ShowModularInfoOverlayWindow(Window *parent)
{
	CloseWindowById(WindowClass::ModularAirportInfoOverlay, 0);
	new BuildModularInfoOverlayWindow(_build_modular_info_overlay_desc, parent);
}

static constexpr std::initializer_list<NWidgetPart> _nested_build_modular_airport_widgets = {
	NWidget(NWID_HORIZONTAL),
		NWidget(WWT_CLOSEBOX, Colours::DarkGreen),
		NWidget(WWT_CAPTION, Colours::DarkGreen), SetStringTip(STR_STATION_BUILD_MODULAR_AIRPORT_CAPTION, STR_TOOLTIP_WINDOW_TITLE_DRAG_THIS),
		NWidget(WWT_STICKYBOX, Colours::DarkGreen),
	EndContainer(),
	NWidget(NWID_HORIZONTAL),
		NWidget(WWT_TEXTBTN, Colours::DarkGreen, WID_MA_PIECE_0),  SetFill(0, 1), SetToolbarMinimalSize(1), SetToolTip(STR_STATION_BUILD_MODULAR_AIRPORT_PIECE_RUNWAY),
		NWidget(WWT_TEXTBTN, Colours::DarkGreen, WID_MA_PIECE_2),  SetFill(0, 1), SetToolbarMinimalSize(1), SetToolTip(STR_STATION_BUILD_MODULAR_AIRPORT_PIECE_RUNWAY_SMALL_MID),
		NWidget(WWT_TEXTBTN, Colours::DarkGreen, WID_MA_PIECE_4),  SetFill(0, 1), SetToolbarMinimalSize(1), SetToolTip(STR_STATION_BUILD_MODULAR_AIRPORT_PIECE_HANGAR),
		NWidget(WWT_TEXTBTN, Colours::DarkGreen, WID_MA_PIECE_5),  SetFill(0, 1), SetToolbarMinimalSize(1), SetToolTip(STR_STATION_BUILD_MODULAR_AIRPORT_PIECE_SMALL_HANGAR),
		NWidget(WWT_TEXTBTN, Colours::DarkGreen, WID_MA_PIECE_3),  SetFill(0, 1), SetToolbarMinimalSize(1), SetToolTip(STR_STATION_BUILD_MODULAR_AIRPORT_PIECE_COSMETIC),
		NWidget(WWT_TEXTBTN, Colours::DarkGreen, WID_MA_PIECE_6),  SetFill(0, 1), SetToolbarMinimalSize(1), SetToolTip(STR_STATION_BUILD_MODULAR_AIRPORT_PIECE_HELIPAD),
		NWidget(WWT_TEXTBTN, Colours::DarkGreen, WID_MA_PIECE_7),  SetFill(0, 1), SetToolbarMinimalSize(1), SetToolTip(STR_STATION_BUILD_MODULAR_AIRPORT_PIECE_STAND),
		NWidget(WWT_TEXTBTN, Colours::DarkGreen, WID_MA_PIECE_8),  SetFill(0, 1), SetToolbarMinimalSize(1), SetToolTip(STR_STATION_BUILD_MODULAR_AIRPORT_PIECE_APRON),
		NWidget(WWT_TEXTBTN, Colours::DarkGreen, WID_MA_PIECE_9),  SetFill(0, 1), SetToolbarMinimalSize(1), SetToolTip(STR_STATION_BUILD_MODULAR_AIRPORT_PIECE_GRASS),
		NWidget(WWT_TEXTBTN, Colours::DarkGreen, WID_MA_PIECE_10), SetFill(0, 1), SetToolbarMinimalSize(1), SetToolTip(STR_STATION_BUILD_MODULAR_AIRPORT_PIECE_EMPTY),
		NWidget(WWT_IMGBTN, Colours::DarkGreen, WID_MA_FENCE_TOOL), SetFill(0, 1), SetToolbarMinimalSize(1),
			SetSpriteTip(SPR_AIRPORT_FENCE_Y, STR_STATION_BUILD_MODULAR_AIRPORT_FENCE_TOOL),
		NWidget(WWT_IMGBTN, Colours::DarkGreen, WID_MA_UPGRADE_TOOL), SetFill(0, 1), SetToolbarMinimalSize(1),
			SetSpriteTip(SPR_IMG_CONVERT_ROAD, STR_STATION_BUILD_MODULAR_AIRPORT_UPGRADE_TOOL),
		NWidget(WWT_IMGBTN, Colours::DarkGreen, WID_MA_TEMPLATE_MANAGER), SetFill(0, 1), SetToolbarMinimalSize(1),
			SetSpriteTip(SPR_IMG_SAVE, STR_STATION_BUILD_MODULAR_AIRPORT_TEMPLATE_MANAGER_TOOLTIP),
		NWidget(WWT_TEXTBTN, Colours::DarkGreen, WID_MA_PIECE_11), SetFill(0, 1), SetToolbarMinimalSize(1), SetToolTip(STR_TOOLTIP_DEMOLISH_BUILDINGS_ETC),
		NWidget(WWT_IMGBTN, Colours::DarkGreen, WID_MA_INFO_OVERLAY), SetFill(0, 1), SetToolbarMinimalSize(1),
			SetSpriteTip(SPR_IMG_QUERY, STR_STATION_BUILD_MODULAR_AIRPORT_INFO_OVERLAY_TOOLTIP),
	EndContainer(),
};

static WindowDesc _build_modular_airport_desc(
	WindowPosition::Manual, "build_modular_airport", 0, 0,
	WindowClass::BuildToolbar, WindowClass::None,
	WindowDefaultFlag::Construction,
	_nested_build_modular_airport_widgets
);

/** Typical cruise altitude: midpoint of the [AIRCRAFT_MIN, AIRCRAFT_MAX] band that GetAircraftFlightLevel enforces. */
static constexpr int HOLDING_OVERLAY_CRUISE_ALTITUDE = (AIRCRAFT_MIN_FLYING_ALTITUDE + AIRCRAFT_MAX_FLYING_ALTITUDE) / 2;

/**
 * Convert world pixel coordinates + altitude to screen pixel coordinates for the holding overlay.
 * @param altitude  Height above terrain in the same units as aircraft z_pos.
 */
static Point HoldingWorldToScreen(const Viewport &vp, int wx, int wy, int altitude = HOLDING_OVERLAY_CRUISE_ALTITUDE)
{
	/* Holding waypoints can lie outside the map; clamp to safe pixel range
	 * to prevent GetSlopePixelZ assertions on boundary tiles. */
	int clamped_wx = Clamp(wx, 0, static_cast<int>(Map::SizeX() * TILE_SIZE) - 1);
	int clamped_wy = Clamp(wy, 0, static_cast<int>(Map::SizeY() * TILE_SIZE) - 1);
	Point p = RemapCoords(wx, wy, GetSlopePixelZOutsideMap(clamped_wx, clamped_wy) + altitude);
	p.x = UnScaleByZoom(p.x - vp.virtual_left, vp.zoom) + vp.left;
	p.y = UnScaleByZoom(p.y - vp.virtual_top,  vp.zoom) + vp.top;
	return p;
}

static Point WorldToScreen(const Viewport &vp, int wx, int wy, int wz)
{
	Point p = RemapCoords(wx, wy, wz);
	p.x = UnScaleByZoom(p.x - vp.virtual_left, vp.zoom) + vp.left;
	p.y = UnScaleByZoom(p.y - vp.virtual_top,  vp.zoom) + vp.top;
	return p;
}

static Point TileCenterToScreen(const Viewport &vp, TileIndex tile, int z_offset = 4)
{
	const int wx = TileX(tile) * TILE_SIZE + TILE_SIZE / 2;
	const int wy = TileY(tile) * TILE_SIZE + TILE_SIZE / 2;
	return WorldToScreen(vp, wx, wy, GetSlopePixelZ(wx, wy) + z_offset);
}

/** Conservative AABB visibility check -- GfxDrawLine clips anyway; this skips obviously off-screen segments. */
static bool HoldingSegVis(Point a, Point b, const DrawPixelInfo *dpi)
{
	int l = dpi->left, r = l + dpi->width, t = dpi->top, bot = t + dpi->height;
	return !(   (a.x < l   && b.x < l)
	         || (a.y < t   && b.y < t)
	         || (a.x > r   && b.x > r)
	         || (a.y > bot && b.y > bot));
}

static PixelColour GetHoldingLoopColour(StationID sid)
{
	static constexpr PixelColour kColours[] = {
		PC_WHITE,
		PC_LIGHT_BLUE,
		PC_ORANGE,
		PC_LIGHT_YELLOW,
		PC_RED,
		PC_VERY_LIGHT_YELLOW,
	};
	uint32_t x = sid.base();
	x ^= x >> 16;
	x *= 0x7feb352dU;
	x ^= x >> 15;
	return kColours[x % lengthof(kColours)];
}

void DrawModularHoldingOverlay(const Viewport &vp, DrawPixelInfo *dpi)
{
	for (const Station *st : Station::Iterate()) {
		if (!st->airport.blocks.Test(AirportBlock::Modular)) continue;

		const ModularHoldingLoop &loop = GetModularHoldingLoop(st);
		const size_t n = loop.waypoints.size();
		if (n < 2 || loop.gates.empty()) continue; /* Skip fallback rectangular loop (no gates). */

		const PixelColour loop_colour = GetHoldingLoopColour(st->index);

		/* Draw the loop polyline at aircraft cruise altitude. */
		for (size_t i = 0; i < n; ++i) {
			const auto &a = loop.waypoints[i];
			const auto &b = loop.waypoints[(i + 1) % n];
			Point pa = HoldingWorldToScreen(vp, a.x, a.y);
			Point pb = HoldingWorldToScreen(vp, b.x, b.y);
			if (HoldingSegVis(pa, pb, dpi)) GfxDrawLine(pa.x, pa.y, pb.x, pb.y, loop_colour, 1);
		}

		/* Draw small squares at each waypoint. */
		for (size_t i = 0; i < n; ++i) {
			const auto &wp = loop.waypoints[i];
			Point p = HoldingWorldToScreen(vp, wp.x, wp.y);
			if (p.x + 2 < dpi->left || p.x - 2 > dpi->left + dpi->width) continue;
			if (p.y + 2 < dpi->top  || p.y - 2 > dpi->top + dpi->height) continue;
			GfxFillRect(p.x - 2, p.y - 2, p.x + 2, p.y + 2, loop_colour);
		}

		for (const auto &gate : loop.gates) {
			if (gate.wp_index >= n) continue;

			/* Yellow: gate waypoint -> runway threshold. */
			const auto &wp = loop.waypoints[gate.wp_index];
			Point pgw = HoldingWorldToScreen(vp, wp.x, wp.y);
			Point pth = HoldingWorldToScreen(vp, gate.threshold_x, gate.threshold_y, 0);
			if (HoldingSegVis(pgw, pth, dpi)) GfxDrawLine(pgw.x, pgw.y, pth.x, pth.y, PC_YELLOW, 1);

			/* Red threshold marker at ground level. */
			if (pth.x + 3 >= dpi->left && pth.x - 3 <= dpi->left + dpi->width &&
					pth.y + 3 >= dpi->top && pth.y - 3 <= dpi->top + dpi->height) {
				GfxFillRect(pth.x - 3, pth.y - 3, pth.x + 3, pth.y + 3, PC_RED);
			}
		}
	}
}

static void AppendRouteTile(std::vector<TileIndex> &route, TileIndex tile)
{
	if (!IsValidTile(tile)) return;
	if (route.empty() || route.back() != tile) route.push_back(tile);
}

/** Append a path only when it joins the route already assembled. */
static bool AppendTaxiPathContinuation(std::vector<TileIndex> &route, const TaxiPath *path, size_t start_index = 0)
{
	if (path == nullptr || !path->valid || start_index >= path->tiles.size()) return false;
	if (!route.empty() && route.back() != path->tiles[start_index]) return false;

	for (size_t i = start_index; i < path->tiles.size(); ++i) AppendRouteTile(route, path->tiles[i]);
	return true;
}

/** Build the runway traversal from the operation end toward the opposite end. */
static bool BuildForwardRunwayRoute(const Station *st, TileIndex operation_end, std::vector<TileIndex> &runway_route, TileIndex stop_at = INVALID_TILE)
{
	runway_route.clear();
	if (st == nullptr || !IsValidTile(operation_end)) return false;

	std::vector<TileIndex> runway_tiles;
	if (!GetContiguousModularRunwayTiles(st, operation_end, runway_tiles) || runway_tiles.empty()) return false;

	/* A landing rollout stops at the braking distance, not at the far end, and the rest
	 * of the runway is not part of the route the aircraft will drive. */
	const TileIndex last = IsValidTile(stop_at) ? stop_at : GetRunwayOtherEnd(st, operation_end);
	const auto start = std::find(runway_tiles.begin(), runway_tiles.end(), operation_end);
	const auto finish = std::find(runway_tiles.begin(), runway_tiles.end(), last);
	if (start == runway_tiles.end() || finish == runway_tiles.end()) return false;

	if (start <= finish) {
		for (auto it = start; it <= finish; ++it) runway_route.push_back(*it);
	} else {
		for (auto it = start;; --it) {
			runway_route.push_back(*it);
			if (it == finish) break;
		}
	}
	return true;
}

/** Draw the known forward route through its last reserved tile. */
static void DrawReservationRoute(const Viewport &vp, DrawPixelInfo *dpi, const Aircraft *v,
		const std::vector<TileIndex> &route, PixelColour colour, std::unordered_set<uint32_t> &drawn)
{
	size_t last_reserved = route.size();
	for (size_t i = 0; i < route.size(); ++i) {
		if (IsTileReservedBy(route[i], v->index)) last_reserved = i;
	}
	if (last_reserved == route.size()) return;

	Point prev = WorldToScreen(vp, v->x_pos, v->y_pos, v->z_pos + 4);
	for (size_t i = 0; i <= last_reserved; ++i) {
		const TileIndex tile = route[i];
		const Point curr = TileCenterToScreen(vp, tile);
		if (HoldingSegVis(prev, curr, dpi)) GfxDrawLine(prev.x, prev.y, curr.x, curr.y, colour, 1);
		if (IsTileReservedBy(tile, v->index)) {
			GfxFillRect(curr.x - 2, curr.y - 2, curr.x + 2, curr.y + 2, colour);
			drawn.insert(tile.base());
		}
		prev = curr;
	}
}

static void DrawReservationMarker(const Viewport &vp, TileIndex tile, PixelColour colour)
{
	const Point point = TileCenterToScreen(vp, tile);
	GfxFillRect(point.x - 2, point.y - 2, point.x + 2, point.y + 2, colour);
}

static PixelColour GetReservationOverlayColour(VehicleID vid)
{
	static constexpr PixelColour kColours[] = {
		PC_RED,
		PC_ORANGE,
		PC_YELLOW,
		PC_LIGHT_YELLOW,
		PC_GREEN,
		PC_LIGHT_BLUE,
		PC_DARK_BLUE,
		PC_DARK_RED,
		PC_GREY,
		PC_WHITE,
	};

	uint32_t x = vid.base();
	x ^= x >> 16;
	x *= 0x7feb352dU;
	x ^= x >> 15;
	x *= 0x846ca68bU;
	x ^= x >> 16;
	return kColours[x % lengthof(kColours)];
}

void DrawModularTaxiReservationOverlay(const Viewport &vp, DrawPixelInfo *dpi)
{
	for (const Aircraft *v : Aircraft::Iterate()) {
		if (!v->IsNormalAircraft()) continue;
		if (v->taxi_path == nullptr && v->landing_chain_path == nullptr &&
				v->taxi_reserved_tiles.empty() && v->modular_runway_reservation.empty()) continue;

		const Station *st = Station::GetIfValid(v->targetairport);
		const bool landing_phase = v->state == LANDING || v->state == ENDLANDING || v->state == HELILANDING || v->state == HELIENDLANDING;
		const PixelColour colour = GetReservationOverlayColour(v->index);
		std::vector<TileIndex> route;

		if (landing_phase && IsValidTile(v->modular_landing_tile)) {
			/* Fixed-wing landing continues through runway rollout. Helicopters hand off
			 * directly from touchdown, even when a no-helipad airport uses a runway tile. */
			AppendRouteTile(route, v->modular_landing_tile);
			if (v->subtype != AIR_HELICOPTER && st != nullptr) {
				/* Only as far as the rollout will actually go: the runway past the
				 * turn-off point is reserved, but it is not route the aircraft drives.
				 *
				 * Where the chain starts on this runway, that tile is the turn-off --
				 * the same answer the touchdown handoff takes, and the one to draw. The
				 * rollout point on its own is just the braking floor, and a chain whose
				 * route carries on to a further exit has already absorbed the tiles past
				 * it into the rollout. Stopping at the floor there would break the route
				 * where the chain picks up, leaving the whole taxi in as bare markers. */
				const TileIndex chain_start = (v->landing_chain_path != nullptr && v->landing_chain_path->valid &&
						!v->landing_chain_path->tiles.empty()) ? v->landing_chain_path->tiles.front() : INVALID_TILE;
				std::vector<TileIndex> runway_route;
				const bool have_route = (IsValidTile(chain_start) &&
						BuildForwardRunwayRoute(st, v->modular_landing_tile, runway_route, chain_start)) ||
						BuildForwardRunwayRoute(st, v->modular_landing_tile, runway_route,
								FindModularRunwayRolloutPoint(st, v, v->modular_landing_tile));
				if (have_route) {
					for (TileIndex tile : runway_route) AppendRouteTile(route, tile);
				}
			}
			AppendTaxiPathContinuation(route, v->landing_chain_path.get());
		} else if (v->taxi_path != nullptr) {
			/* The aircraft has already traversed its current path tile. Begin with the
			 * next tile so even an aircraft between tile centres is never drawn backward. */
			const size_t start_index = std::min<size_t>(static_cast<size_t>(v->taxi_path_index) + 1, v->taxi_path->tiles.size());
			AppendTaxiPathContinuation(route, v->taxi_path.get(), start_index);
			/* During rollout, this is the exact continuation already reserved before landing. */
			const size_t landing_start = route.empty() && v->landing_chain_path != nullptr &&
					!v->landing_chain_path->tiles.empty() && v->landing_chain_path->tiles.front() == v->tile ? 1 : 0;
			AppendTaxiPathContinuation(route, v->landing_chain_path.get(), landing_start);
		}

		const bool takeoff_roll = v->state == TAKEOFF || v->state == STARTTAKEOFF || v->state == ENDTAKEOFF;
		if ((takeoff_roll || v->modular_ground_target == MGT_RUNWAY_TAKEOFF) &&
				IsValidTile(v->modular_takeoff_tile) && !v->modular_runway_reservation.empty()) {
			std::vector<TileIndex> runway_route;
			if (BuildForwardRunwayRoute(st, v->modular_takeoff_tile, runway_route)) {
				if (takeoff_roll && route.empty()) {
					/* Once rolling, start at the aircraft rather than drawing back to runway entry. */
					const auto current = std::find(runway_route.begin(), runway_route.end(), v->tile);
					if (current != runway_route.end()) {
						for (auto it = std::next(current); it != runway_route.end(); ++it) AppendRouteTile(route, *it);
					}
				} else if (!route.empty() && route.back() == runway_route.front()) {
					for (TileIndex tile : runway_route) AppendRouteTile(route, tile);
				}
			}
		}

		std::unordered_set<uint32_t> drawn;
		DrawReservationRoute(vp, dpi, v, route, colour, drawn);

		/* Keep unusual or stale ownership visible without inventing edges between tiles. */
		const auto draw_unmatched = [&](TileIndex tile) {
			if (!IsTileReservedBy(tile, v->index)) return;
			if (!drawn.insert(tile.base()).second) return;
			DrawReservationMarker(vp, tile, colour);
		};
		for (TileIndex tile : v->taxi_reserved_tiles) draw_unmatched(tile);
		for (TileIndex tile : v->modular_runway_reservation) draw_unmatched(tile);
	}
}

void ShowBuildModularAirportWindow()
{
	if (!Company::IsValidID(_local_company)) return;

	/* The builder is a construction toolbar in its own right: it replaces any other
	 * construction toolbar (including a previous instance of itself), and has no parent. */
	CloseWindowByClass(WindowClass::BuildToolbar);
	new BuildModularAirportWindow(_build_modular_airport_desc, nullptr);
}
