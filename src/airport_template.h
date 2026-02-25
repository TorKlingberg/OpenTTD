/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file airport_template.h Structures for modular airport templates. */

#ifndef AIRPORT_TEMPLATE_H
#define AIRPORT_TEMPLATE_H

#include "stdafx.h"
#include <string>
#include <vector>
#include <memory>
#include "tile_type.h"

/** Tile data within an airport template. */
struct AirportTemplateTile {
	uint16_t dx;
	uint16_t dy;
	uint8_t piece_type;
	uint8_t rotation;
	uint8_t runway_flags;
	bool one_way_taxi;
	uint8_t user_taxi_dir_mask;
	uint8_t edge_block_mask;
	uint32_t grfid;   ///< 0 for base-set tiles
	uint16_t local_id; ///< only meaningful when grfid != 0

	/**
	 * Rotate tile metadata clockwise by r (0-3).
	 * @param r Number of 90-degree clockwise steps.
	 * @param template_w Original template width (before rotation).
	 * @param template_h Original template height (before rotation).
	 */
	void Rotate(uint8_t r, uint16_t template_w, uint16_t template_h);
};

/** A saved modular airport design. */
struct AirportTemplate {
	std::string name;
	uint16_t width;
	uint16_t height;
	std::vector<AirportTemplateTile> tiles;
	uint32_t schema_version = 1;

	bool is_available = true; ///< Cached availability based on NewGRFs.

	/**
	 * Get the rotated dimensions of this template.
	 * @param r Rotation (0-3 clockwise).
	 * @param[out] w Resulting width.
	 * @param[out] h Resulting height.
	 */
	void GetRotatedDimensions(uint8_t r, uint16_t &w, uint16_t &h) const
	{
		if (r == 1 || r == 3) {
			w = this->height;
			h = this->width;
		} else {
			w = this->width;
			h = this->height;
		}
	}

	/** Check if all required NewGRFs are present. Updates is_available. */
	void CheckAvailability();
};

/** Manager for loading/saving airport templates. */
class AirportTemplateManager {
public:
	static const std::vector<std::unique_ptr<AirportTemplate>>& GetTemplates();
	static void Refresh();

	static bool SaveTemplate(const AirportTemplate &template_to_save);
};

#endif /* AIRPORT_TEMPLATE_H */
