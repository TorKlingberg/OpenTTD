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
#include "modular_airport_gui.h"
#include "airport_template_gui.h"
#include "newgrf_airporttiles.h"

#include "widgets/airport_widget.h"

#include "table/airporttile_ids.h"
#include "table/strings.h"

#include "safeguards.h"

#include <cctype>
#include <map>
#include <unordered_map>
#include <unordered_set>

void CcBuildAirport(Commands, const CommandCost &result, TileIndex tile);

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
	return IsAirportTile(t) && HasAirportTileReservation(t) && GetAirportTileReserver(t) == vid;
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

static const WindowNumber WN_BUILD_MODULAR_AIRPORT = WindowNumber{TRANSPORT_AIR};
struct ModularAirportPiece {
	StringID name;    ///< Full name (used as tooltip)
	SpriteID icon;    ///< Toolbar button icon sprite
	PixelColour colour;
};

struct CosmeticPiece {
	StringID name;
	SpriteID icon;    ///< Small icon at Out2x zoom (for toolbar button)
	SpriteID ground;  ///< Optional ground sprite drawn behind icon (0 = none)
	uint8_t apt_gfx;  ///< AirportTiles value for placement and full-size picker preview
	int8_t preview_y_offset; ///< Vertical bias in picker preview; positive moves down.
	bool is_multi_tile = false; ///< True if this piece places multiple tiles at once.
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
	{STR_STATION_BUILD_MODULAR_AIRPORT_PIECE_SMALL_TERMINAL_3, 2666,                         0,                    APT_SMALL_BUILDING_2,    10, true},
};

static constexpr HelipadPiece _helipad_pieces[] = {
	{STR_STATION_BUILD_MODULAR_AIRPORT_PIECE_HELIPAD, SPR_AIRPORT_HELIPAD, APT_HELIPAD_2,          0},
	{STR_AIRPORT_HELISTATION,                         SPR_NEWHELIPAD,       APT_HELIPAD_3_FENCE_NW, 0},
	{STR_AIRPORT_HELIPORT,                            SPR_HELIPORT,         APT_HELIPORT,           10},
};

static_assert(lengthof(_cosmetic_pieces) == WID_MACP_PIECE_LAST - WID_MACP_PIECE_FIRST + 1);
static_assert(lengthof(_helipad_pieces) == WID_MAHPAD_PIECE_LAST - WID_MAHPAD_PIECE_FIRST + 1);

static constexpr ModularAirportPiece _modular_airport_pieces[] = {
	{STR_STATION_BUILD_MODULAR_AIRPORT_PIECE_RUNWAY,           SPR_AIRPORT_RUNWAY_EXIT_B,  PC_DARK_GREY},    // 0
	{STR_STATION_BUILD_MODULAR_AIRPORT_PIECE_RUNWAY_END,       SPR_NSRUNWAY_END,           PC_DARK_GREY},    // 1
	{STR_STATION_BUILD_MODULAR_AIRPORT_PIECE_RUNWAY_SMALL_MID, SPR_AIRFIELD_RUNWAY_MIDDLE, PC_DARK_GREY},    // 2  (smart-drag: auto-adds near/far ends)
	{STR_STATION_BUILD_MODULAR_AIRPORT_PIECE_COSMETIC,         SPR_AIRPORT_CONCOURSE,      PC_ORANGE},       // 3 (cosmetic picker)
	{STR_STATION_BUILD_MODULAR_AIRPORT_PIECE_HANGAR,           SPR_AIRPORT_HANGAR_FRONT,   PC_DARK_RED},     // 4
	{STR_STATION_BUILD_MODULAR_AIRPORT_PIECE_SMALL_HANGAR,     SPR_AIRFIELD_HANGAR_FRONT,  PC_DARK_RED},     // 5
	{STR_STATION_BUILD_MODULAR_AIRPORT_PIECE_HELIPAD,          SPR_NEWHELIPAD,             PC_LIGHT_YELLOW}, // 6
	{STR_STATION_BUILD_MODULAR_AIRPORT_PIECE_STAND,            SPR_AIRPORT_AIRCRAFT_STAND, PC_YELLOW},       // 7
	{STR_STATION_BUILD_MODULAR_AIRPORT_PIECE_APRON,            SPR_AIRPORT_APRON,          PC_GREY},         // 8
	{STR_STATION_BUILD_MODULAR_AIRPORT_PIECE_GRASS,            SPR_AIRFIELD_APRON_C,       PC_GREEN},        // 9
	{STR_STATION_BUILD_MODULAR_AIRPORT_PIECE_EMPTY,            SPR_FLAT_GRASS_TILE,        PC_WHITE},        // 10
	{STR_STATION_BUILD_MODULAR_AIRPORT_PIECE_ERASE,            SPR_IMG_DYNAMITE,           PC_WHITE},        // 11
};

static constexpr int MODULAR_AIRPORT_PIECE_ERASE_INDEX = lengthof(_modular_airport_pieces) - 1;

static uint8_t GetModularAirportPieceGfx(uint8_t piece)
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

static void ShowModularHangarPicker(Window *parent, bool is_large);
static void ShowModularCosmeticPicker(Window *parent);
static void ShowModularHelipadPicker(Window *parent);

class BuildModularAirportWindow : public PickerWindowBase {
	static constexpr int PIECE_COUNT = lengthof(_modular_airport_pieces);

	uint8_t selected_piece = static_cast<uint8_t>(PIECE_COUNT);
	bool show_taxi_arrows = true;
	bool show_holding_loop = false;
	bool show_taxi_reservations = false;
	std::map<VehicleID, ReservationOverlayBounds> reservation_overlay_bounds;
	bool updating_cursor = false; ///< True while UpdatePlacementCursor is running (suppresses abort side-effects).
	bool fence_tool_active = false; ///< When true, clicks toggle edge fences instead of placing tiles.
	TimerGameCalendar::Year cached_year = CalendarTime::MIN_YEAR;
	const IntervalTimer<TimerGameCalendar> yearly_interval = {{TimerGameCalendar::YEAR, TimerGameCalendar::Priority::NONE}, [this](auto) {
		this->RefreshYearGating();
	}};

public:
	BuildModularAirportWindow(WindowDesc &desc, Window *parent) : PickerWindowBase(desc, parent)
	{
		this->InitNested(WN_BUILD_MODULAR_AIRPORT);
		this->SetWidgetLoweredState(WID_MA_TOGGLE_SHOW_ARROWS, this->show_taxi_arrows);
		this->SetWidgetLoweredState(WID_MA_TOGGLE_SHOW_HOLDING, this->show_holding_loop);
		this->SetWidgetLoweredState(WID_MA_TOGGLE_SHOW_RESERVATIONS, this->show_taxi_reservations);
		this->SetWidgetLoweredState(WID_MA_TEMPLATE_MANAGER, false);
		this->UpdateYearGating();
		this->cached_year = TimerGameCalendar::year;
		this->UpdatePlacementCursor();
		_show_runway_direction_overlay = this->show_taxi_arrows;
		_show_holding_overlay = this->show_holding_loop;
		_show_taxi_reservation_overlay = this->show_taxi_reservations;
		MarkWholeScreenDirty();
	}

