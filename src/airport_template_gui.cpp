/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file airport_template_gui.cpp GUI window for saved modular airport templates. */

#include "stdafx.h"

#include "airport_template_gui.h"
#include "window_gui.h"
#include "station_gui.h"
#include "textbuf_gui.h"
#include "command_func.h"
#include "station_cmd.h"
#include "map_func.h"
#include "tile_map.h"
#include "tilehighlight_func.h"
#include "viewport_func.h"
#include "strings_func.h"
#include "window_func.h"
#include "sound_func.h"
#include "error.h"
#include "company_func.h"
#include "station_base.h"
#include "station_type.h"
#include "station_map.h"
#include "newgrf_airporttiles.h"
#include "modular_airport_cmd.h"
#include "modular_airport_gui.h"
#include "hotkeys.h"
#include "core/geometry_func.hpp"
#include "station_func.h"
#include "sprite.h"
#include "spritecache.h"
#include "airport.h"
#include "core/backup_type.hpp"
#include "zoom_func.h"
#include "landscape.h"

#include <cctype>

#include "widgets/airport_widget.h"

#include "table/strings.h"

#include "safeguards.h"

std::vector<Point> _saved_template_preview_offsets;
bool _saved_template_preview_active = false;

static constexpr uint16_t MAX_TEMPLATE_TILES = 128;
void CcBuildAirport(Commands, const CommandCost &result, TileIndex tile);

const AirportTemplate *GetAirportTemplateByIndex(int index)
{
	const auto &templates = AirportTemplateManager::GetTemplates();
	if (index < 0 || static_cast<size_t>(index) >= templates.size()) return nullptr;
	return templates[index].get();
}

void UpdateSavedTemplatePreviewCache(const AirportTemplate *templ, uint8_t rotation)
{
	_saved_template_preview_offsets.clear();
	_saved_template_preview_active = false;

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
	_saved_template_preview_active = false;
	_saved_template_preview_offsets.clear();
}

bool ShouldDrawSavedTemplatePreviewAtTileInternal(TileIndex tile)
{
	if (!_saved_template_preview_active) return false;
	if ((_thd.drawstyle & HT_DRAG_MASK) == HT_NONE) return false;

	TileIndex anchor = TileVirtXY(_thd.pos.x, _thd.pos.y);
	int dx = TileX(tile) - TileX(anchor);
	int dy = TileY(tile) - TileY(anchor);
	for (const Point &p : _saved_template_preview_offsets) {
		if (p.x == dx && p.y == dy) return true;
	}
	return false;
}

static bool BuildTemplateFromStation(const Station *st, AirportTemplate &templ)
{
	if (st == nullptr || !st->airport.blocks.Test(AirportBlock::Modular)) return false;
	if (st->airport.modular_tile_data == nullptr || st->airport.modular_tile_data->empty()) return false;

	uint min_x = UINT_MAX;
	uint min_y = UINT_MAX;
	uint max_x = 0;
	uint max_y = 0;

	for (const ModularAirportTileData &md : *st->airport.modular_tile_data) {
		min_x = std::min<uint>(min_x, TileX(md.tile));
		min_y = std::min<uint>(min_y, TileY(md.tile));
		max_x = std::max<uint>(max_x, TileX(md.tile));
		max_y = std::max<uint>(max_y, TileY(md.tile));
	}

	templ.width = ClampTo<uint16_t>(max_x - min_x + 1);
	templ.height = ClampTo<uint16_t>(max_y - min_y + 1);
	templ.tiles.clear();
	templ.tiles.reserve(std::min<size_t>(st->airport.modular_tile_data->size(), MAX_TEMPLATE_TILES));

	for (const ModularAirportTileData &md : *st->airport.modular_tile_data) {
		if (templ.tiles.size() >= MAX_TEMPLATE_TILES) return false;

		AirportTemplateTile tile;
		tile.dx = ClampTo<uint16_t>(TileX(md.tile) - min_x);
		tile.dy = ClampTo<uint16_t>(TileY(md.tile) - min_y);
		tile.piece_type = md.piece_type;
		tile.rotation = md.rotation;
		tile.runway_flags = IsModularRunwayPiece(md.piece_type) ? md.runway_flags : 0;
		tile.one_way_taxi = md.one_way_taxi;
		tile.user_taxi_dir_mask = md.user_taxi_dir_mask;
		tile.edge_block_mask = md.edge_block_mask;
		tile.grfid = 0;
		tile.local_id = 0;

		if (tile.piece_type >= NEW_AIRPORTTILE_OFFSET) {
			const AirportTileSpec *ats = AirportTileSpec::Get(tile.piece_type);
			if (ats != nullptr) {
				tile.grfid = ats->grf_prop.grfid;
				tile.local_id = ats->grf_prop.local_id;
			}
		}

		templ.tiles.push_back(tile);
	}

	std::sort(templ.tiles.begin(), templ.tiles.end(), [](const AirportTemplateTile &a, const AirportTemplateTile &b) {
		if (a.dy != b.dy) return a.dy < b.dy;
		return a.dx < b.dx;
	});

	templ.CheckAvailability();
	return true;
}

