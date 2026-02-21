/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file airport_gui.cpp The GUI for airports. */

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
#include "gui.h"
#include "command_func.h"
#include "airport_cmd.h"
#include "station_cmd.h"
#include "airport_pathfinder.h"
#include "airport_ground_pathfinder.h"
#include "landscape_cmd.h"
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

#include "widgets/airport_widget.h"

#include "table/airporttile_ids.h"
#include "table/strings.h"

#include "safeguards.h"


static AirportClassID _selected_airport_class; ///< the currently visible airport class
static int _selected_airport_index;            ///< the index of the selected airport in the current class or -1
static uint8_t _selected_airport_layout;          ///< selected airport layout number.
static StationID _last_modular_airport_station = StationID::Invalid();
static uint8_t _modular_hangar_rotation = 0;  ///< 0=SE, 1=NE, 2=NW, 3=SW

bool _show_runway_direction_overlay = false; ///< Show runway direction/usage arrows in viewport

static void ShowBuildAirportPicker(Window *parent);
static void ShowBuildModularAirportWindow(Window *parent);

SpriteID GetCustomAirportSprite(const AirportSpec *as, uint8_t layout);

static const WindowNumber WN_BUILD_MODULAR_AIRPORT = WindowNumber{TRANSPORT_AIR};
struct ModularAirportPiece {
	StringID name;    ///< Full name (used as tooltip)
	SpriteID icon;    ///< Toolbar button icon sprite
	PixelColour colour;
};

static constexpr ModularAirportPiece _modular_airport_pieces[] = {
	{STR_STATION_BUILD_MODULAR_AIRPORT_PIECE_RUNWAY,           SPR_AIRPORT_RUNWAY_EXIT_B,  PC_DARK_GREY},   // 0
	{STR_STATION_BUILD_MODULAR_AIRPORT_PIECE_RUNWAY_END,       SPR_AIRPORT_RUNWAY_END,     PC_DARK_GREY},   // 1
	{STR_STATION_BUILD_MODULAR_AIRPORT_PIECE_RUNWAY_SMALL_MID, SPR_AIRFIELD_RUNWAY_MIDDLE, PC_DARK_GREY},   // 2  (smart-drag: auto-adds near/far ends)
	{STR_STATION_BUILD_MODULAR_AIRPORT_PIECE_TERMINAL,         SPR_AIRPORT_TERMINAL_A,     PC_ORANGE},      // 3
	{STR_STATION_BUILD_MODULAR_AIRPORT_PIECE_TERMINAL_ROUND,   SPR_AIRPORT_CONCOURSE,      PC_ORANGE},      // 4
	{STR_STATION_BUILD_MODULAR_AIRPORT_PIECE_HANGAR,           SPR_AIRPORT_HANGAR_FRONT,   PC_DARK_RED},    // 5
	{STR_STATION_BUILD_MODULAR_AIRPORT_PIECE_SMALL_HANGAR,     SPR_AIRFIELD_HANGAR_FRONT,  PC_DARK_RED},    // 6
	{STR_STATION_BUILD_MODULAR_AIRPORT_PIECE_HELIPAD,          SPR_AIRPORT_HELIPAD,        PC_LIGHT_YELLOW},// 7
	{STR_STATION_BUILD_MODULAR_AIRPORT_PIECE_STAND,            SPR_AIRPORT_AIRCRAFT_STAND, PC_YELLOW},      // 8
	{STR_STATION_BUILD_MODULAR_AIRPORT_PIECE_APRON,            SPR_AIRPORT_APRON,          PC_GREY},        // 9
	{STR_STATION_BUILD_MODULAR_AIRPORT_PIECE_TOWER,            SPR_AIRPORT_TOWER,          PC_ORANGE},      // 10
	{STR_STATION_BUILD_MODULAR_AIRPORT_PIECE_RADAR,            SPR_AIRPORT_RADAR_1,        PC_ORANGE},      // 11
	{STR_STATION_BUILD_MODULAR_AIRPORT_PIECE_RADIO_TOWER,      SPR_TRANSMITTER,            PC_ORANGE},      // 12
	{STR_STATION_BUILD_MODULAR_AIRPORT_PIECE_GRASS,            SPR_AIRFIELD_APRON_C,       PC_GREEN},       // 13
	{STR_STATION_BUILD_MODULAR_AIRPORT_PIECE_EMPTY,            SPR_FLAT_GRASS_TILE,        PC_WHITE},       // 14
	{STR_STATION_BUILD_MODULAR_AIRPORT_PIECE_ERASE,            SPR_IMG_DYNAMITE,           PC_WHITE},       // 15
};

static constexpr int MODULAR_AIRPORT_PIECE_ERASE_INDEX = lengthof(_modular_airport_pieces) - 1;

static uint8_t GetModularAirportPieceGfx(uint8_t piece)
{
	switch (piece) {
		case 0:  return APT_RUNWAY_5;
		case 1:  return APT_RUNWAY_END;
		case 2:  return APT_RUNWAY_SMALL_MIDDLE;
		case 3:  return APT_BUILDING_1;
		case 4:  return APT_ROUND_TERMINAL;
		case 5:  return APT_DEPOT_SE;
		case 6:  return APT_SMALL_DEPOT_SE;
		case 7:  return APT_HELIPAD_2;
		case 8:  return APT_STAND;
		case 9:  return APT_APRON;
		case 10: return APT_TOWER;
		case 11: return APT_RADAR_FENCE_NE;
		case 12: return APT_RADIO_TOWER_FENCE_NE;
		case 13: return APT_GRASS_1;
		case 14: return APT_EMPTY;
		default: return APT_APRON;
	}
}

static bool IsModularTaxiwayGfx(uint8_t gfx)
{
	switch (gfx) {
		case APT_APRON_HOR:
		case APT_APRON_VER_CROSSING_N:
		case APT_APRON_HOR_CROSSING_E:
		case APT_APRON_VER_CROSSING_S:
		case APT_APRON:
		case APT_ARPON_N:
		case APT_APRON_E:
		case APT_APRON_S:
		case APT_APRON_W:
		case APT_APRON_HALF_EAST:
		case APT_APRON_HALF_WEST:
			return true;
		default:
			return false;
	}
}

