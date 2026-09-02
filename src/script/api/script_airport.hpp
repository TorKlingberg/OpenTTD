/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file script_airport.hpp Everything to query and build airports. */

#ifndef SCRIPT_AIRPORT_HPP
#define SCRIPT_AIRPORT_HPP

#include "script_object.hpp"
#include "../squirrel_helper_type.hpp"
#include "../../airport.h"
#include "../../station_type.h"
#include "../../table/airporttile_ids.h"

/**
 * Class that handles all airport related functions.
 * @api ai game
 */
class ScriptAirport : public ScriptObject {
public:
	/**
	 * The types of airports available in the game.
	 */
	enum AirportType {
		/* Note: these values represent part of the in-game AirportTypes enum */
		AT_SMALL         = ::AT_SMALL,         ///< The small airport.
		AT_LARGE         = ::AT_LARGE,         ///< The large airport.
		AT_METROPOLITAN  = ::AT_METROPOLITAN,  ///< The metropolitan airport.
		AT_INTERNATIONAL = ::AT_INTERNATIONAL, ///< The international airport.
		AT_COMMUTER      = ::AT_COMMUTER,      ///< The commuter airport.
		AT_INTERCON      = ::AT_INTERCON,      ///< The intercontinental airport.
		AT_HELIPORT      = ::AT_HELIPORT,      ///< The heliport.
		AT_HELISTATION   = ::AT_HELISTATION,   ///< The helistation.
		AT_HELIDEPOT     = ::AT_HELIDEPOT,     ///< The helidepot.
		AT_MODULAR       = ::AT_MODULAR,       ///< A layout-derived modular airport.
		AT_INVALID       = ::AT_INVALID,       ///< Invalid airport.
	};

	/**
	 * All plane types available.
	 */
	enum PlaneType {
		/* Note: these values represent part of the in-game values, which are not defined in an enum */
		PT_HELICOPTER    =   0, ///< A helicopter.
		PT_SMALL_PLANE   =   1, ///< A small plane.
		PT_BIG_PLANE     =   3, ///< A big plane.

		PT_INVALID       =  -1, ///< An invalid PlaneType
	};

	/**
	 * Checks whether the given AirportType is valid and available.
	 * @param type The AirportType to check.
	 * @return True if and only if the AirportType is valid and available.
	 * @post return value == true -> IsAirportInformationAvailable returns true.
	 */
	static bool IsValidAirportType(AirportType type);

	/**
	 * Can you get per-type information on this airport type? As opposed to
	 * IsValidAirportType this also returns true for an airport type that is no
	 * longer buildable. It returns false for AT_MODULAR because a modular
	 * airport's size, coverage, noise, maintenance and helipads come from its
	 * layout rather than from type-level data.
	 * @param type The AirportType to check.
	 * @return True if and only if the AirportType is valid.
	 * @post return value == false -> IsValidAirportType returns false.
	 */
	static bool IsAirportInformationAvailable(AirportType type);

	/**
	 * Get the cost to build this AirportType.
	 * @param type The AirportType to check.
	 * @pre AirportAvailable(type).
	 * @return The cost of building this AirportType.
	 */
	static Money GetPrice(AirportType type);

	/**
	 * Checks whether the given tile is actually a tile with a hangar.
	 * @param tile The tile to check.
	 * @pre ScriptMap::IsValidTile(tile).
	 * @return True if and only if the tile has a hangar.
	 */
	static bool IsHangarTile(TileIndex tile);

	/**
	 * Checks whether the given tile is actually a tile with an airport.
	 * @param tile The tile to check.
	 * @pre ScriptMap::IsValidTile(tile).
	 * @return True if and only if the tile has an airport.
	 */
	static bool IsAirportTile(TileIndex tile);

	/**
	 * Get the width of this type of airport.
	 * @param type The type of airport.
	 * @pre IsAirportInformationAvailable(type).
	 * @return The width in tiles.
	 */
	static SQInteger GetAirportWidth(AirportType type);

	/**
	 * Get the height of this type of airport.
	 * @param type The type of airport.
	 * @pre IsAirportInformationAvailable(type).
	 * @return The height in tiles.
	 */
	static SQInteger GetAirportHeight(AirportType type);

	/**
	 * Get the coverage radius of this type of airport.
	 * @param type The type of airport.
	 * @pre IsAirportInformationAvailable(type).
	 * @return The radius in tiles.
	 */
	static SQInteger GetAirportCoverageRadius(AirportType type);

