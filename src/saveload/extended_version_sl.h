/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file extended_version_sl.h Savegame feature versions for the features this fork adds on top of upstream. */

#ifndef SAVELOAD_EXTENDED_VERSION_SL_H
#define SAVELOAD_EXTENDED_VERSION_SL_H

#include "saveload.h"
#include "../core/enum_type.hpp"

/**
 * Fork features are versioned on their own axis instead of by appending to #SaveLoadVersion.
 *
 * #SaveLoadVersion is upstream's and stays upstream's: merging upstream then never renumbers
 * anything of ours, and a fork savegame keeps saying which upstream version it was written
 * against instead of claiming to be newer than upstream features it does not have.
 *
 * Savegames written here set #SAVEGAME_VERSION_EXT in the version field of the savegame header,
 * so upstream rejects them with a plain "savegame too new" rather than loading a game whose map
 * bits it would misread. The feature versions themselves live in the XVER chunk, which is written
 * first and therefore known before any chunk that depends on it is read.
 *
 * The shape of all this deliberately mirrors JGRPP's SLXI chunk (feature name string, uint16
 * version, ignorable flags), so that porting a feature there is mechanical.
 */
static constexpr uint16_t SAVEGAME_VERSION_EXT = 0x8000;

/** Features this fork adds on top of the upstream savegame format. */
enum class SlxFeature : uint8_t {
	UpstreamVersion, ///< Upstream savegame version this savegame was written against. Its "version" is that version number.
	ModularAirport,  ///< Modular airports: per-tile layout data, ground/air state on aircraft, reservation state.
	End,
};

/** How a loader that does not know a feature, or knows only an older version of it, may treat it. */
enum class SlxFeatureFlag : uint8_t {
	IgnorableUnknown, ///< The feature may be dropped by a loader that does not know it at all.
	IgnorableVersion, ///< The feature may be dropped by a loader that only knows an older version of it.
};
using SlxFeatureFlags = EnumBitSet<SlxFeatureFlag, uint8_t>;

/** Version of #SlxFeature::ModularAirport written by this build. */
static constexpr uint16_t MODULAR_AIRPORT_SL_VERSION = 1;

void SlxResetFeatureVersions();
void SlxSetCurrentFeatureVersions();
void SlxSetFeatureVersion(SlxFeature feature, uint16_t version);
uint16_t SlxGetFeatureVersion(SlxFeature feature);
void SlxSetSavegameIsExtended(bool is_extended);
bool SlxIsExtendedSavegame();

/**
 * Test whether the savegame being loaded has a fork feature, at least at some version.
 * @param feature     The feature to test for.
 * @param min_version Lowest version of the feature that counts as present.
 * @return True when the savegame has the feature at \a min_version or later.
 */
inline bool SlxIsFeaturePresent(SlxFeature feature, uint16_t min_version = 1)
{
	return SlxGetFeatureVersion(feature) >= min_version;
}

/**
 * Test whether the savegame being loaded was written with modular airport support.
 * @param min_version Lowest version of the feature that counts as present.
 * @return True when the savegame has modular airport data.
 */
inline bool IsModularAirportSaveFeaturePresent(uint16_t min_version = 1)
{
	return SlxIsFeaturePresent(SlxFeature::ModularAirport, min_version);
}

#endif /* SAVELOAD_EXTENDED_VERSION_SL_H */
