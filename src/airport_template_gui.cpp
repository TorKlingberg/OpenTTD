/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file airport_template_gui.cpp Shared GUI state/helpers for saved airport templates. */

#include "stdafx.h"

#include "airport_template_gui.h"
#include "map_func.h"
#include "transport_type.h"
#include "tilehighlight_func.h"
#include "window_type.h"

int _selected_airport_template_index = -1;
uint8_t _selected_airport_template_rotation = 0;
std::vector<Point> _saved_template_preview_offsets;
bool _saved_template_preview_active = false;

const AirportClassID APC_SAVED_CUSTOM = static_cast<AirportClassID>(APC_MAX);

bool IsSavedTemplateClassSelected(AirportClassID selected_airport_class)
{
	return selected_airport_class == APC_SAVED_CUSTOM;
}

const AirportTemplate *GetSelectedAirportTemplate()
{
	const auto &templates = AirportTemplateManager::GetTemplates();
	if (_selected_airport_template_index < 0 || static_cast<size_t>(_selected_airport_template_index) >= templates.size()) return nullptr;
	return templates[_selected_airport_template_index].get();
}

void UpdateSavedTemplatePreviewCache(uint8_t rotation)
{
	_saved_template_preview_offsets.clear();
	_saved_template_preview_active = false;

	const AirportTemplate *templ = GetSelectedAirportTemplate();
	if (templ == nullptr || !templ->is_available) return;

	for (const AirportTemplateTile &src : templ->tiles) {
		AirportTemplateTile t = src;
		t.Rotate(rotation, templ->width, templ->height);
		_saved_template_preview_offsets.push_back({static_cast<int>(t.dx), static_cast<int>(t.dy)});
	}
	_saved_template_preview_active = !_saved_template_preview_offsets.empty();
}

void ResetSavedTemplateGuiState()
{
	_selected_airport_template_index = -1;
	_selected_airport_template_rotation = 0;
	_saved_template_preview_active = false;
	_saved_template_preview_offsets.clear();
}

bool ShouldDrawSavedTemplatePreviewAtTileInternal(TileIndex tile, AirportClassID selected_airport_class)
{
	if (!_saved_template_preview_active) return false;
	if (!IsSavedTemplateClassSelected(selected_airport_class)) return false;
	if ((_thd.drawstyle & HT_DRAG_MASK) == HT_NONE) return false;
	if (_thd.window_class != WC_BUILD_TOOLBAR || _thd.window_number != TRANSPORT_AIR) return false;

	TileIndex anchor = TileVirtXY(_thd.pos.x, _thd.pos.y);
	int dx = TileX(tile) - TileX(anchor);
	int dy = TileY(tile) - TileY(anchor);
	for (const Point &p : _saved_template_preview_offsets) {
		if (p.x == dx && p.y == dy) return true;
	}
	return false;
}