	/**
	 * Get the number of hangars of the airport.
	 * @param tile Any tile of the airport.
	 * @pre ScriptMap::IsValidTile(tile).
	 * @return The number of hangars of the airport.
	 */
	static SQInteger GetNumHangars(TileIndex tile);

	/**
	 * Get the first hangar tile of the airport.
	 * @param tile Any tile of the airport.
	 * @pre ScriptMap::IsValidTile(tile).
	 * @pre GetNumHangars(tile) > 0.
	 * @return The first hangar tile of the airport.
	 * @note Possible there are more hangars, but you won't be able to find them
	 *  without walking over all the tiles of the airport and using
	 *  IsHangarTile() on them.
	 */
	static TileIndex GetHangarOfAirport(TileIndex tile);

	/**
	 * Builds a airport with tile at the topleft corner.
	 * @param tile The topleft corner of the airport.
	 * @param type The type of airport to build.
	 * @param station_id The station to join, ScriptStation::STATION_NEW or ScriptStation::STATION_JOIN_ADJACENT.
	 * @pre ScriptMap::IsValidTile(tile).
	 * @pre AirportAvailable(type).
	 * @pre station_id == ScriptStation::STATION_NEW || station_id == ScriptStation::STATION_JOIN_ADJACENT || ScriptStation::IsValidStation(station_id).
	 * @game @pre ScriptCompanyMode::IsValid().
	 * @exception ScriptError::ERR_AREA_NOT_CLEAR
	 * @exception ScriptError::ERR_FLAT_LAND_REQUIRED
	 * @exception ScriptError::ERR_LOCAL_AUTHORITY_REFUSES
	 * @exception ScriptError::ERR_STATION_TOO_SPREAD_OUT
	 * @exception ScriptStation::ERR_STATION_TOO_CLOSE_TO_ANOTHER_STATION
	 * @return Whether the airport has been/can be build or not.
	 */
	static bool BuildAirport(TileIndex tile, AirportType type, StationID station_id);

	/**
	 * Removes an airport.
	 * @param tile Any tile of the airport.
	 * @pre ScriptMap::IsValidTile(tile).
	 * @game @pre ScriptCompanyMode::IsValid().
	 * @exception ScriptError::ERR_OWNED_BY_ANOTHER_COMPANY
	 * @return Whether the airport has been/can be removed or not.
	 */
	static bool RemoveAirport(TileIndex tile);

	/**
	 * Get the AirportType of an existing airport.
	 * @param tile Any tile of the airport.
	 * @pre ScriptTile::IsStationTile(tile).
	 * @pre ScriptStation::HasStationType(ScriptStation.GetStationID(tile), ScriptStation::STATION_AIRPORT).
	 * @return The AirportType of the airport.
	 */
	static AirportType GetAirportType(TileIndex tile);

	/**
	 * Get the noise that will be added to the nearest town if an airport was
	 *  built at this tile.
	 * @param tile The tile to check.
	 * @param type The AirportType to check.
	 * @pre IsAirportInformationAvailable(type).
	 * @return The amount of noise added to the nearest town.
	 * @note The noise will be added to the town with TownID GetNearestTown(tile, type).
	 */
	static SQInteger GetNoiseLevelIncrease(TileIndex tile, AirportType type);

	/**
	 * Get the TownID of the town whose local authority will influence
	 *  an airport at some tile.
	 * @param tile The tile to check.
	 * @param type The AirportType to check.
	 * @pre IsAirportInformationAvailable(type).
	 * @return The TownID of the town closest to the tile.
	 */
	static TownID GetNearestTown(TileIndex tile, AirportType type);

	/**
	 * Get the maintenance cost factor of an airport type.
	 * @param type The airport type to get the maintenance factor of.
	 * @pre IsAirportInformationAvailable(type)
	 * @return Maintenance cost factor of the airport type.
	 */
	static SQInteger GetMaintenanceCostFactor(AirportType type);

	/**
	 * Get the monthly maintenance cost of an airport type.
	 * @param type The airport type to get the monthly maintenance cost of.
	 * @pre IsAirportInformationAvailable(type)
	 * @return Maintenance cost of the airport type per economy-month.
	 * @see \ref ScriptEconomyTime
	 */
	static Money GetMonthlyMaintenanceCost(AirportType type);