/**
 * Get the tile layout for a template tile, suitable for GUI drawing.
 * Reuses airport/modular layout override logic from station drawing.
 * NewGRF tiles fall back to plain apron in preview.
 */
static const DrawTileSprites *GetTileLayoutForTemplateTile(const AirportTemplateTile &t)
{
	uint8_t gfx = t.piece_type >= NEW_AIRPORTTILE_OFFSET ? APT_APRON : t.piece_type;
	return GetAirportTileLayoutWithModularOverrides(gfx, t.piece_type, t.rotation, 0);
}

static void DrawTileLayoutInGUIZoom(int x, int y, const DrawTileSprites *layout, PaletteID pal, ZoomLevel zoom)
{
	Point child_offset = {0, 0};
	bool skip_childs = false;

	DrawSprite(layout->ground.sprite, HasBit(layout->ground.sprite, PALETTE_MODIFIER_COLOUR) ? pal : PAL_NONE, x, y, nullptr, zoom);
	for (const DrawTileSeqStruct &dtss : layout->GetSequence()) {
		SpriteID image = dtss.image.sprite;
		PaletteID seq_pal = dtss.image.pal;

		if (skip_childs) {
			if (!dtss.IsParentSprite()) continue;
			skip_childs = false;
		}

		if (GB(image, 0, SPRITE_WIDTH) == 0 && !HasBit(image, SPRITE_MODIFIER_CUSTOM_SPRITE)) {
			skip_childs = dtss.IsParentSprite();
			continue;
		}

		seq_pal = SpriteLayoutPaletteTransform(image, seq_pal, pal);
		if (dtss.IsParentSprite()) {
			Point pt = RemapCoords(dtss.origin.x, dtss.origin.y, dtss.origin.z);
			DrawSprite(image, seq_pal, x + UnScaleByZoom(pt.x, zoom), y + UnScaleByZoom(pt.y, zoom), nullptr, zoom);
			const Sprite *spr = GetSprite(image & SPRITE_MASK, SpriteType::Normal);
			child_offset.x = UnScaleByZoom(pt.x + spr->x_offs, zoom);
			child_offset.y = UnScaleByZoom(pt.y + spr->y_offs, zoom);
		} else {
			DrawSprite(image, seq_pal,
					x + child_offset.x + UnScaleByZoom(dtss.origin.x * ZOOM_BASE, zoom),
					y + child_offset.y + UnScaleByZoom(dtss.origin.y * ZOOM_BASE, zoom),
					nullptr, zoom);
		}
	}
}

static void NormalizePreviewSmallRunwayEnds(std::vector<AirportTemplateTile> &tiles)
{
	/* Preview must match build-time runway end normalization:
	 * low end of a small-runway segment = FAR, high end = NEAR.
	 * Rotate() swaps near/far for odd quarter-turns, then we normalize by segment
	 * so 0-degree and 180-degree previews stay consistent with placed tiles. */
	auto is_small_runway_piece = [](uint8_t piece_type) {
		return piece_type == APT_RUNWAY_SMALL_NEAR_END || piece_type == APT_RUNWAY_SMALL_MIDDLE || piece_type == APT_RUNWAY_SMALL_FAR_END;
	};

	std::vector<bool> visited(tiles.size(), false);
	for (size_t i = 0; i < tiles.size(); i++) {
		if (visited[i] || !is_small_runway_piece(tiles[i].piece_type)) continue;

		const bool horizontal = (tiles[i].rotation % 2) == 0;
		std::vector<size_t> segment;
		segment.reserve(8);
		segment.push_back(i);
		visited[i] = true;

		for (size_t j = 0; j < tiles.size(); j++) {
			if (visited[j] || !is_small_runway_piece(tiles[j].piece_type)) continue;
			if (((tiles[j].rotation % 2) == 0) != horizontal) continue;

			bool adjacent = false;
			for (size_t k : segment) {
				const int dx = static_cast<int>(tiles[j].dx) - static_cast<int>(tiles[k].dx);
				const int dy = static_cast<int>(tiles[j].dy) - static_cast<int>(tiles[k].dy);
				if ((horizontal && std::abs(dx) == 1 && dy == 0) || (!horizontal && std::abs(dy) == 1 && dx == 0)) {
					adjacent = true;
					break;
				}
			}
			if (adjacent) {
				segment.push_back(j);
				visited[j] = true;
				j = static_cast<size_t>(-1);
			}
		}

		std::sort(segment.begin(), segment.end(), [&](size_t a, size_t b) {
			if (horizontal) return tiles[a].dx < tiles[b].dx;
			return tiles[a].dy < tiles[b].dy;
		});

		if (segment.size() == 1) {
			tiles[segment[0]].piece_type = APT_RUNWAY_SMALL_NEAR_END;
		} else {
			tiles[segment.front()].piece_type = APT_RUNWAY_SMALL_FAR_END;
			tiles[segment.back()].piece_type = APT_RUNWAY_SMALL_NEAR_END;
			for (size_t s = 1; s + 1 < segment.size(); s++) {
				tiles[segment[s]].piece_type = APT_RUNWAY_SMALL_MIDDLE;
			}
		}
	}
}