	void StopPlacementFromClosedPicker(uint8_t picker_piece)
	{
		if (this->selected_piece != picker_piece) return;
		this->RaiseWidget(WID_MA_PIECE_0 + this->selected_piece);
		this->selected_piece = static_cast<uint8_t>(PIECE_COUNT);
		this->UpdatePlacementCursor();
		this->SetDirty();
	}

	static bool IsGfxLockedByYear(uint8_t gfx)
	{
		return IsModernModularPiece(gfx) && TimerGameCalendar::year < GetModularPieceMinYear(gfx);
	}

	static uint8_t FirstAvailableCosmeticPiece()
	{
		for (uint8_t i = 0; i < lengthof(_cosmetic_pieces); i++) {
			if (!IsGfxLockedByYear(_cosmetic_pieces[i].apt_gfx)) return i;
		}
		return 0;
	}

	static uint8_t FirstAvailableHelipadPiece()
	{
		for (uint8_t i = 0; i < lengthof(_helipad_pieces); i++) {
			if (!IsGfxLockedByYear(_helipad_pieces[i].apt_gfx)) return i;
		}
		return 0;
	}

	void NormalizePickerSelectionsForYear()
	{
		if (_modular_cosmetic_piece >= lengthof(_cosmetic_pieces) || IsGfxLockedByYear(_cosmetic_pieces[_modular_cosmetic_piece].apt_gfx)) {
			_modular_cosmetic_piece = FirstAvailableCosmeticPiece();
		}
		if (_modular_helipad_piece >= lengthof(_helipad_pieces) || IsGfxLockedByYear(_helipad_pieces[_modular_helipad_piece].apt_gfx)) {
			_modular_helipad_piece = FirstAvailableHelipadPiece();
		}
	}

	bool IsPieceButtonLocked(uint8_t piece) const
	{
		if (piece == MODULAR_AIRPORT_PIECE_ERASE_INDEX) return false;
		if (piece == 3) {
			for (const CosmeticPiece &cp : _cosmetic_pieces) {
				if (!IsGfxLockedByYear(cp.apt_gfx)) return false;
			}
			return true;
		}
		if (piece == 6) {
			for (const HelipadPiece &hp : _helipad_pieces) {
				if (!IsGfxLockedByYear(hp.apt_gfx)) return false;
			}
			return true;
		}
		return IsGfxLockedByYear(GetModularAirportPieceGfx(piece));
	}

	/** Disable piece buttons whose GFX is gated behind a future year. */
	void UpdateYearGating()
	{
		this->NormalizePickerSelectionsForYear();
		for (int i = 0; i < PIECE_COUNT; i++) {
			this->SetWidgetDisabledState(WID_MA_PIECE_0 + i, this->IsPieceButtonLocked(static_cast<uint8_t>(i)));
		}
		if (this->selected_piece < PIECE_COUNT && this->IsWidgetDisabled(WID_MA_PIECE_0 + this->selected_piece)) {
			this->RaiseWidget(WID_MA_PIECE_0 + this->selected_piece);
			this->selected_piece = static_cast<uint8_t>(PIECE_COUNT);
			this->UpdatePlacementCursor();
		}
	}

	void RefreshYearGating()
	{
		this->UpdateYearGating();
		InvalidateWindowClassesData(WC_BUILD_DEPOT, 0);
		this->SetDirty();
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
		CloseWindowById(WC_AIRPORT_TEMPLATE_MANAGER, 0);
		CloseWindowByClass(WC_BUILD_DEPOT);
		/* Raise the modular button on the airport toolbar. */
		if (this->parent != nullptr) {
			this->parent->RaiseWidget(WID_AT_MODULAR);
			this->parent->SetDirty();
		}
		/* Use Window::Close() instead of PickerWindowBase::Close() to avoid
		 * an unconditional ResetObjectToPlace() — the guard above already
		 * handles our own cursor, and we must not reset another window's cursor
		 * (e.g. the stock airport builder toolbar). */
		this->Window::Close();
	}

	void UpdateWidgetSize(WidgetID widget, Dimension &size, [[maybe_unused]] const Dimension &padding, [[maybe_unused]] Dimension &fill, [[maybe_unused]] Dimension &resize) override
	{
		if (widget < WID_MA_PIECE_FIRST || widget > WID_MA_PIECE_LAST) return;
		/* Keep piece buttons the same size as standard construction toolbar buttons. */
		size.width = std::max<uint>(size.width, ScaleGUITrad(22));
		size.height = std::max<uint>(size.height, ScaleGUITrad(22));
	}