	/**
	 * Get the number of helipads of this airport type.
	 * @param type The airport type.
	 * @pre IsAirportInformationAvailable(type)
	 * @return Number of helipads of this type of airport. When 0 helicopters will go to normal terminals.
	 */
	static SQInteger GetAirportNumHelipads(AirportType type);

	/**
	 * The pieces a modular airport can be built from.
	 *
	 * A modular airport has no type-level data: its size, capabilities, noise,
	 * catchment and maintenance all follow from the pieces it is made of.
	 *
	 * This is exactly what the interactive builder offers, and deliberately not
	 * one graphic more. The game holds many further airport tiles, but they belong
	 * to stock airports: they reach a modular airport only when one is converted,
	 * and several of them draw things -- a jetway, a fence, half of a compound
	 * building -- that only make sense in the stock layout they were cut from. A
	 * script builds from the same vocabulary a player has, so the two cannot
	 * produce airports the other could not.
	 */
	enum ModularPiece {
		/* Movement surfaces. */
		MP_APRON,                  ///< Plain apron. Aircraft taxi over it and it is not a stopping place.
		MP_STAND,                  ///< Aircraft stand (a terminal). Aircraft load and unload here.
		MP_RUNWAY,                 ///< Middle piece of a large runway.
		MP_RUNWAY_END,             ///< End piece of a large runway. Landings target these.
		MP_RUNWAY_SMALL_MIDDLE,    ///< Middle piece of a small (grass) runway.
		MP_RUNWAY_SMALL_NEAR_END,  ///< High-coordinate end piece of a small runway.
		MP_RUNWAY_SMALL_FAR_END,   ///< Low-coordinate end piece of a small runway.
		MP_HANGAR,                 ///< Large hangar. Aircraft are built and serviced here.
		MP_SMALL_HANGAR,           ///< Small hangar.
		MP_HELIPAD,                ///< Helipad.
		MP_HELIPAD_PLAIN,          ///< Helipad, plain "H" variant.
		MP_HELIPORT,               ///< Rooftop heliport.

		/* Buildings and decoration. Aircraft cannot enter these. */
		MP_TERMINAL,               ///< Large terminal building.
		MP_TERMINAL_ALT,           ///< Large terminal building, second variant.
		MP_TERMINAL_OTHER,         ///< Large terminal building, third variant.
		MP_TERMINAL_ROUND,         ///< Round terminal concourse.
		MP_LOW_TERMINAL,           ///< Low terminal building. Does not count as a large terminal.
		MP_SMALL_TERMINAL_3,       ///< Small terminal, three tiles long. Placed from its north end; rotation parity selects the axis it runs along.
		MP_TOWER,                  ///< Control tower.
		MP_RADIO_TOWER,            ///< Radio tower.
		MP_RADAR,                  ///< Radar.
		MP_RADAR_GRASS,            ///< Radar on grass.
		MP_FLAG_GRASS,             ///< Windsock on grass.
		MP_GRASS = 23,             ///< Plain airport grass.
		MP_EMPTY = 24,             ///< Empty airport tile. Reserves the ground without building anything on it.
		MP_FIRE_STATION = 25,      ///< Airport fire station with a visible fire engine; rotation parity selects the side the bay faces.
		MP_CARGO_TERMINAL = 26,    ///< Cargo terminal / warehouse.
		MP_FUEL_FARM = 27,         ///< Aviation fuel tanks and pumping equipment.
		MP_CAR_PARK = 28,          ///< Multi-storey car park; rotation parity selects the entrance axis.

		MP_INVALID = -1,           ///< Not a modular airport piece.
	};

	/**
	 * Usage flags of a modular runway. A runway carries one set of flags along its
	 * whole contiguous length; setting them on any tile sets them on all of it.
	 *
	 * A valid combination has at least one of MRF_LANDING and MRF_TAKEOFF, and
	 * exactly one of MRF_DIR_LOW and MRF_DIR_HIGH: a runway is used in one
	 * direction, so asking for both, or for neither, is refused. Splitting landing
	 * onto one runway and takeoff onto another is how a busy airport raises its
	 * throughput.
	 */
	enum ModularRunwayFlags {
		MRF_LANDING  = 0x01, ///< Aircraft may land on this runway.
		MRF_TAKEOFF  = 0x02, ///< Aircraft may take off from this runway.
		MRF_DIR_LOW  = 0x04, ///< Operations run towards the low-coordinate end.
		MRF_DIR_HIGH = 0x08, ///< Operations run towards the high-coordinate end.
	};