enum TemplateManagerHotkeys {
	TMHK_ROTATE_LEFT = 1,
	TMHK_ROTATE_RIGHT,
};

class BuildModularTemplateManagerWindow : public PickerWindowBase {
	Scrollbar *vscroll = nullptr;
	int line_height = 0;
	int selected_template_index = -1;
	uint8_t selected_rotation = 0;
	bool updating_cursor = false;
	TileIndex save_pick_tile = INVALID_TILE;
	bool has_save_pick_tile = false;
	std::string pending_delete_stem;

	enum class TemplateManagerMode : uint8_t {
		None,
		SavingPickAirport,
		LoadingPlace,
	};
	TemplateManagerMode mode = TemplateManagerMode::None;

	void ClampSelection()
	{
		const auto &templates = AirportTemplateManager::GetTemplates();
		if (templates.empty()) {
			this->selected_template_index = -1;
			return;
		}
		this->selected_template_index = Clamp(this->selected_template_index, 0, static_cast<int>(templates.size()) - 1);
	}

	void RefreshTemplateList(std::optional<std::string> preferred_stem = std::nullopt, int deleted_index = -1)
	{
		AirportTemplateManager::Refresh();
		const auto &templates = AirportTemplateManager::GetTemplates();
		this->vscroll->SetCount(templates.size());

		if (templates.empty()) {
			this->selected_template_index = -1;
		} else if (preferred_stem.has_value()) {
			int found = -1;
			for (size_t i = 0; i < templates.size(); i++) {
				if (templates[i]->file_stem == preferred_stem.value()) {
					found = static_cast<int>(i);
					break;
				}
			}
			if (found >= 0) {
				this->selected_template_index = found;
			} else if (deleted_index >= 0) {
				this->selected_template_index = std::min<int>(deleted_index, static_cast<int>(templates.size()) - 1);
			} else {
				this->ClampSelection();
			}
		} else if (deleted_index >= 0) {
			this->selected_template_index = std::min<int>(deleted_index, static_cast<int>(templates.size()) - 1);
		} else {
			if (this->selected_template_index < 0) this->selected_template_index = 0;
			this->ClampSelection();
		}

		this->UpdateRotationButtons();
		if (this->mode == TemplateManagerMode::LoadingPlace) this->UpdateLoadingPlacementPreview();
		this->SetDirty();
	}

	void UpdateLoadingPlacementPreview()
	{
		if (this->mode != TemplateManagerMode::LoadingPlace) {
			ResetSavedTemplateGuiState();
			SetTileSelectSize(1, 1);
			return;
		}

		const AirportTemplate *templ = GetAirportTemplateByIndex(this->selected_template_index);
		if (templ == nullptr || !templ->is_available) {
			ResetSavedTemplateGuiState();
			SetTileSelectSize(1, 1);
			return;
		}

		uint16_t w, h;
		templ->GetRotatedDimensions(this->selected_rotation, w, h);
		SetTileSelectSize(w, h);
		UpdateSavedTemplatePreviewCache(templ, this->selected_rotation);
	}

	void UpdateCursor()
	{
		this->updating_cursor = true;
		if (this->mode == TemplateManagerMode::None) {
			ResetSavedTemplateGuiState();
			if (_thd.window_class == this->window_class && _thd.window_number == this->window_number) {
				ResetObjectToPlace();
			}
		} else {
			SetObjectToPlaceWnd(SPR_CURSOR_AIRPORT, PAL_NONE, HT_RECT, this);
			if (this->mode == TemplateManagerMode::LoadingPlace) {
				/* SetObjectToPlaceWnd() can reset size to 1x1; re-apply template footprint after taking cursor ownership. */
				this->UpdateLoadingPlacementPreview();
			} else {
				SetTileSelectSize(1, 1);
				ResetSavedTemplateGuiState();
			}
		}
		this->SetWidgetLoweredState(WID_TM_SAVE, this->mode == TemplateManagerMode::SavingPickAirport);
		this->SetWidgetLoweredState(WID_TM_LOAD, this->mode == TemplateManagerMode::LoadingPlace);
		this->updating_cursor = false;
	}

