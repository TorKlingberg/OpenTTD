/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/**
 * @file legacy_modular_version_sl.cpp
 *
 * TEMPORARY: loading savegames written before the fork moved its features off #SaveLoadVersion.
 *
 * Those savegames are stamped 367-375, which were entries appended to #SaveLoadVersion for the
 * modular airport branch. They have no XVER chunk, so their feature versions have to be inferred
 * from the stamped version, and the stamped version has to be mapped back to the upstream version
 * the savegame was actually written against:
 *
 *  - 375 was the format at the time of the switch, written against upstream 366.
 *  - 367-374 were written by the branch before it was merged with upstream 16.0, i.e. against
 *    upstream 364; they predate every upstream change from #SaveLoadVersion::DriveBackwards on.
 *    This is what the old IsPreMergeModularSavegame() helper existed for, except that mapping the
 *    version also makes upstream's own conversions for 365 and 366 run, which that helper had to
 *    list by hand.
 *
 * To remove: delete this file (and its entry in CMakeLists.txt), the declaration at the bottom of
 * extended_version_sl.h, and the call in DetermineSaveLoadFormat(). Nothing else refers to it.
 * The static_assert below forces the issue: as soon as an upstream merge takes SAVEGAME_VERSION
 * into this range the numbers become ambiguous, and this code must be gone by then.
 */

#include "../stdafx.h"

#include "extended_version_sl.h"
#include "../debug.h"

#include "../safeguards.h"

/** First savegame version stamped by the fork before the switch to feature versions. */
static constexpr uint16_t LEGACY_MODULAR_FIRST = 367;
/** Last such version, i.e. the format at the time of the switch. */
static constexpr uint16_t LEGACY_MODULAR_LAST = 375;

static_assert(to_underlying(SaveLoadVersion::MaxVersion) - 1 < LEGACY_MODULAR_FIRST,
		"Upstream savegame versions have reached the range the fork used to stamp its own savegames. "
		"Delete legacy_modular_version_sl.cpp and its call site before merging this.");

/**
 * Recognise a savegame written by the fork before it used feature versions, and translate it into
 * an upstream version plus a set of feature versions.
 *
 * Only called for savegames that do not have #SAVEGAME_VERSION_EXT set in their header.
 */
void SlxHandleLegacyModularSavegameVersion()
{
	extern SaveLoadVersion _sl_version;

	const uint16_t version = to_underlying(_sl_version);
	if (version < LEGACY_MODULAR_FIRST || version > LEGACY_MODULAR_LAST) return;

	const SaveLoadVersion upstream_version = (version == LEGACY_MODULAR_LAST) ? SaveLoadVersion::DepotsUnderBridges : SaveLoadVersion::BuoysAt0_0;

	Debug(sl, 1, "Loading a pre-feature-version modular airport savegame version {} as upstream version {}", version, to_underlying(upstream_version));

	_sl_version = upstream_version;
	SlxSetSavegameIsExtended(true);
	SlxSetFeatureVersion(SlxFeature::UpstreamVersion, to_underlying(upstream_version));
	SlxSetFeatureVersion(SlxFeature::ModularAirport, MODULAR_AIRPORT_SL_VERSION);
}