	/**
	 * What a modular airport is missing before it is safe for large (fast jet)
	 * aircraft. A fast jet using an airport that is not large-safe runs a much
	 * higher risk of crashing on landing, regardless of the "plane crashes" setting.
	 */
	enum ModularSafety {
		MS_OK                      = 0x00, ///< Nothing missing; the airport is safe for large aircraft.
		MS_MISSING_TOWER           = 0x01, ///< No control tower.
		MS_MISSING_BIG_TERMINAL    = 0x02, ///< No large terminal building.
		MS_MISSING_LANDING_RUNWAY  = 0x04, ///< No large runway of at least 6 tiles that allows landing.
		MS_MISSING_TAKEOFF_RUNWAY  = 0x08, ///< No large runway of at least 6 tiles that allows takeoff.
	};

	/**
	 * Layout of the flat array describing a modular airport layout.
	 *
	 * A layout is a flat array of integers holding MLF_STRIDE values per tile, in
	 * the order given here. It is flat rather than an array of arrays because the
	 * script API cannot pass nested arrays. For example, a two-tile layout of an
	 * apron at (0,0) and a stand at (1,0), both with default settings, is:
	 *
	 *   [0, 0, AIAirport.MP_APRON, 0, 0, 0, 15, 0,
	 *    1, 0, AIAirport.MP_STAND, 0, 0, 0, 15, 0]
	 */
	enum ModularLayoutField {
		MLF_DX,               ///< X offset from the layout's north tile, 0 or more.
		MLF_DY,               ///< Y offset from the layout's north tile, 0 or more.
		MLF_PIECE,            ///< The ModularPiece to place.
		MLF_ROTATION,         ///< Rotation of the piece, 0 to 3.
		MLF_RUNWAY_FLAGS,     ///< ModularRunwayFlags for a runway piece, ignored otherwise.
		MLF_ONE_WAY_TAXI,     ///< 1 to make a taxiway one-way, 0 otherwise.
		MLF_TAXI_DIR_MASK,    ///< Permitted taxi directions, 15 for all.
		MLF_EDGE_FENCE_MASK,  ///< Bitmask of tile edges to fence off, 0 for none.
		MLF_STRIDE,           ///< Number of values per tile; not a field itself.
	};

	/**
	 * Checks whether the given tile is part of a modular airport.
	 * @param tile The tile to check.
	 * @pre ScriptMap::IsValidTile(tile).
	 * @return True if and only if the tile belongs to a modular airport.
	 */
	static bool IsModularAirportTile(TileIndex tile);

	/**
	 * Get the piece a modular airport tile is built from.
	 * @param tile The tile to check.
	 * @pre ScriptMap::IsValidTile(tile).
	 * @return The piece, or MP_INVALID when the tile is not part of a modular
	 *  airport or holds a piece this API does not name.
	 */
	static ModularPiece GetModularPiece(TileIndex tile);

	/**
	 * Get the rotation of a modular airport tile.
	 * @param tile The tile to check.
	 * @pre IsModularAirportTile(tile).
	 * @return The rotation, 0 to 3, or -1 when the tile is not modular.
	 */
	static SQInteger GetModularPieceRotation(TileIndex tile);

	/**
	 * Get the usage flags of the runway a tile belongs to.
	 * @param tile The tile to check.
	 * @pre IsModularAirportTile(tile).
	 * @return A bitmask of ModularRunwayFlags, or -1 when the tile is not a
	 *  modular runway piece.
	 */
	static SQInteger GetModularRunwayFlags(TileIndex tile);

	/**
	 * Checks whether a modular piece can be built right now.
	 * Some pieces only become available once large airports do. Pieces drawn from
	 * stored bitmaps are only offered while the "new airport graphics" setting is
	 * on; runtime mirrors of base-set sprites are unaffected.
	 * @param piece The piece to check.
	 * @return True if and only if the piece can be built now in rotation 0.
	 * @note A piece that is available may still be refused in some rotations. Use
	 *  IsModularPieceAvailableInRotation to ask about a particular one.
	 */
	static bool IsModularPieceAvailable(ModularPiece piece);