	bool RotateSelection(int delta)
	{
		if (this->mode != TemplateManagerMode::LoadingPlace) return false;
		const AirportTemplate *templ = GetAirportTemplateByIndex(this->selected_template_index);
		if (templ == nullptr || !templ->is_available) return false;
		if (templ->HasNonRotatablePieces()) return false;
		if (templ->HasLegacySmallRunwayPieces()) {
			this->selected_rotation ^= 2;
			this->UpdateLoadingPlacementPreview();
			this->SetDirty();
			return true;
		}

		this->selected_rotation = (this->selected_rotation + delta + 4) & 3;
		this->UpdateLoadingPlacementPreview();
		this->SetDirty();
		return true;
	}

	/** Update disabled state of rotation arrow buttons based on selected template. */
	void UpdateRotationButtons()
	{
		const AirportTemplate *templ = GetAirportTemplateByIndex(this->selected_template_index);
		bool disable = (templ != nullptr && templ->HasNonRotatablePieces());
		this->SetWidgetDisabledState(WID_TM_ROTATE_LEFT, disable);
		this->SetWidgetDisabledState(WID_TM_ROTATE_RIGHT, disable);
		if (disable) this->selected_rotation = 0;
		if (!disable && templ != nullptr && templ->HasLegacySmallRunwayPieces() && (this->selected_rotation & 1) != 0) {
			this->selected_rotation &= 2;
		}
	}

	void StartSavePickMode()
	{
		this->mode = TemplateManagerMode::SavingPickAirport;
		this->UpdateCursor();
		this->SetDirty();
	}

	void StartLoadPlaceMode()
	{
		const AirportTemplate *templ = GetAirportTemplateByIndex(this->selected_template_index);
		if (templ == nullptr) return;
		if (!templ->is_available) {
			ShowErrorMessage(GetEncodedString(STR_ERROR_AIRPORT_TEMPLATE_NEWGRF_MISSING), {}, WL_INFO);
			return;
		}
		this->mode = TemplateManagerMode::LoadingPlace;
		this->UpdateCursor();
		this->SetDirty();
	}

	void ExitPlacementMode()
	{
		this->mode = TemplateManagerMode::None;
		this->UpdateCursor();
		this->SetDirty();
	}

	bool DeleteSelectedTemplate()
	{
		if (this->pending_delete_stem.empty()) return false;

		int deleted_index = this->selected_template_index;
		if (!AirportTemplateManager::DeleteTemplateByFileStem(this->pending_delete_stem)) {
			ShowErrorMessage(GetEncodedString(STR_ERROR_AIRPORT_TEMPLATE_DELETE_FAILED), {}, WL_INFO);
			return false;
		}

		this->pending_delete_stem.clear();
		this->RefreshTemplateList(std::nullopt, deleted_index);
		return true;
	}

	static void DeleteTemplateConfirmationCallback(Window *w, bool confirmed)
	{
		auto *self = dynamic_cast<BuildModularTemplateManagerWindow *>(w);
		if (self == nullptr || !confirmed) return;
		self->DeleteSelectedTemplate();
	}

public:
	BuildModularTemplateManagerWindow(WindowDesc &desc, Window *parent) : PickerWindowBase(desc, parent)
	{
		this->CreateNestedTree();
		this->vscroll = this->GetScrollbar(WID_TM_SCROLLBAR);
		this->vscroll->SetCapacity(6);
		this->FinishInitNested(0);
		this->RefreshTemplateList();
		this->UpdateCursor();
	}

	void Close([[maybe_unused]] int data = 0) override
	{
		this->mode = TemplateManagerMode::None;
		this->UpdateCursor();
		CloseWindowById(WC_SELECT_STATION, 0);
		this->PickerWindowBase::Close();
	}

	void UpdateWidgetSize(WidgetID widget, Dimension &size, [[maybe_unused]] const Dimension &padding, [[maybe_unused]] Dimension &fill, [[maybe_unused]] Dimension &resize) override
	{
		switch (widget) {
			case WID_TM_PREVIEW:
				size.width  = std::max<uint>(size.width, ScaleGUITrad(150));
				size.height = std::max<uint>(size.height, ScaleGUITrad(100));
				break;

			case WID_TM_TEMPLATE_LIST:
				for (const auto &templ : AirportTemplateManager::GetTemplates()) {
					if (templ == nullptr) continue;
					std::string label = templ->name;
					if (!templ->is_available) label += " (NewGRF missing)";
					size.width = std::max(size.width, GetStringBoundingBox(label).width + padding.width);
				}
				this->line_height = GetCharacterHeight(FS_NORMAL) + padding.height;
				size.height = 6 * this->line_height;
				break;

			case WID_TM_ROTATION:
				size = maxdim(size, GetStringBoundingBox(GetString(STR_STATION_BUILD_MODULAR_AIRPORT_TEMPLATE_ROTATION_LABEL, 270)));
				size = maxdim(size, GetStringBoundingBox(GetString(STR_STATION_BUILD_MODULAR_AIRPORT_TEMPLATE_ROTATION_LOCKED)));
				size.width += padding.width + 20;
				size.height += padding.height;
				break;

			case WID_TM_INFO:
				size = maxdim(size, GetStringBoundingBox(GetString(STR_OBJECT_BUILD_SIZE, 99, 99)));
				size.height += padding.height;
				break;

			default: break;
		}
	}

