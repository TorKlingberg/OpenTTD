/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file airport_widget.h Types related to the airport widgets. */

#ifndef WIDGETS_AIRPORT_WIDGET_H
#define WIDGETS_AIRPORT_WIDGET_H

/** Widgets of the #BuildAirToolbarWindow class. */
enum AirportToolbarWidgets : WidgetID {
	WID_AT_AIRPORT,  ///< Build airport button.
	WID_AT_DEMOLISH, ///< Demolish button.
	WID_AT_MODULAR,  ///< Modular airport builder button (prototype).
};

/** Widgets of the #BuildAirportWindow class. */
enum AirportPickerWidgets : WidgetID {
	WID_AP_CLASS_DROPDOWN,  ///< Dropdown of airport classes.
	WID_AP_AIRPORT_LIST,    ///< List of airports.
	WID_AP_SCROLLBAR,       ///< Scrollbar of the list.
	WID_AP_LAYOUT_NUM,      ///< Current number of the layout.
	WID_AP_LAYOUT_DECREASE, ///< Decrease the layout number.
	WID_AP_LAYOUT_INCREASE, ///< Increase the layout number.
	WID_AP_AIRPORT_SPRITE,  ///< A visual display of the airport currently selected.
	WID_AP_EXTRA_TEXT,      ///< Additional text about the airport.
	WID_AP_COVERAGE_LABEL,  ///< Label if you want to see the coverage.
	WID_AP_BTN_DONTHILIGHT, ///< Don't show the coverage button.
	WID_AP_BTN_DOHILIGHT,   ///< Show the coverage button.
	WID_AP_ACCEPTANCE,             ///< Acceptance info.
	WID_AP_BTN_NOTMODULAR,         ///< Don't build as modular button.
	WID_AP_BTN_MODULAR,            ///< Build as modular button.
};

/** Widgets of the #BuildModularAirportWindow class (prototype). */
enum ModularAirportBuilderWidgets : WidgetID {
	WID_MA_PIECE_0,   ///< Runway piece button.
	WID_MA_PIECE_1,   ///< Runway end piece button.
	WID_MA_PIECE_2,   ///< Small runway piece button.
	WID_MA_PIECE_3,   ///< Cosmetic tiles (opens cosmetic picker).
	WID_MA_PIECE_4,   ///< Large hangar piece button (opens direction picker).
	WID_MA_PIECE_5,   ///< Small hangar piece button (opens direction picker).
	WID_MA_PIECE_6,   ///< Helipad piece button.
	WID_MA_PIECE_7,   ///< Stand piece button.
	WID_MA_PIECE_8,   ///< Apron piece button.
	WID_MA_PIECE_9,   ///< Grass piece button.
	WID_MA_PIECE_10,  ///< Empty piece button.
	WID_MA_PIECE_11,  ///< Erase piece button.
	WID_MA_TOGGLE_SHOW_ARROWS,   ///< Show runway direction arrows toggle.
	WID_MA_TOGGLE_SHOW_HOLDING,  ///< Show holding loop overlay toggle.
};

static constexpr WidgetID WID_MA_PIECE_FIRST = WID_MA_PIECE_0;
static constexpr WidgetID WID_MA_PIECE_LAST  = WID_MA_PIECE_11;

/** Widgets of the #BuildModularHangarPickerWindow class. */
enum ModularAirportHangarPickerWidgets : WidgetID {
	WID_MAHP_CAPTION,   ///< Caption.
	WID_MAHP_DIR_NW,    ///< NW hangar direction button.
	WID_MAHP_DIR_NE,    ///< NE hangar direction button.
	WID_MAHP_DIR_SW,    ///< SW hangar direction button.
	WID_MAHP_DIR_SE,    ///< SE hangar direction button.
};

/** Widgets of the #BuildModularCosmeticPickerWindow class. */
enum ModularAirportCosmeticPickerWidgets : WidgetID {
	WID_MACP_CAPTION,
	WID_MACP_PIECE_0,  ///< Terminal button.
	WID_MACP_PIECE_1,  ///< Terminal (alternate orientation) button.
	WID_MACP_PIECE_2,  ///< Terminal (other) button.
	WID_MACP_PIECE_3,  ///< Round terminal button.
	WID_MACP_PIECE_4,  ///< Low terminal button.
	WID_MACP_PIECE_5,  ///< Tower button.
	WID_MACP_PIECE_6,  ///< Radar button.
	WID_MACP_PIECE_7,  ///< Radar on grass button.
	WID_MACP_PIECE_8,  ///< Radio tower button.
	WID_MACP_PIECE_9,  ///< Flag on grass button.
};

static constexpr WidgetID WID_MACP_PIECE_FIRST = WID_MACP_PIECE_0;
static constexpr WidgetID WID_MACP_PIECE_LAST  = WID_MACP_PIECE_9;

#endif /* WIDGETS_AIRPORT_WIDGET_H */