	/**
	 * Checks whether a modular piece can be built right now in a given rotation.
	 * Same as IsModularPieceAvailable, but for one specific rotation: a piece can
	 * be drawn from stored bitmaps in some rotations only, and is then unavailable
	 * in exactly those while the "new airport graphics" setting is off. The small
	 * hangar's closed-back views (rotations 1 and 2) are the case that exists
	 * today, so with that setting off the same piece is available in rotations 0
	 * and 3 and unavailable in 1 and 2.
	 * @param piece The piece to check.
	 * @param rotation The rotation to check, 0 to 3.
	 * @return True if and only if the piece can be built now in that rotation.
	 */
	static bool IsModularPieceAvailableInRotation(ModularPiece piece, SQInteger rotation);

	/**
	 * Get the first year in which a modular piece can be built.
	 * @param piece The piece to check.
	 * @return The year the piece becomes available.
	 * @see \ref ScriptCalendarTime
	 */
	static SQInteger GetModularPieceMinYear(ModularPiece piece);

	/**
	 * Get what an existing modular airport is missing before it is safe for large aircraft.
	 * @param tile Any tile of the airport.
	 * @pre IsModularAirportTile(tile).
	 * @return A bitmask of ModularSafety, MS_OK when nothing is missing, or -1
	 *  when the tile is not part of a modular airport.
	 */
	static SQInteger GetModularAirportSafety(TileIndex tile);

	/**
	 * Build a single modular airport tile.
	 *
	 * Unlike BuildAirport() this places one piece at a time, so an airport is built
	 * up over many calls and a failure part-way leaves the tiles already built in
	 * place. Use PlaceModularAirportLayout() to build a whole layout in one
	 * all-or-nothing command.
	 *
	 * Every tile of a modular airport must sit at the same height, so the first
	 * tile fixes the height of the whole airport and any later tile at a different
	 * height fails with ERR_FLAT_LAND_REQUIRED.
	 *
	 * @param tile The tile to build on.
	 * @param piece The piece to build.
	 * @param rotation The rotation of the piece, 0 to 3. For hangars this is the
	 *  direction the hangar faces (0 = SE, 1 = NE, 2 = NW, 3 = SW). For runway
	 *  pieces an even rotation lays the runway along the X axis and an odd one
	 *  along the Y axis. For MP_CAR_PARK, rotations 0/2 select one road-facing
	 *  entrance axis and rotations 1/3 select the perpendicular axis. For
	 *  MP_FIRE_STATION an odd rotation turns the appliance bay to face the other
	 *  way. MP_SMALL_TERMINAL_3 covers three tiles: an even rotation lays them
	 *  along the X axis and an odd one along the Y axis, and it accepts only 0 or
	 *  1. Ignored by other pieces that cannot rotate.
	 * @param station_id The station to join, ScriptStation::STATION_NEW or ScriptStation::STATION_JOIN_ADJACENT.
	 * @pre ScriptMap::IsValidTile(tile).
	 * @pre rotation >= 0 && rotation <= 3.
	 * @pre IsModularPieceAvailableInRotation(piece, rotation).
	 * @pre station_id == ScriptStation::STATION_NEW || station_id == ScriptStation::STATION_JOIN_ADJACENT || ScriptStation::IsValidStation(station_id).
	 * @game @pre ScriptCompanyMode::IsValid().
	 * @exception ScriptError::ERR_AREA_NOT_CLEAR
	 * @exception ScriptError::ERR_FLAT_LAND_REQUIRED
	 * @exception ScriptError::ERR_LOCAL_AUTHORITY_REFUSES
	 * @exception ScriptError::ERR_STATION_TOO_SPREAD_OUT
	 * @exception ScriptStation::ERR_STATION_TOO_CLOSE_TO_ANOTHER_STATION
	 * @return Whether the tile has been/can be built or not.
	 */
	static bool BuildModularAirportTile(TileIndex tile, ModularPiece piece, SQInteger rotation, StationID station_id);

	/**
	 * Upgrade one legacy modular-airport tile to its modern equivalent.
	 *
	 * Small runway pieces become paved runway pieces, a small hangar becomes a
	 * large hangar, and legacy grass becomes apron. The operation fails when the
	 * tile has no upgrade, the modern piece is not available yet, or an aircraft
	 * is currently on the tile.
	 * @param tile The modular airport tile to upgrade.
	 * @pre ScriptMap::IsValidTile(tile).
	 * @pre IsModularAirportTile(tile).
	 * @game @pre ScriptCompanyMode::IsValid().
	 * @exception ScriptError::ERR_OWNED_BY_ANOTHER_COMPANY
	 * @return Whether the tile has been/can be upgraded or not.
	 */
	static bool UpgradeModularAirportTile(TileIndex tile);

