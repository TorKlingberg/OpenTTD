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
	WID_AP_ACCEPTANCE,      ///< Acceptance info.
};

/** Widgets of the #BuildModularAirportWindow class (prototype). */
enum ModularAirportBuilderWidgets : WidgetID {
	WID_MA_PIECES_LABEL,        ///< Label for pieces list.
	WID_MA_PIECES,              ///< List of modular pieces.
	WID_MA_PREVIEW_LABEL,       ///< Label for preview.
	WID_MA_CANVAS,              ///< Preview canvas.
	WID_MA_ROTATE_LABEL,        ///< Label for rotation.
	WID_MA_ROTATE_DECREASE,     ///< Rotate left.
	WID_MA_ROTATE_INCREASE,     ///< Rotate right.
	WID_MA_TAXI_DIR_LABEL,      ///< Label for taxi direction.
	WID_MA_TAXI_DIR_N,          ///< Taxi direction north.
	WID_MA_TAXI_DIR_E,          ///< Taxi direction east.
	WID_MA_TAXI_DIR_S,          ///< Taxi direction south.
	WID_MA_TAXI_DIR_W,          ///< Taxi direction west.
	WID_MA_TOGGLE_ONEWAY,       ///< One-way taxi toggle.
	WID_MA_TOGGLE_SHOW_ARROWS,  ///< Show taxi arrows toggle.
	WID_MA_TOGGLE_SNAP,         ///< Snap to grid toggle.
	WID_MA_STATUS,              ///< Status text.
	WID_MA_VALIDATE,            ///< Validate button.
	WID_MA_BUILD,               ///< Build button (disabled).
};

#endif /* WIDGETS_AIRPORT_WIDGET_H */
