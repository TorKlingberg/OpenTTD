/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file airport_sl.cpp Code handling saving and loading airport ids. */

#include "../stdafx.h"

#include "saveload.h"
#include "newgrf_sl.h"

#include "../airport_ground_pathfinder.h"

#include "../safeguards.h"

static const SaveLoad _modular_airport_crossing_path_cache_desc[] = {
	SLEG_CONDVECTOR("keys", _modular_airport_crossing_required_path_cache, SLE_UINT64, SLV_MODULAR_AIRPORT, SL_MAX_VERSION),
};

struct APIDChunkHandler : NewGRFMappingChunkHandler {
	APIDChunkHandler() : NewGRFMappingChunkHandler('APID', _airport_mngr) {}
};

struct ATIDChunkHandler : NewGRFMappingChunkHandler {
	ATIDChunkHandler() : NewGRFMappingChunkHandler('ATID', _airporttile_mngr) {}
};

struct MACPChunkHandler : ChunkHandler {
	MACPChunkHandler() : ChunkHandler('MACP', CH_TABLE) {}

	void Save() const override
	{
		NormalizeModularAirportCrossingPathCache();
		SlTableHeader(_modular_airport_crossing_path_cache_desc);

		SlSetArrayIndex(0);
		SlGlobList(_modular_airport_crossing_path_cache_desc);
	}

	void Load() const override
	{
		_modular_airport_crossing_required_path_cache.clear();
		const std::vector<SaveLoad> slt = SlTableHeader(_modular_airport_crossing_path_cache_desc);

		if (SlIterateArray() == -1) return;
		SlGlobList(slt);
		if (SlIterateArray() != -1) SlErrorCorrupt("Too many MACP entries");
		NormalizeModularAirportCrossingPathCache();
	}
};

static const ATIDChunkHandler ATID;
static const APIDChunkHandler APID;
static const MACPChunkHandler MACP;
static const ChunkHandlerRef airport_chunk_handlers[] = {
	ATID,
	APID,
	MACP,
};

extern const ChunkHandlerTable _airport_chunk_handlers(airport_chunk_handlers);