	/**
	 * Upgrade legacy modular-airport tiles in a rectangular area to their modern equivalents.
	 *
	 * Small runway pieces become paved runway pieces, small hangars become large
	 * hangars, and legacy grass becomes apron. The operation is atomic: if any
	 * upgradeable tile in the area cannot be converted, for example because an
	 * aircraft occupies it, none of the tiles are changed.
	 * @param start_tile One corner of the area to upgrade.
	 * @param end_tile The opposite corner of the area to upgrade.
	 * @pre ScriptMap::IsValidTile(start_tile).
	 * @pre ScriptMap::IsValidTile(end_tile).
	 * @game @pre ScriptCompanyMode::IsValid().
	 * @exception ScriptError::ERR_OWNED_BY_ANOTHER_COMPANY
	 * @return Whether at least one tile has been/can be upgraded.
	 */
	static bool UpgradeModularAirportArea(TileIndex start_tile, TileIndex end_tile);

	/**
	 * Set the usage flags of a modular runway. The flags apply to the whole
	 * contiguous runway the tile belongs to, not just this tile.
	 * @param tile Any tile of the runway.
	 * @param flags A bitmask of ModularRunwayFlags.
	 * @pre IsModularAirportTile(tile).
	 * @pre flags has at least one of MRF_LANDING and MRF_TAKEOFF.
	 * @pre flags has exactly one of MRF_DIR_LOW and MRF_DIR_HIGH.
	 * @game @pre ScriptCompanyMode::IsValid().
	 * @exception ScriptError::ERR_OWNED_BY_ANOTHER_COMPANY
	 * @return Whether the flags have been/can be set or not.
	 */
	static bool SetModularRunwayFlags(TileIndex tile, SQInteger flags);

	/**
	 * Set the taxi restrictions of a modular taxiway tile. One-way taxiways let
	 * aircraft queue through a taxiway one tile at a time instead of reserving a
	 * whole area, which raises throughput on a busy airport.
	 * @param tile The taxiway tile.
	 * @param dir_mask Bitmask of permitted taxi directions, 15 for all. A one-way
	 *  taxiway must name exactly one direction, and it has to be one the piece
	 *  already allows.
	 * @param one_way Whether the taxiway is one-way.
	 * @pre IsModularAirportTile(tile).
	 * @pre dir_mask >= 0 && dir_mask <= 15.
	 * @pre !one_way || dir_mask has exactly one bit set.
	 * @game @pre ScriptCompanyMode::IsValid().
	 * @exception ScriptError::ERR_OWNED_BY_ANOTHER_COMPANY
	 * @return Whether the flags have been/can be set or not.
	 */
	static bool SetModularTaxiwayFlags(TileIndex tile, SQInteger dir_mask, bool one_way);