	void DrawWidget(const Rect &r, WidgetID widget) const override
	{
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
			for (const DrawTileSeqStruct &dtss : t->GetSequence()) {
				SpriteID image = dtss.image.sprite;
				PaletteID seq_pal = dtss.image.pal;

				/* TTD sprite 0 means no sprite. */
				if (GB(image, 0, SPRITE_WIDTH) == 0 && !HasBit(image, SPRITE_MODIFIER_CUSTOM_SPRITE)) continue;

				seq_pal = SpriteLayoutPaletteTransform(image, seq_pal, pal);
				if (dtss.IsParentSprite()) {
					Point pt = RemapCoords(dtss.origin.x, dtss.origin.y, dtss.origin.z);
					DrawSprite(image, seq_pal, x + UnScaleByZoom(pt.x, icon_zoom), y + UnScaleByZoom(pt.y, icon_zoom), nullptr, icon_zoom);
				}
			}
		} else {
			SpriteID icon = piece.icon;
			ZoomLevel icon_zoom = _gui_zoom;
			if (icon_zoom < ZoomLevel::Max) ++icon_zoom;
			Dimension d = GetSpriteSize(icon, &offset, icon_zoom);
			d.width  -= offset.x;
			d.height -= offset.y;
			int x = (ir.Width()  - static_cast<int>(d.width))  / 2;
			int y = (ir.Height() - static_cast<int>(d.height)) / 2;
			DrawSprite(icon, pal, x - offset.x, y - offset.y, nullptr, icon_zoom);
		}
	}

	void OnClick([[maybe_unused]] Point pt, WidgetID widget, [[maybe_unused]] int click_count) override
	{
		if (widget >= WID_MA_PIECE_FIRST && widget <= WID_MA_PIECE_LAST) {
			uint8_t new_piece = static_cast<uint8_t>(widget - WID_MA_PIECE_FIRST);
			bool already_selected = (new_piece == this->selected_piece);
			bool wants_picker = (new_piece == 3 || new_piece == 4 || new_piece == 6);

			/* Deactivate fence tool when selecting a piece. */
			if (this->fence_tool_active) {
				this->fence_tool_active = false;
				this->SetWidgetLoweredState(WID_MA_FENCE_TOOL, false);
			}
			/* Raise the previously selected piece button. */
			if (this->selected_piece < PIECE_COUNT) this->RaiseWidget(WID_MA_PIECE_0 + this->selected_piece);

			/* Close any open sub-picker and update the cursor. Both can trigger
			 * OnPlaceObjectAbort on this window; the updating_cursor guard in
			 * UpdatePlacementCursor prevents that from clearing our state. */
			CloseWindowByClass(WC_BUILD_DEPOT);

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
				if (new_piece == 4) {
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
			case WID_MA_TOGGLE_SHOW_ARROWS:
				this->show_taxi_arrows = !this->show_taxi_arrows;
				_show_runway_direction_overlay = this->show_taxi_arrows;
				this->SetWidgetLoweredState(WID_MA_TOGGLE_SHOW_ARROWS, this->show_taxi_arrows);
				MarkWholeScreenDirty();
				break;

			case WID_MA_TOGGLE_SHOW_HOLDING:
				this->show_holding_loop = !this->show_holding_loop;
				_show_holding_overlay = this->show_holding_loop;
				this->SetWidgetLoweredState(WID_MA_TOGGLE_SHOW_HOLDING, this->show_holding_loop);
				MarkWholeScreenDirty();
				break;

			case WID_MA_TOGGLE_SHOW_RESERVATIONS:
				this->show_taxi_reservations = !this->show_taxi_reservations;
				_show_taxi_reservation_overlay = this->show_taxi_reservations;
				if (!this->show_taxi_reservations) this->reservation_overlay_bounds.clear();
				this->SetWidgetLoweredState(WID_MA_TOGGLE_SHOW_RESERVATIONS, this->show_taxi_reservations);
				MarkWholeScreenDirty();
				break;

			case WID_MA_FENCE_TOOL:
				this->fence_tool_active = !this->fence_tool_active;
				this->SetWidgetLoweredState(WID_MA_FENCE_TOOL, this->fence_tool_active);
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
					CloseWindowByClass(WC_BUILD_DEPOT);
					SetObjectToPlace(SPR_CURSOR_AIRPORT, PAL_NONE, HT_RECT, this->window_class, this->window_number);
					this->updating_cursor = false;
				} else {
					this->UpdatePlacementCursor();
				}
				this->SetDirty();
				break;

			case WID_MA_TEMPLATE_MANAGER:
				this->fence_tool_active = false;
				this->SetWidgetLoweredState(WID_MA_FENCE_TOOL, false);
				if (this->selected_piece < PIECE_COUNT) {
					this->RaiseWidget(WID_MA_PIECE_0 + this->selected_piece);
					this->selected_piece = static_cast<uint8_t>(PIECE_COUNT);
				}
				this->UpdatePlacementCursor();
				ShowBuildAirportTemplateManagerWindow(this);
				break;

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

		Command<CMD_SET_RUNWAY_FLAGS>::Post(tile, flags);
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

		Command<CMD_SET_TAXIWAY_FLAGS>::Post(tile, next_mask, next_one_way);
		return true;
	}

	void OnPlaceObject([[maybe_unused]] Point pt, TileIndex tile) override
	{
		if (this->selected_piece == MODULAR_AIRPORT_PIECE_ERASE_INDEX) {
			VpStartPlaceSizing(tile, VPM_X_AND_Y, DDSP_DEMOLISH_AREA);
			return;
		}

		/* Fence tool: determine closest edge from click position and toggle fence.
		 * Uses _tile_fract_coords (0-15 sub-tile position in world X/Y) set by
		 * the viewport system on each click — same mechanism as the autoroad tool. */
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
			Command<CMD_SET_MODULAR_AIRPORT_EDGE_FENCE>::Post(tile, edge_bit, !currently_set);
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
				/* Legacy (small) runway can only drag on the stock axis. */
				if (this->selected_piece == 2) {
					VpStartPlaceSizing(tile, VPM_FIX_Y, DDSP_BUILD_STATION);
				} else {
					/* Linear pieces: allow drag in X or Y direction only */
					VpStartPlaceSizing(tile, VPM_X_OR_Y, DDSP_BUILD_STATION);
				}
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

		if (select_proc == DDSP_DEMOLISH_AREA) {
			/* Erase mode */
			if (start_tile != end_tile) {
				/* Drag-erase area */
				GUIPlaceProcDragXY(select_proc, start_tile, end_tile);
			} else {
				/* Single tile erase */
				Command<CMD_LANDSCAPE_CLEAR>::Post(STR_ERROR_CAN_T_CLEAR_THIS_AREA, CcBuildAirport, start_tile);
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

		/* Multi-tile drag: find existing station near the drag area.
		 * Pass it to ALL tiles so they join the same station via distant_join,
		 * even if a particular tile isn't directly adjacent to existing tiles. */
		StationID nearby_station = FindNearbyStation(ta);
		Debug(misc, 3, "[Airport] Drag-building: nearby_station={}", nearby_station != StationID::Invalid() ? (int)nearby_station.base() : -1);

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

			Debug(misc, 3, "[Airport] Drag-building runway: {} tiles, first={}", ordered_tiles.size(), ordered_tiles.front().base());

			if (is_main_runway) {
				/* Place large runway end pieces at both ends */
				this->PlaceDragTile(ordered_tiles.front(), 1, nearby_station, drag_rotation);
				for (size_t i = 1; i < ordered_tiles.size() - 1; i++) {
					this->PlaceDragTile(ordered_tiles[i], this->selected_piece, nearby_station, drag_rotation);
				}
				if (ordered_tiles.size() > 1) {
					this->PlaceDragTile(ordered_tiles.back(), 1, nearby_station, drag_rotation);
				}
			} else {
				/* Small runway: place contiguously front→middles→back so each tile is adjacent
				 * to the last, allowing GetStationAround to find the growing station. */
				this->PlaceDragTileRawGfx(ordered_tiles.front(), APT_RUNWAY_SMALL_NEAR_END, nearby_station, drag_rotation);
				for (size_t i = 1; i < ordered_tiles.size() - 1; i++) {
					this->PlaceDragTile(ordered_tiles[i], this->selected_piece, nearby_station, drag_rotation);
				}
				if (ordered_tiles.size() > 1) {
					this->PlaceDragTileRawGfx(ordered_tiles.back(), APT_RUNWAY_SMALL_FAR_END, nearby_station, drag_rotation);
				}
			}
		} else {
			/* Normal multi-tile drag */
			bool is_runway = (this->selected_piece == 0 || this->selected_piece == 1 || this->selected_piece == 2);
			for (TileIndex tile : ta) {
				if (is_runway) {
					uint8_t drag_rotation = (ta.w >= ta.h) ? 0 : 1;
					this->PlaceDragTile(tile, this->selected_piece, nearby_station, drag_rotation);
				} else {
					this->PlaceDragTile(tile, this->selected_piece, nearby_station);
				}
			}
		}
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
		if (piece.apt_gfx == APT_SMALL_BUILDING_2) { // 3-tile terminal
			ModularTemplatePlacementData data;
			data.width = 3;
			data.height = 1;
			data.rotation = 0;
			data.tiles.push_back({0, 0, APT_SMALL_BUILDING_1, 0, 0, false, 0x0F, 0});
			data.tiles.push_back({1, 0, APT_SMALL_BUILDING_2, 0, 0, false, 0x0F, 0});
			data.tiles.push_back({2, 0, APT_SMALL_BUILDING_3, 0, 0, false, 0x0F, 0});

			auto proc = [=](bool test, StationID to_join) -> bool {
				if (test) {
					return Command<CMD_PLACE_MODULAR_AIRPORT_TEMPLATE>::Do(CommandFlagsToDCFlags(GetCommandFlags<CMD_PLACE_MODULAR_AIRPORT_TEMPLATE>()),
							tile, StationID::Invalid(), _ctrl_pressed, data).Succeeded();
				} else {
					return Command<CMD_PLACE_MODULAR_AIRPORT_TEMPLATE>::Post(STR_ERROR_CAN_T_BUILD_AIRPORT_HERE, CcBuildAirport,
							tile, to_join, _ctrl_pressed, data);
				}
			};

			ShowSelectStationIfNeeded(TileArea(tile, 3, 1), proc);
		}
	}

	/**
	 * Post a single tile build command for a drag operation.
	 * Bypasses ShowSelectStationIfNeeded - all tiles are posted directly.
	 * @param tile The tile to build on
	 * @param piece_index The piece type to build
	 * @param nearby_station Station to join (Invalid = auto-detect via GetStationAround)
	 */
	void PlaceDragTile(TileIndex tile, uint8_t piece_index, StationID nearby_station, uint8_t rot = 0)
	{
		uint8_t gfx = GetModularAirportPieceGfx(piece_index);

		/* Pass the nearby station as station_to_join. In the command handler:
		 * - GetStationAround checks adjacent tiles for a station to join.
		 * - If no adjacent station found, distant_join uses nearby_station.
		 * This ensures all drag tiles join the same station even if not all
		 * are directly adjacent to existing tiles. */
		Command<CMD_BUILD_MODULAR_AIRPORT_TILE>::Post(STR_ERROR_CAN_T_BUILD_AIRPORT_HERE, CcBuildAirport,
			tile, gfx, nearby_station, false, rot, (uint8_t)0x0F, false);
	}

	/**
	 * Post a drag tile build command using a raw gfx value (bypasses piece-index lookup).
	 * Used for auto-placed runway end pieces.
	 */
	void PlaceDragTileRawGfx(TileIndex tile, uint8_t gfx, StationID nearby_station, uint8_t rot = 0)
	{
		Command<CMD_BUILD_MODULAR_AIRPORT_TILE>::Post(STR_ERROR_CAN_T_BUILD_AIRPORT_HERE, CcBuildAirport,
			tile, gfx, nearby_station, false, rot, (uint8_t)0x0F, false);
	}

	/**
	 * Place a single tile with station selection dialog (for non-drag placement).
	 */
	void PlaceSingleTileWithDialog(TileIndex tile)
	{
		uint8_t gfx = GetModularAirportPieceGfx(this->selected_piece);
		bool adjacent = _ctrl_pressed;
		/* Use hangar rotation for hangar pieces; rotation 0 for everything else. */
		uint8_t rot = (this->selected_piece == 4) ? _modular_hangar_rotation : 0;

		auto proc = [=](bool test, StationID to_join) -> bool {
			if (test) {
				return Command<CMD_BUILD_MODULAR_AIRPORT_TILE>::Do(CommandFlagsToDCFlags(GetCommandFlags<CMD_BUILD_MODULAR_AIRPORT_TILE>()),
						tile, gfx, StationID::Invalid(), adjacent, rot, (uint8_t)0x0F, false).Succeeded();
			} else {
				return Command<CMD_BUILD_MODULAR_AIRPORT_TILE>::Post(STR_ERROR_CAN_T_BUILD_AIRPORT_HERE, CcBuildAirport,
						tile, gfx, to_join, adjacent, rot, (uint8_t)0x0F, false);
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
			if (dir == INVALID_DIAGDIR && (ta.w > 1 || ta.h > 1)) {
				/* Not a straight line */
				ShowErrorMessage(GetEncodedString(STR_ERROR_AIRPORT_MUST_BE_STRAIGHT_LINE), {}, WL_INFO);
				return false;
			}
		}

		/* Minimum runway length check */
		if (is_runway) {
			uint piece_count = std::max(ta.w, ta.h);
			if (piece_count < 3) {
				ShowErrorMessage(GetEncodedString(STR_ERROR_AIRPORT_RUNWAY_TOO_SHORT), {}, WL_INFO);
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
				ShowErrorMessage(GetEncodedString(STR_ERROR_FLAT_LAND_REQUIRED), {}, WL_INFO);
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
					ShowErrorMessage(GetEncodedString(STR_ERROR_FLAT_LAND_REQUIRED), {}, WL_INFO);
					return false;
				}
			}
		}

		return true;
	}

	void OnPlaceObjectAbort() override
	{
		if (this->updating_cursor) return; // We're re-setting our own cursor; ignore.

		/* External window stole the cursor — deselect and raise all buttons. */
		this->selected_piece = static_cast<uint8_t>(PIECE_COUNT);
		for (WidgetID w = WID_MA_PIECE_FIRST; w <= WID_MA_PIECE_LAST; w++) {
			this->RaiseWidget(w);
		}
		this->fence_tool_active = false;
		this->SetWidgetLoweredState(WID_MA_FENCE_TOOL, false);
		this->SetWidgetLoweredState(WID_MA_TEMPLATE_MANAGER, false);
		this->SetDirty();
	}

	void OnGameTick() override
	{
		if (this->cached_year != TimerGameCalendar::year) {
			this->cached_year = TimerGameCalendar::year;
			this->RefreshYearGating();
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
		this->RefreshYearGating();
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
				if (piece.is_multi_tile && piece.apt_gfx == APT_SMALL_BUILDING_2) {
					SetTileSelectSize(3, 1);
				}
			}
		}
		this->updating_cursor = false;
	}
};

