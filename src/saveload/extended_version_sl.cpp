/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file extended_version_sl.cpp Handling of the fork's savegame feature versions and the XVER chunk. */

#include "../stdafx.h"

#include "extended_version_sl.h"
#include "../debug.h"

#include "table/strings.h"

#include "../safeguards.h"

/** Description of a fork feature, as this build knows it. */
struct SlxFeatureInfo {
	SlxFeature feature;    ///< The feature itself.
	const char *name;      ///< Name as written to the savegame; globally unique and never changes once released.
	uint16_t max_version;  ///< Highest version of the feature this build can load.
	SlxFeatureFlags flags; ///< What a loader without this feature may do with a savegame that has it.
};

/**
 * All fork features.
 *
 * Neither feature is ignorable: modular airports repurpose map bits and add tile data, so a
 * loader that drops the feature would not get a sane game, and the upstream version is what
 * tells such a loader which upstream format the rest of the savegame is in.
 */
static const SlxFeatureInfo _slx_features[] = {
	{ SlxFeature::UpstreamVersion, "upstream_version", 0xFFFF,                      {} },
	{ SlxFeature::ModularAirport,  "modular_airport",  MODULAR_AIRPORT_SL_VERSION,  {} },
};
static_assert(lengthof(_slx_features) == to_underlying(SlxFeature::End));

/** Version of each feature in the savegame being loaded, or being written. 0 means absent. */
static std::array<uint16_t, to_underlying(SlxFeature::End)> _slx_feature_versions{};

/** Whether the savegame being loaded had #SAVEGAME_VERSION_EXT set in its header. */
static bool _slx_is_extended_savegame = false;

/**
 * Look up a feature by the name it is saved under.
 * @param name Name from the savegame.
 * @return The feature, or \c nullptr when this build does not know it.
 */
static const SlxFeatureInfo *SlxFindFeature(std::string_view name)
{
	for (const SlxFeatureInfo &info : _slx_features) {
		if (name == info.name) return &info;
	}
	return nullptr;
}

/** Forget all feature versions, i.e. assume a savegame with no fork features at all. */
void SlxResetFeatureVersions()
{
	_slx_feature_versions.fill(0);
	_slx_is_extended_savegame = false;
}

/** Set all feature versions to what this build writes. */
void SlxSetCurrentFeatureVersions()
{
	SlxResetFeatureVersions();
	_slx_is_extended_savegame = true;

	_slx_feature_versions[to_underlying(SlxFeature::UpstreamVersion)] = to_underlying(SAVEGAME_VERSION);
	_slx_feature_versions[to_underlying(SlxFeature::ModularAirport)] = MODULAR_AIRPORT_SL_VERSION;
}

/**
 * Set the version of a single feature.
 * @param feature The feature to set.
 * @param version Version to set it to, 0 for absent.
 */
void SlxSetFeatureVersion(SlxFeature feature, uint16_t version)
{
	_slx_feature_versions[to_underlying(feature)] = version;
}

/**
 * Get the version of a feature in the savegame being loaded.
 * @param feature The feature to query.
 * @return Its version, or 0 when the savegame does not have it.
 */
uint16_t SlxGetFeatureVersion(SlxFeature feature)
{
	return _slx_feature_versions[to_underlying(feature)];
}

/**
 * Record whether the savegame being loaded is one of ours.
 * @param is_extended Whether #SAVEGAME_VERSION_EXT was set in the savegame header.
 */
void SlxSetSavegameIsExtended(bool is_extended)
{
	_slx_is_extended_savegame = is_extended;
}

/**
 * Whether the savegame being loaded is one of ours.
 * @return True when the savegame header had #SAVEGAME_VERSION_EXT set.
 */
bool SlxIsExtendedSavegame()
{
	return _slx_is_extended_savegame;
}

static std::string _slx_sl_name;
static uint16_t _slx_sl_version;
static uint8_t _slx_sl_flags;

static const SaveLoad _slx_feature_desc[] = {
	SLEG_SSTR("name", _slx_sl_name, VarTypes::STR),
	 SLEG_VAR("version", _slx_sl_version, VarTypes::U16),
	 SLEG_VAR("flags", _slx_sl_flags, VarTypes::U8),
};

struct XVERChunkHandler : ChunkHandler {
	XVERChunkHandler() : ChunkHandler("XVER", ChunkType::Table) {}

	void Save() const override
	{
		SlTableHeader(_slx_feature_desc);

		int index = 0;
		for (const SlxFeatureInfo &info : _slx_features) {
			const uint16_t version = _slx_feature_versions[to_underlying(info.feature)];
			if (version == 0) continue;

			_slx_sl_name = info.name;
			_slx_sl_version = version;
			_slx_sl_flags = info.flags.base();

			SlSetArrayIndex(index++);
			SlObject(nullptr, _slx_feature_desc);
		}
	}

	void Load() const override
	{
		if (!SlxIsExtendedSavegame()) SlErrorCorrupt("XVER chunk is unexpectedly present");

		const std::vector<SaveLoad> slt = SlTableHeader(_slx_feature_desc);

		while (SlIterateArray() != -1) {
			_slx_sl_name.clear();
			_slx_sl_version = 0;
			_slx_sl_flags = 0;
			SlObject(nullptr, slt);

			/* The flags come from the savegame, not from our own table: it is the writer of the
			 * savegame that knows whether its feature can be dropped without breaking the rest. */
			const SlxFeatureFlags flags{_slx_sl_flags};
			const SlxFeatureInfo *info = SlxFindFeature(_slx_sl_name);

			if (info == nullptr) {
				if (flags.Test(SlxFeatureFlag::IgnorableUnknown)) {
					Debug(sl, 1, "XVER chunk: unknown feature '{}' version {}, ignoring", _slx_sl_name, _slx_sl_version);
					continue;
				}
				SlError(STR_GAME_SAVELOAD_ERROR_TOO_NEW_SAVEGAME, fmt::format("Savegame uses unknown feature '{}'", _slx_sl_name));
			}

			if (_slx_sl_version > info->max_version) {
				if (flags.Test(SlxFeatureFlag::IgnorableVersion)) {
					Debug(sl, 1, "XVER chunk: feature '{}' version {} is too new (max {}), ignoring", _slx_sl_name, _slx_sl_version, info->max_version);
					continue;
				}
				SlError(STR_GAME_SAVELOAD_ERROR_TOO_NEW_SAVEGAME, fmt::format("Savegame feature '{}' has version {}, this build supports up to {}", _slx_sl_name, _slx_sl_version, info->max_version));
			}

			Debug(sl, 1, "XVER chunk: feature '{}' version {}", _slx_sl_name, _slx_sl_version);
			_slx_feature_versions[to_underlying(info->feature)] = _slx_sl_version;
		}
	}
};

static const XVERChunkHandler XVER;
static const ChunkHandlerRef extended_version_chunk_handlers[] = {
	XVER,
};

extern const ChunkHandlerTable _extended_version_chunk_handlers(extended_version_chunk_handlers);