	/**
	 * Build a whole modular airport layout in one command.
	 *
	 * The entire layout is checked before anything is built, so this either builds
	 * all of it or none of it. That makes it the safer way to build a new airport;
	 * BuildModularAirportTile() is for growing one that already exists.
	 *
	 * All tiles of the layout must sit at the same height.
	 *
	 * @param tile The tile the layout's (0,0) offset lands on.
	 * @param station_id The station to join, ScriptStation::STATION_NEW or ScriptStation::STATION_JOIN_ADJACENT.
	 * @param rotation Rotation to apply to the whole layout, 0 to 3, clockwise.
	 * @param width Width of the layout in tiles, before rotation.
	 * @param height Height of the layout in tiles, before rotation.
	 * @param layout The layout, as described by ModularLayoutField.
	 * @pre ScriptMap::IsValidTile(tile).
	 * @pre rotation >= 0 && rotation <= 3.
	 * @pre width > 0 && height > 0.
	 * @pre layout.len() > 0 && layout.len() % MLF_STRIDE == 0.
	 * @pre layout.len() / MLF_STRIDE <= 128.
	 * @pre station_id == ScriptStation::STATION_NEW || station_id == ScriptStation::STATION_JOIN_ADJACENT || ScriptStation::IsValidStation(station_id).
	 * @game @pre ScriptCompanyMode::IsValid().
	 * @exception ScriptError::ERR_AREA_NOT_CLEAR
	 * @exception ScriptError::ERR_FLAT_LAND_REQUIRED
	 * @exception ScriptError::ERR_LOCAL_AUTHORITY_REFUSES
	 * @exception ScriptError::ERR_STATION_TOO_SPREAD_OUT
	 * @exception ScriptStation::ERR_STATION_TOO_CLOSE_TO_ANOTHER_STATION
	 * @return Whether the layout has been/can be built or not.
	 * @note MP_SMALL_TERMINAL_3 covers three tiles that join up along one axis, so
	 *  its own rotation field selects which axis and accepts 0 or 1 only. The
	 *  layout's own rotation is a separate thing and turns every tile, which
	 *  these three graphics have no form for, so a layout containing one must be
	 *  placed with rotation 0. Both fail if you try.
	 * @note A tile's availability is checked in rotation 0, so a piece that is
	 *  available there but gated in the rotation the layout gives it -- today
	 *  MP_SMALL_HANGAR in rotation 1 or 2 with the "new airport graphics" setting
	 *  off -- is refused by the command rather than by a precondition.
	 */
	static bool PlaceModularAirportLayout(TileIndex tile, StationID station_id, SQInteger rotation, SQInteger width, SQInteger height, Array<SQInteger> &&layout);

	/**
	 * Get the noise a layout would add to a town, without building it.
	 * @param layout The layout, as described by ModularLayoutField.
	 * @pre layout.len() % MLF_STRIDE == 0.
	 * @return The noise level of the layout, or -1 when the layout is malformed.
	 */
	static SQInteger GetModularLayoutNoiseLevel(Array<SQInteger> &&layout);

	/**
	 * Get the catchment radius a layout would have, without building it.
	 * @param layout The layout, as described by ModularLayoutField.
	 * @pre layout.len() % MLF_STRIDE == 0.
	 * @return The catchment radius in tiles, or -1 when the layout is malformed.
	 */
	static SQInteger GetModularLayoutCatchmentRadius(Array<SQInteger> &&layout);

	/**
	 * Get the monthly maintenance cost a layout would have, without building it.
	 * Directly comparable with GetMonthlyMaintenanceCost() for a stock airport type.
	 * @param layout The layout, as described by ModularLayoutField.
	 * @pre layout.len() % MLF_STRIDE == 0.
	 * @return The maintenance cost per economy-month, or -1 when the layout is malformed.
	 * @see \ref ScriptEconomyTime
	 */
	static Money GetModularLayoutMonthlyMaintenanceCost(Array<SQInteger> &&layout);

	/**
	 * Check whether a layout could serve planes, without building it.
	 * @param layout The layout, as described by ModularLayoutField.
	 * @pre layout.len() % MLF_STRIDE == 0.
	 * @return True if and only if planes can use the layout.
	 */
	static bool GetModularLayoutAcceptsPlanes(Array<SQInteger> &&layout);

	/**
	 * Check whether a layout contains a helipad, without building it.
	 * @param layout The layout, as described by ModularLayoutField.
	 * @pre layout.len() % MLF_STRIDE == 0.
	 * @return True if and only if the layout has at least one helipad.
	 * @note This is not the same question as whether helicopters can use the
	 *  finished airport: one without a helipad may still take them on its apron,
	 *  which depends on where the airport ends up and can only be answered once it
	 *  is built.
	 */
	static bool GetModularLayoutHasHelipad(Array<SQInteger> &&layout);

	/**
	 * Get what a layout would be missing before it is safe for large aircraft,
	 * without building it.
	 * @param layout The layout, as described by ModularLayoutField.
	 * @pre layout.len() % MLF_STRIDE == 0.
	 * @return A bitmask of ModularSafety, MS_OK when nothing is missing, or -1
	 *  when the layout is malformed.
	 */
	static SQInteger GetModularLayoutSafety(Array<SQInteger> &&layout);
};

/** Convert a script-visible modular piece to its airport tile graphic. */
ModularAirportPieceID GetGfxForModularPiece(ScriptAirport::ModularPiece piece);

/** Convert an airport tile graphic to its script-visible modular piece. */
ScriptAirport::ModularPiece GetModularPieceForGfx(ModularAirportPieceID gfx);

#endif /* SCRIPT_AIRPORT_HPP */
