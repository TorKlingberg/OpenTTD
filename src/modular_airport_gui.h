/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file modular_airport_gui.h Declarations for modular airport build UI. */

#ifndef MODULAR_AIRPORT_GUI_H
#define MODULAR_AIRPORT_GUI_H

#include "station_type.h"
#include "viewport_type.h"
#include "gfx_type.h"

struct Window;

void ShowBuildModularAirportWindow(Window *parent);

extern StationID _last_modular_airport_station;
extern bool _show_runway_direction_overlay;
extern bool _show_holding_overlay;

void DrawModularHoldingOverlay(const Viewport &vp, DrawPixelInfo *dpi);

#endif /* MODULAR_AIRPORT_GUI_H */