void CcBuildAirport(Commands, const CommandCost &result, TileIndex tile)
{
	if (result.Failed()) return;

	if (_settings_client.sound.confirm) SndPlayTileFx(SND_1F_CONSTRUCTION_OTHER, tile);
	Tile t(tile);
	/* Check if tile is a station before calling IsAirport (which asserts this) */
	if (IsTileType(t, TileType::Station) && IsAirport(t)) {
		Station *st = Station::GetByTile(tile);
		if (st->airport.blocks.Test(AirportBlock::Modular)) {
			_last_modular_airport_station = st->index;
		}
	}
	if (!_settings_client.gui.persistent_buildingtools) ResetObjectToPlace();
}

/**
 * Place an airport.
 * @param tile Position to put the new airport.
 */
static void PlaceAirport(TileIndex tile)
{
	if (_selected_airport_index == -1) return;

	uint8_t airport_type = AirportClass::Get(_selected_airport_class)->GetSpec(_selected_airport_index)->GetIndex();
	uint8_t layout = _selected_airport_layout;
	bool adjacent = _ctrl_pressed;

	auto proc = [=](bool test, StationID to_join) -> bool {
		if (test) {
			return Command<CMD_BUILD_AIRPORT>::Do(CommandFlagsToDCFlags(GetCommandFlags<CMD_BUILD_AIRPORT>()), tile, airport_type, layout, StationID::Invalid(), adjacent).Succeeded();
		} else {
			return Command<CMD_BUILD_AIRPORT>::Post(STR_ERROR_CAN_T_BUILD_AIRPORT_HERE, CcBuildAirport, tile, airport_type, layout, to_join, adjacent);
		}
	};

	ShowSelectStationIfNeeded(TileArea(tile, _thd.size.x / TILE_SIZE, _thd.size.y / TILE_SIZE), proc);
}

/** Airport build toolbar window handler. */
struct BuildAirToolbarWindow : Window {
	WidgetID last_user_action = INVALID_WIDGET; // Last started user action.

	BuildAirToolbarWindow(WindowDesc &desc, WindowNumber window_number) : Window(desc)
	{
		this->InitNested(window_number);
		this->OnInvalidateData();
		if (_settings_client.gui.link_terraform_toolbar) ShowTerraformToolbar(this);
	}

	void Close([[maybe_unused]] int data = 0) override
	{
		if (this->IsWidgetLowered(WID_AT_AIRPORT)) SetViewportCatchmentStation(nullptr, true);
		if (_settings_client.gui.link_terraform_toolbar) CloseWindowById(WC_SCEN_LAND_GEN, 0, false);
		this->Window::Close();
	}

	/**
	 * Some data on this window has become invalid.
	 * @param data Information about the changed data.
	 * @param gui_scope Whether the call is done from GUI scope. You may not do everything when not in GUI scope. See #InvalidateWindowData() for details.
	 */
	void OnInvalidateData([[maybe_unused]] int data = 0, [[maybe_unused]] bool gui_scope = true) override
	{
		if (!gui_scope) return;

		bool can_build = CanBuildVehicleInfrastructure(VEH_AIRCRAFT);
		this->SetWidgetDisabledState(WID_AT_AIRPORT, !can_build);
		this->SetWidgetDisabledState(WID_AT_MODULAR, !can_build);
		if (!can_build) {
			CloseWindowById(WC_BUILD_STATION, TRANSPORT_AIR);
			CloseWindowById(WC_BUILD_STATION, WN_BUILD_MODULAR_AIRPORT);

			/* Show in the tooltip why this button is disabled. */
			this->GetWidget<NWidgetCore>(WID_AT_AIRPORT)->SetToolTip(STR_TOOLBAR_DISABLED_NO_VEHICLE_AVAILABLE);
			this->GetWidget<NWidgetCore>(WID_AT_MODULAR)->SetToolTip(STR_TOOLBAR_DISABLED_NO_VEHICLE_AVAILABLE);
		} else {
			this->GetWidget<NWidgetCore>(WID_AT_AIRPORT)->SetToolTip(STR_TOOLBAR_AIRCRAFT_BUILD_AIRPORT_TOOLTIP);
			this->GetWidget<NWidgetCore>(WID_AT_MODULAR)->SetToolTip(STR_STATION_BUILD_MODULAR_AIRPORT_TOOLTIP);
		}
	}

	void OnClick([[maybe_unused]] Point pt, WidgetID widget, [[maybe_unused]] int click_count) override
	{
		switch (widget) {
			case WID_AT_AIRPORT:
				if (HandlePlacePushButton(this, WID_AT_AIRPORT, SPR_CURSOR_AIRPORT, HT_RECT)) {
					ShowBuildAirportPicker(this);
					this->last_user_action = widget;
				}
				break;

			case WID_AT_MODULAR:
				ShowBuildModularAirportWindow(this);
				this->last_user_action = INVALID_WIDGET;
				break;

			case WID_AT_DEMOLISH:
				HandlePlacePushButton(this, WID_AT_DEMOLISH, ANIMCURSOR_DEMOLISH, HT_RECT | HT_DIAGONAL);
				this->last_user_action = widget;
				break;

			default: break;
		}
	}


	void OnPlaceObject([[maybe_unused]] Point pt, TileIndex tile) override
	{
		switch (this->last_user_action) {
			case WID_AT_AIRPORT:
				PlaceAirport(tile);
				break;

			case WID_AT_DEMOLISH:
				PlaceProc_DemolishArea(tile);
				break;

			default: NOT_REACHED();
		}
	}

	void OnPlaceDrag(ViewportPlaceMethod select_method, [[maybe_unused]] ViewportDragDropSelectionProcess select_proc, [[maybe_unused]] Point pt) override
	{
		VpSelectTilesWithMethod(pt.x, pt.y, select_method);
	}

	Point OnInitialPosition(int16_t sm_width, [[maybe_unused]] int16_t sm_height, [[maybe_unused]] int window_number) override
	{
		return AlignInitialConstructionToolbar(sm_width);
	}