	std::string GetWidgetString(WidgetID widget, StringID stringid) const override
	{
		switch (widget) {
			case WID_TM_ROTATION: {
				const AirportTemplate *templ = GetAirportTemplateByIndex(this->selected_template_index);
				if (templ != nullptr && templ->HasNonRotatablePieces()) {
					return GetString(STR_STATION_BUILD_MODULAR_AIRPORT_TEMPLATE_ROTATION_LOCKED);
				}
				return GetString(STR_STATION_BUILD_MODULAR_AIRPORT_TEMPLATE_ROTATION_LABEL, static_cast<uint>(this->selected_rotation) * 90);
			}

			default:
				return this->Window::GetWidgetString(widget, stringid);
		}
	}

	void DrawWidget(const Rect &r, WidgetID widget) const override
	{
		switch (widget) {
			case WID_TM_PREVIEW: {
				const AirportTemplate *templ = GetAirportTemplateByIndex(this->selected_template_index);
				if (templ == nullptr || templ->tiles.empty()) break;

				/* Build rotated tile list. */
				std::vector<AirportTemplateTile> tiles;
				tiles.reserve(templ->tiles.size());
				for (const AirportTemplateTile &src : templ->tiles) {
					AirportTemplateTile t = src;
					t.Rotate(this->selected_rotation, templ->width, templ->height);
					tiles.push_back(t);
				}
				NormalizePreviewSmallRunwayEnds(tiles);

				struct IsoTile { int iso_x; int iso_y; size_t idx; int depth; };

				/* Set up clipped draw area. */
				DrawPixelInfo tmp_dpi;
				Rect ir = r.Shrink(WidgetDimensions::scaled.bevel);
				if (!FillDrawPixelInfo(&tmp_dpi, ir)) break;
				AutoRestoreBackup dpi_backup(_cur_dpi, &tmp_dpi);

				/* Pick a zoom level that fits larger templates into the preview area. */
				ZoomLevel preview_zoom = _gui_zoom;
				Point so;
				Dimension sd;
				int tile_w = 0, tile_h = 0, half_w = 0, half_h = 0;
				std::vector<IsoTile> iso_tiles;
				int bb_left = 0, bb_right = 0, bb_top = 0, bb_bottom = 0;

				auto BuildIsoTilesForZoom = [&](ZoomLevel zoom) {
					so = {};
					sd = GetSpriteSize(SPR_FLAT_GRASS_TILE, &so, zoom);
					tile_w = static_cast<int>(sd.width) - so.x;
					tile_h = static_cast<int>(sd.height) - so.y;
					half_w = tile_w / 2;
					half_h = tile_h / 2;

					iso_tiles.clear();
					iso_tiles.reserve(tiles.size());
					bb_left = INT_MAX;
					bb_right = INT_MIN;
					bb_top = INT_MAX;
					bb_bottom = INT_MIN;
					for (size_t i = 0; i < tiles.size(); i++) {
						int dx = tiles[i].dx;
						int dy = tiles[i].dy;
						/* Match in-game isometric projection handedness.
						 * Using (dx - dy) here mirrors the whole preview. */
						int iso_x = (dy - dx) * half_w;
						int iso_y = (dx + dy) * half_h;
						iso_tiles.push_back({iso_x, iso_y, i, dx + dy});
						bb_left = std::min(bb_left, iso_x + so.x);
						bb_right = std::max(bb_right, iso_x + so.x + tile_w);
						bb_top = std::min(bb_top, iso_y + so.y);
						bb_bottom = std::max(bb_bottom, iso_y + so.y + tile_h);
					}
				};

				constexpr int PREVIEW_OVERSIZE_TOLERANCE = 16;
				BuildIsoTilesForZoom(preview_zoom);
				while (preview_zoom < ZoomLevel::Max &&
						((bb_right - bb_left) > (ir.Width() + PREVIEW_OVERSIZE_TOLERANCE) ||
						 (bb_bottom - bb_top) > (ir.Height() + PREVIEW_OVERSIZE_TOLERANCE))) {
					++preview_zoom;
					BuildIsoTilesForZoom(preview_zoom);
				}

				/* Centre the bounding box in the widget. */
				int off_x = (ir.Width()  - (bb_right + bb_left)) / 2;
				int off_y = (ir.Height() - (bb_bottom + bb_top)) / 2;

				/* Sort back-to-front (painter's algorithm). */
				std::sort(iso_tiles.begin(), iso_tiles.end(), [](const IsoTile &a, const IsoTile &b) {
					return a.depth < b.depth;
				});

				/* Draw each tile. */
				PaletteID pal = GetCompanyPalette(_local_company);
				for (const IsoTile &it : iso_tiles) {
					const AirportTemplateTile &t = tiles[it.idx];
					const DrawTileSprites *layout = GetTileLayoutForTemplateTile(t);
					int x = it.iso_x + off_x;
					int y = it.iso_y + off_y;
					DrawTileLayoutInGUIZoom(x, y, layout, pal, preview_zoom);
				}
				break;
			}

			case WID_TM_TEMPLATE_LIST: {
				Rect row = r.WithHeight(this->line_height).Shrink(WidgetDimensions::scaled.bevel);
				Rect text = r.WithHeight(this->line_height).Shrink(WidgetDimensions::scaled.matrix);
				const auto &templates = AirportTemplateManager::GetTemplates();
				auto [first, last] = this->vscroll->GetVisibleRangeIterators(templates);
				for (auto it = first; it != last; ++it) {
					const AirportTemplate *templ = it->get();
					std::string label = templ->name;
					TextColour tc = TC_BLACK;
					if (!templ->is_available) {
						label += " (NewGRF missing)";
						GfxFillRect(row, PC_BLACK, FILLRECT_CHECKER);
						tc = TC_RED;
					}
					if (static_cast<int>(std::distance(templates.begin(), it)) == this->selected_template_index) tc = TC_WHITE;
					DrawString(text, label, tc);
					row = row.Translate(0, this->line_height);
					text = text.Translate(0, this->line_height);
				}
				break;
			}

			case WID_TM_INFO: {
				const AirportTemplate *templ = GetAirportTemplateByIndex(this->selected_template_index);
				if (templ == nullptr) break;
				uint16_t w, h;
				templ->GetRotatedDimensions(this->selected_rotation, w, h);
				DrawString(r, GetString(STR_OBJECT_BUILD_SIZE, w, h), TC_BLACK);
				if (!templ->is_available) {
					Rect line2 = r.Translate(0, GetCharacterHeight(FS_NORMAL) + WidgetDimensions::scaled.vsep_normal);
					DrawString(line2, GetString(STR_ERROR_AIRPORT_TEMPLATE_NEWGRF_MISSING), TC_RED);
				}
				break;
			}

			default: break;
		}
	}