/** Hangar direction picker window (opened when clicking a hangar piece button). */
class BuildModularHangarPickerWindow : public PickerWindowBase {
	bool large_hangar; ///< true = large hangar (piece 5), false = small (piece 6)

	/** Widget-to-rotation mapping: NW=2, NE=1, SW=3, SE=0 */
	static constexpr uint8_t _widget_to_rot[4] = {2, 1, 3, 0}; // indexed by (widget - WID_MAHP_DIR_NW)

public:
	BuildModularHangarPickerWindow(WindowDesc &desc, Window *parent, bool is_large)
		: PickerWindowBase(desc, parent), large_hangar(is_large)
	{
		this->InitNested(0);
		/* Lower the button matching the current rotation */
		for (WidgetID w = WID_MAHP_DIR_NW; w <= WID_MAHP_DIR_SE; w++) {
			if (_widget_to_rot[w - WID_MAHP_DIR_NW] == _modular_hangar_rotation) {
				this->LowerWidget(w);
				break;
			}
		}
	}

	void Close([[maybe_unused]] int data = 0) override
	{
		if (this->parent != nullptr &&
				this->parent->window_class == WC_BUILD_STATION &&
				this->parent->window_number == WN_BUILD_MODULAR_AIRPORT) {
			static_cast<BuildModularAirportWindow *>(this->parent)->StopPlacementFromClosedPicker(4);
		}
		/* Skip PickerWindowBase::Close() which calls ResetObjectToPlace() —
		 * we're a child picker and must not steal the parent's cursor. */
		this->Window::Close();
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
			/* Use the modular hangar layout directly — StationPickerDrawSprite can't handle
			 * the high gfx indices (APT_DEPOT_NW/NE/SW = 88-90) which are beyond the
			 * airport tile layout table size and get clamped to index 0 (apron). */
			const DrawTileSprites *t = GetModularHangarTileLayout(rot, !this->large_hangar);
			PaletteID pal = GetCompanyPalette(_local_company);
			DrawSprite(t->ground.sprite, HasBit(t->ground.sprite, PALETTE_MODIFIER_COLOUR) ? pal : PAL_NONE, x, y);
			DrawRailTileSeqInGUI(x, y, t, 0, 0, pal);
		}
	}

	void OnClick([[maybe_unused]] Point pt, WidgetID widget, [[maybe_unused]] int click_count) override
	{
		if (widget < WID_MAHP_DIR_NW || widget > WID_MAHP_DIR_SE) return;

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
		NWidget(WWT_CLOSEBOX, COLOUR_DARK_GREEN),
		NWidget(WWT_CAPTION, COLOUR_DARK_GREEN, WID_MAHP_CAPTION),
			SetStringTip(STR_STATION_BUILD_MODULAR_AIRPORT_HANGAR_PICKER_CAPTION, STR_TOOLTIP_WINDOW_TITLE_DRAG_THIS),
	EndContainer(),
	NWidget(WWT_PANEL, COLOUR_DARK_GREEN),
		NWidget(NWID_HORIZONTAL_LTR), SetPIP(0, WidgetDimensions::unscaled.hsep_normal, 0), SetPIPRatio(1, 0, 1), SetPadding(WidgetDimensions::unscaled.picker),
			NWidget(NWID_VERTICAL), SetPIP(0, WidgetDimensions::unscaled.vsep_normal, 0),
				NWidget(WWT_TEXTBTN, COLOUR_GREY, WID_MAHP_DIR_NW), SetFill(0, 0), SetToolTip(STR_STATION_BUILD_MODULAR_AIRPORT_PIECE_HANGAR),
				NWidget(WWT_TEXTBTN, COLOUR_GREY, WID_MAHP_DIR_SW), SetFill(0, 0), SetToolTip(STR_STATION_BUILD_MODULAR_AIRPORT_PIECE_HANGAR),
			EndContainer(),
			NWidget(NWID_VERTICAL), SetPIP(0, WidgetDimensions::unscaled.vsep_normal, 0),
				NWidget(WWT_TEXTBTN, COLOUR_GREY, WID_MAHP_DIR_NE), SetFill(0, 0), SetToolTip(STR_STATION_BUILD_MODULAR_AIRPORT_PIECE_HANGAR),
				NWidget(WWT_TEXTBTN, COLOUR_GREY, WID_MAHP_DIR_SE), SetFill(0, 0), SetToolTip(STR_STATION_BUILD_MODULAR_AIRPORT_PIECE_HANGAR),
			EndContainer(),
		EndContainer(),
	EndContainer(),
};