	void OnPlaceMouseUp([[maybe_unused]] ViewportPlaceMethod select_method, ViewportDragDropSelectionProcess select_proc, [[maybe_unused]] Point pt, TileIndex start_tile, TileIndex end_tile) override
	{
		if (pt.x != -1 && select_proc == DDSP_DEMOLISH_AREA) {
			GUIPlaceProcDragXY(select_proc, start_tile, end_tile);
		}
	}

	void OnPlaceObjectAbort() override
	{
		if (this->IsWidgetLowered(WID_AT_AIRPORT)) SetViewportCatchmentStation(nullptr, true);

		this->RaiseButtons();

		CloseWindowById(WC_BUILD_STATION, TRANSPORT_AIR);
		CloseWindowById(WC_SELECT_STATION, 0);
	}

	/**
	 * Handler for global hotkeys of the BuildAirToolbarWindow.
	 * @param hotkey Hotkey
	 * @return ES_HANDLED if hotkey was accepted.
	 */
	static EventState AirportToolbarGlobalHotkeys(int hotkey)
	{
		if (_game_mode != GM_NORMAL) return ES_NOT_HANDLED;
		Window *w = ShowBuildAirToolbar();
		if (w == nullptr) return ES_NOT_HANDLED;
		return w->OnHotkey(hotkey);
	}

	static inline HotkeyList hotkeys{"airtoolbar", {
		Hotkey('1', "airport", WID_AT_AIRPORT),
		Hotkey('2', "demolish", WID_AT_DEMOLISH),
	}, AirportToolbarGlobalHotkeys};
};

static constexpr std::initializer_list<NWidgetPart> _nested_air_toolbar_widgets = {
	NWidget(NWID_HORIZONTAL),
		NWidget(WWT_CLOSEBOX, COLOUR_DARK_GREEN),
		NWidget(WWT_CAPTION, COLOUR_DARK_GREEN), SetStringTip(STR_TOOLBAR_AIRCRAFT_CAPTION, STR_TOOLTIP_WINDOW_TITLE_DRAG_THIS),
		NWidget(WWT_STICKYBOX, COLOUR_DARK_GREEN),
	EndContainer(),
	NWidget(NWID_HORIZONTAL),
		NWidget(WWT_IMGBTN, COLOUR_DARK_GREEN, WID_AT_AIRPORT), SetFill(0, 1), SetToolbarMinimalSize(2), SetSpriteTip(SPR_IMG_AIRPORT, STR_TOOLBAR_AIRCRAFT_BUILD_AIRPORT_TOOLTIP),
		NWidget(WWT_IMGBTN, COLOUR_DARK_GREEN, WID_AT_MODULAR), SetFill(0, 1), SetToolbarMinimalSize(1), SetSpriteTip(SPR_IMG_AIRPORT, STR_STATION_BUILD_MODULAR_AIRPORT_TOOLTIP),
		NWidget(WWT_PANEL, COLOUR_DARK_GREEN), SetToolbarSpacerMinimalSize(), SetFill(1, 1), EndContainer(),
		NWidget(WWT_IMGBTN, COLOUR_DARK_GREEN, WID_AT_DEMOLISH), SetFill(0, 1), SetToolbarMinimalSize(1), SetSpriteTip(SPR_IMG_DYNAMITE, STR_TOOLTIP_DEMOLISH_BUILDINGS_ETC),
	EndContainer(),
};

static WindowDesc _air_toolbar_desc(
	WDP_MANUAL, "toolbar_air", 0, 0,
	WC_BUILD_TOOLBAR, WC_NONE,
	WindowDefaultFlag::Construction,
	_nested_air_toolbar_widgets,
	&BuildAirToolbarWindow::hotkeys
);

/**
 * Open the build airport toolbar window
 *
 * If the terraform toolbar is linked to the toolbar, that window is also opened.
 *
 * @return newly opened airport toolbar, or nullptr if the toolbar could not be opened.
 */
Window *ShowBuildAirToolbar()
{
	if (!Company::IsValidID(_local_company)) return nullptr;

	CloseWindowByClass(WC_BUILD_TOOLBAR);
	return AllocateWindowDescFront<BuildAirToolbarWindow>(_air_toolbar_desc, TRANSPORT_AIR);
}

class BuildAirportWindow : public PickerWindowBase {
	SpriteID preview_sprite{}; ///< Cached airport preview sprite.
	int line_height = 0;
	Scrollbar *vscroll = nullptr;

	/** Build a dropdown list of available airport classes */
	static DropDownList BuildAirportClassDropDown()
	{
		DropDownList list;

		for (const auto &cls : AirportClass::Classes()) {
			list.push_back(MakeDropDownListStringItem(cls.name, cls.Index()));
		}

		return list;
	}

public:
	BuildAirportWindow(WindowDesc &desc, Window *parent) : PickerWindowBase(desc, parent)
	{
		this->CreateNestedTree();

		this->vscroll = this->GetScrollbar(WID_AP_SCROLLBAR);
		this->vscroll->SetCapacity(5);
		this->vscroll->SetPosition(0);

		this->FinishInitNested(TRANSPORT_AIR);

		this->SetWidgetLoweredState(WID_AP_BTN_DONTHILIGHT, !_settings_client.gui.station_show_coverage);
		this->SetWidgetLoweredState(WID_AP_BTN_DOHILIGHT, _settings_client.gui.station_show_coverage);
		this->OnInvalidateData();

		/* Ensure airport class is valid (changing NewGRFs). */
		_selected_airport_class = Clamp(_selected_airport_class, APC_BEGIN, (AirportClassID)(AirportClass::GetClassCount() - 1));
		const AirportClass *ac = AirportClass::Get(_selected_airport_class);
		this->vscroll->SetCount(ac->GetSpecCount());

		/* Ensure the airport index is valid for this class (changing NewGRFs). */
		_selected_airport_index = Clamp(_selected_airport_index, -1, ac->GetSpecCount() - 1);

		/* Only when no valid airport was selected, we want to select the first airport. */
		bool select_first_airport = true;
		if (_selected_airport_index != -1) {
			const AirportSpec *as = ac->GetSpec(_selected_airport_index);
			if (as->IsAvailable()) {
				/* Ensure the airport layout is valid. */
				_selected_airport_layout = Clamp(_selected_airport_layout, 0, static_cast<uint8_t>(as->layouts.size() - 1));
				select_first_airport = false;
				this->UpdateSelectSize();
			}
		}

		if (select_first_airport) this->SelectFirstAvailableAirport(true);
	}