	void OnClick(Point pt, WidgetID widget, [[maybe_unused]] int click_count) override
	{
		switch (widget) {
			case WID_TM_TEMPLATE_LIST: {
				int32_t num_clicked = this->vscroll->GetScrolledRowFromWidget(pt.y, this, widget, 0, this->line_height);
				if (num_clicked == INT32_MAX) break;
				if (num_clicked < 0 || static_cast<size_t>(num_clicked) >= AirportTemplateManager::GetTemplates().size()) break;
				this->selected_template_index = num_clicked;
				this->UpdateRotationButtons();
				if (this->mode == TemplateManagerMode::LoadingPlace) this->UpdateLoadingPlacementPreview();
				this->SetDirty();
				break;
			}

			case WID_TM_SAVE:
				if (this->mode == TemplateManagerMode::SavingPickAirport) {
					this->ExitPlacementMode();
				} else {
					this->StartSavePickMode();
				}
				break;

			case WID_TM_LOAD:
				if (this->mode == TemplateManagerMode::LoadingPlace) {
					this->ExitPlacementMode();
				} else {
					this->StartLoadPlaceMode();
				}
				break;

			case WID_TM_DELETE: {
				const AirportTemplate *templ = GetAirportTemplateByIndex(this->selected_template_index);
				if (templ == nullptr) break;
				this->pending_delete_stem = templ->file_stem;
				ShowQuery(
					GetEncodedString(STR_QUERY_DELETE_AIRPORT_TEMPLATE_CAPTION),
					GetEncodedString(STR_QUERY_DELETE_AIRPORT_TEMPLATE_TEXT, templ->name),
					this,
					DeleteTemplateConfirmationCallback
				);
				break;
			}

			case WID_TM_ROTATE_LEFT:
				this->RotateSelection(-1);
				break;

			case WID_TM_ROTATE_RIGHT:
				this->RotateSelection(1);
				break;

			default: break;
		}
	}