static WindowDesc _build_modular_hangar_picker_desc(
	WDP_AUTO, {}, 0, 0,
	WC_BUILD_DEPOT, WC_BUILD_STATION,
	WindowDefaultFlag::Construction,
	_nested_build_modular_hangar_picker_widgets
);

static void ShowModularHangarPicker(Window *parent, bool is_large)
{
	CloseWindowByClass(WC_BUILD_DEPOT);
	new BuildModularHangarPickerWindow(_build_modular_hangar_picker_desc, parent, is_large);
}

/** Cosmetic tile picker window (opened when clicking the Cosmetic piece button). */
class BuildModularCosmeticPickerWindow : public PickerWindowBase {
public:
	void RefreshAvailability()
	{
		/* Disable pieces gated behind a future year. */
		for (uint i = 0; i < lengthof(_cosmetic_pieces); i++) {
			bool locked = IsModernModularPiece(_cosmetic_pieces[i].apt_gfx) &&
				TimerGameCalendar::year < GetModularPieceMinYear(_cosmetic_pieces[i].apt_gfx);
			this->SetWidgetDisabledState(WID_MACP_PIECE_0 + i, locked);
		}
		if (this->IsWidgetDisabled(WID_MACP_PIECE_0 + _modular_cosmetic_piece)) {
			/* Selected piece is locked; pick the first available one. */
			for (uint i = 0; i < lengthof(_cosmetic_pieces); i++) {
				if (!this->IsWidgetDisabled(WID_MACP_PIECE_0 + i)) {
					_modular_cosmetic_piece = static_cast<uint8_t>(i);
					break;
				}
			}
		}
		for (uint i = 0; i < lengthof(_cosmetic_pieces); i++) {
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
				this->parent->window_class == WC_BUILD_STATION &&
				this->parent->window_number == WN_BUILD_MODULAR_AIRPORT) {
			static_cast<BuildModularAirportWindow *>(this->parent)->StopPlacementFromClosedPicker(3);
		}
		this->Window::Close();
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
		/* 3-tile terminal: compute size from actual tile sprite bounding box. */
		if (widget == WID_MACP_PIECE_10) {
			ZoomLevel icon_zoom = _gui_zoom;
			Point so;
			Dimension sd = GetSpriteSize(SPR_AIRFIELD_TERM_A, &so, icon_zoom);
			int tile_w = static_cast<int>(sd.width)  - so.x;
			int tile_h = static_cast<int>(sd.height) - so.y;
			int step_x = tile_w / 2;
			int step_y = -(tile_h / 2);
			int bb_left = INT_MAX, bb_right = INT_MIN, bb_top = INT_MAX, bb_bottom = INT_MIN;
			for (int i = 0; i < 3; i++) {
				int tx = (i - 1) * step_x;
				int ty = (i - 1) * step_y;
				bb_left   = std::min(bb_left, tx + so.x);
				bb_right  = std::max(bb_right, tx + so.x + tile_w);
				bb_top    = std::min(bb_top, ty + so.y);
				bb_bottom = std::max(bb_bottom, ty + so.y + tile_h);
			}
			/* Also account for TERM_C_BUILD overlay on tile 0. */
			{
				Point bo;
				Dimension bd = GetSpriteSize(SPR_AIRFIELD_TERM_C_BUILD, &bo, icon_zoom);
				int tx = (0 - 1) * step_x;
				int ty = (0 - 1) * step_y;
				bb_left   = std::min(bb_left, tx + bo.x);
				bb_right  = std::max(bb_right, tx + bo.x + static_cast<int>(bd.width));
				bb_top    = std::min(bb_top, ty + bo.y);
				bb_bottom = std::max(bb_bottom, ty + bo.y + static_cast<int>(bd.height));
			}
			size.width  = std::max(size.width,  static_cast<uint>(bb_right - bb_left) + WidgetDimensions::scaled.fullbevel.Horizontal() + ScaleGUITrad(4));
			size.height = std::max(size.height, static_cast<uint>(bb_bottom - bb_top) + WidgetDimensions::scaled.fullbevel.Vertical() + ScaleGUITrad(4));
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

		/* 3-tile terminal: draw all 3 tiles in isometric layout.
		 * Tiles are: APT_SMALL_BUILDING_3 (TERM_A), APT_SMALL_BUILDING_2 (TERM_B),
		 * APT_SMALL_BUILDING_1 (TERM_C_GROUND + TERM_C_BUILD overlay).
		 * TERM_A and TERM_B are single full-tile sprites; TERM_C is ground + child building. */
		if (piece_idx == 10) {
			/* Derive isometric tile step from the ground sprite's full pixel extent.
			 * GetSpriteSize returns d.width = max(0, x_offs + pixel_width), so the
			 * full tile width = d.width - offset.x (e.g. 33 - (-31) = 64 at normal zoom). */
			Point so;
			Dimension sd = GetSpriteSize(SPR_AIRFIELD_TERM_A, &so, icon_zoom);
			int tile_w = static_cast<int>(sd.width)  - so.x;  /* full tile pixel width  */
			int tile_h = static_cast<int>(sd.height) - so.y;  /* full tile pixel height  */
			int step_x = tile_w / 2;
			int step_y = -(tile_h / 2);

			/* Bounding box of all 3 ground tiles around origin. */
			int bb_left = INT_MAX, bb_right = INT_MIN, bb_top = INT_MAX, bb_bottom = INT_MIN;
			for (int i = 0; i < 3; i++) {
				int tx = (i - 1) * step_x;
				int ty = (i - 1) * step_y;
				bb_left   = std::min(bb_left, tx + so.x);
				bb_right  = std::max(bb_right, tx + so.x + tile_w);
				bb_top    = std::min(bb_top, ty + so.y);
				bb_bottom = std::max(bb_bottom, ty + so.y + tile_h);
			}
			/* Also account for TERM_C_BUILD overlay on tile 0 (bottom-left). */
			{
				Point bo;
				Dimension bd = GetSpriteSize(SPR_AIRFIELD_TERM_C_BUILD, &bo, icon_zoom);
				int tx = (0 - 1) * step_x;
				int ty = (0 - 1) * step_y;
				bb_left   = std::min(bb_left, tx + bo.x);
				bb_right  = std::max(bb_right, tx + bo.x + static_cast<int>(bd.width));
				bb_top    = std::min(bb_top, ty + bo.y);
				bb_bottom = std::max(bb_bottom, ty + bo.y + static_cast<int>(bd.height));
			}

			/* Offset to centre the bounding box within the widget. */
			int off_x = (ir.Width()  - (bb_right + bb_left)) / 2;
			int off_y = (ir.Height() - (bb_bottom + bb_top)) / 2;
			off_y += ScaleSpriteTrad(piece.preview_y_offset);

			/* Draw back-to-front: tile 2 (top-right) first, tile 0 (bottom-left) last. */
			for (int i = 2; i >= 0; i--) {
				int tx = (i - 1) * step_x + off_x;
				int ty = (i - 1) * step_y + off_y;
				if (i == 2) {
					/* APT_SMALL_BUILDING_1: ground + building overlay (the globe). */
					DrawSprite(SPR_AIRFIELD_TERM_C_GROUND, pal, tx, ty, nullptr, icon_zoom);
					DrawSprite(SPR_AIRFIELD_TERM_C_BUILD, pal, tx, ty, nullptr, icon_zoom);
				} else if (i == 1) {
					DrawSprite(SPR_AIRFIELD_TERM_B, pal, tx, ty, nullptr, icon_zoom);
				} else {
					DrawSprite(SPR_AIRFIELD_TERM_A, pal, tx, ty, nullptr, icon_zoom);
				}
			}
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
		const CosmeticPiece &piece = _cosmetic_pieces[std::min<uint8_t>(_modular_cosmetic_piece, lengthof(_cosmetic_pieces) - 1)];
		if (piece.is_multi_tile && piece.apt_gfx == APT_SMALL_BUILDING_2) {
			SetTileSelectSize(3, 1);
		} else {
			SetTileSelectSize(1, 1);
		}
	}
};

static constexpr std::initializer_list<NWidgetPart> _nested_build_modular_cosmetic_picker_widgets = {
	NWidget(NWID_HORIZONTAL),
		NWidget(WWT_CLOSEBOX, COLOUR_DARK_GREEN),
		NWidget(WWT_CAPTION, COLOUR_DARK_GREEN, WID_MACP_CAPTION),
			SetStringTip(STR_STATION_BUILD_MODULAR_AIRPORT_COSMETIC_PICKER_CAPTION,
			             STR_TOOLTIP_WINDOW_TITLE_DRAG_THIS),
	EndContainer(),
	NWidget(WWT_PANEL, COLOUR_DARK_GREEN),
		NWidget(NWID_VERTICAL), SetPIP(0, WidgetDimensions::unscaled.vsep_normal, 0),
		                          SetPIPRatio(1, 0, 1), SetPadding(WidgetDimensions::unscaled.picker),
			NWidget(NWID_HORIZONTAL), SetPIP(0, WidgetDimensions::unscaled.hsep_normal, 0), SetPIPRatio(1, 0, 1),
				NWidget(WWT_TEXTBTN, COLOUR_GREY, WID_MACP_PIECE_0), SetFill(0, 0),
					SetToolTip(STR_STATION_BUILD_MODULAR_AIRPORT_PIECE_TERMINAL),
				NWidget(WWT_TEXTBTN, COLOUR_GREY, WID_MACP_PIECE_1), SetFill(0, 0),
					SetToolTip(STR_STATION_BUILD_MODULAR_AIRPORT_PIECE_TERMINAL_ALT),
				NWidget(WWT_TEXTBTN, COLOUR_GREY, WID_MACP_PIECE_2), SetFill(0, 0),
					SetToolTip(STR_STATION_BUILD_MODULAR_AIRPORT_PIECE_TERMINAL_OTHER),
				NWidget(WWT_TEXTBTN, COLOUR_GREY, WID_MACP_PIECE_3), SetFill(0, 0),
					SetToolTip(STR_STATION_BUILD_MODULAR_AIRPORT_PIECE_TERMINAL_ROUND),
			EndContainer(),
			NWidget(NWID_HORIZONTAL), SetPIP(0, WidgetDimensions::unscaled.hsep_normal, 0), SetPIPRatio(1, 0, 1),
				NWidget(WWT_TEXTBTN, COLOUR_GREY, WID_MACP_PIECE_4), SetFill(0, 0),
					SetToolTip(STR_STATION_BUILD_MODULAR_AIRPORT_PIECE_LOW_TERMINAL),
				NWidget(WWT_TEXTBTN, COLOUR_GREY, WID_MACP_PIECE_5), SetFill(0, 0),
					SetToolTip(STR_STATION_BUILD_MODULAR_AIRPORT_PIECE_TOWER),
				NWidget(WWT_TEXTBTN, COLOUR_GREY, WID_MACP_PIECE_6), SetFill(0, 0),
					SetToolTip(STR_STATION_BUILD_MODULAR_AIRPORT_PIECE_RADIO_TOWER),
				NWidget(WWT_TEXTBTN, COLOUR_GREY, WID_MACP_PIECE_7), SetFill(0, 0),
					SetToolTip(STR_STATION_BUILD_MODULAR_AIRPORT_PIECE_FLAG_GRASS),
			EndContainer(),
			NWidget(NWID_HORIZONTAL), SetPIP(0, WidgetDimensions::unscaled.hsep_normal, 0), SetPIPRatio(1, 0, 1),
				NWidget(WWT_TEXTBTN, COLOUR_GREY, WID_MACP_PIECE_8), SetFill(0, 0),
					SetToolTip(STR_STATION_BUILD_MODULAR_AIRPORT_PIECE_RADAR),
				NWidget(WWT_TEXTBTN, COLOUR_GREY, WID_MACP_PIECE_9), SetFill(0, 0),
					SetToolTip(STR_STATION_BUILD_MODULAR_AIRPORT_PIECE_RADAR_GRASS),
				NWidget(WWT_TEXTBTN, COLOUR_GREY, WID_MACP_PIECE_10), SetFill(1, 0), SetMinimalSize(120, 0),
					SetToolTip(STR_STATION_BUILD_MODULAR_AIRPORT_PIECE_SMALL_TERMINAL_3),
			EndContainer(),
		EndContainer(),
	EndContainer(),
};

static WindowDesc _build_modular_cosmetic_picker_desc(
	WDP_AUTO, {}, 0, 0,
	WC_BUILD_DEPOT, WC_BUILD_STATION,
	WindowDefaultFlag::Construction,
	_nested_build_modular_cosmetic_picker_widgets
);

static void ShowModularCosmeticPicker(Window *parent)
{
	CloseWindowByClass(WC_BUILD_DEPOT);
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
			bool locked = IsModernModularPiece(_helipad_pieces[i].apt_gfx) &&
				TimerGameCalendar::year < GetModularPieceMinYear(_helipad_pieces[i].apt_gfx);
			this->SetWidgetDisabledState(WID_MAHPAD_PIECE_0 + i, locked);
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
				this->parent->window_class == WC_BUILD_STATION &&
				this->parent->window_number == WN_BUILD_MODULAR_AIRPORT) {
			static_cast<BuildModularAirportWindow *>(this->parent)->StopPlacementFromClosedPicker(6);
		}
		this->Window::Close();
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
		NWidget(WWT_CLOSEBOX, COLOUR_DARK_GREEN),
		NWidget(WWT_CAPTION, COLOUR_DARK_GREEN, WID_MAHPAD_CAPTION),
			SetStringTip(STR_AIRPORT_CLASS_HELIPORTS, STR_TOOLTIP_WINDOW_TITLE_DRAG_THIS),
	EndContainer(),
	NWidget(WWT_PANEL, COLOUR_DARK_GREEN),
		NWidget(NWID_HORIZONTAL_LTR), SetPIP(0, WidgetDimensions::unscaled.hsep_normal, 0),
		                              SetPIPRatio(1, 0, 1), SetPadding(WidgetDimensions::unscaled.picker),
			NWidget(WWT_TEXTBTN, COLOUR_GREY, WID_MAHPAD_PIECE_0), SetFill(0, 0),
				SetToolTip(STR_STATION_BUILD_MODULAR_AIRPORT_PIECE_HELIPAD),
			NWidget(WWT_TEXTBTN, COLOUR_GREY, WID_MAHPAD_PIECE_1), SetFill(0, 0),
				SetToolTip(STR_AIRPORT_HELISTATION),
			NWidget(WWT_TEXTBTN, COLOUR_GREY, WID_MAHPAD_PIECE_2), SetFill(0, 0),
				SetToolTip(STR_AIRPORT_HELIPORT),
		EndContainer(),
	EndContainer(),
};

static WindowDesc _build_modular_helipad_picker_desc(
	WDP_AUTO, {}, 0, 0,
	WC_BUILD_DEPOT, WC_BUILD_STATION,
	WindowDefaultFlag::Construction,
	_nested_build_modular_helipad_picker_widgets
);

static void ShowModularHelipadPicker(Window *parent)
{
	CloseWindowByClass(WC_BUILD_DEPOT);
	new BuildModularHelipadPickerWindow(_build_modular_helipad_picker_desc, parent);
}

static constexpr std::initializer_list<NWidgetPart> _nested_build_modular_airport_widgets = {
	NWidget(NWID_HORIZONTAL),
		NWidget(WWT_CLOSEBOX, COLOUR_DARK_GREEN),
		NWidget(WWT_CAPTION, COLOUR_DARK_GREEN), SetStringTip(STR_STATION_BUILD_MODULAR_AIRPORT_CAPTION, STR_TOOLTIP_WINDOW_TITLE_DRAG_THIS),
		NWidget(WWT_STICKYBOX, COLOUR_DARK_GREEN),
	EndContainer(),
	NWidget(NWID_HORIZONTAL),
		NWidget(WWT_TEXTBTN, COLOUR_DARK_GREEN, WID_MA_PIECE_0),  SetFill(0, 1), SetToolbarMinimalSize(1), SetToolTip(STR_STATION_BUILD_MODULAR_AIRPORT_PIECE_RUNWAY),
		NWidget(WWT_TEXTBTN, COLOUR_DARK_GREEN, WID_MA_PIECE_2),  SetFill(0, 1), SetToolbarMinimalSize(1), SetToolTip(STR_STATION_BUILD_MODULAR_AIRPORT_PIECE_RUNWAY_SMALL_MID),
		NWidget(WWT_TEXTBTN, COLOUR_DARK_GREEN, WID_MA_PIECE_4),  SetFill(0, 1), SetToolbarMinimalSize(1), SetToolTip(STR_STATION_BUILD_MODULAR_AIRPORT_PIECE_HANGAR),
		NWidget(WWT_TEXTBTN, COLOUR_DARK_GREEN, WID_MA_PIECE_5),  SetFill(0, 1), SetToolbarMinimalSize(1), SetToolTip(STR_STATION_BUILD_MODULAR_AIRPORT_PIECE_SMALL_HANGAR),
		NWidget(WWT_TEXTBTN, COLOUR_DARK_GREEN, WID_MA_PIECE_3),  SetFill(0, 1), SetToolbarMinimalSize(1), SetToolTip(STR_STATION_BUILD_MODULAR_AIRPORT_PIECE_COSMETIC),
		NWidget(WWT_TEXTBTN, COLOUR_DARK_GREEN, WID_MA_PIECE_6),  SetFill(0, 1), SetToolbarMinimalSize(1), SetToolTip(STR_STATION_BUILD_MODULAR_AIRPORT_PIECE_HELIPAD),
		NWidget(WWT_TEXTBTN, COLOUR_DARK_GREEN, WID_MA_PIECE_7),  SetFill(0, 1), SetToolbarMinimalSize(1), SetToolTip(STR_STATION_BUILD_MODULAR_AIRPORT_PIECE_STAND),
		NWidget(WWT_TEXTBTN, COLOUR_DARK_GREEN, WID_MA_PIECE_8),  SetFill(0, 1), SetToolbarMinimalSize(1), SetToolTip(STR_STATION_BUILD_MODULAR_AIRPORT_PIECE_APRON),
		NWidget(WWT_TEXTBTN, COLOUR_DARK_GREEN, WID_MA_PIECE_9),  SetFill(0, 1), SetToolbarMinimalSize(1), SetToolTip(STR_STATION_BUILD_MODULAR_AIRPORT_PIECE_GRASS),
		NWidget(WWT_TEXTBTN, COLOUR_DARK_GREEN, WID_MA_PIECE_10), SetFill(0, 1), SetToolbarMinimalSize(1), SetToolTip(STR_STATION_BUILD_MODULAR_AIRPORT_PIECE_EMPTY),
		NWidget(WWT_IMGBTN, COLOUR_DARK_GREEN, WID_MA_FENCE_TOOL), SetFill(0, 1), SetToolbarMinimalSize(1),
			SetSpriteTip(SPR_AIRPORT_FENCE_Y, STR_STATION_BUILD_MODULAR_AIRPORT_FENCE_TOOL),
		NWidget(WWT_IMGBTN, COLOUR_DARK_GREEN, WID_MA_TEMPLATE_MANAGER), SetFill(0, 1), SetToolbarMinimalSize(1),
			SetSpriteTip(SPR_IMG_SAVE, STR_STATION_BUILD_MODULAR_AIRPORT_TEMPLATE_MANAGER_TOOLTIP),
		NWidget(WWT_TEXTBTN, COLOUR_DARK_GREEN, WID_MA_PIECE_11), SetFill(0, 1), SetToolbarMinimalSize(1), SetToolTip(STR_TOOLTIP_DEMOLISH_BUILDINGS_ETC),
		NWidget(WWT_PANEL, COLOUR_DARK_GREEN), SetToolbarSpacerMinimalSize(), EndContainer(),
		NWidget(WWT_IMGBTN, COLOUR_DARK_GREEN, WID_MA_TOGGLE_SHOW_ARROWS), SetFill(0, 1), SetToolbarMinimalSize(1),
			SetSpriteTip(SPR_ONEWAY_BASE + 2, STR_STATION_BUILD_MODULAR_AIRPORT_TOGGLE_SHOW_ARROWS),
		NWidget(WWT_IMGBTN, COLOUR_DARK_GREEN, WID_MA_TOGGLE_SHOW_HOLDING), SetFill(0, 1), SetToolbarMinimalSize(1),
			SetSpriteTip(SPR_BLOT, STR_STATION_BUILD_MODULAR_AIRPORT_TOGGLE_SHOW_HOLDING),
		NWidget(WWT_IMGBTN, COLOUR_DARK_GREEN, WID_MA_TOGGLE_SHOW_RESERVATIONS), SetFill(0, 1), SetToolbarMinimalSize(1),
			SetSpriteTip(SPR_SQUARE, STR_STATION_BUILD_MODULAR_AIRPORT_TOGGLE_SHOW_RESERVATIONS),
	EndContainer(),
};

static WindowDesc _build_modular_airport_desc(
	WDP_AUTO, "build_modular_airport", 0, 0,
	WC_BUILD_STATION, WC_BUILD_TOOLBAR,
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

/** Conservative AABB visibility check — GfxDrawLine clips anyway; this skips obviously off-screen segments. */
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
			GfxFillRect(p.x - 2, p.y - 2, p.x + 2, p.y + 2, loop_colour);
		}