	void Close([[maybe_unused]] int data = 0) override
	{
		CloseWindowById(WC_SELECT_STATION, 0);
		this->PickerWindowBase::Close();
	}

	std::string GetWidgetString(WidgetID widget, StringID stringid) const override
	{
		switch (widget) {
			case WID_AP_CLASS_DROPDOWN:
				return GetString(AirportClass::Get(_selected_airport_class)->name);

			case WID_AP_LAYOUT_NUM:
				if (_selected_airport_index != -1) {
					const AirportSpec *as = AirportClass::Get(_selected_airport_class)->GetSpec(_selected_airport_index);
					StringID string = GetAirportTextCallback(as, _selected_airport_layout, CBID_AIRPORT_LAYOUT_NAME);
					if (string != STR_UNDEFINED) {
						return GetString(string);
					} else if (as->layouts.size() > 1) {
						return GetString(STR_STATION_BUILD_AIRPORT_LAYOUT_NAME, _selected_airport_layout + 1);
					}
				}
				return {};

			default:
				return this->Window::GetWidgetString(widget, stringid);
		}
	}

	void UpdateWidgetSize(WidgetID widget, Dimension &size, [[maybe_unused]] const Dimension &padding, [[maybe_unused]] Dimension &fill, [[maybe_unused]] Dimension &resize) override
	{
		switch (widget) {
			case WID_AP_CLASS_DROPDOWN: {
				Dimension d = {0, 0};
				for (const auto &cls : AirportClass::Classes()) {
					d = maxdim(d, GetStringBoundingBox(cls.name));
				}
				d.width += padding.width;
				d.height += padding.height;
				size = maxdim(size, d);
				break;
			}

			case WID_AP_AIRPORT_LIST: {
				for (int i = 0; i < NUM_AIRPORTS; i++) {
					const AirportSpec *as = AirportSpec::Get(i);
					if (!as->enabled) continue;

					size.width = std::max(size.width, GetStringBoundingBox(as->name).width + padding.width);
				}

				this->line_height = GetCharacterHeight(FS_NORMAL) + padding.height;
				size.height = 5 * this->line_height;
				break;
			}

			case WID_AP_AIRPORT_SPRITE:
				for (int i = 0; i < NUM_AIRPORTS; i++) {
					const AirportSpec *as = AirportSpec::Get(i);
					if (!as->enabled) continue;
					for (uint8_t layout = 0; layout < static_cast<uint8_t>(as->layouts.size()); layout++) {
						SpriteID sprite = GetCustomAirportSprite(as, layout);
						if (sprite != 0) {
							Dimension d = GetSpriteSize(sprite);
							d.width += WidgetDimensions::scaled.framerect.Horizontal();
							d.height += WidgetDimensions::scaled.framerect.Vertical();
							size = maxdim(d, size);
						}
					}
				}
				break;

			case WID_AP_EXTRA_TEXT:
				for (int i = NEW_AIRPORT_OFFSET; i < NUM_AIRPORTS; i++) {
					const AirportSpec *as = AirportSpec::Get(i);
					if (!as->enabled) continue;
					for (uint8_t layout = 0; layout < static_cast<uint8_t>(as->layouts.size()); layout++) {
						StringID string = GetAirportTextCallback(as, layout, CBID_AIRPORT_ADDITIONAL_TEXT);
						if (string == STR_UNDEFINED) continue;

						Dimension d = GetStringMultiLineBoundingBox(string, size);
						size = maxdim(d, size);
					}
				}
				break;

			default: break;
		}
	}

	void DrawWidget(const Rect &r, WidgetID widget) const override
	{
		switch (widget) {
			case WID_AP_AIRPORT_LIST: {
				Rect row = r.WithHeight(this->line_height).Shrink(WidgetDimensions::scaled.bevel);
				Rect text = r.WithHeight(this->line_height).Shrink(WidgetDimensions::scaled.matrix);
				const auto specs = AirportClass::Get(_selected_airport_class)->Specs();
				auto [first, last] = this->vscroll->GetVisibleRangeIterators(specs);
				for (auto it = first; it != last; ++it) {
					const AirportSpec *as = *it;
					if (!as->IsAvailable()) {
						GfxFillRect(row, PC_BLACK, FILLRECT_CHECKER);
					}
					DrawString(text, as->name, (static_cast<int>(as->index) == _selected_airport_index) ? TC_WHITE : TC_BLACK);
					row = row.Translate(0, this->line_height);
					text = text.Translate(0, this->line_height);
				}
				break;
			}

			case WID_AP_AIRPORT_SPRITE:
				if (this->preview_sprite != 0) {
					Dimension d = GetSpriteSize(this->preview_sprite);
					DrawSprite(this->preview_sprite, GetCompanyPalette(_local_company), CentreBounds(r.left, r.right, d.width), CentreBounds(r.top, r.bottom, d.height));
				}
				break;

			case WID_AP_EXTRA_TEXT:
				if (_selected_airport_index != -1) {
					const AirportSpec *as = AirportClass::Get(_selected_airport_class)->GetSpec(_selected_airport_index);
					StringID string = GetAirportTextCallback(as, _selected_airport_layout, CBID_AIRPORT_ADDITIONAL_TEXT);
					if (string != STR_UNDEFINED) {
						DrawStringMultiLine(r, string, TC_BLACK);
					}
				}
				break;
		}
	}