	void OnPlaceObject([[maybe_unused]] Point pt, TileIndex tile) override
	{
		if (this->mode == TemplateManagerMode::SavingPickAirport) {
			if (!IsTileType(tile, TileType::Station) || !IsAirport(tile)) {
				ShowErrorMessage(GetEncodedString(STR_ERROR_AIRPORT_TEMPLATE_INVALID_SELECTION), {}, WL_INFO);
				return;
			}

			Station *st = Station::GetByTile(tile);
			if (st == nullptr || st->owner != _local_company || !st->airport.blocks.Test(AirportBlock::Modular)) {
				ShowErrorMessage(GetEncodedString(STR_ERROR_AIRPORT_TEMPLATE_INVALID_SELECTION), {}, WL_INFO);
				return;
			}

			this->save_pick_tile = tile;
			this->has_save_pick_tile = true;
			ShowQueryString({}, STR_QUERY_SAVE_AIRPORT_TEMPLATE_CAPTION, MAX_LENGTH_STATION_NAME_CHARS, this, CS_ALPHANUMERAL, {QueryStringFlag::EnableDefault, QueryStringFlag::LengthIsInChars});
			return;
		}

		if (this->mode != TemplateManagerMode::LoadingPlace) return;

		const AirportTemplate *templ = GetAirportTemplateByIndex(this->selected_template_index);
		if (templ == nullptr || !templ->is_available) {
			ShowErrorMessage(GetEncodedString(STR_ERROR_AIRPORT_TEMPLATE_NEWGRF_MISSING), {}, WL_INFO);
			return;
		}

		ModularTemplatePlacementData data;
		data.width = templ->width;
		data.height = templ->height;
		data.rotation = this->selected_rotation;
		data.tiles.reserve(templ->tiles.size());
		for (const AirportTemplateTile &tt : templ->tiles) {
			ModularTemplatePlacementTile ct;
			ct.dx = tt.dx;
			ct.dy = tt.dy;
			ct.piece_type = tt.piece_type;
			ct.rotation = tt.rotation;
			ct.runway_flags = tt.runway_flags;
			ct.one_way_taxi = tt.one_way_taxi;
			ct.user_taxi_dir_mask = tt.user_taxi_dir_mask;
			ct.edge_block_mask = tt.edge_block_mask;
			data.tiles.push_back(ct);
		}

		bool adjacent = _ctrl_pressed;
		auto proc = [=](bool test, StationID to_join) -> bool {
			if (test) {
				return Command<CMD_PLACE_MODULAR_AIRPORT_TEMPLATE>::Do(CommandFlagsToDCFlags(GetCommandFlags<CMD_PLACE_MODULAR_AIRPORT_TEMPLATE>()), tile, StationID::Invalid(), adjacent, data).Succeeded();
			}
			return Command<CMD_PLACE_MODULAR_AIRPORT_TEMPLATE>::Post(STR_ERROR_CAN_T_BUILD_AIRPORT_HERE, CcBuildAirport, tile, to_join, adjacent, data);
		};

		uint16_t w, h;
		templ->GetRotatedDimensions(this->selected_rotation, w, h);
		ShowSelectStationIfNeeded(TileArea(tile, w, h), proc);
	}

	void OnPlaceObjectAbort() override
	{
		if (this->updating_cursor) return;
		this->mode = TemplateManagerMode::None;
		this->SetWidgetLoweredState(WID_TM_SAVE, false);
		this->SetWidgetLoweredState(WID_TM_LOAD, false);
		ResetSavedTemplateGuiState();
		this->SetDirty();
	}

	void OnQueryTextFinished(std::optional<std::string> str) override
	{
		if (!this->has_save_pick_tile) return;
		if (!str.has_value()) {
			this->has_save_pick_tile = false;
			this->save_pick_tile = INVALID_TILE;
			return;
		}

		std::string name = *str;
		bool all_ws = std::all_of(name.begin(), name.end(), [](char c) {
			return std::isspace(static_cast<unsigned char>(c)) != 0;
		});
		if (name.empty() || all_ws) {
			ShowErrorMessage(GetEncodedString(STR_ERROR_AIRPORT_TEMPLATE_NAME_EMPTY), {}, WL_INFO);
			this->has_save_pick_tile = false;
			this->save_pick_tile = INVALID_TILE;
			return;
		}

		if (!IsValidTile(this->save_pick_tile) || !IsTileType(this->save_pick_tile, TileType::Station) || !IsAirport(this->save_pick_tile)) {
			ShowErrorMessage(GetEncodedString(STR_ERROR_AIRPORT_TEMPLATE_INVALID_SELECTION), {}, WL_INFO);
			this->has_save_pick_tile = false;
			this->save_pick_tile = INVALID_TILE;
			return;
		}

		Station *st = Station::GetByTile(this->save_pick_tile);
		AirportTemplate templ;
		if (st == nullptr || st->owner != _local_company || !BuildTemplateFromStation(st, templ)) {
			ShowErrorMessage(GetEncodedString(STR_ERROR_AIRPORT_TEMPLATE_IO), {}, WL_INFO);
			this->has_save_pick_tile = false;
			this->save_pick_tile = INVALID_TILE;
			return;
		}

		templ.name = name;
		std::string saved_stem;
		if (!AirportTemplateManager::SaveTemplate(templ, &saved_stem)) {
			ShowErrorMessage(GetEncodedString(STR_ERROR_AIRPORT_TEMPLATE_IO), {}, WL_INFO);
			this->has_save_pick_tile = false;
			this->save_pick_tile = INVALID_TILE;
			return;
		}

		this->has_save_pick_tile = false;
		this->save_pick_tile = INVALID_TILE;
		this->RefreshTemplateList(saved_stem);
	}

