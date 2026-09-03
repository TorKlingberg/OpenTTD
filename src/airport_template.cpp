/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file airport_template.cpp Implementation of modular airport templates. */

#include "stdafx.h"
#include "airport_template.h"
#include "fileio_func.h"
#include "fileio_type.h"
#include "newgrf_airporttiles.h"
#include "newgrf.h"
#include "modular_airport_cmd.h"
#include "debug.h"
#include "string_func.h"
#include "core/geometry_func.hpp"

#include "3rdparty/nlohmann/json.hpp"

#include <fstream>
#include <filesystem>
#include <algorithm>
#include <cctype>
#include <optional>

using json = nlohmann::json;

static std::vector<std::unique_ptr<AirportTemplate>> _templates;
static bool _templates_dirty = true;

static std::string GetTemplatesDirectory()
{
	std::string dir = FioGetDirectory(Searchpath::PersonalDir, Subdirectory::Base);
	AppendPathSeparator(dir);
	dir += "airport_templates";
	AppendPathSeparator(dir);
	return dir;
}

static std::optional<uint8_t> ResolveAirportTileIndexByGRF(uint32_t grfid, uint16_t local_id)
{
	for (uint16_t gfx = NEW_AIRPORTTILE_OFFSET; gfx < NUM_AIRPORTTILES; gfx++) {
		const AirportTileSpec *ats = AirportTileSpec::Get(static_cast<StationGfx>(gfx));
		if (ats == nullptr) continue;
		if (FlattenNewGRFLabel(ats->grf_prop.grfid) == grfid && ats->grf_prop.local_id == local_id) return static_cast<uint8_t>(gfx);
	}
	return std::nullopt;
}

void AirportTemplateTile::Rotate(uint8_t r, uint16_t template_w, uint16_t template_h)
{
	RotateModularTemplateTile(*this, r, template_w, template_h);
}

uint AirportTemplate::GetCatchmentRadius() const
{
	std::vector<ModularCatchmentPiece> pieces;
	pieces.reserve(this->tiles.size());
	for (const AirportTemplateTile &t : this->tiles) {
		pieces.push_back({static_cast<int>(t.dx), static_cast<int>(t.dy), t.piece_type, t.rotation, t.runway_flags});
	}
	return GetModularAirportCatchmentRadiusFromPieces(pieces);
}

void AirportTemplate::CheckAvailability()
{
	this->is_available = true;
	for (auto &tile : this->tiles) {
		if (tile.grfid != 0) {
			auto resolved = ResolveAirportTileIndexByGRF(tile.grfid, tile.local_id);
			if (!resolved.has_value()) {
				this->is_available = false;
				return;
			}
			tile.piece_type = resolved.value();
		} else if (tile.piece_type >= NUM_AIRPORTTILES && !IsModularAirportDecorationPiece(tile.piece_type)) {
			/* Only the explicitly defined metadata-only pieces may live outside
			 * the 8-bit airport-tile namespace. Reject unknown values from a
			 * hand-edited template before its preview reaches the map-gfx mapper. */
			this->is_available = false;
			return;
		}
	}
}

const std::vector<std::unique_ptr<AirportTemplate>>& AirportTemplateManager::GetTemplates()
{
	return _templates;
}

void AirportTemplateManager::Refresh()
{
	if (!_templates_dirty) return;
	_templates_dirty = false;

	_templates.clear();

	std::string dir = GetTemplatesDirectory();
	if (dir.empty()) return;

	std::error_code ec;
	for (const auto &entry : std::filesystem::directory_iterator(OTTD2FS(dir), ec)) {
		if (ec) break;
		if (!entry.is_regular_file()) continue;
		if (entry.path().extension() != ".json") continue;

		try {
			std::ifstream f(entry.path());
			json j = json::parse(f);

			auto t = std::make_unique<AirportTemplate>();
			t->file_stem = FS2OTTD(entry.path().stem().native());
			t->name = j.value("name", std::string{});
			t->width = j.value("width", static_cast<uint16_t>(0));
			t->height = j.value("height", static_cast<uint16_t>(0));
			t->schema_version = j.value("schema_version", static_cast<uint32_t>(1));
			if (t->name.empty() || t->width == 0 || t->height == 0) continue;
			if (t->width > MAX_TEMPLATE_DIM || t->height > MAX_TEMPLATE_DIM) continue;

			for (const auto &jt : j.value("tiles", json::array())) {
				AirportTemplateTile tile;
				tile.dx = jt.value("dx", static_cast<uint16_t>(0));
				tile.dy = jt.value("dy", static_cast<uint16_t>(0));
				tile.piece_type = jt.value("piece_type", static_cast<ModularAirportPieceID>(0));
				/* Rotation is mod 4 everywhere downstream -- hangar facings, the
				 * (old_rotation + r) & 3 accumulation in Rotate(). Clamp on the way in
				 * so a hand-edited file cannot smuggle an out-of-range value past the
				 * dx/dy bounds checks below. */
				tile.rotation = jt.value("rotation", static_cast<uint8_t>(0)) & 3;
				tile.runway_flags = jt.value("runway_flags", static_cast<uint8_t>(0));
				tile.one_way_taxi = jt.value("one_way_taxi", false);
				tile.user_taxi_dir_mask = jt.value("user_taxi_dir_mask", static_cast<uint8_t>(0x0F));
				tile.edge_block_mask = jt.value("edge_block_mask", static_cast<uint8_t>(0));
				tile.grfid = jt.value("grfid", 0u);
				tile.local_id = jt.value("local_id", 0u);
				if (tile.dx >= t->width || tile.dy >= t->height) continue;
				t->tiles.push_back(tile);
				if (t->tiles.size() > MAX_TEMPLATE_TILES) break;
			}
			if (t->tiles.empty()) continue;

			t->CheckAvailability();
			_templates.push_back(std::move(t));
		} catch (const std::exception &e) {
			Debug(misc, 1, "Failed to load airport template {}: {}", FS2OTTD(entry.path().native()), e.what());
		}
	}

	/* Sort templates by name. */
	std::sort(_templates.begin(), _templates.end(), [](const auto &a, const auto &b) {
		return a->name < b->name;
	});
}