		for (const auto &gate : loop.gates) {
			if (gate.wp_index >= n) continue;

			/* Yellow: gate waypoint → runway threshold. */
			const auto &wp = loop.waypoints[gate.wp_index];
			Point pgw = HoldingWorldToScreen(vp, wp.x, wp.y);
			Point pth = HoldingWorldToScreen(vp, gate.threshold_x, gate.threshold_y, 0);
			if (HoldingSegVis(pgw, pth, dpi)) GfxDrawLine(pgw.x, pgw.y, pth.x, pth.y, PC_YELLOW, 1);

			/* Red threshold marker at ground level. */
			GfxFillRect(pth.x - 3, pth.y - 3, pth.x + 3, pth.y + 3, PC_RED);
		}
	}
}

static void DrawReservationChain(const Viewport &vp, DrawPixelInfo *dpi, const Point &origin, const std::vector<TileIndex> &chain, PixelColour colour)
{
	Point prev = origin;
	for (TileIndex tile : chain) {
		Point curr = TileCenterToScreen(vp, tile);
		if (HoldingSegVis(prev, curr, dpi)) GfxDrawLine(prev.x, prev.y, curr.x, curr.y, colour, 1);
		GfxFillRect(curr.x - 2, curr.y - 2, curr.x + 2, curr.y + 2, colour);
		prev = curr;
	}
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
		if (v->taxi_path == nullptr && v->taxi_reserved_tiles.empty() && v->modular_runway_reservation.empty()) continue;

		std::unordered_map<uint32_t, uint16_t> path_index_by_tile;
		if (v->taxi_path != nullptr) {
			const auto &path = v->taxi_path->tiles;
			for (uint16_t i = 0; i < path.size(); ++i) {
				path_index_by_tile.emplace(path[i].base(), i);
			}
		}

		std::unordered_set<uint32_t> seen;
		std::vector<std::pair<uint16_t, TileIndex>> behind_path_tiles;
		std::vector<std::pair<uint16_t, TileIndex>> ahead_path_tiles;
		std::vector<TileIndex> off_path_tiles;
		const uint16_t current_index = std::min<uint16_t>(v->taxi_path_index, v->taxi_path != nullptr ? v->taxi_path->tiles.size() : 0);
		const bool landing_phase = v->state == LANDING || v->state == ENDLANDING || v->state == HELILANDING || v->state == HELIENDLANDING;

		auto append_classified = [&](TileIndex tile) {
			if (!IsTileReservedBy(tile, v->index)) return;
			if (!seen.insert(tile.base()).second) return;

			auto path_it = path_index_by_tile.find(tile.base());
			if (path_it == path_index_by_tile.end()) {
				off_path_tiles.push_back(tile);
				return;
			}

			if (path_it->second < current_index) {
				behind_path_tiles.emplace_back(path_it->second, tile);
			} else {
				ahead_path_tiles.emplace_back(path_it->second, tile);
			}
		};

		if (landing_phase) {
			for (TileIndex tile : v->modular_runway_reservation) append_classified(tile);
		}

		if (v->taxi_path != nullptr) {
			for (TileIndex tile : v->taxi_path->tiles) append_classified(tile);
		}
		for (TileIndex tile : v->taxi_reserved_tiles) append_classified(tile);

		if (!landing_phase) {
			for (TileIndex tile : v->modular_runway_reservation) append_classified(tile);
		}

		if (seen.empty()) continue;
		const PixelColour colour = GetReservationOverlayColour(v->index);
		const Point origin = WorldToScreen(vp, v->x_pos, v->y_pos, v->z_pos + 4);

		std::sort(ahead_path_tiles.begin(), ahead_path_tiles.end(), [](const auto &a, const auto &b) { return a.first < b.first; });
		std::sort(behind_path_tiles.begin(), behind_path_tiles.end(), [](const auto &a, const auto &b) { return a.first < b.first; });

		std::vector<TileIndex> forward_chain;
		std::unordered_set<uint32_t> forward_seen;
		auto append_forward_unique = [&](TileIndex tile) {
			if (forward_seen.insert(tile.base()).second) forward_chain.push_back(tile);
		};

		/* For active landings, anchor the first segment at the touchdown tile. */
		if (landing_phase && IsValidTile(v->modular_landing_tile) && IsTileReservedBy(v->modular_landing_tile, v->index)) {
			append_forward_unique(v->modular_landing_tile);
		}

		for (const auto &[idx, tile] : ahead_path_tiles) {
			(void)idx;
			append_forward_unique(tile);
		}
		for (TileIndex tile : off_path_tiles) append_forward_unique(tile);

		std::vector<TileIndex> backward_chain;
		backward_chain.reserve(behind_path_tiles.size());
		for (const auto &[idx, tile] : behind_path_tiles) {
			(void)idx;
			backward_chain.push_back(tile);
		}

		DrawReservationChain(vp, dpi, origin, forward_chain, colour);
		DrawReservationChain(vp, dpi, origin, backward_chain, colour);
	}
}

void ShowBuildModularAirportWindow(Window *parent)
{
	CloseWindowById(WC_BUILD_STATION, WN_BUILD_MODULAR_AIRPORT);
	new BuildModularAirportWindow(_build_modular_airport_desc, parent);
}
