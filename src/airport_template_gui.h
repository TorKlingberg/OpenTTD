/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file airport_template_gui.h Shared GUI state/helpers for saved airport templates. */

#ifndef AIRPORT_TEMPLATE_GUI_H
#define AIRPORT_TEMPLATE_GUI_H

#include "airport.h"
#include "core/geometry_type.hpp"
#include "tile_type.h"
#include "airport_template.h"

#include <vector>

struct Window;

extern std::vector<Point> _saved_template_preview_offsets;
extern bool _saved_template_preview_active;

const AirportTemplate *GetAirportTemplateByIndex(int index);
void UpdateSavedTemplatePreviewCache(const AirportTemplate *templ, uint8_t rotation);
void ResetSavedTemplateGuiState();
bool ShouldDrawSavedTemplatePreviewAtTileInternal(TileIndex tile);

void ShowBuildAirportTemplateManagerWindow(Window *parent);

#endif /* AIRPORT_TEMPLATE_GUI_H */