	void OnPaint() override
	{
		this->DrawWidgets();

		Rect r = this->GetWidget<NWidgetBase>(WID_AP_ACCEPTANCE)->GetCurrentRect();
		const int bottom = r.bottom;
		r.bottom = INT_MAX; // Allow overflow as we want to know the required height.

		if (_selected_airport_index != -1) {
			const AirportSpec *as = AirportClass::Get(_selected_airport_class)->GetSpec(_selected_airport_index);
			int rad = _settings_game.station.modified_catchment ? as->catchment : (uint)CA_UNMODIFIED;

			/* only show the station (airport) noise, if the noise option is activated */
			if (_settings_game.economy.station_noise_level) {
				/* show the noise of the selected airport */
				DrawString(r, GetString(STR_STATION_BUILD_NOISE, as->noise_level));
				r.top += GetCharacterHeight(FS_NORMAL) + WidgetDimensions::scaled.vsep_normal;
			}

			if (_settings_game.economy.infrastructure_maintenance) {
				Money monthly = _price[Price::InfrastructureAirport] * as->maintenance_cost >> 3;
				DrawString(r, GetString(TimerGameEconomy::UsingWallclockUnits() ? STR_STATION_BUILD_INFRASTRUCTURE_COST_PERIOD : STR_STATION_BUILD_INFRASTRUCTURE_COST_YEAR, monthly * 12));
				r.top += GetCharacterHeight(FS_NORMAL) + WidgetDimensions::scaled.vsep_normal;
			}

			/* strings such as 'Size' and 'Coverage Area' */
			r.top = DrawBadgeNameList(r, as->badges, GSF_AIRPORTS) + WidgetDimensions::scaled.vsep_normal;
			r.top = DrawStationCoverageAreaText(r, SCT_ALL, rad, false) + WidgetDimensions::scaled.vsep_normal;
			r.top = DrawStationCoverageAreaText(r, SCT_ALL, rad, true);
		}

		/* Resize background if the window is too small.
		 * Never make the window smaller to avoid oscillating if the size change affects the acceptance.
		 * (This is the case, if making the window bigger moves the mouse into the window.) */
		if (r.top > bottom) {
			ResizeWindow(this, 0, r.top - bottom, false);
		}
	}

	void SelectOtherAirport(int airport_index)
	{
		_selected_airport_index = airport_index;
		_selected_airport_layout = 0;

		this->UpdateSelectSize();
		this->SetDirty();
	}

	void UpdateSelectSize()
	{
		if (_selected_airport_index == -1) {
			SetTileSelectSize(1, 1);
			this->DisableWidget(WID_AP_LAYOUT_DECREASE);
			this->DisableWidget(WID_AP_LAYOUT_INCREASE);
		} else {
			const AirportSpec *as = AirportClass::Get(_selected_airport_class)->GetSpec(_selected_airport_index);
			int w = as->size_x;
			int h = as->size_y;
			Direction rotation = as->layouts[_selected_airport_layout].rotation;
			if (rotation == DIR_E || rotation == DIR_W) std::swap(w, h);
			SetTileSelectSize(w, h);

			this->preview_sprite = GetCustomAirportSprite(as, _selected_airport_layout);

			this->SetWidgetDisabledState(WID_AP_LAYOUT_DECREASE, _selected_airport_layout == 0);
			this->SetWidgetDisabledState(WID_AP_LAYOUT_INCREASE, _selected_airport_layout + 1U >= as->layouts.size());

			int rad = _settings_game.station.modified_catchment ? as->catchment : (uint)CA_UNMODIFIED;
			if (_settings_client.gui.station_show_coverage) SetTileSelectBigSize(-rad, -rad, 2 * rad, 2 * rad);
		}
	}

	void OnClick([[maybe_unused]] Point pt, WidgetID widget, [[maybe_unused]] int click_count) override
	{
		switch (widget) {
			case WID_AP_CLASS_DROPDOWN:
				ShowDropDownList(this, BuildAirportClassDropDown(), _selected_airport_class, WID_AP_CLASS_DROPDOWN);
				break;

			case WID_AP_AIRPORT_LIST: {
				int32_t num_clicked = this->vscroll->GetScrolledRowFromWidget(pt.y, this, widget, 0, this->line_height);
				if (num_clicked == INT32_MAX) break;
				const AirportSpec *as = AirportClass::Get(_selected_airport_class)->GetSpec(num_clicked);
				if (as->IsAvailable()) this->SelectOtherAirport(num_clicked);
				break;
			}

			case WID_AP_BTN_DONTHILIGHT: case WID_AP_BTN_DOHILIGHT:
				_settings_client.gui.station_show_coverage = (widget != WID_AP_BTN_DONTHILIGHT);
				this->SetWidgetLoweredState(WID_AP_BTN_DONTHILIGHT, !_settings_client.gui.station_show_coverage);
				this->SetWidgetLoweredState(WID_AP_BTN_DOHILIGHT, _settings_client.gui.station_show_coverage);
				this->SetDirty();
				SndClickBeep();
				this->UpdateSelectSize();
				SetViewportCatchmentStation(nullptr, true);
				break;

			case WID_AP_LAYOUT_DECREASE:
				_selected_airport_layout--;
				this->UpdateSelectSize();
				this->SetDirty();
				break;

			case WID_AP_LAYOUT_INCREASE:
				_selected_airport_layout++;
				this->UpdateSelectSize();
				this->SetDirty();
				break;
		}
	}

	/**
	 * Select the first available airport.
	 * @param change_class If true, change the class if no airport in the current
	 *   class is available.
	 */
	void SelectFirstAvailableAirport(bool change_class)
	{
		/* First try to select an airport in the selected class. */
		AirportClass *sel_apclass = AirportClass::Get(_selected_airport_class);
		for (const AirportSpec *as : sel_apclass->Specs()) {
			if (as->IsAvailable()) {
				this->SelectOtherAirport(as->index);
				return;
			}
		}
		if (change_class) {
			/* If that fails, select the first available airport
			 * from the first class where airports are available. */
			for (const auto &cls : AirportClass::Classes()) {
				for (const auto &as : cls.Specs()) {
					if (as->IsAvailable()) {
						_selected_airport_class = cls.Index();
						this->vscroll->SetCount(cls.GetSpecCount());
						this->SelectOtherAirport(as->index);
						return;
					}
				}
			}
		}
		/* If all airports are unavailable, select nothing. */
		this->SelectOtherAirport(-1);
	}

	void OnDropdownSelect(WidgetID widget, int index, int) override
	{
		if (widget == WID_AP_CLASS_DROPDOWN) {
			_selected_airport_class = (AirportClassID)index;
			this->vscroll->SetCount(AirportClass::Get(_selected_airport_class)->GetSpecCount());
			this->SelectFirstAvailableAirport(false);
		}
	}

