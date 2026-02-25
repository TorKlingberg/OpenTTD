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
#include "newgrf_airport.h"

#include <vector>

extern int _selected_airport_template_index;
extern uint8_t _selected_airport_template_rotation;
extern std::vector<Point> _saved_template_preview_offsets;
extern bool _saved_template_preview_active;
extern const AirportClassID APC_SAVED_CUSTOM;

bool IsSavedTemplateClassSelected(AirportClassID selected_airport_class);
const AirportTemplate *GetSelectedAirportTemplate();
void UpdateSavedTemplatePreviewCache(uint8_t rotation);
void ResetSavedTemplateGuiState();
bool ShouldDrawSavedTemplatePreviewAtTileInternal(TileIndex tile, AirportClassID selected_airport_class);

#endif /* AIRPORT_TEMPLATE_GUI_H */