static std::string SanitizedSlug(std::string name)
{
	std::string slug;
	for (char c : name) {
		uint8_t uc = static_cast<uint8_t>(c);
		if (std::isalnum(uc)) {
			slug += static_cast<char>(std::tolower(uc));
		} else if (c == ' ' || c == '-' || c == '_') {
			if (!slug.empty() && slug.back() != '-') slug += '-';
		}
	}
	while (!slug.empty() && slug.back() == '-') slug.pop_back();
	if (slug.empty()) slug = "template";
	return slug;
}

bool AirportTemplateManager::SaveTemplate(const AirportTemplate &template_to_save, std::string *saved_file_stem)
{
	if (template_to_save.name.empty() || template_to_save.tiles.empty()) return false;
	if (template_to_save.width == 0 || template_to_save.height == 0) return false;
	if (template_to_save.width > MAX_TEMPLATE_DIM || template_to_save.height > MAX_TEMPLATE_DIM) return false;
	if (template_to_save.tiles.size() > MAX_TEMPLATE_TILES) return false;

	std::string base_slug = SanitizedSlug(template_to_save.name);
	std::string dir = GetTemplatesDirectory();
	FioCreateDirectory(dir);

	std::string file_stem = base_slug;
	std::string filename = file_stem + ".json";
	int i = 2;
	while (FileExists(dir + filename)) {
		file_stem = base_slug + "-" + std::to_string(i++);
		filename = file_stem + ".json";
	}

	json j;
	j["name"] = template_to_save.name;
	j["width"] = template_to_save.width;
	j["height"] = template_to_save.height;
	j["schema_version"] = template_to_save.schema_version;

	std::vector<AirportTemplateTile> sorted_tiles = template_to_save.tiles;
	std::sort(sorted_tiles.begin(), sorted_tiles.end(), [](const AirportTemplateTile &a, const AirportTemplateTile &b) {
		if (a.dy != b.dy) return a.dy < b.dy;
		return a.dx < b.dx;
	});

	json j_tiles = json::array();
	for (const auto &tile : sorted_tiles) {
		json jt;
		jt["dx"] = tile.dx;
		jt["dy"] = tile.dy;
		jt["piece_type"] = tile.piece_type;
		jt["rotation"] = tile.rotation;
		jt["runway_flags"] = tile.runway_flags;
		jt["one_way_taxi"] = tile.one_way_taxi;
		jt["user_taxi_dir_mask"] = tile.user_taxi_dir_mask;
		jt["edge_block_mask"] = tile.edge_block_mask;
		if (tile.grfid != 0) {
			jt["grfid"] = tile.grfid;
			jt["local_id"] = tile.local_id;
		}
		j_tiles.push_back(jt);
	}
	j["tiles"] = j_tiles;

	try {
		std::ofstream f(OTTD2FS(dir + filename).c_str());
		if (!f.is_open()) return false;
		f << j.dump(4);
		f.flush();
		if (!f.good()) return false;
		f.close();
		if (saved_file_stem != nullptr) *saved_file_stem = file_stem;
		_templates_dirty = true;
		AirportTemplateManager::Refresh();
		return true;
	} catch (const std::exception &e) {
		Debug(misc, 1, "Failed to save airport template {}: {}", filename, e.what());
		return false;
	}
}

bool AirportTemplateManager::DeleteTemplateByFileStem(const std::string &file_stem)
{
	if (file_stem.empty()) return false;

	std::string dir = GetTemplatesDirectory();
	std::filesystem::path path = std::filesystem::path(OTTD2FS(dir)) / std::filesystem::path(OTTD2FS(file_stem + ".json"));
	std::error_code ec;
	if (!std::filesystem::remove(path, ec)) {
		if (!ec) return false;
		Debug(misc, 1, "Failed to delete airport template {}: {}", FS2OTTD(path.native()), ec.message());
		return false;
	}

	_templates_dirty = true;
	AirportTemplateManager::Refresh();
	return true;
}