	void OnRealtimeTick([[maybe_unused]] uint delta_ms) override
	{
		CheckRedrawStationCoverage(this);
	}

	const IntervalTimer<TimerGameCalendar> yearly_interval = {{TimerGameCalendar::YEAR, TimerGameCalendar::Priority::NONE}, [this](auto) {
		this->InvalidateData();
	}};
};

static void ShowModularHangarPicker(Window *parent, bool is_large);

class BuildModularAirportWindow : public PickerWindowBase {
	static constexpr int PIECE_COUNT = lengthof(_modular_airport_pieces);
	static constexpr ZoomLevel ICON_ZOOM = ZoomLevel::Out2x; ///< Zoom level for piece button icons.

	uint8_t selected_piece = 0;
	bool show_taxi_arrows = true;

public:
	BuildModularAirportWindow(WindowDesc &desc, Window *parent) : PickerWindowBase(desc, parent)
	{
		this->InitNested(WN_BUILD_MODULAR_AIRPORT);
		this->LowerWidget(WID_MA_PIECE_0 + this->selected_piece);
		this->SetWidgetLoweredState(WID_MA_TOGGLE_SHOW_ARROWS, this->show_taxi_arrows);
		this->UpdatePlacementCursor();
		_show_runway_direction_overlay = this->show_taxi_arrows;
		MarkWholeScreenDirty();
	}

	void Close([[maybe_unused]] int data = 0) override
	{
		_show_runway_direction_overlay = false;
		MarkWholeScreenDirty();
		ResetObjectToPlace();
		CloseWindowByClass(WC_BUILD_DEPOT);
		this->PickerWindowBase::Close();
	}

	void UpdateWidgetSize(WidgetID widget, Dimension &size, [[maybe_unused]] const Dimension &padding, [[maybe_unused]] Dimension &fill, [[maybe_unused]] Dimension &resize) override
	{
		if (widget < WID_MA_PIECE_FIRST || widget > WID_MA_PIECE_LAST) return;
		/* Size all piece buttons uniformly to fit the largest icon at the reduced zoom level. */
		Dimension max_dim = {0, 0};
		for (const auto &piece : _modular_airport_pieces) {
			Point offset;
			Dimension d = GetSpriteSize(piece.icon, &offset, ICON_ZOOM);
			d.width  -= offset.x;
			d.height -= offset.y;
			max_dim = maxdim(max_dim, d);
		}
		size.width  = std::max(size.width,  max_dim.width  + WidgetDimensions::scaled.bevel.Horizontal());
		size.height = std::max(size.height, max_dim.height + WidgetDimensions::scaled.bevel.Vertical());
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
		Dimension d = GetSpriteSize(piece.icon, &offset, ICON_ZOOM);
		d.width  -= offset.x;
		d.height -= offset.y;
		int x = (ir.Width()  - static_cast<int>(d.width))  / 2;
		int y = (ir.Height() - static_cast<int>(d.height)) / 2;
		DrawSprite(piece.icon, PAL_NONE, x - offset.x, y - offset.y, nullptr, ICON_ZOOM);
	}