	EventState OnHotkey(int hotkey) override
	{
		switch (hotkey) {
			case TMHK_ROTATE_LEFT:
				return this->RotateSelection(-1) ? ES_HANDLED : ES_NOT_HANDLED;

			case TMHK_ROTATE_RIGHT:
				return this->RotateSelection(1) ? ES_HANDLED : ES_NOT_HANDLED;

			default:
				return ES_NOT_HANDLED;
		}
	}

	static inline HotkeyList hotkeys{"modairporttemplates", {
		Hotkey('E', "rotate_left", TMHK_ROTATE_LEFT),
		Hotkey('R', "rotate_right", TMHK_ROTATE_RIGHT),
	}};
};

static constexpr std::initializer_list<NWidgetPart> _nested_build_modular_template_manager_widgets = {
	NWidget(NWID_HORIZONTAL),
		NWidget(WWT_CLOSEBOX, COLOUR_DARK_GREEN),
		NWidget(WWT_CAPTION, COLOUR_DARK_GREEN, WID_TM_CAPTION), SetStringTip(STR_STATION_BUILD_MODULAR_AIRPORT_TEMPLATE_MANAGER_CAPTION, STR_TOOLTIP_WINDOW_TITLE_DRAG_THIS),
		NWidget(WWT_STICKYBOX, COLOUR_DARK_GREEN),
	EndContainer(),
	NWidget(WWT_PANEL, COLOUR_DARK_GREEN),
		NWidget(NWID_VERTICAL), SetPadding(WidgetDimensions::unscaled.picker), SetPIP(0, WidgetDimensions::unscaled.vsep_normal, 0),
			NWidget(WWT_EMPTY, INVALID_COLOUR, WID_TM_PREVIEW), SetFill(1, 0), SetMinimalSize(150, 100),
			NWidget(NWID_HORIZONTAL),
				NWidget(WWT_MATRIX, COLOUR_GREY, WID_TM_TEMPLATE_LIST), SetFill(1, 1), SetMatrixDataTip(1, 6, STR_STATION_BUILD_MODULAR_AIRPORT_TEMPLATE_LIST_TOOLTIP), SetScrollbar(WID_TM_SCROLLBAR),
				NWidget(NWID_VSCROLLBAR, COLOUR_GREY, WID_TM_SCROLLBAR),
			EndContainer(),
			NWidget(NWID_HORIZONTAL), SetPIP(0, WidgetDimensions::unscaled.hsep_normal, 0), SetPIPRatio(1, 1, 1),
				NWidget(WWT_TEXTBTN, COLOUR_GREY, WID_TM_SAVE), SetMinimalSize(70, 12), SetStringTip(STR_STATION_BUILD_MODULAR_AIRPORT_TEMPLATE_SAVE),
				NWidget(WWT_TEXTBTN, COLOUR_GREY, WID_TM_LOAD), SetMinimalSize(70, 12), SetStringTip(STR_STATION_BUILD_MODULAR_AIRPORT_TEMPLATE_LOAD),
				NWidget(WWT_TEXTBTN, COLOUR_GREY, WID_TM_DELETE), SetMinimalSize(70, 12), SetStringTip(STR_STATION_BUILD_MODULAR_AIRPORT_TEMPLATE_DELETE),
			EndContainer(),
			NWidget(NWID_HORIZONTAL), SetPIP(0, WidgetDimensions::unscaled.hsep_normal, 0), SetPIPRatio(1, 0, 1),
				NWidget(WWT_PUSHARROWBTN, COLOUR_GREY, WID_TM_ROTATE_LEFT), SetMinimalSize(12, 0), SetArrowWidgetTypeTip(AWV_DECREASE),
				NWidget(WWT_LABEL, INVALID_COLOUR, WID_TM_ROTATION), SetResize(1, 0), SetFill(1, 0),
				NWidget(WWT_PUSHARROWBTN, COLOUR_GREY, WID_TM_ROTATE_RIGHT), SetMinimalSize(12, 0), SetArrowWidgetTypeTip(AWV_INCREASE),
			EndContainer(),
			NWidget(WWT_EMPTY, INVALID_COLOUR, WID_TM_INFO), SetFill(1, 0), SetMinimalTextLines(2, WidgetDimensions::unscaled.vsep_normal),
		EndContainer(),
	EndContainer(),
};

static WindowDesc _build_modular_template_manager_desc(
	WDP_AUTO, "build_modular_template_manager", 0, 0,
	WC_AIRPORT_TEMPLATE_MANAGER, WC_BUILD_STATION,
	WindowDefaultFlag::Construction,
	_nested_build_modular_template_manager_widgets,
	&BuildModularTemplateManagerWindow::hotkeys
);

void ShowBuildAirportTemplateManagerWindow(Window *parent)
{
	CloseWindowById(WC_AIRPORT_TEMPLATE_MANAGER, 0);
	new BuildModularTemplateManagerWindow(_build_modular_template_manager_desc, parent);
}