	void OnClick([[maybe_unused]] Point pt, WidgetID widget, [[maybe_unused]] int click_count) override
	{
		if (widget >= WID_MA_PIECE_FIRST && widget <= WID_MA_PIECE_LAST) {
			uint8_t new_piece = static_cast<uint8_t>(widget - WID_MA_PIECE_FIRST);
			this->RaiseWidget(WID_MA_PIECE_0 + this->selected_piece);
			this->selected_piece = new_piece;
			this->LowerWidget(WID_MA_PIECE_0 + this->selected_piece);
			this->UpdatePlacementCursor();
			this->SetDirty();
			if (new_piece == 5 || new_piece == 6) {
				ShowModularHangarPicker(this, new_piece == 5);
			} else {
				CloseWindowByClass(WC_BUILD_DEPOT);
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

		/* Check if it's a runway piece */
		switch (data->piece_type) {
			case APT_RUNWAY_1:
			case APT_RUNWAY_2:
			case APT_RUNWAY_3:
			case APT_RUNWAY_4:
			case APT_RUNWAY_5:
			case APT_RUNWAY_END:
			case APT_RUNWAY_SMALL_NEAR_END:
			case APT_RUNWAY_SMALL_MIDDLE:
			case APT_RUNWAY_SMALL_FAR_END:
				break;
			default:
				return false;
		}

		uint8_t flags = data->runway_flags;

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
			/* Left-click: cycle direction (both -> low only -> high only -> both) */
			uint8_t dirs = flags & (RUF_DIR_LOW | RUF_DIR_HIGH);
			if (dirs == (RUF_DIR_LOW | RUF_DIR_HIGH)) {
				flags = (flags & ~(RUF_DIR_LOW | RUF_DIR_HIGH)) | RUF_DIR_LOW;
			} else if (dirs == RUF_DIR_LOW) {
				flags = (flags & ~(RUF_DIR_LOW | RUF_DIR_HIGH)) | RUF_DIR_HIGH;
			} else {
				flags = (flags & ~(RUF_DIR_LOW | RUF_DIR_HIGH)) | RUF_DIR_LOW | RUF_DIR_HIGH;
			}
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
		if (data == nullptr || !IsModularTaxiwayGfx(data->piece_type)) return false;

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

		/* When overlay is active, clicking on runway tiles edits their flags */
		if (this->TryEditRunwayFlags(tile)) return;
		/* When overlay is active, clicking on taxiway tiles edits one-way exit direction */
		if (this->TryEditTaxiwayFlags(tile)) return;

		/* Determine if this piece type supports drag-building */
		bool is_runway = (this->selected_piece >= 0 && this->selected_piece <= 2);  // Pieces 0-2: Runways
		bool is_apron  = (this->selected_piece == 9);                                // Piece 9: Apron
		bool is_grass  = (this->selected_piece == 13);                               // Piece 13: Grass
		bool is_empty  = (this->selected_piece == 14);                               // Piece 14: Empty

		bool supports_drag = is_runway || is_apron || is_grass || is_empty;

		if (supports_drag) {
			/* Enable drag-building */
			if (is_runway) {
				/* Linear pieces: allow drag in X or Y direction only */
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
				this->PlaceDragTileRawGfx(ordered_tiles.front(), APT_RUNWAY_SMALL_FAR_END, nearby_station, drag_rotation);
				for (size_t i = 1; i < ordered_tiles.size() - 1; i++) {
					this->PlaceDragTile(ordered_tiles[i], this->selected_piece, nearby_station, drag_rotation);
				}
				if (ordered_tiles.size() > 1) {
					this->PlaceDragTileRawGfx(ordered_tiles.back(), APT_RUNWAY_SMALL_NEAR_END, nearby_station, drag_rotation);
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
		this->PlaceSingleTileWithDialog(tile);
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
		uint8_t rot = (this->selected_piece == 5 || this->selected_piece == 6)
		              ? _modular_hangar_rotation : 0;

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
		bool is_runway = (this->selected_piece >= 0 && this->selected_piece <= 2);

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

		return true;
	}

	void OnPlaceObjectAbort() override
	{
		ResetObjectToPlace();
	}

private:
	void UpdatePlacementCursor()
	{
		SetTileSelectSize(1, 1);
		if (this->selected_piece == MODULAR_AIRPORT_PIECE_ERASE_INDEX) {
			SetObjectToPlace(ANIMCURSOR_DEMOLISH, PAL_NONE, HT_RECT | HT_DIAGONAL, this->window_class, this->window_number);
		} else {
			SetObjectToPlace(SPR_CURSOR_AIRPORT, PAL_NONE, HT_RECT, this->window_class, this->window_number);
		}
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

static constexpr std::initializer_list<NWidgetPart> _nested_build_modular_airport_widgets = {
	NWidget(NWID_HORIZONTAL),
		NWidget(WWT_CLOSEBOX, COLOUR_DARK_GREEN),
		NWidget(WWT_CAPTION, COLOUR_DARK_GREEN), SetStringTip(STR_STATION_BUILD_MODULAR_AIRPORT_CAPTION, STR_TOOLTIP_WINDOW_TITLE_DRAG_THIS),
		NWidget(WWT_STICKYBOX, COLOUR_DARK_GREEN),
	EndContainer(),
	NWidget(NWID_HORIZONTAL),
		NWidget(WWT_TEXTBTN, COLOUR_DARK_GREEN, WID_MA_PIECE_0),  SetFill(0, 1), SetToolTip(STR_STATION_BUILD_MODULAR_AIRPORT_PIECE_RUNWAY),
		NWidget(WWT_TEXTBTN, COLOUR_DARK_GREEN, WID_MA_PIECE_1),  SetFill(0, 1), SetToolTip(STR_STATION_BUILD_MODULAR_AIRPORT_PIECE_RUNWAY_END),
		NWidget(WWT_TEXTBTN, COLOUR_DARK_GREEN, WID_MA_PIECE_2),  SetFill(0, 1), SetToolTip(STR_STATION_BUILD_MODULAR_AIRPORT_PIECE_RUNWAY_SMALL_MID),
		NWidget(WWT_TEXTBTN, COLOUR_DARK_GREEN, WID_MA_PIECE_3),  SetFill(0, 1), SetToolTip(STR_STATION_BUILD_MODULAR_AIRPORT_PIECE_TERMINAL),
		NWidget(WWT_TEXTBTN, COLOUR_DARK_GREEN, WID_MA_PIECE_4),  SetFill(0, 1), SetToolTip(STR_STATION_BUILD_MODULAR_AIRPORT_PIECE_TERMINAL_ROUND),
		NWidget(WWT_TEXTBTN, COLOUR_DARK_GREEN, WID_MA_PIECE_5),  SetFill(0, 1), SetToolTip(STR_STATION_BUILD_MODULAR_AIRPORT_PIECE_HANGAR),
		NWidget(WWT_TEXTBTN, COLOUR_DARK_GREEN, WID_MA_PIECE_6),  SetFill(0, 1), SetToolTip(STR_STATION_BUILD_MODULAR_AIRPORT_PIECE_SMALL_HANGAR),
		NWidget(WWT_TEXTBTN, COLOUR_DARK_GREEN, WID_MA_PIECE_7),  SetFill(0, 1), SetToolTip(STR_STATION_BUILD_MODULAR_AIRPORT_PIECE_HELIPAD),
		NWidget(WWT_TEXTBTN, COLOUR_DARK_GREEN, WID_MA_PIECE_8),  SetFill(0, 1), SetToolTip(STR_STATION_BUILD_MODULAR_AIRPORT_PIECE_STAND),
		NWidget(WWT_TEXTBTN, COLOUR_DARK_GREEN, WID_MA_PIECE_9),  SetFill(0, 1), SetToolTip(STR_STATION_BUILD_MODULAR_AIRPORT_PIECE_APRON),
		NWidget(WWT_TEXTBTN, COLOUR_DARK_GREEN, WID_MA_PIECE_10), SetFill(0, 1), SetToolTip(STR_STATION_BUILD_MODULAR_AIRPORT_PIECE_TOWER),
		NWidget(WWT_TEXTBTN, COLOUR_DARK_GREEN, WID_MA_PIECE_11), SetFill(0, 1), SetToolTip(STR_STATION_BUILD_MODULAR_AIRPORT_PIECE_RADAR),
		NWidget(WWT_TEXTBTN, COLOUR_DARK_GREEN, WID_MA_PIECE_12), SetFill(0, 1), SetToolTip(STR_STATION_BUILD_MODULAR_AIRPORT_PIECE_RADIO_TOWER),
		NWidget(WWT_TEXTBTN, COLOUR_DARK_GREEN, WID_MA_PIECE_13), SetFill(0, 1), SetToolTip(STR_STATION_BUILD_MODULAR_AIRPORT_PIECE_GRASS),
		NWidget(WWT_TEXTBTN, COLOUR_DARK_GREEN, WID_MA_PIECE_14), SetFill(0, 1), SetToolTip(STR_STATION_BUILD_MODULAR_AIRPORT_PIECE_EMPTY),
		NWidget(WWT_TEXTBTN, COLOUR_DARK_GREEN, WID_MA_PIECE_15), SetFill(0, 1), SetToolTip(STR_STATION_BUILD_MODULAR_AIRPORT_PIECE_ERASE),
		NWidget(WWT_PANEL, COLOUR_DARK_GREEN), SetFill(1, 1), EndContainer(),
		NWidget(WWT_TEXTBTN, COLOUR_DARK_GREEN, WID_MA_TOGGLE_SHOW_ARROWS), SetFill(0, 1), SetStringTip(STR_STATION_BUILD_MODULAR_AIRPORT_TOGGLE_SHOW_ARROWS, STR_STATION_BUILD_MODULAR_AIRPORT_TOGGLE_SHOW_ARROWS),
	EndContainer(),
};

static WindowDesc _build_modular_airport_desc(
	WDP_AUTO, {}, 0, 0,
	WC_BUILD_STATION, WC_BUILD_TOOLBAR,
	WindowDefaultFlag::Construction,
	_nested_build_modular_airport_widgets
);

static constexpr std::initializer_list<NWidgetPart> _nested_build_airport_widgets = {
	NWidget(NWID_HORIZONTAL),
		NWidget(WWT_CLOSEBOX, COLOUR_DARK_GREEN),
		NWidget(WWT_CAPTION, COLOUR_DARK_GREEN), SetStringTip(STR_STATION_BUILD_AIRPORT_CAPTION, STR_TOOLTIP_WINDOW_TITLE_DRAG_THIS),
	EndContainer(),
	NWidget(WWT_PANEL, COLOUR_DARK_GREEN),
		NWidget(NWID_VERTICAL), SetPIP(0, WidgetDimensions::unscaled.vsep_normal, 0), SetPadding(WidgetDimensions::unscaled.picker),
			NWidget(NWID_VERTICAL), SetPIP(0, WidgetDimensions::unscaled.vsep_picker, 0),
				NWidget(WWT_LABEL, INVALID_COLOUR), SetStringTip(STR_STATION_BUILD_AIRPORT_CLASS_LABEL), SetFill(1, 0),
				NWidget(WWT_DROPDOWN, COLOUR_GREY, WID_AP_CLASS_DROPDOWN), SetFill(1, 0), SetToolTip(STR_STATION_BUILD_AIRPORT_TOOLTIP),
				NWidget(WWT_EMPTY, INVALID_COLOUR, WID_AP_AIRPORT_SPRITE), SetFill(1, 0),
				NWidget(NWID_HORIZONTAL),
					NWidget(WWT_MATRIX, COLOUR_GREY, WID_AP_AIRPORT_LIST), SetFill(1, 0), SetMatrixDataTip(1, 5, STR_STATION_BUILD_AIRPORT_TOOLTIP), SetScrollbar(WID_AP_SCROLLBAR),
					NWidget(NWID_VSCROLLBAR, COLOUR_GREY, WID_AP_SCROLLBAR),
				EndContainer(),
				NWidget(WWT_LABEL, INVALID_COLOUR), SetStringTip(STR_STATION_BUILD_ORIENTATION), SetFill(1, 0),
				NWidget(NWID_HORIZONTAL),
					NWidget(WWT_PUSHARROWBTN, COLOUR_GREY, WID_AP_LAYOUT_DECREASE), SetMinimalSize(12, 0), SetArrowWidgetTypeTip(AWV_DECREASE),
					NWidget(WWT_LABEL, INVALID_COLOUR, WID_AP_LAYOUT_NUM), SetResize(1, 0), SetFill(1, 0),
					NWidget(WWT_PUSHARROWBTN, COLOUR_GREY, WID_AP_LAYOUT_INCREASE), SetMinimalSize(12, 0), SetArrowWidgetTypeTip(AWV_INCREASE),
				EndContainer(),
				NWidget(WWT_EMPTY, INVALID_COLOUR, WID_AP_EXTRA_TEXT), SetFill(1, 0), SetMinimalSize(150, 0),
				NWidget(WWT_LABEL, INVALID_COLOUR), SetStringTip(STR_STATION_BUILD_COVERAGE_AREA_TITLE), SetFill(1, 0),
				NWidget(NWID_HORIZONTAL), SetPIP(14, 0, 14), SetPIPRatio(1, 0, 1),
					NWidget(NWID_HORIZONTAL, NWidContainerFlag::EqualSize),
						NWidget(WWT_TEXTBTN, COLOUR_GREY, WID_AP_BTN_DONTHILIGHT), SetMinimalSize(60, 12), SetFill(1, 0),
													SetStringTip(STR_STATION_BUILD_COVERAGE_OFF, STR_STATION_BUILD_COVERAGE_AREA_OFF_TOOLTIP),
						NWidget(WWT_TEXTBTN, COLOUR_GREY, WID_AP_BTN_DOHILIGHT), SetMinimalSize(60, 12), SetFill(1, 0),
													SetStringTip(STR_STATION_BUILD_COVERAGE_ON, STR_STATION_BUILD_COVERAGE_AREA_ON_TOOLTIP),
					EndContainer(),
				EndContainer(),
			EndContainer(),
			NWidget(WWT_EMPTY, INVALID_COLOUR, WID_AP_ACCEPTANCE), SetResize(0, 1), SetFill(1, 0), SetMinimalTextLines(2, WidgetDimensions::unscaled.vsep_normal),
		EndContainer(),
	EndContainer(),
};

static WindowDesc _build_airport_desc(
	WDP_AUTO, {}, 0, 0,
	WC_BUILD_STATION, WC_BUILD_TOOLBAR,
	WindowDefaultFlag::Construction,
	_nested_build_airport_widgets
);

static void ShowBuildAirportPicker(Window *parent)
{
	new BuildAirportWindow(_build_airport_desc, parent);
}

static void ShowBuildModularAirportWindow(Window *parent)
{
	CloseWindowById(WC_BUILD_STATION, WN_BUILD_MODULAR_AIRPORT);
	new BuildModularAirportWindow(_build_modular_airport_desc, parent);
}

void InitializeAirportGui()
{
	_selected_airport_class = APC_BEGIN;
	_selected_airport_index = -1;
}
