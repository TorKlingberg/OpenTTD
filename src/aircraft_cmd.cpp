/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file aircraft_cmd.cpp This file deals with aircraft and airport movements functionalities. */

#include "stdafx.h"
#include "aircraft.h"
#include "landscape.h"
#include "news_func.h"
#include "newgrf_engine.h"
#include "newgrf_sound.h"
#include "error_func.h"
#include "strings_func.h"
#include "command_func.h"
#include "window_func.h"
#include "timer/timer_game_calendar.h"
#include "timer/timer_game_economy.h"
#include "vehicle_func.h"
#include "sound_func.h"
#include "cheat_type.h"
#include "company_base.h"
#include "ai/ai.hpp"
#include "game/game.hpp"
#include "company_func.h"
#include "effectvehicle_func.h"
#include "station_base.h"
#include "station_map.h"
#include "engine_base.h"
#include "core/random_func.hpp"
#include "core/backup_type.hpp"
#include "zoom_func.h"
#include "disaster_vehicle.h"
#include "newgrf_airporttiles.h"
#include "framerate_type.h"
#include "aircraft_cmd.h"
#include "vehicle_cmd.h"
#include "airport_ground_pathfinder.h"

#include "table/strings.h"
#include "table/airporttile_ids.h"

#include "safeguards.h"

void Aircraft::UpdateDeltaXY()
{
	this->bounds = {{-1, -1, 0}, {2, 2, 0}, {}};

	switch (this->subtype) {
		default: NOT_REACHED();

		case AIR_AIRCRAFT:
		case AIR_HELICOPTER:
			switch (this->state) {
				default: break;
				case ENDTAKEOFF:
				case LANDING:
				case HELILANDING:
				case FLYING:
					/* Bounds are not centred on the aircraft. */
					this->bounds.extent.x = 24;
					this->bounds.extent.y = 24;
					break;
			}
			this->bounds.extent.z = 5;
			break;

		case AIR_SHADOW:
			this->bounds.extent.z = 1;
			this->bounds.origin = {};
			break;

		case AIR_ROTOR:
			this->bounds.extent.z = 1;
			break;
	}
}

static bool AirportMove(Aircraft *v, const AirportFTAClass *apc);
static TileIndex FindNearestModularRunwayTile(const Station *st, const Aircraft *v);
static bool AirportMoveModularLanding(Aircraft *v, const Station *st);
static bool AirportMoveModularTakeoff(Aircraft *v, const Station *st);
static void ClearGroundPathReservation(const std::vector<TileIndex> *path, VehicleID vid);
static TileIndex FindFreeModularHelipad(const Station *st);
static TileIndex FindFreeModularHangar(const Station *st);
static TileIndex FindModularRunwayTileForTakeoff(const Station *st, const Aircraft *v);
static TileIndex FindModularRunwayRolloutPoint(const Station *st, TileIndex landing_tile);
static void AircraftEventHandler_HeliTakeOff(Aircraft *v, const AirportFTAClass *apc);

static constexpr uint8_t MGT_NONE = 0;
static constexpr uint8_t MGT_TERMINAL = 1;
static constexpr uint8_t MGT_HELIPAD = 2;
static constexpr uint8_t MGT_HANGAR = 3;
static constexpr uint8_t MGT_RUNWAY_TAKEOFF = 4;
static constexpr uint8_t MGT_ROLLOUT = 5;
static bool AirportSetBlocks(Aircraft *v, const AirportFTA *current_pos, const AirportFTAClass *apc);
static bool AirportHasBlock(Aircraft *v, const AirportFTA *current_pos, const AirportFTAClass *apc);
static bool AirportFindFreeTerminal(Aircraft *v, const AirportFTAClass *apc);
static bool AirportFindFreeHelipad(Aircraft *v, const AirportFTAClass *apc);
static void CrashAirplane(Aircraft *v);
static TileIndex FindFreeModularTerminal(const Station *st, const Aircraft *v);

static const SpriteID _aircraft_sprite[] = {
	0x0EB5, 0x0EBD, 0x0EC5, 0x0ECD,
	0x0ED5, 0x0EDD, 0x0E9D, 0x0EA5,
	0x0EAD, 0x0EE5, 0x0F05, 0x0F0D,
	0x0F15, 0x0F1D, 0x0F25, 0x0F2D,
	0x0EED, 0x0EF5, 0x0EFD, 0x0F35,
	0x0E9D, 0x0EA5, 0x0EAD, 0x0EB5,
	0x0EBD, 0x0EC5
};

template <>
bool IsValidImageIndex<VEH_AIRCRAFT>(uint8_t image_index)
{
	return image_index < lengthof(_aircraft_sprite);
}

/** Helicopter rotor animation states */
enum HelicopterRotorStates : uint8_t {
	HRS_ROTOR_STOPPED,
	HRS_ROTOR_MOVING_1,
	HRS_ROTOR_MOVING_2,
	HRS_ROTOR_MOVING_3,
};

/**
 * Find the nearest hangar to v
 * StationID::Invalid() is returned, if the company does not have any suitable
 * airports (like helipads only)
 * @param v vehicle looking for a hangar
 * @return the StationID if one is found, otherwise, StationID::Invalid()
 */
static StationID FindNearestHangar(const Aircraft *v)
{
	uint best = 0;
	StationID index = StationID::Invalid();
	TileIndex vtile = TileVirtXY(v->x_pos, v->y_pos);
	const AircraftVehicleInfo *avi = AircraftVehInfo(v->engine_type);
	uint max_range = v->acache.cached_max_range_sqr;

	/* Determine destinations where it's coming from and where it's heading to */
	const Station *last_dest = nullptr;
	const Station *next_dest = nullptr;
	if (max_range != 0) {
		if (v->current_order.IsType(OT_GOTO_STATION) ||
				(v->current_order.IsType(OT_GOTO_DEPOT) && !v->current_order.GetDepotActionType().Test(OrderDepotActionFlag::NearestDepot))) {
			last_dest = Station::GetIfValid(v->last_station_visited);
			next_dest = Station::GetIfValid(v->current_order.GetDestination().ToStationID());
		} else {
			last_dest = GetTargetAirportIfValid(v);
			std::vector<StationID> next_station;
			v->GetNextStoppingStation(next_station);
			if (!next_station.empty()) next_dest = Station::GetIfValid(next_station.back());
		}
	}

	for (const Station *st : Station::Iterate()) {
		if (st->owner != v->owner || !st->facilities.Test(StationFacility::Airport) || !st->airport.HasHangar()) continue;

		const AirportFTAClass *afc = st->airport.GetFTA();

		/* don't crash the plane if we know it can't land at the airport */
		if (afc->flags.Test(AirportFTAClass::Flag::ShortStrip) && (avi->subtype & AIR_FAST) && !_cheats.no_jetcrash.value) continue;

		/* the plane won't land at any helicopter station */
		if (!afc->flags.Test(AirportFTAClass::Flag::Airplanes) && (avi->subtype & AIR_CTOL)) continue;

		/* Check if our last and next destinations can be reached from the depot airport. */
		if (max_range != 0) {
			uint last_dist = (last_dest != nullptr && last_dest->airport.tile != INVALID_TILE) ? DistanceSquare(st->airport.tile, last_dest->airport.tile) : 0;
			uint next_dist = (next_dest != nullptr && next_dest->airport.tile != INVALID_TILE) ? DistanceSquare(st->airport.tile, next_dest->airport.tile) : 0;
			if (last_dist > max_range || next_dist > max_range) continue;
		}

		/* v->tile can't be used here, when aircraft is flying v->tile is set to 0 */
		uint distance = DistanceSquare(vtile, st->airport.tile);
		if (distance < best || index == StationID::Invalid()) {
			best = distance;
			index = st->index;
		}
	}
	return index;
}

void Aircraft::GetImage(Direction direction, EngineImageType image_type, VehicleSpriteSeq *result) const
{
	uint8_t spritenum = this->spritenum;

	if (IsCustomVehicleSpriteNum(spritenum)) {
		GetCustomVehicleSprite(this, direction, image_type, result);
		if (result->IsValid()) return;

		spritenum = this->GetEngine()->original_image_index;
	}

	assert(IsValidImageIndex<VEH_AIRCRAFT>(spritenum));
	result->Set(direction + _aircraft_sprite[spritenum]);
}

void GetRotorImage(const Aircraft *v, EngineImageType image_type, VehicleSpriteSeq *result)
{
	assert(v->subtype == AIR_HELICOPTER);

	const Aircraft *w = v->Next()->Next();
	if (IsCustomVehicleSpriteNum(v->spritenum)) {
		GetCustomRotorSprite(v, image_type, result);
		if (result->IsValid()) return;
	}

	/* Return standard rotor sprites if there are no custom sprites for this helicopter */
	result->Set(SPR_ROTOR_STOPPED + w->state);
}

static void GetAircraftIcon(EngineID engine, EngineImageType image_type, VehicleSpriteSeq *result)
{
	const Engine *e = Engine::Get(engine);
	uint8_t spritenum = e->VehInfo<AircraftVehicleInfo>().image_index;

	if (IsCustomVehicleSpriteNum(spritenum)) {
		GetCustomVehicleIcon(engine, DIR_W, image_type, result);
		if (result->IsValid()) return;

		spritenum = e->original_image_index;
	}

	assert(IsValidImageIndex<VEH_AIRCRAFT>(spritenum));
	result->Set(DIR_W + _aircraft_sprite[spritenum]);
}

void DrawAircraftEngine(int left, int right, int preferred_x, int y, EngineID engine, PaletteID pal, EngineImageType image_type)
{
	VehicleSpriteSeq seq;
	GetAircraftIcon(engine, image_type, &seq);

	Rect rect;
	seq.GetBounds(&rect);
	preferred_x = Clamp(preferred_x,
			left - UnScaleGUI(rect.left),
			right - UnScaleGUI(rect.right));

	seq.Draw(preferred_x, y, pal, pal == PALETTE_CRASH);

	if (!(AircraftVehInfo(engine)->subtype & AIR_CTOL)) {
		VehicleSpriteSeq rotor_seq;
		GetCustomRotorIcon(engine, image_type, &rotor_seq);
		if (!rotor_seq.IsValid()) rotor_seq.Set(SPR_ROTOR_STOPPED);
		rotor_seq.Draw(preferred_x, y - ScaleSpriteTrad(5), PAL_NONE, false);
	}
}

/**
 * Get the size of the sprite of an aircraft sprite heading west (used for lists).
 * @param engine The engine to get the sprite from.
 * @param[out] width The width of the sprite.
 * @param[out] height The height of the sprite.
 * @param[out] xoffs Number of pixels to shift the sprite to the right.
 * @param[out] yoffs Number of pixels to shift the sprite downwards.
 * @param image_type Context the sprite is used in.
 */
void GetAircraftSpriteSize(EngineID engine, uint &width, uint &height, int &xoffs, int &yoffs, EngineImageType image_type)
{
	VehicleSpriteSeq seq;
	GetAircraftIcon(engine, image_type, &seq);

	Rect rect;
	seq.GetBounds(&rect);

	width  = UnScaleGUI(rect.Width());
	height = UnScaleGUI(rect.Height());
	xoffs  = UnScaleGUI(rect.left);
	yoffs  = UnScaleGUI(rect.top);
}

/**
 * Build an aircraft.
 * @param flags    type of operation.
 * @param tile     tile of the depot where aircraft is built.
 * @param e        the engine to build.
 * @param[out] ret the vehicle that has been built.
 * @return the cost of this operation or an error.
 */
CommandCost CmdBuildAircraft(DoCommandFlags flags, TileIndex tile, const Engine *e, Vehicle **ret)
{
	const AircraftVehicleInfo *avi = &e->VehInfo<AircraftVehicleInfo>();
	const Station *st = Station::GetByTile(tile);

	/* Prevent building aircraft types at places which can't handle them */
	if (!CanVehicleUseStation(e->index, st)) return CMD_ERROR;

	/* Make sure all aircraft end up in the first tile of the hangar. */
	if (!st->airport.blocks.Test(AirportBlock::Modular)) {
		tile = st->airport.GetHangarTile(st->airport.GetHangarNum(tile));
	}

	if (flags.Test(DoCommandFlag::Execute)) {
		Aircraft *v = Aircraft::Create(); // aircraft
		Aircraft *u = Aircraft::Create(); // shadow
		*ret = v;

		v->direction = DIR_SE;

		v->owner = u->owner = _current_company;

		v->tile = tile;

		uint x = TileX(tile) * TILE_SIZE + 5;
		uint y = TileY(tile) * TILE_SIZE + 3;

		v->x_pos = u->x_pos = x;
		v->y_pos = u->y_pos = y;

		u->z_pos = GetSlopePixelZ(x, y);
		v->z_pos = u->z_pos + 1;

		v->vehstatus = {VehState::Hidden, VehState::Stopped, VehState::DefaultPalette};
		u->vehstatus = {VehState::Hidden, VehState::Unclickable, VehState::Shadow};

		v->spritenum = avi->image_index;

		v->cargo_cap = avi->passenger_capacity;
		v->refit_cap = 0;
		u->refit_cap = 0;

		v->cargo_type = e->GetDefaultCargoType();
		assert(IsValidCargoType(v->cargo_type));

		CargoType mail = GetCargoTypeByLabel(CT_MAIL);
		if (IsValidCargoType(mail)) {
			u->cargo_type = mail;
			u->cargo_cap = avi->mail_capacity;
		}

		v->name.clear();
		v->last_station_visited = StationID::Invalid();
		v->last_loading_station = StationID::Invalid();

		v->acceleration = avi->acceleration;
		v->engine_type = e->index;
		u->engine_type = e->index;

		v->subtype = (avi->subtype & AIR_CTOL ? AIR_AIRCRAFT : AIR_HELICOPTER);
		v->UpdateDeltaXY();

		u->subtype = AIR_SHADOW;
		u->UpdateDeltaXY();

		v->reliability = e->reliability;
		v->reliability_spd_dec = e->reliability_spd_dec;
		v->max_age = e->GetLifeLengthInDays();

		if (st->airport.blocks.Test(AirportBlock::Modular)) {
			v->pos = 0;
		} else {
			v->pos = GetVehiclePosOnBuild(tile);
		}

		v->state = HANGAR;
		v->previous_pos = v->pos;
		v->targetairport = GetStationIndex(tile);
		v->SetNext(u);

		v->SetServiceInterval(Company::Get(_current_company)->settings.vehicle.servint_aircraft);

		v->date_of_last_service = TimerGameEconomy::date;
		v->date_of_last_service_newgrf = TimerGameCalendar::date;
		v->build_year = u->build_year = TimerGameCalendar::year;

		v->sprite_cache.sprite_seq.Set(SPR_IMG_QUERY);
		u->sprite_cache.sprite_seq.Set(SPR_IMG_QUERY);

		v->random_bits = Random();
		u->random_bits = Random();

		v->vehicle_flags = {};
		if (e->flags.Test(EngineFlag::ExclusivePreview)) v->vehicle_flags.Set(VehicleFlag::BuiltAsPrototype);
		v->SetServiceIntervalIsPercent(Company::Get(_current_company)->settings.vehicle.servint_ispercent);

		v->InvalidateNewGRFCacheOfChain();

		v->cargo_cap = e->DetermineCapacity(v, &u->cargo_cap);

		v->InvalidateNewGRFCacheOfChain();

		UpdateAircraftCache(v, true);

		v->UpdatePosition();
		u->UpdatePosition();

		/* Aircraft with 3 vehicles (chopper)? */
		if (v->subtype == AIR_HELICOPTER) {
			Aircraft *w = Aircraft::Create();
			w->engine_type = e->index;
			w->direction = DIR_N;
			w->owner = _current_company;
			w->x_pos = v->x_pos;
			w->y_pos = v->y_pos;
			w->z_pos = v->z_pos + ROTOR_Z_OFFSET;
			w->vehstatus = {VehState::Hidden, VehState::Unclickable};
			w->spritenum = 0xFF;
			w->subtype = AIR_ROTOR;
			w->sprite_cache.sprite_seq.Set(SPR_ROTOR_STOPPED);
			w->random_bits = Random();
			/* Use rotor's air.state to store the rotor animation frame */
			w->state = HRS_ROTOR_STOPPED;
			w->UpdateDeltaXY();

			u->SetNext(w);
			w->UpdatePosition();
		}
	}

	return CommandCost();
}


ClosestDepot Aircraft::FindClosestDepot()
{
	const Station *st = GetTargetAirportIfValid(this);
	/* If the station is not a valid airport or if it has no hangars */
	if (st == nullptr || !CanVehicleUseStation(this, st) || !st->airport.HasHangar()) {
		/* the aircraft has to search for a hangar on its own */
		StationID station = FindNearestHangar(this);

		if (station == StationID::Invalid()) return ClosestDepot();

		st = Station::Get(station);
	}

	return ClosestDepot(st->xy, st->index);
}

static void CheckIfAircraftNeedsService(Aircraft *v)
{
	if (Company::Get(v->owner)->settings.vehicle.servint_aircraft == 0 || !v->NeedsAutomaticServicing()) return;
	if (v->IsChainInDepot()) {
		VehicleServiceInDepot(v);
		return;
	}

	/* When we're parsing conditional orders and the like
	 * we don't want to consider going to a depot too. */
	if (!v->current_order.IsType(OT_GOTO_DEPOT) && !v->current_order.IsType(OT_GOTO_STATION)) return;

	const Station *st = Station::Get(v->current_order.GetDestination().ToStationID());

	assert(st != nullptr);

	/* only goto depot if the target airport has a depot */
	if (st->airport.HasHangar() && CanVehicleUseStation(v, st)) {
		v->current_order.MakeGoToDepot(st->index, OrderDepotTypeFlag::Service);
		SetWindowWidgetDirty(WC_VEHICLE_VIEW, v->index, WID_VV_START_STOP);
	} else if (v->current_order.IsType(OT_GOTO_DEPOT)) {
		v->current_order.MakeDummy();
		SetWindowWidgetDirty(WC_VEHICLE_VIEW, v->index, WID_VV_START_STOP);
	}
}

Money Aircraft::GetRunningCost() const
{
	const Engine *e = this->GetEngine();
	uint cost_factor = GetVehicleProperty(this, PROP_AIRCRAFT_RUNNING_COST_FACTOR, e->VehInfo<AircraftVehicleInfo>().running_cost);
	return GetPrice(Price::RunningAircraft, cost_factor, e->GetGRF());
}

/** Calendar day handler */
void Aircraft::OnNewCalendarDay()
{
	if (!this->IsNormalAircraft()) return;
	AgeVehicle(this);
}

/** Economy day handler */
void Aircraft::OnNewEconomyDay()
{
	if (!this->IsNormalAircraft()) return;
	EconomyAgeVehicle(this);

	if ((++this->day_counter & 7) == 0) DecreaseVehicleValue(this);

	CheckOrders(this);

	CheckVehicleBreakdown(this);
	CheckIfAircraftNeedsService(this);

	if (this->running_ticks == 0) return;

	CommandCost cost(EXPENSES_AIRCRAFT_RUN, this->GetRunningCost() * this->running_ticks / (CalendarTime::DAYS_IN_YEAR * Ticks::DAY_TICKS));

	this->profit_this_year -= cost.GetCost();
	this->running_ticks = 0;

	SubtractMoneyFromCompanyFract(this->owner, cost);

	SetWindowDirty(WC_VEHICLE_DETAILS, this->index);
	SetWindowClassesDirty(WC_AIRCRAFT_LIST);
}

static void HelicopterTickHandler(Aircraft *v)
{
	Aircraft *u = v->Next()->Next();

	if (u->vehstatus.Test(VehState::Hidden)) return;

	/* if true, helicopter rotors do not rotate. This should only be the case if a helicopter is
	 * loading/unloading at a terminal or stopped */
	if (v->current_order.IsType(OT_LOADING) || v->vehstatus.Test(VehState::Stopped)) {
		if (u->cur_speed != 0) {
			u->cur_speed++;
			if (u->cur_speed >= 0x80 && u->state == HRS_ROTOR_MOVING_3) {
				u->cur_speed = 0;
			}
		}
	} else {
		if (u->cur_speed == 0) {
			u->cur_speed = 0x70;
		}
		if (u->cur_speed >= 0x50) {
			u->cur_speed--;
		}
	}

	int tick = ++u->tick_counter;
	int spd = u->cur_speed >> 4;

	VehicleSpriteSeq seq;
	if (spd == 0) {
		u->state = HRS_ROTOR_STOPPED;
		GetRotorImage(v, EIT_ON_MAP, &seq);
		if (u->sprite_cache.sprite_seq == seq) return;
	} else if (tick >= spd) {
		u->tick_counter = 0;
		u->state++;
		if (u->state > HRS_ROTOR_MOVING_3) u->state = HRS_ROTOR_MOVING_1;
		GetRotorImage(v, EIT_ON_MAP, &seq);
	} else {
		return;
	}

	u->sprite_cache.sprite_seq = seq;

	u->UpdatePositionAndViewport();
}

/**
 * Set aircraft position.
 * @param v Aircraft to position.
 * @param x New X position.
 * @param y New y position.
 * @param z New z position.
 */
void SetAircraftPosition(Aircraft *v, int x, int y, int z)
{
	v->x_pos = x;
	v->y_pos = y;
	v->z_pos = z;

	v->UpdatePosition();
	v->UpdateViewport(true, false);
	if (v->subtype == AIR_HELICOPTER) {
		GetRotorImage(v, EIT_ON_MAP, &v->Next()->Next()->sprite_cache.sprite_seq);
	}

	Aircraft *u = v->Next();

	int safe_x = Clamp(x, 0, Map::MaxX() * TILE_SIZE);
	int safe_y = Clamp(y - 1, 0, Map::MaxY() * TILE_SIZE);
	u->x_pos = x;
	u->y_pos = y - ((v->z_pos - GetSlopePixelZ(safe_x, safe_y)) >> 3);

	safe_y = Clamp(u->y_pos, 0, Map::MaxY() * TILE_SIZE);
	u->z_pos = GetSlopePixelZ(safe_x, safe_y);
	u->sprite_cache.sprite_seq.CopyWithoutPalette(v->sprite_cache.sprite_seq); // the shadow is never coloured

	u->UpdatePositionAndViewport();

	u = u->Next();
	if (u != nullptr) {
		u->x_pos = x;
		u->y_pos = y;
		u->z_pos = z + ROTOR_Z_OFFSET;

		u->UpdatePositionAndViewport();
	}
}

/**
 * Handle Aircraft specific tasks when an Aircraft enters a hangar
 * @param *v Vehicle that enters the hangar
 */
void HandleAircraftEnterHangar(Aircraft *v)
{
	v->subspeed = 0;
	v->progress = 0;

	Aircraft *u = v->Next();
	u->vehstatus.Set(VehState::Hidden);
	u = u->Next();
	if (u != nullptr) {
		u->vehstatus.Set(VehState::Hidden);
		u->cur_speed = 0;
	}

	SetAircraftPosition(v, v->x_pos, v->y_pos, v->z_pos);
}

static void PlayAircraftSound(const Vehicle *v)
{
	if (!PlayVehicleSound(v, VSE_START)) {
		SndPlayVehicleFx(AircraftVehInfo(v->engine_type)->sfx, v);
	}
}


/**
 * Update cached values of an aircraft.
 * Currently caches callback 36 max speed.
 * @param v Vehicle
 * @param update_range Update the aircraft range.
 */
void UpdateAircraftCache(Aircraft *v, bool update_range)
{
	uint max_speed = GetVehicleProperty(v, PROP_AIRCRAFT_SPEED, 0);
	if (max_speed != 0) {
		/* Convert from original units to km-ish/h */
		max_speed = (max_speed * 128) / 10;

		v->vcache.cached_max_speed = max_speed;
	} else {
		/* Use the default max speed of the vehicle. */
		v->vcache.cached_max_speed = AircraftVehInfo(v->engine_type)->max_speed;
	}

	/* Update cargo aging period. */
	v->vcache.cached_cargo_age_period = GetVehicleProperty(v, PROP_AIRCRAFT_CARGO_AGE_PERIOD, EngInfo(v->engine_type)->cargo_age_period);
	Aircraft *u = v->Next(); // Shadow for mail
	u->vcache.cached_cargo_age_period = GetVehicleProperty(u, PROP_AIRCRAFT_CARGO_AGE_PERIOD, EngInfo(u->engine_type)->cargo_age_period);

	/* Update aircraft range. */
	if (update_range) {
		v->acache.cached_max_range = GetVehicleProperty(v, PROP_AIRCRAFT_RANGE, AircraftVehInfo(v->engine_type)->max_range);
		/* Squared it now so we don't have to do it later all the time. */
		v->acache.cached_max_range_sqr = v->acache.cached_max_range * v->acache.cached_max_range;
	}
}


/**
 * Special velocities for aircraft
 */
static constexpr uint16_t SPEED_LIMIT_TAXI = 50; ///< Maximum speed of an aircraft while taxiing
static constexpr uint16_t SPEED_LIMIT_APPROACH = 230; ///< Maximum speed of an aircraft on finals
static constexpr uint16_t SPEED_LIMIT_BROKEN = 320; ///< Maximum speed of an aircraft that is broken
static constexpr uint16_t SPEED_LIMIT_HOLD = 425; ///< Maximum speed of an aircraft that flies the holding pattern
static constexpr uint16_t SPEED_LIMIT_NONE = UINT16_MAX; ///< No environmental speed limit. Speed limit is type dependent

/**
 * Sets the new speed for an aircraft
 * @param v The vehicle for which the speed should be obtained
 * @param speed_limit The maximum speed the vehicle may have.
 * @param hard_limit If true, the limit is directly enforced, otherwise the plane is slowed down gradually
 * @return The number of position updates needed within the tick
 */
static int UpdateAircraftSpeed(Aircraft *v, uint speed_limit = SPEED_LIMIT_NONE, bool hard_limit = true)
{
	/**
	 * 'acceleration' has the unit 3/8 mph/tick. This function is called twice per tick.
	 * So the speed amount we need to accelerate is:
	 *     acceleration * 3 / 16 mph = acceleration * 3 / 16 * 16 / 10 km-ish/h
	 *                               = acceleration * 3 / 10 * 256 * (km-ish/h / 256)
	 *                               ~ acceleration * 77 (km-ish/h / 256)
	 */
	uint spd = v->acceleration * 77;
	uint8_t t;

	/* Adjust speed limits by plane speed factor to prevent taxiing
	 * and take-off speeds being too low. */
	speed_limit *= _settings_game.vehicle.plane_speed;

	/* adjust speed for broken vehicles */
	if (v->vehstatus.Test(VehState::AircraftBroken)) {
		if (SPEED_LIMIT_BROKEN < speed_limit) hard_limit = false;
		speed_limit = std::min<uint>(speed_limit, SPEED_LIMIT_BROKEN);
	}

	if (v->vcache.cached_max_speed < speed_limit) {
		if (v->cur_speed < speed_limit) hard_limit = false;
		speed_limit = v->vcache.cached_max_speed;
	}

	v->subspeed = (t = v->subspeed) + (uint8_t)spd;

	/* Aircraft's current speed is used twice so that very fast planes are
	 * forced to slow down rapidly in the short distance needed. The magic
	 * value 16384 was determined to give similar results to the old speed/48
	 * method at slower speeds. This also results in less reduction at slow
	 * speeds to that aircraft do not get to taxi speed straight after
	 * touchdown. */
	if (!hard_limit && v->cur_speed > speed_limit) {
		speed_limit = v->cur_speed - std::max(1, ((v->cur_speed * v->cur_speed) / 16384) / _settings_game.vehicle.plane_speed);
	}

	spd = std::min(v->cur_speed + (spd >> 8) + (v->subspeed < t), speed_limit);

	/* updates statusbar only if speed have changed to save CPU time */
	if (spd != v->cur_speed) {
		v->cur_speed = spd;
		SetWindowWidgetDirty(WC_VEHICLE_VIEW, v->index, WID_VV_START_STOP);
	}

	/* Adjust distance moved by plane speed setting */
	if (_settings_game.vehicle.plane_speed > 1) spd /= _settings_game.vehicle.plane_speed;

	/* Convert direction-independent speed into direction-dependent speed. (old movement method) */
	spd = v->GetOldAdvanceSpeed(spd);

	spd += v->progress;
	v->progress = (uint8_t)spd;
	return spd >> 8;
}

/**
 * Get the tile height below the aircraft.
 * This function is needed because aircraft can leave the mapborders.
 *
 * @param v The vehicle to get the height for.
 * @return The height in pixels from 'z_pos' 0.
 */
int GetTileHeightBelowAircraft(const Vehicle *v)
{
	int safe_x = Clamp(v->x_pos, 0, Map::MaxX() * TILE_SIZE);
	int safe_y = Clamp(v->y_pos, 0, Map::MaxY() * TILE_SIZE);
	return TileHeight(TileVirtXY(safe_x, safe_y)) * TILE_HEIGHT;
}

/**
 * Get the 'flight level' bounds, in pixels from 'z_pos' 0 for a particular
 * vehicle for normal flight situation.
 * When the maximum is reached the vehicle should consider descending.
 * When the minimum is reached the vehicle should consider ascending.
 *
 * @param v              The vehicle to get the flight levels for.
 * @param[out] min_level The minimum bounds for flight level.
 * @param[out] max_level The maximum bounds for flight level.
 */
void GetAircraftFlightLevelBounds(const Vehicle *v, int *min_level, int *max_level)
{
	int base_altitude = GetTileHeightBelowAircraft(v);
	if (v->type == VEH_AIRCRAFT && Aircraft::From(v)->subtype == AIR_HELICOPTER) {
		base_altitude += HELICOPTER_HOLD_MAX_FLYING_ALTITUDE - PLANE_HOLD_MAX_FLYING_ALTITUDE;
	}

	/* Make sure eastbound and westbound planes do not "crash" into each
	 * other by providing them with vertical separation
	 */
	switch (v->direction) {
		case DIR_N:
		case DIR_NE:
		case DIR_E:
		case DIR_SE:
			base_altitude += 10;
			break;

		default: break;
	}

	/* Make faster planes fly higher so that they can overtake slower ones */
	base_altitude += std::min(20 * (v->vcache.cached_max_speed / 200) - 90, 0);

	if (min_level != nullptr) *min_level = base_altitude + AIRCRAFT_MIN_FLYING_ALTITUDE;
	if (max_level != nullptr) *max_level = base_altitude + AIRCRAFT_MAX_FLYING_ALTITUDE;
}

/**
 * Gets the maximum 'flight level' for the holding pattern of the aircraft,
 * in pixels 'z_pos' 0, depending on terrain below.
 *
 * @param v The aircraft that may or may not need to decrease its altitude.
 * @return Maximal aircraft holding altitude, while in normal flight, in pixels.
 */
int GetAircraftHoldMaxAltitude(const Aircraft *v)
{
	int tile_height = GetTileHeightBelowAircraft(v);

	return tile_height + ((v->subtype == AIR_HELICOPTER) ? HELICOPTER_HOLD_MAX_FLYING_ALTITUDE : PLANE_HOLD_MAX_FLYING_ALTITUDE);
}

template <class T>
int GetAircraftFlightLevel(T *v, bool takeoff)
{
	/* Aircraft is in flight. We want to enforce it being somewhere
	 * between the minimum and the maximum allowed altitude. */
	int aircraft_min_altitude;
	int aircraft_max_altitude;
	GetAircraftFlightLevelBounds(v, &aircraft_min_altitude, &aircraft_max_altitude);
	int aircraft_middle_altitude = (aircraft_min_altitude + aircraft_max_altitude) / 2;

	/* If those assumptions would be violated, aircraft would behave fairly strange. */
	assert(aircraft_min_altitude < aircraft_middle_altitude);
	assert(aircraft_middle_altitude < aircraft_max_altitude);

	int z = v->z_pos;
	if (z < aircraft_min_altitude ||
			(v->flags.Test(VehicleAirFlag::InMinimumHeightCorrection) && z < aircraft_middle_altitude)) {
		/* Ascend. And don't fly into that mountain right ahead.
		 * And avoid our aircraft become a stairclimber, so if we start
		 * correcting altitude, then we stop correction not too early. */
		v->flags.Set(VehicleAirFlag::InMinimumHeightCorrection);
		z += takeoff ? 2 : 1;
	} else if (!takeoff && (z > aircraft_max_altitude ||
			(v->flags.Test(VehicleAirFlag::InMaximumHeightCorrection) && z > aircraft_middle_altitude))) {
		/* Descend lower. You are an aircraft, not an space ship.
		 * And again, don't stop correcting altitude too early. */
		v->flags.Set(VehicleAirFlag::InMaximumHeightCorrection);
		z--;
	} else if (v->flags.Test(VehicleAirFlag::InMinimumHeightCorrection) && z >= aircraft_middle_altitude) {
		/* Now, we have corrected altitude enough. */
		v->flags.Reset(VehicleAirFlag::InMinimumHeightCorrection);
	} else if (v->flags.Test(VehicleAirFlag::InMaximumHeightCorrection) && z <= aircraft_middle_altitude) {
		/* Now, we have corrected altitude enough. */
		v->flags.Reset(VehicleAirFlag::InMaximumHeightCorrection);
	}

	return z;
}

template int GetAircraftFlightLevel(DisasterVehicle *v, bool takeoff);
template int GetAircraftFlightLevel(Aircraft *v, bool takeoff);

/**
 * Find the entry point to an airport depending on direction which
 * the airport is being approached from. Each airport can have up to
 * four entry points for its approach system so that approaching
 * aircraft do not fly through each other or are forced to do 180
 * degree turns during the approach. The arrivals are grouped into
 * four sectors dependent on the DiagDirection from which the airport
 * is approached.
 *
 * @param v   The vehicle that is approaching the airport
 * @param apc The Airport Class being approached.
 * @param rotation The rotation of the airport.
 * @return   The index of the entry point
 */
static uint8_t AircraftGetEntryPoint(const Aircraft *v, const AirportFTAClass *apc, Direction rotation)
{
	assert(v != nullptr);
	assert(apc != nullptr);

	/* In the case the station doesn't exit anymore, set target tile 0.
	 * It doesn't hurt much, aircraft will go to next order, nearest hangar
	 * or it will simply crash in next tick */
	TileIndex tile{};

	const Station *st = Station::GetIfValid(v->targetairport);
	if (st != nullptr) {
		/* Make sure we don't go to INVALID_TILE if the airport has been removed. */
		tile = (st->airport.tile != INVALID_TILE) ? st->airport.tile : st->xy;
	}

	int delta_x = v->x_pos - TileX(tile) * TILE_SIZE;
	int delta_y = v->y_pos - TileY(tile) * TILE_SIZE;

	DiagDirection dir;
	if (abs(delta_y) < abs(delta_x)) {
		/* We are northeast or southwest of the airport */
		dir = delta_x < 0 ? DIAGDIR_NE : DIAGDIR_SW;
	} else {
		/* We are northwest or southeast of the airport */
		dir = delta_y < 0 ? DIAGDIR_NW : DIAGDIR_SE;
	}
	dir = ChangeDiagDir(dir, DiagDirDifference(DIAGDIR_NE, DirToDiagDir(rotation)));
	return apc->entry_points[dir];
}


static void MaybeCrashAirplane(Aircraft *v);

/**
 * Controls the movement of an aircraft. This function actually moves the vehicle
 * on the map and takes care of minor things like sound playback.
 * @todo    De-mystify the cur_speed values for helicopter rotors.
 * @param v The vehicle that is moved. Must be the first vehicle of the chain
 * @return  Whether the position requested by the State Machine has been reached
 */
static bool AircraftController(Aircraft *v)
{
	/* nullptr if station is invalid */
	const Station *st = Station::GetIfValid(v->targetairport);
	/* INVALID_TILE if there is no station */
	TileIndex tile = INVALID_TILE;
	Direction rotation = DIR_N;
	uint size_x = 1, size_y = 1;
	if (st != nullptr) {
		if (st->airport.tile != INVALID_TILE) {
			tile = st->airport.tile;
			rotation = st->airport.rotation;
			size_x = st->airport.w;
			size_y = st->airport.h;
		} else {
			tile = st->xy;
		}
	}
	/* DUMMY if there is no station or no airport */
	const AirportFTAClass *afc = tile == INVALID_TILE ? GetAirport(AT_DUMMY) : st->airport.GetFTA();

	/* prevent going to INVALID_TILE if airport is deleted. */
	if (st == nullptr || st->airport.tile == INVALID_TILE) {
		/* Jump into our "holding pattern" state machine if possible */
		if (v->pos >= afc->nofelements) {
			v->pos = v->previous_pos = AircraftGetEntryPoint(v, afc, DIR_N);
		} else if (v->targetairport != v->current_order.GetDestination()) {
			/* If not possible, just get out of here fast */
			v->state = FLYING;
			UpdateAircraftCache(v);
			AircraftNextAirportPos_and_Order(v);
			/* get aircraft back on running altitude */
			SetAircraftPosition(v, v->x_pos, v->y_pos, GetAircraftFlightLevel(v));
			return false;
		}
	}

	/*  get airport moving data */
	const AirportMovingData amd = RotateAirportMovingData(afc->MovingData(v->pos), rotation, size_x, size_y);

	int x = TileX(tile) * TILE_SIZE;
	int y = TileY(tile) * TILE_SIZE;

	/* Helicopter raise */
	if (amd.flags.Test(AirportMovingDataFlag::HeliRaise)) {
		Aircraft *u = v->Next()->Next();

		/* Make sure the rotors don't rotate too fast */
		if (u->cur_speed > 32) {
			v->cur_speed = 0;
			if (--u->cur_speed == 32) {
				if (!PlayVehicleSound(v, VSE_START)) {
					SoundID sfx = AircraftVehInfo(v->engine_type)->sfx;
					/* For compatibility with old NewGRF we ignore the sfx property, unless a NewGRF-defined sound is used.
					 * The baseset has only one helicopter sound, so this only limits using plane or cow sounds. */
					if (sfx < ORIGINAL_SAMPLE_COUNT) sfx = SND_18_TAKEOFF_HELICOPTER;
					SndPlayVehicleFx(sfx, v);
				}
			}
		} else {
			u->cur_speed = 32;
			int count = UpdateAircraftSpeed(v);
			if (count > 0) {
				v->tile = TileIndex{};

				int z_dest;
				GetAircraftFlightLevelBounds(v, &z_dest, nullptr);

				/* Reached altitude? */
				if (v->z_pos >= z_dest) {
					v->cur_speed = 0;
					return true;
				}
				SetAircraftPosition(v, v->x_pos, v->y_pos, std::min(v->z_pos + count, z_dest));
			}
		}
		return false;
	}

	/* Helicopter landing. */
	if (amd.flags.Test(AirportMovingDataFlag::HeliLower)) {
		v->flags.Set(VehicleAirFlag::HelicopterDirectDescent);

		if (st == nullptr) {
			v->state = FLYING;
			UpdateAircraftCache(v);
			AircraftNextAirportPos_and_Order(v);
			return false;
		}

		/* Vehicle is now at the airport.
		 * Helicopter has arrived at the target landing pad, so the current position is also where it should land.
		 * Except for Oilrigs which are special due to being a 1x1 station, and helicopters land outside it. */
		if (st->airport.type != AT_OILRIG) {
			x = v->x_pos;
			y = v->y_pos;
			tile = TileVirtXY(x, y);
		}
		v->tile = tile;

		/* Find altitude of landing position. */
		int z = GetSlopePixelZ(x, y) + 1 + afc->delta_z;

		if (z == v->z_pos) {
			Vehicle *u = v->Next()->Next();

			/*  Increase speed of rotors. When speed is 80, we've landed. */
			if (u->cur_speed >= 80) {
				v->flags.Reset(VehicleAirFlag::HelicopterDirectDescent);
				return true;
			}
			u->cur_speed += 4;
		} else {
			int count = UpdateAircraftSpeed(v);
			if (count > 0) {
				if (v->z_pos > z) {
					SetAircraftPosition(v, v->x_pos, v->y_pos, std::max(v->z_pos - count, z));
				} else {
					SetAircraftPosition(v, v->x_pos, v->y_pos, std::min(v->z_pos + count, z));
				}
			}
		}
		return false;
	}

	/* Get distance from destination pos to current pos. */
	uint dist = abs(x + amd.x - v->x_pos) + abs(y + amd.y - v->y_pos);

	/* Need exact position? */
	if (!amd.flags.Test(AirportMovingDataFlag::ExactPosition) && dist <= (amd.flags.Test(AirportMovingDataFlag::SlowTurn) ? 8U : 4U)) return true;

	/* At final pos? */
	if (dist == 0) {
		/* Change direction smoothly to final direction. */
		DirDiff dirdiff = DirDifference(amd.direction, v->direction);
		/* if distance is 0, and plane points in right direction, no point in calling
		 * UpdateAircraftSpeed(). So do it only afterwards */
		if (dirdiff == DIRDIFF_SAME) {
			v->cur_speed = 0;
			return true;
		}

		if (!UpdateAircraftSpeed(v, SPEED_LIMIT_TAXI)) return false;

		v->direction = ChangeDir(v->direction, dirdiff > DIRDIFF_REVERSE ? DIRDIFF_45LEFT : DIRDIFF_45RIGHT);
		v->cur_speed >>= 1;

		SetAircraftPosition(v, v->x_pos, v->y_pos, v->z_pos);
		return false;
	}

	if (amd.flags.Test(AirportMovingDataFlag::Brake) && v->cur_speed > SPEED_LIMIT_TAXI * _settings_game.vehicle.plane_speed) {
		MaybeCrashAirplane(v);
		if (v->vehstatus.Test(VehState::Crashed)) return false;
	}

	uint speed_limit = SPEED_LIMIT_TAXI;
	bool hard_limit = true;

	if (amd.flags.Test(AirportMovingDataFlag::NoSpeedClamp)) {
		speed_limit = SPEED_LIMIT_NONE;
	}
	if (amd.flags.Test(AirportMovingDataFlag::Hold)) {
		speed_limit = SPEED_LIMIT_HOLD;
		hard_limit = false;
	}
	if (amd.flags.Test(AirportMovingDataFlag::Land)) {
		speed_limit = SPEED_LIMIT_APPROACH;
		hard_limit = false;
	}
	if (amd.flags.Test(AirportMovingDataFlag::Brake)) {
		speed_limit = SPEED_LIMIT_TAXI;
		hard_limit = false;
	}

	int count = UpdateAircraftSpeed(v, speed_limit, hard_limit);
	if (count == 0) return false;

	/* If the plane will be a few subpixels away from the destination after
	 * this movement loop, start nudging it towards the exact position for
	 * the whole loop. Otherwise, heavily depending on the speed of the plane,
	 * it is possible we totally overshoot the target, causing the plane to
	 * make a loop, and trying again, and again, and again .. */
	bool nudge_towards_target = static_cast<uint>(count) + 3 > dist;

	if (v->turn_counter != 0) v->turn_counter--;

	do {

		GetNewVehiclePosResult gp;

		if (nudge_towards_target || amd.flags.Test(AirportMovingDataFlag::Land)) {
			/* move vehicle one pixel towards target */
			gp.x = (v->x_pos != (x + amd.x)) ?
					v->x_pos + ((x + amd.x > v->x_pos) ? 1 : -1) :
					v->x_pos;
			gp.y = (v->y_pos != (y + amd.y)) ?
					v->y_pos + ((y + amd.y > v->y_pos) ? 1 : -1) :
					v->y_pos;

			/* Oilrigs must keep v->tile as st->airport.tile, since the landing pad is in a non-airport tile */
			gp.new_tile = (st->airport.type == AT_OILRIG) ? st->airport.tile : TileVirtXY(gp.x, gp.y);

		} else {

			/* Turn. Do it slowly if in the air. */
			Direction newdir = GetDirectionTowards(v, x + amd.x, y + amd.y);
			if (newdir != v->direction) {
				if (amd.flags.Test(AirportMovingDataFlag::SlowTurn) && v->number_consecutive_turns < 8 && v->subtype == AIR_AIRCRAFT) {
					if (v->turn_counter == 0 || newdir == v->last_direction) {
						if (newdir == v->last_direction) {
							v->number_consecutive_turns = 0;
						} else {
							v->number_consecutive_turns++;
						}
						v->turn_counter = 2 * _settings_game.vehicle.plane_speed;
						v->last_direction = v->direction;
						v->direction = newdir;
					}

					/* Move vehicle. */
					gp = GetNewVehiclePos(v);
				} else {
					v->cur_speed >>= 1;
					v->direction = newdir;

					/* When leaving a terminal an aircraft often goes to a position
					 * directly in front of it. If it would move while turning it
					 * would need an two extra turns to end up at the correct position.
					 * To make it easier just disallow all moving while turning as
					 * long as an aircraft is on the ground. */
					gp.x = v->x_pos;
					gp.y = v->y_pos;
					gp.new_tile = gp.old_tile = v->tile;
				}
			} else {
				v->number_consecutive_turns = 0;
				/* Move vehicle. */
				gp = GetNewVehiclePos(v);
			}
		}

		v->tile = gp.new_tile;
		/* If vehicle is in the air, use tile coordinate 0. */
		if (amd.flags.Any({AirportMovingDataFlag::Takeoff, AirportMovingDataFlag::SlowTurn, AirportMovingDataFlag::Land})) v->tile = TileIndex{};

		/* Adjust Z for land or takeoff? */
		int z = v->z_pos;

		if (amd.flags.Test(AirportMovingDataFlag::Takeoff)) {
			z = GetAircraftFlightLevel(v, true);
		} else if (amd.flags.Test(AirportMovingDataFlag::Hold)) {
			/* Let the plane drop from normal flight altitude to holding pattern altitude */
			if (z > GetAircraftHoldMaxAltitude(v)) z--;
		} else if (amd.flags.All({AirportMovingDataFlag::SlowTurn, AirportMovingDataFlag::NoSpeedClamp})) {
			z = GetAircraftFlightLevel(v);
		}

		/* NewGRF airports (like a rotated intercontinental from OpenGFX+Airports) can be non-rectangular
		 * and their primary (north-most) tile does not have to be part of the airport.
		 * As such, the height of the primary tile can be different from the rest of the airport.
		 * Given we are landing/breaking, and as such are not a helicopter, we know that there has to be a hangar.
		 * We also know that the airport itself has to be completely flat (otherwise it is not a valid airport).
		 * Therefore, use the height of this hangar to calculate our z-value. */
		int airport_z = v->z_pos;
		if (amd.flags.Any({AirportMovingDataFlag::Land, AirportMovingDataFlag::Brake}) && st != nullptr) {
			assert(st->airport.HasHangar());
			TileIndex hangar_tile = st->airport.GetHangarTile(0);
			airport_z = GetTileMaxPixelZ(hangar_tile) + 1; // To avoid clashing with the shadow
		}

		if (amd.flags.Test(AirportMovingDataFlag::Land)) {
			if (st->airport.blocks.Test(AirportBlock::Zeppeliner)) {
				/* Zeppeliner blocked the runway, abort landing */
				v->state = FLYING;
				UpdateAircraftCache(v);
				SetAircraftPosition(v, gp.x, gp.y, GetAircraftFlightLevel(v));
				v->pos = v->previous_pos;
				continue;
			}

			if (st->airport.tile == INVALID_TILE) {
				/* Airport has been removed, abort the landing procedure */
				v->state = FLYING;
				UpdateAircraftCache(v);
				AircraftNextAirportPos_and_Order(v);
				/* get aircraft back on running altitude */
				SetAircraftPosition(v, gp.x, gp.y, GetAircraftFlightLevel(v));
				continue;
			}

			/* We're not flying below our destination, right? */
			assert(airport_z <= z);
			int t = std::max(1U, dist - 4);
			int delta = z - airport_z;

			/* Only start lowering when we're sufficiently close for a 1:1 glide */
			if (delta >= t) {
				z -= CeilDiv(z - airport_z, t);
			}
			if (z < airport_z) z = airport_z;
		}

		/* We've landed. Decrease speed when we're reaching end of runway. */
		if (amd.flags.Test(AirportMovingDataFlag::Brake)) {

			if (z > airport_z) {
				z--;
			} else if (z < airport_z) {
				z++;
			}

		}

		SetAircraftPosition(v, gp.x, gp.y, z);
	} while (--count != 0);
	return false;
}

/**
 * Handle crashed aircraft \a v.
 * @param v Crashed aircraft.
 */
static bool HandleCrashedAircraft(Aircraft *v)
{
	v->crashed_counter += 3;

	Station *st = GetTargetAirportIfValid(v);

	/* make aircraft crash down to the ground */
	if (v->crashed_counter < 500 && st == nullptr && ((v->crashed_counter % 3) == 0)) {
		int z = GetSlopePixelZ(Clamp(v->x_pos, 0, Map::MaxX() * TILE_SIZE), Clamp(v->y_pos, 0, Map::MaxY() * TILE_SIZE));
		v->z_pos -= 1;
		if (v->z_pos <= z) {
			v->crashed_counter = 500;
			v->z_pos = z + 1;
		} else {
			v->crashed_counter = 0;
		}
		SetAircraftPosition(v, v->x_pos, v->y_pos, v->z_pos);
	}

	if (v->crashed_counter < 650) {
		uint32_t r;
		if (Chance16R(1, 32, r)) {
			static const DirDiff delta[] = {
				DIRDIFF_45LEFT, DIRDIFF_SAME, DIRDIFF_SAME, DIRDIFF_45RIGHT
			};

			v->direction = ChangeDir(v->direction, delta[GB(r, 16, 2)]);
			SetAircraftPosition(v, v->x_pos, v->y_pos, v->z_pos);
			r = Random();
			CreateEffectVehicleRel(v,
				GB(r, 0, 4) - 4,
				GB(r, 4, 4) - 4,
				GB(r, 8, 4),
				EV_EXPLOSION_SMALL);
		}
	} else if (v->crashed_counter >= 10000) {
		/*  remove rubble of crashed airplane */

		/* clear runway-in on all airports, set by crashing plane
		 * small airports use AirportBlock::AirportBusy, city airports use AirportBlock::RunwayInOut, etc.
		 * but they all share the same number */
		if (st != nullptr) {
			st->airport.blocks.Reset(AirportBlock::RunwayIn);
			st->airport.blocks.Reset(AirportBlock::RunwayInOut); // commuter airport
			st->airport.blocks.Reset(AirportBlock::RunwayIn2);    // intercontinental
		}

		delete v;

		return false;
	}

	return true;
}


/**
 * Handle smoke of broken aircraft.
 * @param v Aircraft
 * @param mode Is this the non-first call for this vehicle in this tick?
 */
static void HandleAircraftSmoke(Aircraft *v, bool mode)
{
	static const struct {
		int8_t x;
		int8_t y;
	} smoke_pos[] = {
		{  5,  5 },
		{  6,  0 },
		{  5, -5 },
		{  0, -6 },
		{ -5, -5 },
		{ -6,  0 },
		{ -5,  5 },
		{  0,  6 }
	};

	if (!v->vehstatus.Test(VehState::AircraftBroken)) return;

	/* Stop smoking when landed */
	if (v->cur_speed < 10) {
		v->vehstatus.Reset(VehState::AircraftBroken);
		v->breakdown_ctr = 0;
		return;
	}

	/* Spawn effect et most once per Tick, i.e. !mode */
	if (!mode && (v->tick_counter & 0x0F) == 0) {
		CreateEffectVehicleRel(v,
			smoke_pos[v->direction].x,
			smoke_pos[v->direction].y,
			2,
			EV_BREAKDOWN_SMOKE_AIRCRAFT
		);
	}
}

void HandleMissingAircraftOrders(Aircraft *v)
{
	/*
	 * We do not have an order. This can be divided into two cases:
	 * 1) we are heading to an invalid station. In this case we must
	 *    find another airport to go to. If there is nowhere to go,
	 *    we will destroy the aircraft as it otherwise will enter
	 *    the holding pattern for the first airport, which can cause
	 *    the plane to go into an undefined state when building an
	 *    airport with the same StationID.
	 * 2) we are (still) heading to a (still) valid airport, then we
	 *    can continue going there. This can happen when you are
	 *    changing the aircraft's orders while in-flight or in for
	 *    example a depot. However, when we have a current order to
	 *    go to a depot, we have to keep that order so the aircraft
	 *    actually stops.
	 */
	const Station *st = GetTargetAirportIfValid(v);
	if (st == nullptr) {
		Backup<CompanyID> cur_company(_current_company, v->owner);
		CommandCost ret = Command<CMD_SEND_VEHICLE_TO_DEPOT>::Do(DoCommandFlag::Execute, v->index, DepotCommandFlag{}, {});
		cur_company.Restore();

		if (ret.Failed()) CrashAirplane(v);
	} else if (!v->current_order.IsType(OT_GOTO_DEPOT)) {
		v->current_order.Free();
	}
}


TileIndex Aircraft::GetOrderStationLocation(StationID)
{
	/* Orders are changed in flight, ensure going to the right station. */
	if (this->state == FLYING) {
		AircraftNextAirportPos_and_Order(this);
	}

	/* Aircraft do not use dest-tile */
	return TileIndex{};
}

void Aircraft::MarkDirty()
{
	this->colourmap = PAL_NONE;
	this->UpdateViewport(true, false);
	if (this->subtype == AIR_HELICOPTER) {
		GetRotorImage(this, EIT_ON_MAP, &this->Next()->Next()->sprite_cache.sprite_seq);
	}
}


uint Aircraft::Crash(bool flooded)
{
	uint victims = Vehicle::Crash(flooded) + 2; // pilots
	this->crashed_counter = flooded ? 9000 : 0; // max 10000, disappear pretty fast when flooded

	/* Clean up modular airport ground path */
	ClearGroundPathReservation(this->ground_path, this->index);
	delete this->ground_path;
	this->ground_path = nullptr;
	this->modular_landing_tile = INVALID_TILE;
	this->modular_landing_stage = 0;
	this->modular_takeoff_tile = INVALID_TILE;
	this->modular_takeoff_progress = 0;
	this->modular_ground_target = 0;
	this->ground_path_last_tile = INVALID_TILE;
	this->ground_path_stall_counter = 0;

	return victims;
}

/**
 * Bring the aircraft in a crashed state, create the explosion animation, and create a news item about the crash.
 * @param v Aircraft that crashed.
 */
static void CrashAirplane(Aircraft *v)
{
	CreateEffectVehicleRel(v, 4, 4, 8, EV_EXPLOSION_LARGE);

	uint victims = v->Crash();

	v->cargo.Truncate();
	v->Next()->cargo.Truncate();
	const Station *st = GetTargetAirportIfValid(v);
	TileIndex vt = TileVirtXY(v->x_pos, v->y_pos);

	EncodedString headline;
	if (st == nullptr) {
		headline = GetEncodedString(STR_NEWS_PLANE_CRASH_OUT_OF_FUEL, victims);
	} else {
		headline = GetEncodedString(STR_NEWS_AIRCRAFT_CRASH, victims, st->index);
	}

	AI::NewEvent(v->owner, new ScriptEventVehicleCrashed(v->index, vt, st == nullptr ? ScriptEventVehicleCrashed::CRASH_AIRCRAFT_NO_AIRPORT : ScriptEventVehicleCrashed::CRASH_PLANE_LANDING, victims, v->owner));
	Game::NewEvent(new ScriptEventVehicleCrashed(v->index, vt, st == nullptr ? ScriptEventVehicleCrashed::CRASH_AIRCRAFT_NO_AIRPORT : ScriptEventVehicleCrashed::CRASH_PLANE_LANDING, victims, v->owner));

	NewsType newstype = NewsType::Accident;
	if (v->owner != _local_company) {
		newstype = NewsType::AccidentOther;
	}

	AddTileNewsItem(std::move(headline), newstype, vt, st != nullptr ? st->index : StationID::Invalid());

	ModifyStationRatingAround(vt, v->owner, -160, 30);
	if (_settings_client.sound.disaster) SndPlayVehicleFx(SND_12_EXPLOSION, v);
}

/**
 * Decide whether aircraft \a v should crash.
 * @param v Aircraft to test.
 */
static void MaybeCrashAirplane(Aircraft *v)
{

	Station *st = Station::Get(v->targetairport);

	uint32_t prob;
	if (st->airport.GetFTA()->flags.Test(AirportFTAClass::Flag::ShortStrip) &&
			(AircraftVehInfo(v->engine_type)->subtype & AIR_FAST) &&
			!_cheats.no_jetcrash.value) {
		prob = 3276;
	} else {
		if (_settings_game.vehicle.plane_crashes == 0) return;
		prob = (0x4000 << _settings_game.vehicle.plane_crashes) / 1500;
	}

	if (GB(Random(), 0, 22) > prob) return;

	/* Crash the airplane. Remove all goods stored at the station. */
	for (GoodsEntry &ge : st->goods) {
		ge.rating = 1;
		if (ge.HasData()) ge.GetData().cargo.Truncate();
	}

	CrashAirplane(v);
}

/**
 * Aircraft arrives at a terminal. If it is the first aircraft, throw a party.
 * Start loading cargo.
 * @param v Aircraft that arrived.
 */
static void AircraftEntersTerminal(Aircraft *v)
{
	if (v->current_order.IsType(OT_GOTO_DEPOT)) return;

	Station *st = Station::Get(v->targetairport);
	v->last_station_visited = v->targetairport;

	/* Check if station was ever visited before */
	if (!(st->had_vehicle_of_type & HVOT_AIRCRAFT)) {
		st->had_vehicle_of_type |= HVOT_AIRCRAFT;
		/* show newsitem of celebrating citizens */
		AddVehicleNewsItem(
			GetEncodedString(STR_NEWS_FIRST_AIRCRAFT_ARRIVAL, st->index),
			(v->owner == _local_company) ? NewsType::ArrivalCompany : NewsType::ArrivalOther,
			v->index,
			st->index
		);
		AI::NewEvent(v->owner, new ScriptEventStationFirstVehicle(st->index, v->index));
		Game::NewEvent(new ScriptEventStationFirstVehicle(st->index, v->index));
	}

	v->BeginLoading();
}

/**
 * Aircraft touched down at the landing strip.
 * @param v Aircraft that landed.
 */
static void AircraftLandAirplane(Aircraft *v)
{
	Station *st = Station::Get(v->targetairport);

	TileIndex vt = TileVirtXY(v->x_pos, v->y_pos);

	v->UpdateDeltaXY();

	TriggerAirportTileAnimation(st, vt, AirportAnimationTrigger::AirplaneTouchdown);

	if (!PlayVehicleSound(v, VSE_TOUCHDOWN)) {
		SndPlayVehicleFx(SND_17_SKID_PLANE, v);
	}
}


/** set the right pos when heading to other airports after takeoff */
void AircraftNextAirportPos_and_Order(Aircraft *v)
{
	if (v->current_order.IsType(OT_GOTO_STATION) || v->current_order.IsType(OT_GOTO_DEPOT)) {
		v->targetairport = v->current_order.GetDestination().ToStationID();
	}

	const Station *st = GetTargetAirportIfValid(v);
	const AirportFTAClass *apc = st == nullptr ? GetAirport(AT_DUMMY) : st->airport.GetFTA();
	Direction rotation = st == nullptr ? DIR_N : st->airport.rotation;
	v->pos = v->previous_pos = AircraftGetEntryPoint(v, apc, rotation);
}

/**
 * Aircraft is about to leave the hangar.
 * @param v Aircraft leaving.
 * @param exit_dir The direction the vehicle leaves the hangar.
 * @note This function is called in AfterLoadGame for old savegames, so don't rely
 *       on any data to be valid, especially don't rely on the fact that the vehicle
 *       is actually on the ground inside a depot.
 */
void AircraftLeaveHangar(Aircraft *v, Direction exit_dir)
{
	v->cur_speed = 0;
	v->subspeed = 0;
	v->progress = 0;
	v->direction = exit_dir;
	v->vehstatus.Reset(VehState::Hidden);
	{
		Vehicle *u = v->Next();
		u->vehstatus.Reset(VehState::Hidden);

		/* Rotor blades */
		u = u->Next();
		if (u != nullptr) {
			u->vehstatus.Reset(VehState::Hidden);
			u->cur_speed = 80;
		}
	}

	VehicleServiceInDepot(v);
	v->LeaveUnbunchingDepot();
	SetAircraftPosition(v, v->x_pos, v->y_pos, v->z_pos);
	InvalidateWindowData(WC_VEHICLE_DEPOT, v->tile);
	SetWindowClassesDirty(WC_AIRCRAFT_LIST);
}

////////////////////////////////////////////////////////////////////////////////
///////////////////   AIRCRAFT MOVEMENT SCHEME  ////////////////////////////////
////////////////////////////////////////////////////////////////////////////////
static void AircraftEventHandler_EnterTerminal(Aircraft *v, const AirportFTAClass *apc)
{
	AircraftEntersTerminal(v);
	v->state = apc->layout[v->pos].heading;
}

/**
 * Aircraft arrived in an airport hangar.
 * @param v Aircraft in the hangar.
 * @param apc Airport description containing the hangar.
 */
static void AircraftEventHandler_EnterHangar(Aircraft *v, const AirportFTAClass *apc)
{
	VehicleEnterDepot(v);
	v->state = apc->layout[v->pos].heading;
}

static Direction GetModularHangarExitDirection(const Station *st, TileIndex tile)
{
	/* Hangar sprite is fixed to face SE. Force exit direction to match. */
	return DIR_SE;
}

/**
 * Handle aircraft movement/decision making in an airport hangar.
 * @param v Aircraft in the hangar.
 * @param apc Airport description containing the hangar.
 */
static void AircraftEventHandler_InHangar(Aircraft *v, const AirportFTAClass *apc)
{
	/* if we just arrived, execute EnterHangar first */
	if (v->previous_pos != v->pos) {
		AircraftEventHandler_EnterHangar(v, apc);
		return;
	}

	/* if we were sent to the depot, stay there */
	if (v->current_order.IsType(OT_GOTO_DEPOT) && v->vehstatus.Test(VehState::Stopped)) {
		v->current_order.Free();
		return;
	}

	/* Check if we should wait here for unbunching. */
	if (v->IsWaitingForUnbunching()) return;

	if (!v->current_order.IsType(OT_GOTO_STATION) &&
			!v->current_order.IsType(OT_GOTO_DEPOT))
		return;

	/* We are leaving a hangar, but have to go to the exact same one; re-enter */
	if (v->current_order.IsType(OT_GOTO_DEPOT) && v->current_order.GetDestination() == v->targetairport) {
		VehicleEnterDepot(v);
		return;
	}

	const Station *st = Station::GetByTile(v->tile);
	if (st->airport.blocks.Test(AirportBlock::Modular)) {
		bool at_target = v->current_order.GetDestination() == v->targetairport;
		Direction exit_dir = GetModularHangarExitDirection(st, v->tile);

		if (at_target) {
			TileIndex goal = INVALID_TILE;
			uint8_t target = MGT_NONE;
			if (v->subtype == AIR_HELICOPTER) {
				goal = FindFreeModularHelipad(st);
				target = (goal != INVALID_TILE) ? MGT_HELIPAD : MGT_TERMINAL;
			}
			if (goal == INVALID_TILE) {
				goal = FindFreeModularTerminal(st, v);
				target = MGT_TERMINAL;
			}

			if (goal != INVALID_TILE) {
				v->ground_path_goal = goal;
				v->modular_ground_target = target;
				AircraftLeaveHangar(v, exit_dir);
				return;
			}
		} else {
			if (v->subtype == AIR_HELICOPTER) {
				AircraftLeaveHangar(v, exit_dir);
				AircraftEventHandler_HeliTakeOff(v, apc);
				return;
			}

			TileIndex runway = FindModularRunwayTileForTakeoff(st, v);
			if (runway != INVALID_TILE) {
				v->ground_path_goal = runway;
				v->modular_ground_target = MGT_RUNWAY_TAKEOFF;
				AircraftLeaveHangar(v, exit_dir);
				return;
			}
		}
	}

	/* if the block of the next position is busy, stay put */
	if (AirportHasBlock(v, &apc->layout[v->pos], apc)) return;

	/* We are already at the target airport, we need to find a terminal */
	if (v->current_order.GetDestination() == v->targetairport) {
		/* FindFreeTerminal:
		 * 1. Find a free terminal, 2. Occupy it, 3. Set the vehicle's state to that terminal */
		if (v->subtype == AIR_HELICOPTER) {
			if (!AirportFindFreeHelipad(v, apc)) return; // helicopter
		} else {
			if (!AirportFindFreeTerminal(v, apc)) return; // airplane
		}
	} else { // Else prepare for launch.
		/* airplane goto state takeoff, helicopter to helitakeoff */
		v->state = (v->subtype == AIR_HELICOPTER) ? HELITAKEOFF : TAKEOFF;
	}
	AircraftLeaveHangar(v, st->airport.GetHangarExitDirection(v->tile));
	AirportMove(v, apc);
}

/** At one of the Airport's Terminals */
static void AircraftEventHandler_AtTerminal(Aircraft *v, const AirportFTAClass *apc)
{
	/* if we just arrived, execute EnterTerminal first */
	if (v->previous_pos != v->pos) {
		AircraftEventHandler_EnterTerminal(v, apc);
		/* on an airport with helipads, a helicopter will always land there
		 * and get serviced at the same time - setting */
		if (_settings_game.order.serviceathelipad) {
			if (v->subtype == AIR_HELICOPTER && apc->num_helipads > 0) {
				/* an excerpt of ServiceAircraft, without the invisibility stuff */
				v->date_of_last_service = TimerGameEconomy::date;
				v->date_of_last_service_newgrf = TimerGameCalendar::date;
				v->breakdowns_since_last_service = 0;
				v->reliability = v->GetEngine()->reliability;
				SetWindowDirty(WC_VEHICLE_DETAILS, v->index);
			}
		}
		return;
	}

	if (v->current_order.IsType(OT_NOTHING)) return;

	/* Modular airport logic */
	if (Station::Get(v->targetairport)->airport.blocks.Test(AirportBlock::Modular)) {
		const Station *st = Station::Get(v->targetairport);
		if (v->modular_ground_target != MGT_NONE) return;

		bool go_to_hangar = false;
		switch (v->current_order.GetType()) {
			case OT_GOTO_STATION: // ready to fly to another airport
				break;
			case OT_GOTO_DEPOT:   // visit hangar for servicing, sale, etc.
				go_to_hangar = v->current_order.GetDestination() == v->targetairport;
				break;
			case OT_CONDITIONAL:
				return;
			default:  // orders have been deleted (no orders), goto depot and don't bother us
				v->current_order.Free();
				go_to_hangar = true;
		}

		if (v->subtype == AIR_HELICOPTER && !go_to_hangar) {
			AircraftEventHandler_HeliTakeOff(v, apc);
			return;
		}

		TileIndex goal = INVALID_TILE;
		uint8_t target = MGT_NONE;
		if (go_to_hangar) {
			goal = FindFreeModularHangar(st);
			target = MGT_HANGAR;
		} else {
			goal = FindModularRunwayTileForTakeoff(st, v);
			target = MGT_RUNWAY_TAKEOFF;
		}

		if (goal != INVALID_TILE) {
			v->ground_path_goal = goal;
			v->modular_ground_target = target;
			Debug(misc, 3, "[ModAp] Vehicle {} found takeoff runway tile {}", v->index, goal.base());
			return;
		}
		Debug(misc, 3, "[ModAp] Vehicle {} failed to find takeoff runway", v->index);
		return;
	}

	/* if the block of the next position is busy, stay put */
	if (AirportHasBlock(v, &apc->layout[v->pos], apc)) return;

	/* airport-road is free. We either have to go to another airport, or to the hangar
	 * ---> start moving */

	bool go_to_hangar = false;
	switch (v->current_order.GetType()) {
		case OT_GOTO_STATION: // ready to fly to another airport
			break;
		case OT_GOTO_DEPOT:   // visit hangar for servicing, sale, etc.
			go_to_hangar = v->current_order.GetDestination() == v->targetairport;
			break;
		case OT_CONDITIONAL:
			/* In case of a conditional order we just have to wait a tick
			 * longer, so the conditional order can actually be processed;
			 * we should not clear the order as that makes us go nowhere. */
			return;
		default:  // orders have been deleted (no orders), goto depot and don't bother us
			v->current_order.Free();
			go_to_hangar = true;
	}

	if (go_to_hangar && Station::Get(v->targetairport)->airport.HasHangar()) {
		v->state = HANGAR;
	} else {
		/* airplane goto state takeoff, helicopter to helitakeoff */
		v->state = (v->subtype == AIR_HELICOPTER) ? HELITAKEOFF : TAKEOFF;
	}

	AirportMove(v, apc);
}

static void AircraftEventHandler_General(Aircraft *, const AirportFTAClass *)
{
	FatalError("OK, you shouldn't be here, check your Airport Scheme!");
}

static void AircraftEventHandler_TakeOff(Aircraft *v, const AirportFTAClass *)
{
	PlayAircraftSound(v); // play takeoffsound for airplanes
	v->state = STARTTAKEOFF;
}

static void AircraftEventHandler_StartTakeOff(Aircraft *v, const AirportFTAClass *)
{
	v->state = ENDTAKEOFF;
	v->UpdateDeltaXY();
}

static void AircraftEventHandler_EndTakeOff(Aircraft *v, const AirportFTAClass *)
{
	v->state = FLYING;
	/* get the next position to go to, differs per airport */
	AircraftNextAirportPos_and_Order(v);
}

static void AircraftEventHandler_HeliTakeOff(Aircraft *v, const AirportFTAClass *)
{
	v->state = FLYING;
	v->UpdateDeltaXY();

	/* get the next position to go to, differs per airport */
	AircraftNextAirportPos_and_Order(v);

	/* Send the helicopter to a hangar if needed for replacement */
	if (v->NeedsAutomaticServicing()) {
		Backup<CompanyID> cur_company(_current_company, v->owner);
		Command<CMD_SEND_VEHICLE_TO_DEPOT>::Do(DoCommandFlag::Execute, v->index, DepotCommandFlag::Service, {});
		cur_company.Restore();
	}
}

static void AircraftEventHandler_Flying(Aircraft *v, const AirportFTAClass *apc)
{
	Station *st = Station::Get(v->targetairport);

	bool closed = st->airport.blocks.Test(AirportBlock::AirportClosed);
	bool is_modular = st->airport.blocks.Test(AirportBlock::Modular);
	if (is_modular) {
		if (closed) Debug(misc, 5, "[ModAp] airport {} flagged closed; ignoring for landing", st->index);
		closed = false;
		Debug(misc, 3, "[ModAp] Vehicle {} in FLYING state at modular airport {}", v->index, st->index);
	}

	/* Runway busy, not allowed to use this airstation or closed, circle. */
	if (CanVehicleUseStation(v, st) && (st->owner == OWNER_NONE || st->owner == v->owner) && !closed) {
		if (is_modular) {
			Debug(misc, 3, "[ModAp] Vehicle {} passed access checks, checking for landing", v->index);
		}
		/* For modular airports, handle landing differently */
		if (st->airport.blocks.Test(AirportBlock::Modular)) {
			/* Find runway for modular airport */
			TileIndex runway_tile = FindNearestModularRunwayTile(st, v);
			Debug(misc, 3, "[ModAp] Vehicle {} approaching modular airport {}, runway_tile={}", v->index, st->index, runway_tile.base());
			
			if (runway_tile != INVALID_TILE) {
				/* Check distance - only land if close enough (e.g. 50 tiles) */
				/* Always use pixel coordinates as v->tile is 0 when flying */
				int dist = (abs(v->x_pos - TileX(runway_tile) * TILE_SIZE) + abs(v->y_pos - TileY(runway_tile) * TILE_SIZE)) / TILE_SIZE;

				if (dist < 50) {
					/* Start landing sequence */
					uint8_t landingtype = (v->subtype == AIR_HELICOPTER) ? HELILANDING : LANDING;
					v->modular_landing_tile = runway_tile;
					v->modular_landing_stage = 0;
					v->state = landingtype; // LANDING / HELILANDING
					if (v->state == HELILANDING) v->flags.Set(VehicleAirFlag::HelicopterDirectDescent);
					Debug(misc, 3, "[ModAp] Vehicle {} starting landing, state={}", v->index, v->state);
					/* Don't set v->pos for modular airports - not used */
					return;
				}
			}
			
			/* Too far or no runway found, keep flying */
			Debug(misc, 3, "[ModAp] Vehicle {} keeping FLYING state (dist or no runway)", v->index);
			v->state = FLYING;
			return;
		}

		/* Traditional FTA logic for preset airports */
		/* {32,FLYING,AirportBlock::Nothing,37}, {32,LANDING,N,33}, {32,HELILANDING,N,41},
		 * if it is an airplane, look for LANDING, for helicopter HELILANDING
		 * it is possible to choose from multiple landing runways, so loop until a free one is found */
		uint8_t landingtype = (v->subtype == AIR_HELICOPTER) ? HELILANDING : LANDING;
		const AirportFTA *current = apc->layout[v->pos].next.get();
		while (current != nullptr) {
			if (current->heading == landingtype) {
				/* save speed before, since if AirportHasBlock is false, it resets them to 0
				 * we don't want that for plane in air
				 * hack for speed thingy */
				uint16_t tcur_speed = v->cur_speed;
				uint16_t tsubspeed = v->subspeed;
				if (!AirportHasBlock(v, current, apc)) {
					v->state = landingtype; // LANDING / HELILANDING
					if (v->state == HELILANDING) v->flags.Set(VehicleAirFlag::HelicopterDirectDescent);
					/* it's a bit dirty, but I need to set position to next position, otherwise
					 * if there are multiple runways, plane won't know which one it took (because
					 * they all have heading LANDING). And also occupy that block! */
					v->pos = current->next_position;
					st->airport.blocks.Set(apc->layout[v->pos].blocks);
					return;
				}
				v->cur_speed = tcur_speed;
				v->subspeed = tsubspeed;
			}
			current = current->next.get();
		}
	} else if (is_modular) {
		Debug(misc, 3, "[ModAp] Vehicle {} failed access checks (can't land), staying in FLYING", v->index);
	}
	v->state = FLYING;
	/* Don't update v->pos for modular airports */
	if (!is_modular) {
		v->pos = apc->layout[v->pos].next_position;
	}
}

static void AircraftEventHandler_Landing(Aircraft *v, const AirportFTAClass *)
{
	v->state = ENDLANDING;
	AircraftLandAirplane(v);  // maybe crash airplane

	/* check if the aircraft needs to be replaced or renewed and send it to a hangar if needed */
	if (v->NeedsAutomaticServicing()) {
		Backup<CompanyID> cur_company(_current_company, v->owner);
		Command<CMD_SEND_VEHICLE_TO_DEPOT>::Do(DoCommandFlag::Execute, v->index, DepotCommandFlag::Service, {});
		cur_company.Restore();
	}
}

static void AircraftEventHandler_HeliLanding(Aircraft *v, const AirportFTAClass *)
{
	v->state = HELIENDLANDING;
	v->UpdateDeltaXY();
}

static void AircraftEventHandler_EndLanding(Aircraft *v, const AirportFTAClass *apc)
{
	const Station *st = Station::Get(v->targetairport);

	/* Check if this is a modular airport */
	if (st->airport.blocks.Test(AirportBlock::Modular)) {
		bool wants_depot = v->current_order.IsType(OT_GOTO_DEPOT);
		TileIndex goal = INVALID_TILE;
		uint8_t target = MGT_NONE;

		if (v->subtype == AIR_HELICOPTER && !wants_depot) {
			goal = FindFreeModularHelipad(st);
			target = MGT_HELIPAD;
		}

		if (goal == INVALID_TILE && wants_depot) {
			goal = FindFreeModularHangar(st);
			target = MGT_HANGAR;
		}

		if (goal == INVALID_TILE) {
			goal = FindFreeModularTerminal(st, v);
			target = MGT_TERMINAL;
		}

		if (goal != INVALID_TILE) {
			v->ground_path_goal = goal;
			v->modular_ground_target = target;
			v->state = (target == MGT_HANGAR) ? HANGAR : TERM1;
			return;
		}

		/* No suitable destination found - stay in place for now */
		return;
	}

	/* Traditional FTA logic for preset airports */
	/* next block busy, don't do a thing, just wait */
	if (AirportHasBlock(v, &apc->layout[v->pos], apc)) return;

	/* if going to terminal (OT_GOTO_STATION) choose one
	 * 1. in case all terminals are busy AirportFindFreeTerminal() returns false or
	 * 2. not going for terminal (but depot, no order),
	 * --> get out of the way to the hangar. */
	if (v->current_order.IsType(OT_GOTO_STATION)) {
		if (AirportFindFreeTerminal(v, apc)) return;
	}
	v->state = HANGAR;

}

static void AircraftEventHandler_HeliEndLanding(Aircraft *v, const AirportFTAClass *apc)
{
	/*  next block busy, don't do a thing, just wait */
	if (AirportHasBlock(v, &apc->layout[v->pos], apc)) return;

	/* if going to helipad (OT_GOTO_STATION) choose one. If airport doesn't have helipads, choose terminal
	 * 1. in case all terminals/helipads are busy (AirportFindFreeHelipad() returns false) or
	 * 2. not going for terminal (but depot, no order),
	 * --> get out of the way to the hangar IF there are terminals on the airport.
	 * --> else TAKEOFF
	 * the reason behind this is that if an airport has a terminal, it also has a hangar. Airplanes
	 * must go to a hangar. */
	if (v->current_order.IsType(OT_GOTO_STATION)) {
		if (AirportFindFreeHelipad(v, apc)) return;
	}
	v->state = Station::Get(v->targetairport)->airport.HasHangar() ? HANGAR : HELITAKEOFF;
}

/**
 * Signature of the aircraft handler function.
 * @param v Aircraft to handle.
 * @param apc Airport state machine.
 */
typedef void AircraftStateHandler(Aircraft *v, const AirportFTAClass *apc);
/** Array of handler functions for each target of the aircraft. */
static AircraftStateHandler * const _aircraft_state_handlers[] = {
	AircraftEventHandler_General,        // TO_ALL         =  0
	AircraftEventHandler_InHangar,       // HANGAR         =  1
	AircraftEventHandler_AtTerminal,     // TERM1          =  2
	AircraftEventHandler_AtTerminal,     // TERM2          =  3
	AircraftEventHandler_AtTerminal,     // TERM3          =  4
	AircraftEventHandler_AtTerminal,     // TERM4          =  5
	AircraftEventHandler_AtTerminal,     // TERM5          =  6
	AircraftEventHandler_AtTerminal,     // TERM6          =  7
	AircraftEventHandler_AtTerminal,     // HELIPAD1       =  8
	AircraftEventHandler_AtTerminal,     // HELIPAD2       =  9
	AircraftEventHandler_TakeOff,        // TAKEOFF        = 10
	AircraftEventHandler_StartTakeOff,   // STARTTAKEOFF   = 11
	AircraftEventHandler_EndTakeOff,     // ENDTAKEOFF     = 12
	AircraftEventHandler_HeliTakeOff,    // HELITAKEOFF    = 13
	AircraftEventHandler_Flying,         // FLYING         = 14
	AircraftEventHandler_Landing,        // LANDING        = 15
	AircraftEventHandler_EndLanding,     // ENDLANDING     = 16
	AircraftEventHandler_HeliLanding,    // HELILANDING    = 17
	AircraftEventHandler_HeliEndLanding, // HELIENDLANDING = 18
	AircraftEventHandler_AtTerminal,     // TERM7          = 19
	AircraftEventHandler_AtTerminal,     // TERM8          = 20
	AircraftEventHandler_AtTerminal,     // HELIPAD3       = 21
};

static void AirportClearBlock(const Aircraft *v, const AirportFTAClass *apc)
{
	/* we have left the previous block, and entered the new one. Free the previous block */
	if (apc->layout[v->previous_pos].blocks != apc->layout[v->pos].blocks) {
		Station *st = Station::Get(v->targetairport);

		if (st->airport.blocks.Test(AirportBlock::Zeppeliner) &&
				apc->layout[v->previous_pos].blocks == AirportBlock::RunwayIn) {
			return;
		}

		st->airport.blocks.Reset(apc->layout[v->previous_pos].blocks);
	}
}

static bool IsModularRunwayPiece(uint8_t gfx)
{
	switch (gfx) {
		case APT_RUNWAY_1:
		case APT_RUNWAY_2:
		case APT_RUNWAY_3:
		case APT_RUNWAY_4:
		case APT_RUNWAY_5:
		case APT_RUNWAY_END:
		case APT_RUNWAY_SMALL_NEAR_END:
		case APT_RUNWAY_SMALL_MIDDLE:
		case APT_RUNWAY_SMALL_FAR_END:
			return true;
		default:
			return false;
	}
}

static TileIndex GetRunwayOtherEnd(const Station *st, TileIndex start_tile)
{
	const ModularAirportTileData *data = st->airport.GetModularTileData(start_tile);
	if (data == nullptr) return start_tile;

	bool horizontal = (data->rotation % 2) == 0;
	TileIndexDiff diff = horizontal ? TileDiffXY(1, 0) : TileDiffXY(0, 1);
	
	/* Determine direction by checking which neighbor is also runway */
	TileIndex check = start_tile + diff;
	const ModularAirportTileData *check_data = st->airport.GetModularTileData(check);
	if (check_data == nullptr || !IsModularRunwayPiece(check_data->piece_type)) {
		diff = -diff; /* Go the other way */
	}

	TileIndex current = start_tile;
	TileIndex next = current + diff;
	
	/* Walk until we find the end */
	while (true) {
		const ModularAirportTileData *next_data = st->airport.GetModularTileData(next);
		if (next_data == nullptr || !IsModularRunwayPiece(next_data->piece_type)) {
			return current;
		}
		current = next;
		next = current + diff;
	}
}

static TileIndex FindNearestModularRunwayTile(const Station *st, const Aircraft *v)
{
	if (st->airport.modular_tile_data == nullptr) return INVALID_TILE;

	TileIndex best_tile = INVALID_TILE;
	int best_score = INT_MAX;

	TileIndex term_tile = FindFreeModularTerminal(st, v);
	bool has_term = (term_tile != INVALID_TILE);
	int term_x = has_term ? TileX(term_tile) * TILE_SIZE : 0;
	int term_y = has_term ? TileY(term_tile) * TILE_SIZE : 0;

	for (const ModularAirportTileData &data : *st->airport.modular_tile_data) {
		if (!IsModularRunwayPiece(data.piece_type)) continue;

		/* Only consider runway ends as valid landing targets */
		bool is_end = (data.piece_type == APT_RUNWAY_END || data.piece_type == APT_RUNWAY_SMALL_NEAR_END || data.piece_type == APT_RUNWAY_SMALL_FAR_END);
		if (!is_end) continue;

		int cx = TileX(data.tile) * TILE_SIZE + TILE_SIZE / 2;
		int cy = TileY(data.tile) * TILE_SIZE + TILE_SIZE / 2;
		int dist_flight = abs(cx - v->x_pos) + abs(cy - v->y_pos);

		int score = dist_flight;

		if (has_term) {
			/* Calculate distance from the *other* end of the runway to the terminal.
			   This favors landings that roll out towards the terminal. */
			TileIndex other_end = GetRunwayOtherEnd(st, data.tile);
			int end_x = TileX(other_end) * TILE_SIZE;
			int end_y = TileY(other_end) * TILE_SIZE;
			int dist_taxi = abs(end_x - term_x) + abs(end_y - term_y);

			/* Penalize taxi distance more because taxiing is slower than flying. */
			score = dist_flight + dist_taxi * 4;
		}

		if (score < best_score) {
			best_score = score;
			best_tile = data.tile;
		}
	}

	return best_tile;
}

static void GetModularLandingApproachPoint(const Station *st, TileIndex runway_tile, int *target_x, int *target_y)
{
	/* Default to runway tile center */
	*target_x = TileX(runway_tile) * TILE_SIZE + TILE_SIZE / 2;
	*target_y = TileY(runway_tile) * TILE_SIZE + TILE_SIZE / 2;

	const ModularAirportTileData *data = st->airport.GetModularTileData(runway_tile);
	if (data == nullptr) return;

	bool horizontal = (data->rotation % 2) == 0; // 0=X-axis (NW-SE), 1=Y-axis (NE-SW)
	int approach_dist = 15 * TILE_SIZE; // 15 tiles out

	/* Determine which way is "out" by checking neighbors or rotation */
	/* If horizontal (X-axis), check X+1 and X-1 */
	/* If vertical (Y-axis), check Y+1 and Y-1 */

	bool runway_extends_positive = false;
	bool runway_extends_negative = false;

	if (horizontal) {
		/* Check X axis neighbors */
		TileIndex next = runway_tile + TileDiffXY(1, 0);
		TileIndex prev = runway_tile - TileDiffXY(1, 0);
		const ModularAirportTileData *next_data = st->airport.GetModularTileData(next);
		const ModularAirportTileData *prev_data = st->airport.GetModularTileData(prev);

		if (next_data != nullptr && IsModularRunwayPiece(next_data->piece_type)) runway_extends_positive = true;
		if (prev_data != nullptr && IsModularRunwayPiece(prev_data->piece_type)) runway_extends_negative = true;
	} else {
		/* Check Y axis neighbors */
		TileIndex next = runway_tile + TileDiffXY(0, 1);
		TileIndex prev = runway_tile - TileDiffXY(0, 1);
		const ModularAirportTileData *next_data = st->airport.GetModularTileData(next);
		const ModularAirportTileData *prev_data = st->airport.GetModularTileData(prev);

		if (next_data != nullptr && IsModularRunwayPiece(next_data->piece_type)) runway_extends_positive = true;
		if (prev_data != nullptr && IsModularRunwayPiece(prev_data->piece_type)) runway_extends_negative = true;
	}

	/* If we are at the negative end (runway extends positive), approach from negative */
	if (runway_extends_positive && !runway_extends_negative) {
		if (horizontal) *target_x -= approach_dist; else *target_y -= approach_dist;
	}
	/* If we are at the positive end (runway extends negative), approach from positive */
	else if (!runway_extends_positive && runway_extends_negative) {
		if (horizontal) *target_x += approach_dist; else *target_y += approach_dist;
	}
	/* Single tile or middle piece? Use rotation to guess typical approach (from "left/top") */
	else {
		if (horizontal) *target_x -= approach_dist; else *target_y -= approach_dist;
	}
}

static bool AirportMoveModularLanding(Aircraft *v, const Station *st)
{
	if (v->modular_landing_tile == INVALID_TILE) {
		v->modular_landing_tile = FindNearestModularRunwayTile(st, v);
		v->modular_landing_stage = 0;
		if (v->modular_landing_tile == INVALID_TILE) {
			Debug(misc, 3, "[ModAp] no runway tile found for landing at station {}", st->index);
			return false;
		}
		Debug(misc, 3, "[ModAp] Vehicle {} starting approach to runway tile {}, pos=({},{},{})", v->index, v->modular_landing_tile.base(), v->x_pos, v->y_pos, v->z_pos);
	}

	int target_x, target_y;
	int airport_z = GetTileMaxPixelZ(v->modular_landing_tile) + 1;
	int target_z = airport_z;

	if (v->modular_landing_stage == 0) {
		/* Approach phase: fly to FAF */
		GetModularLandingApproachPoint(st, v->modular_landing_tile, &target_x, &target_y);
		target_z = airport_z + 20 * 5; // Stay high
	} else {
		/* Final phase: fly to threshold */
		target_x = TileX(v->modular_landing_tile) * TILE_SIZE + TILE_SIZE / 2;
		target_y = TileY(v->modular_landing_tile) * TILE_SIZE + TILE_SIZE / 2;
	}

	/* Calculate distance to target */
	int dist = abs(v->x_pos - target_x) + abs(v->y_pos - target_y);

	/* Move one pixel towards target */
	int new_x = (v->x_pos != target_x) ? v->x_pos + ((target_x > v->x_pos) ? 1 : -1) : v->x_pos;
	int new_y = (v->y_pos != target_y) ? v->y_pos + ((target_y > v->y_pos) ? 1 : -1) : v->y_pos;

	/* Update direction */
	if (new_x != v->x_pos || new_y != v->y_pos) {
		v->direction = GetDirectionTowards(v, target_x, target_y);
	}

	/* Aircraft in air has no tile */
	v->tile = TileIndex{};

	/* Altitude logic */
	int z = v->z_pos;
	if (v->modular_landing_stage == 0) {
		/* Maintain approach altitude */
		/* Simple seek towards target_z */
		if (z < target_z) z++; else if (z > target_z) z--;
	} else {
		/* Glide slope for final */
		if (z > airport_z) {
			int t = std::max(1, dist - 4);
			int delta = z - airport_z;
			if (delta >= t) {
				z -= CeilDiv(z - airport_z, t);
			}
		}
	}

	SetAircraftPosition(v, new_x, new_y, z);

	Debug(misc, 5, "[ModAp] Vehicle {} landing stage {}: pos=({},{},{}), target=({},{},{}), dist={}",
		v->index, v->modular_landing_stage, v->x_pos, v->y_pos, v->z_pos, target_x, target_y, target_z, dist);

	/* Check if reached target */
	if (v->x_pos == target_x && v->y_pos == target_y) {
		if (v->modular_landing_stage == 0) {
			/* Reached FAF, switch to final */
			v->modular_landing_stage = 1;
		} else {
			/* Reached threshold, land and start rollout */
			Debug(misc, 3, "[ModAp] Vehicle {} touchdown at ({},{},{})", v->index, target_x, target_y, airport_z);
			v->tile = v->modular_landing_tile;

			/* Set up rollout phase - taxi along runway to opposite end */
			TileIndex rollout_point = FindModularRunwayRolloutPoint(st, v->modular_landing_tile);

			v->modular_landing_tile = INVALID_TILE;
			v->modular_landing_stage = 0;

			AircraftEventHandler_Landing(v, st->airport.GetFTA());

			/* If we have a rollout point, go there first, otherwise go directly to terminal */
			if (rollout_point != INVALID_TILE) {
				Debug(misc, 3, "[ModAp] Vehicle {} starting rollout to tile {}", v->index, rollout_point.base());
				v->ground_path_goal = rollout_point;
				v->modular_ground_target = MGT_ROLLOUT;
				v->state = TERM1;
			} else {
				/* No rollout, go directly to terminal */
				AircraftEventHandler_EndLanding(v, st->airport.GetFTA());
			}
			return true;
		}
	}

	return false;
}

static bool AirportMoveModularTakeoff(Aircraft *v, const Station *st)
{
	if (v->modular_takeoff_tile == INVALID_TILE) return false;

	const ModularAirportTileData *data = st->airport.GetModularTileData(v->modular_takeoff_tile);
	if (data == nullptr) return false;

	const bool horizontal = (data->rotation % 2) == 0;

	if (v->modular_takeoff_progress == 0) {
		/* Determine takeoff direction by finding the other end of the runway */
		TileIndex end_tile = GetRunwayOtherEnd(st, v->modular_takeoff_tile);
		int end_x = TileX(end_tile) * TILE_SIZE + TILE_SIZE / 2;
		int end_y = TileY(end_tile) * TILE_SIZE + TILE_SIZE / 2;
		
		/* If single tile runway, end_tile == start_tile. 
		   Fallback to rotation-based direction if we can't determine direction from length. */
		if (end_tile == v->modular_takeoff_tile) {
			Direction dir = horizontal ? DIR_SE : DIR_SW;
			v->direction = dir;
		} else {
			v->direction = GetDirectionTowards(v, end_x, end_y);
		}
		
		PlayAircraftSound(v);
	}

	/* Accelerate and move */
	int count = UpdateAircraftSpeed(v, SPEED_LIMIT_NONE);
	for (int i = 0; i < count; i++) {
		/* Move forward along runway */
		GetNewVehiclePosResult gp = GetNewVehiclePos(v);

		/* Calculate altitude - gradual climb starting after initial acceleration */
		int z = v->z_pos;
		int target_z = GetAircraftFlightLevel(v, true);

		/* Start climbing after 1 tile of acceleration, climb at ~1.5 pixels per tile traveled */
		if (v->modular_takeoff_progress > TILE_SIZE) {
			int climb_progress = v->modular_takeoff_progress - TILE_SIZE;
			int desired_altitude = GetTileMaxPixelZ(v->modular_takeoff_tile) + 1 + (climb_progress * 3 / 2);

			if (z < std::min(desired_altitude, target_z)) {
				z = std::min(desired_altitude, target_z);
			}
		}

		/* Use SetAircraftPosition for proper viewport and shadow updates */
		SetAircraftPosition(v, gp.x, gp.y, z);

		/* Update tile reference */
		TileIndex current_tile = TileVirtXY(v->x_pos, v->y_pos);
		const ModularAirportTileData *tile_data = st->airport.GetModularTileData(current_tile);
		bool on_runway = (tile_data != nullptr && IsModularRunwayPiece(tile_data->piece_type));

		if (on_runway) {
			v->tile = current_tile;
		} else {
			v->tile = TileIndex{};  /* In air */
		}

		v->modular_takeoff_progress++;

		/* Continue takeoff for at least 12 tiles to match stock airport behavior */
		/* Stock airports have planes continue in takeoff direction for some distance */
		if (v->modular_takeoff_progress > TILE_SIZE * 12 && v->z_pos >= target_z) {
			Debug(misc, 3, "[ModAp] Vehicle {} takeoff complete, transitioning to FLYING", v->index);
			v->state = FLYING;
			v->modular_takeoff_tile = INVALID_TILE;
			v->modular_takeoff_progress = 0;
			v->tile = TileIndex{};
			AircraftNextAirportPos_and_Order(v);
			return true;
		}
	}

	return false;
}

/**
 * Find a free modular terminal for an aircraft.
 * @param st The station.
 * @param v The aircraft.
 * @return Terminal tile or INVALID_TILE if none found.
 */
static TileIndex FindModularRunwayRolloutPoint(const Station *st, TileIndex landing_tile)
{
	/* Find a rollout point along the runway towards the NW end */
	if (st->airport.modular_tile_data == nullptr) return INVALID_TILE;

	/* Get all runway tiles */
	std::vector<TileIndex> runway_tiles;
	for (const ModularAirportTileData &data : *st->airport.modular_tile_data) {
		if (IsModularRunwayPiece(data.piece_type)) {
			runway_tiles.push_back(data.tile);
		}
	}

	if (runway_tiles.empty()) return INVALID_TILE;

	/* Find the NW end (min X+Y) */
	TileIndex nw_end = runway_tiles[0];
	int min_sum = TileX(nw_end) + TileY(nw_end);

	for (TileIndex tile : runway_tiles) {
		int sum = TileX(tile) + TileY(tile);
		if (sum < min_sum) {
			min_sum = sum;
			nw_end = tile;
		}
	}

	/* If we landed at the NW end, no rollout needed */
	if (nw_end == landing_tile) return INVALID_TILE;

	/* Find a runway tile about 60-70% of the way towards NW end */
	int land_sum = TileX(landing_tile) + TileY(landing_tile);
	int target_sum = land_sum - (land_sum - min_sum) * 2 / 3;

	TileIndex best_tile = nw_end;
	int best_diff = INT_MAX;

	for (TileIndex tile : runway_tiles) {
		int sum = TileX(tile) + TileY(tile);
		/* Only consider tiles between landing and NW end */
		if (sum >= min_sum && sum <= land_sum) {
			int diff = abs(sum - target_sum);
			if (diff < best_diff) {
				best_diff = diff;
				best_tile = tile;
			}
		}
	}

	Debug(misc, 3, "[ModAp] Rollout point: landing={} ({},{}), nw_end={} ({},{}), rollout={} ({},{})",
		landing_tile.base(), TileX(landing_tile), TileY(landing_tile),
		nw_end.base(), TileX(nw_end), TileY(nw_end),
		best_tile.base(), TileX(best_tile), TileY(best_tile));

	return (best_tile != landing_tile) ? best_tile : INVALID_TILE;
}

static TileIndex FindFreeModularTerminal(const Station *st, [[maybe_unused]] const Aircraft *v)
{
	if (st->airport.modular_tile_data == nullptr) return INVALID_TILE;

	/* Terminal piece types: APT_STAND, APT_STAND_1 */
	/* TODO: Also support hangars (9,10) and helipads (11) */
	for (const ModularAirportTileData &data : *st->airport.modular_tile_data) {
		if (data.piece_type == APT_STAND || data.piece_type == APT_STAND_1) {
			/* Check if tile is free */
			Tile t(data.tile);
			if (HasAirportTileReservation(t)) {
				/* If reserved by us, it's fine (we might be re-evaluating) */
				if (v != nullptr && GetAirportTileReserver(t) == v->index) return data.tile;
				continue;
			}
			return data.tile;
		}
	}

	return INVALID_TILE;
}

static TileIndex FindFreeModularHelipad(const Station *st)
{
	if (st->airport.modular_tile_data == nullptr) return INVALID_TILE;

	for (const ModularAirportTileData &data : *st->airport.modular_tile_data) {
		switch (data.piece_type) {
			case APT_HELIPAD_1:
			case APT_HELIPAD_2:
			case APT_HELIPAD_2_FENCE_NW:
			case APT_HELIPAD_2_FENCE_NE_SE:
			case APT_HELIPAD_3_FENCE_SE_SW:
			case APT_HELIPAD_3_FENCE_NW_SW:
			case APT_HELIPAD_3_FENCE_NW:
				return data.tile;
			default:
				break;
		}
	}

	return INVALID_TILE;
}

static TileIndex FindFreeModularHangar(const Station *st)
{
	if (st->airport.modular_tile_data == nullptr) return INVALID_TILE;

	for (const ModularAirportTileData &data : *st->airport.modular_tile_data) {
		if (data.piece_type == APT_DEPOT_SE || data.piece_type == APT_SMALL_DEPOT_SE) {
			return data.tile;
		}
	}

	return INVALID_TILE;
}

static TileIndex FindModularRunwayTileForTakeoff(const Station *st, [[maybe_unused]] const Aircraft *v)
{
	/* For takeoff, always use the same end as landing (bottom-left = SE end).
	   This matches stock airport behavior. */
	if (st->airport.modular_tile_data == nullptr) return INVALID_TILE;

	/* Find all runway tiles */
	std::vector<TileIndex> runway_tiles;
	for (const ModularAirportTileData &data : *st->airport.modular_tile_data) {
		if (IsModularRunwayPiece(data.piece_type)) {
			runway_tiles.push_back(data.tile);
		}
	}

	if (runway_tiles.empty()) return INVALID_TILE;

	/* Find the SE end (max X+Y) - this is the bottom-left visually, same as landing */
	TileIndex max_tile = runway_tiles[0];
	int max_sum = TileX(max_tile) + TileY(max_tile);

	for (TileIndex tile : runway_tiles) {
		int sum = TileX(tile) + TileY(tile);
		if (sum > max_sum) {
			max_sum = sum;
			max_tile = tile;
		}
	}

	return max_tile;
}

static void ClearGroundPathReservation(const std::vector<TileIndex> *path, VehicleID vid)
{
	if (path == nullptr) return;
	for (TileIndex tile : *path) {
		Tile t(tile);
		if (!IsAirport(t)) continue;
		if (HasAirportTileReservation(t) && GetAirportTileReserver(t) == vid) {
			SetAirportTileReservation(t, false);
		}
	}
}

static bool TryReserveGroundPath(const std::vector<TileIndex> &path, VehicleID vid)
{
	for (TileIndex tile : path) {
		Tile t(tile);
		if (!IsAirport(t)) continue;
		if (HasAirportTileReservation(t) && GetAirportTileReserver(t) != vid) {
			return false;
		}
	}

	for (TileIndex tile : path) {
		Tile t(tile);
		if (!IsAirport(t)) continue;
		SetAirportTileReservation(t, true);
		SetAirportTileReserver(t, vid);
	}

	return true;
}

static void HandleModularGroundArrival(Aircraft *v)
{
	const Station *st = Station::Get(v->targetairport);

	switch (v->modular_ground_target) {
		case MGT_ROLLOUT:
			/* Completed rollout along runway, now find a terminal */
			Debug(misc, 3, "[ModAp] Vehicle {} completed rollout, finding terminal", v->index);
			{
				bool wants_depot = v->current_order.IsType(OT_GOTO_DEPOT);
				TileIndex goal = INVALID_TILE;
				uint8_t target = MGT_NONE;

				if (v->subtype == AIR_HELICOPTER && !wants_depot) {
					goal = FindFreeModularHelipad(st);
					target = MGT_HELIPAD;
				}

				if (goal == INVALID_TILE && wants_depot) {
					goal = FindFreeModularHangar(st);
					target = MGT_HANGAR;
				}

				if (goal == INVALID_TILE) {
					goal = FindFreeModularTerminal(st, v);
					target = MGT_TERMINAL;
				}

				if (goal != INVALID_TILE) {
					v->ground_path_goal = goal;
					v->modular_ground_target = target;
					v->state = (target == MGT_HANGAR) ? HANGAR : TERM1;
				}
			}
			break;

		case MGT_TERMINAL:
		case MGT_HELIPAD:
			AircraftEntersTerminal(v);
			v->state = (v->subtype == AIR_HELICOPTER) ? HELIPAD1 : TERM1;
			v->modular_ground_target = MGT_NONE;
			break;

		case MGT_HANGAR:
			Debug(misc, 3, "[ModAp] Vehicle {} entering hangar at tile {}", v->index, v->tile.base());
			VehicleEnterDepot(v);
			v->state = HANGAR;
			v->modular_ground_target = MGT_NONE;
			break;

		case MGT_RUNWAY_TAKEOFF:
			v->state = TAKEOFF;
			v->modular_takeoff_tile = v->tile;
			v->modular_takeoff_progress = 0;
			v->modular_ground_target = MGT_NONE;
			break;

		default:
			v->modular_ground_target = MGT_NONE;
			break;
	}

	/* Keep the arrival tile reserved if it's a parking spot */
	if (v->modular_ground_target == MGT_TERMINAL || v->modular_ground_target == MGT_HELIPAD || v->modular_ground_target == MGT_HANGAR) {
		Tile t(v->tile);
		if (IsAirport(t)) {
			SetAirportTileReservation(t, true);
			SetAirportTileReserver(t, v->index);
		}
	}
}

static void LogModularAirportLayout(const Station *st)
{
	if (st->airport.modular_tile_data == nullptr) return;
	Debug(misc, 3, "[ModAp] --- Layout for modular airport {} ---", st->index);
	for (const ModularAirportTileData &data : *st->airport.modular_tile_data) {
		Debug(misc, 3, "[ModAp] Tile {}: pos=({},{}), gfx={}, rot={}, user_mask={:x}, auto_mask={:x}",
			data.tile.base(), TileX(data.tile), TileY(data.tile), data.piece_type, data.rotation, 
			data.user_taxi_dir_mask, data.auto_taxi_dir_mask);
	}
	Debug(misc, 3, "[ModAp] --- End Layout ---");
}

/**
 * Move aircraft on modular airport ground path.
 * @param v The aircraft.
 * @param st The station.
 * @return True if reached destination.
 */
static bool AirportMoveModular(Aircraft *v, const Station *st)
{
	/* If no goal set, we're done */
	if (v->ground_path_goal == INVALID_TILE) return true;

	/* If we don't have a path yet, calculate one */
	if (v->ground_path == nullptr) {
		AirportGroundPath path = FindAirportGroundPath(st, v->tile, v->ground_path_goal, v);
		if (!path.found) {
			/* No path found - stay where we are for now */
			Debug(misc, 3, "[ModAp] Vehicle {} pathfinding failed from {} to {}", v->index, v->tile.base(), v->ground_path_goal.base());
			LogModularAirportLayout(st);
			/* TODO: Handle this better - maybe try alternate terminals */
			return false;
		}

		if (!TryReserveGroundPath(path.tiles, v->index)) {
			return false;
		}

		v->ground_path = new std::vector<TileIndex>(path.tiles);
		v->ground_path_index = 0;
		v->ground_path_last_tile = v->tile;
		v->ground_path_stall_counter = 0;
	}

	/* Follow the path */
	if (v->ground_path_index >= v->ground_path->size()) {
		/* Reached end of path */
		ClearGroundPathReservation(v->ground_path, v->index);
		delete v->ground_path;
		v->ground_path = nullptr;
		v->ground_path_index = 0;
		v->ground_path_goal = INVALID_TILE;
		v->ground_path_last_tile = INVALID_TILE;
		v->ground_path_stall_counter = 0;
		HandleModularGroundArrival(v);
		return true;
	}

	/* Move to next tile in path */
	TileIndex next_tile = (*v->ground_path)[v->ground_path_index];
	Tile next(next_tile);
	if (HasAirportTileReservation(next) && GetAirportTileReserver(next) != v->index) {
		v->ground_path_stall_counter++;
		if (v->ground_path_stall_counter > 50) {
			ClearGroundPathReservation(v->ground_path, v->index);
			delete v->ground_path;
			v->ground_path = nullptr;
			v->ground_path_index = 0;
			v->ground_path_goal = INVALID_TILE;
			v->ground_path_last_tile = INVALID_TILE;
			v->ground_path_stall_counter = 0;
		}
		return false;
	}

	const int target_x = TileX(next_tile) * TILE_SIZE + TILE_SIZE / 2;
	const int target_y = TileY(next_tile) * TILE_SIZE + TILE_SIZE / 2;
	const int dist = abs(v->x_pos - target_x) + abs(v->y_pos - target_y);

	if (dist > 0) {
		v->direction = GetDirectionTowards(v, target_x, target_y);
		int count = UpdateAircraftSpeed(v, SPEED_LIMIT_TAXI);
		if (count > 0) {
			do {
				GetNewVehiclePosResult gp = GetNewVehiclePos(v);
				v->x_pos = gp.x;
				v->y_pos = gp.y;
				SetAircraftPosition(v, v->x_pos, v->y_pos, v->z_pos);

				int new_dist = abs(v->x_pos - target_x) + abs(v->y_pos - target_y);
				if (new_dist == 0) break;
			} while (--count > 0);
		}
	}

	if (v->x_pos == target_x && v->y_pos == target_y) {
		v->tile = next_tile;
		v->ground_path_index++;
		v->ground_path_last_tile = v->tile;
		v->ground_path_stall_counter = 0;

		if (v->ground_path_index > 1) {
			TileIndex prev_tile = (*v->ground_path)[v->ground_path_index - 2];
			Tile prev(prev_tile);
			if (HasAirportTileReservation(prev) && GetAirportTileReserver(prev) == v->index) {
				SetAirportTileReservation(prev, false);
			}
		}

		if (next_tile == v->ground_path_goal) {
			ClearGroundPathReservation(v->ground_path, v->index);
			delete v->ground_path;
			v->ground_path = nullptr;
			v->ground_path_index = 0;
			v->ground_path_goal = INVALID_TILE;
			v->ground_path_last_tile = INVALID_TILE;
			v->ground_path_stall_counter = 0;
			HandleModularGroundArrival(v);
			return true;
		}
	}

	return false;
}

static void AirportMoveModularFlying(Aircraft *v, const Station *st)
{
	/* Target is the station center or runway */
	TileIndex target = st->airport.tile;
	if (target == INVALID_TILE) target = st->xy;

	/* Try to target the approach point of the nearest runway */
	TileIndex runway = FindNearestModularRunwayTile(st, v);
	int target_x, target_y;
	if (runway != INVALID_TILE) {
		GetModularLandingApproachPoint(st, runway, &target_x, &target_y);
	} else {
		target_x = TileX(target) * TILE_SIZE + TILE_SIZE / 2;
		target_y = TileY(target) * TILE_SIZE + TILE_SIZE / 2;
	}

	int dist = abs(v->x_pos - target_x) + abs(v->y_pos - target_y);

	if (dist > 0) {
		v->direction = GetDirectionTowards(v, target_x, target_y);
		int count = UpdateAircraftSpeed(v, SPEED_LIMIT_NONE);

		/* Move */
		for (int i = 0; i < count; i++) {
			GetNewVehiclePosResult gp = GetNewVehiclePos(v);
			v->x_pos = gp.x;
			v->y_pos = gp.y;
			v->tile = TileIndex{}; // In air

			/* Altitude correction */
			int target_z = GetAircraftFlightLevel(v);
			if (v->z_pos < target_z) v->z_pos++;
			else if (v->z_pos > target_z) v->z_pos--;

			SetAircraftPosition(v, v->x_pos, v->y_pos, v->z_pos);

			if ((uint)(abs(v->x_pos - target_x) + abs(v->y_pos - target_y)) < TILE_SIZE) break;
		}
	}
}

static void AirportGoToNextPosition(Aircraft *v)
{
	const Station *st = Station::Get(v->targetairport);

	/* Check if this is a modular airport - handle before AircraftController */
	if (st->airport.blocks.Test(AirportBlock::Modular)) {
		/* For landing aircraft, use custom modular landing logic */
		if (v->state == LANDING || v->state == ENDLANDING) {
			/* Don't call AircraftController - it uses FTA MovingData which doesn't match modular layout */
			/* AirportMoveModularLanding handles all movement to the runway */
			if (AirportMoveModularLanding(v, st)) return;
			return;
		}

		/* For takeoff, use custom modular takeoff logic */
		if (v->state == TAKEOFF || v->state == STARTTAKEOFF || v->state == ENDTAKEOFF) {
			if (AirportMoveModularTakeoff(v, st)) return;
			return;
		}

		/* For flying state, use modular movement */
		if (v->state == FLYING) {
			AirportMoveModularFlying(v, st);
			/* Call handler to check for landing conditions */
			const AirportFTAClass *apc = st->airport.GetFTA();
			_aircraft_state_handlers[v->state](v, apc);
			return;
		}

		/* For ground movement with active pathfinding goal */
		if (v->ground_path != nullptr || v->ground_path_goal != INVALID_TILE) {
			/* Skip AircraftController - we handle movement ourselves */
			if (AirportMoveModular(v, st)) {
				/* Reached destination */
			}
			return;
		}

		/* For states like TERM1, HANGAR without ground_path_goal set yet,
		 * we must call the handler to process loading/orders */
		if (v->state == TERM1 || v->state == HANGAR || v->state == HELIPAD1) {
			const AirportFTAClass *apc = st->airport.GetFTA();
			_aircraft_state_handlers[v->state](v, apc);
			return;
		}

		/* For FLYING state on modular airports, don't use AircraftController
		 * because it relies on FTA MovingData which doesn't exist for modular airports.
		 * Instead, just check if landing is possible via the event handler. */
		if (v->state == FLYING) {
			/* Don't call AircraftController - modular airports don't use FTA MovingData */
			/* Just call event handler to check for landing */
			const AirportFTAClass *apc = st->airport.GetFTA();
			_aircraft_state_handlers[v->state](v, apc);
			return;
		}

		/* For any other unexpected states on modular airports, log and keep circling */
		Debug(misc, 1, "[ModAp] Vehicle {} in unexpected state {} on modular airport {}", v->index, v->state, st->index);
		v->state = FLYING;
		return;
	}

	/* Traditional FTA system for preset airports */
	if (!AircraftController(v)) return;

	const AirportFTAClass *apc = st->airport.GetFTA();
	AirportClearBlock(v, apc);
	AirportMove(v, apc); // move aircraft to next position
}

/* gets pos from vehicle and next orders */
static bool AirportMove(Aircraft *v, const AirportFTAClass *apc)
{
	/* error handling */
	if (v->pos >= apc->nofelements) {
		Debug(misc, 0, "[Ap] position {} is not valid for current airport. Max position is {}", v->pos, apc->nofelements - 1);
		assert(v->pos < apc->nofelements);
	}

	const AirportFTA *current = &apc->layout[v->pos];
	/* we have arrived in an important state (eg terminal, hangar, etc.) */
	if (current->heading == v->state) {
		uint8_t prev_pos = v->pos; // location could be changed in state, so save it before-hand
		uint8_t prev_state = v->state;
		_aircraft_state_handlers[v->state](v, apc);
		if (v->state != FLYING) v->previous_pos = prev_pos;
		if (v->state != prev_state || v->pos != prev_pos) UpdateAircraftCache(v);
		return true;
	}

	v->previous_pos = v->pos; // save previous location

	/* there is only one choice to move to */
	if (current->next == nullptr) {
		if (AirportSetBlocks(v, current, apc)) {
			v->pos = current->next_position;
			UpdateAircraftCache(v);
		} // move to next position
		return false;
	}

	/* there are more choices to choose from, choose the one that
	 * matches our heading */
	do {
		if (v->state == current->heading || current->heading == TO_ALL) {
			if (AirportSetBlocks(v, current, apc)) {
				v->pos = current->next_position;
				UpdateAircraftCache(v);
			} // move to next position
			return false;
		}
		current = current->next.get();
	} while (current != nullptr);

	Debug(misc, 0, "[Ap] cannot move further on Airport! (pos {} state {}) for vehicle {}", v->pos, v->state, v->index);
	NOT_REACHED();
}

/** returns true if the road ahead is busy, eg. you must wait before proceeding. */
static bool AirportHasBlock(Aircraft *v, const AirportFTA *current_pos, const AirportFTAClass *apc)
{
	const AirportFTA *reference = &apc->layout[v->pos];
	const AirportFTA *next = &apc->layout[current_pos->next_position];

	/* same block, then of course we can move */
	if (apc->layout[current_pos->position].blocks != next->blocks) {
		const Station *st = Station::Get(v->targetairport);
		AirportBlocks blocks = next->blocks;

		/* check additional possible extra blocks */
		if (current_pos != reference && current_pos->blocks != AirportBlock::Nothing) {
			blocks.Set(current_pos->blocks);
		}

		if (st->airport.blocks.Any(blocks)) {
			v->cur_speed = 0;
			v->subspeed = 0;
			return true;
		}
	}
	return false;
}

/**
 * "reserve" a block for the plane
 * @param v airplane that requires the operation
 * @param current_pos of the vehicle in the list of blocks
 * @param apc airport on which block is requested to be set
 * @returns true on success. Eg, next block was free and we have occupied it
 */
static bool AirportSetBlocks(Aircraft *v, const AirportFTA *current_pos, const AirportFTAClass *apc)
{
	const AirportFTA *next = &apc->layout[current_pos->next_position];
	const AirportFTA *reference = &apc->layout[v->pos];

	/* if the next position is in another block, check it and wait until it is free */
	if (!apc->layout[current_pos->position].blocks.All(next->blocks)) {
		AirportBlocks blocks = next->blocks;
		/* search for all all elements in the list with the same state, and blocks != N
		 * this means more blocks should be checked/set */
		const AirportFTA *current = current_pos;
		if (current == reference) current = current->next.get();
		while (current != nullptr) {
			if (current->heading == current_pos->heading && current->blocks.Any()) {
				blocks.Set(current->blocks);
				break;
			}
			current = current->next.get();
		}

		/* if the block to be checked is in the next position, then exclude that from
		 * checking, because it has been set by the airplane before */
		if (current_pos->blocks == next->blocks) blocks.Flip(next->blocks);

		Station *st = Station::Get(v->targetairport);
		if (st->airport.blocks.Any(blocks)) {
			v->cur_speed = 0;
			v->subspeed = 0;
			return false;
		}

		if (next->blocks != AirportBlock::Nothing) {
			st->airport.blocks.Set(blocks); // occupy next block
		}
	}
	return true;
}

/**
 * Combination of aircraft state for going to a certain terminal and the
 * airport flag for that terminal block.
 */
struct MovementTerminalMapping {
	AirportMovementStates state; ///< Aircraft movement state when going to this terminal.
	AirportBlock blocks; ///< Bitmask in the airport flags that need to be free for this terminal.
};

/** A list of all valid terminals and their associated blocks. */
static const MovementTerminalMapping _airport_terminal_mapping[] = {
	{TERM1, AirportBlock::Term1},
	{TERM2, AirportBlock::Term2},
	{TERM3, AirportBlock::Term3},
	{TERM4, AirportBlock::Term4},
	{TERM5, AirportBlock::Term5},
	{TERM6, AirportBlock::Term6},
	{TERM7, AirportBlock::Term7},
	{TERM8, AirportBlock::Term8},
	{HELIPAD1, AirportBlock::Helipad1},
	{HELIPAD2, AirportBlock::Helipad2},
	{HELIPAD3, AirportBlock::Helipad3},
};

/**
 * Find a free terminal or helipad, and if available, assign it.
 * @param v Aircraft looking for a free terminal or helipad.
 * @param i First terminal to examine.
 * @param last_terminal Terminal number to stop examining.
 * @return A terminal or helipad has been found, and has been assigned to the aircraft.
 */
static bool FreeTerminal(Aircraft *v, uint8_t i, uint8_t last_terminal)
{
	assert(last_terminal <= lengthof(_airport_terminal_mapping));
	Station *st = Station::Get(v->targetairport);
	for (; i < last_terminal; i++) {
		if (!st->airport.blocks.Any(_airport_terminal_mapping[i].blocks)) {
			/* TERMINAL# HELIPAD# */
			v->state = _airport_terminal_mapping[i].state; // start moving to that terminal/helipad
			st->airport.blocks.Set(_airport_terminal_mapping[i].blocks); // occupy terminal/helipad
			return true;
		}
	}
	return false;
}

/**
 * Get the number of terminals at the airport.
 * @param apc Airport description.
 * @return Number of terminals.
 */
static uint GetNumTerminals(const AirportFTAClass *apc)
{
	uint num = 0;

	for (uint i = apc->terminals[0]; i > 0; i--) num += apc->terminals[i];

	return num;
}

/**
 * Find a free terminal, and assign it if available.
 * @param v Aircraft to handle.
 * @param apc Airport state machine.
 * @return Found a free terminal and assigned it.
 */
static bool AirportFindFreeTerminal(Aircraft *v, const AirportFTAClass *apc)
{
	/* example of more terminalgroups
	 * {0,HANGAR,AirportBlock::Nothing,1}, {0,TERMGROUP,AirportBlock::TermGroup1,0}, {0,TERMGROUP,TERM_GROUP2_ENTER_block,1}, {0,0,N,1},
	 * Heading TERMGROUP denotes a group. We see 2 groups here:
	 * 1. group 0 -- AirportBlock::TermGroup1 (check block)
	 * 2. group 1 -- TERM_GROUP2_ENTER_block (check block)
	 * First in line is checked first, group 0. If the block (AirportBlock::TermGroup1) is free, it
	 * looks at the corresponding terminals of that group. If no free ones are found, other
	 * possible groups are checked (in this case group 1, since that is after group 0). If that
	 * fails, then attempt fails and plane waits
	 */
	if (apc->terminals[0] > 1) {
		const Station *st = Station::Get(v->targetairport);
		const AirportFTA *temp = apc->layout[v->pos].next.get();

		while (temp != nullptr) {
			if (temp->heading == TERMGROUP) {
				if (!st->airport.blocks.Any(temp->blocks)) {
					/* read which group do we want to go to?
					 * (the first free group) */
					uint target_group = temp->next_position + 1;

					/* at what terminal does the group start?
					 * that means, sum up all terminals of
					 * groups with lower number */
					uint group_start = 0;
					for (uint i = 1; i < target_group; i++) {
						group_start += apc->terminals[i];
					}

					uint group_end = group_start + apc->terminals[target_group];
					if (FreeTerminal(v, group_start, group_end)) return true;
				}
			} else {
				/* once the heading isn't 255, we've exhausted the possible blocks.
				 * So we cannot move */
				return false;
			}
			temp = temp->next.get();
		}
	}

	/* if there is only 1 terminalgroup, all terminals are checked (starting from 0 to max) */
	return FreeTerminal(v, 0, GetNumTerminals(apc));
}

/**
 * Find a free helipad, and assign it if available.
 * @param v Aircraft to handle.
 * @param apc Airport state machine.
 * @return Found a free helipad and assigned it.
 */
static bool AirportFindFreeHelipad(Aircraft *v, const AirportFTAClass *apc)
{
	/* if an airport doesn't have helipads, use terminals */
	if (apc->num_helipads == 0) return AirportFindFreeTerminal(v, apc);

	/* only 1 helicoptergroup, check all helipads
	 * The blocks for helipads start after the last terminal (MAX_TERMINALS) */
	return FreeTerminal(v, MAX_TERMINALS, apc->num_helipads + MAX_TERMINALS);
}

/**
 * Handle the 'dest too far' flag and the corresponding news message for aircraft.
 * @param v The aircraft.
 * @param too_far True if the current destination is too far away.
 */
static void AircraftHandleDestTooFar(Aircraft *v, bool too_far)
{
	if (too_far) {
		if (!v->flags.Test(VehicleAirFlag::DestinationTooFar)) {
			v->flags.Set(VehicleAirFlag::DestinationTooFar);
			SetWindowWidgetDirty(WC_VEHICLE_VIEW, v->index, WID_VV_START_STOP);
			AI::NewEvent(v->owner, new ScriptEventAircraftDestTooFar(v->index));
			if (v->owner == _local_company) {
				/* Post a news message. */
				AddVehicleAdviceNewsItem(AdviceType::AircraftDestinationTooFar, GetEncodedString(STR_NEWS_AIRCRAFT_DEST_TOO_FAR, v->index), v->index);
			}
		}
		return;
	}

	if (v->flags.Test(VehicleAirFlag::DestinationTooFar)) {
		/* Not too far anymore, clear flag and message. */
		v->flags.Reset(VehicleAirFlag::DestinationTooFar);
		SetWindowWidgetDirty(WC_VEHICLE_VIEW, v->index, WID_VV_START_STOP);
		DeleteVehicleNews(v->index, AdviceType::AircraftDestinationTooFar);
	}
}

static bool AircraftEventHandler(Aircraft *v, int loop)
{
	if (v->vehstatus.Test(VehState::Crashed)) {
		return HandleCrashedAircraft(v);
	}

	if (v->vehstatus.Test(VehState::Stopped)) return true;

	v->HandleBreakdown();

	HandleAircraftSmoke(v, loop != 0);
	ProcessOrders(v);
	v->HandleLoading(loop != 0);

	if (v->current_order.IsType(OT_LOADING) || v->current_order.IsType(OT_LEAVESTATION)) return true;

	if (v->state >= ENDTAKEOFF && v->state <= HELIENDLANDING) {
		/* If we are flying, unconditionally clear the 'dest too far' state. */
		AircraftHandleDestTooFar(v, false);
	} else if (v->acache.cached_max_range_sqr != 0) {
		/* Check the distance to the next destination. This code works because the target
		 * airport is only updated after take off and not on the ground. */
		Station *cur_st = Station::GetIfValid(v->targetairport);
		Station *next_st = v->current_order.IsType(OT_GOTO_STATION) || v->current_order.IsType(OT_GOTO_DEPOT) ? Station::GetIfValid(v->current_order.GetDestination().ToStationID()) : nullptr;

		if (cur_st != nullptr && cur_st->airport.tile != INVALID_TILE && next_st != nullptr && next_st->airport.tile != INVALID_TILE) {
			uint dist = DistanceSquare(cur_st->airport.tile, next_st->airport.tile);
			AircraftHandleDestTooFar(v, dist > v->acache.cached_max_range_sqr);
		}
	}

	if (!v->flags.Test(VehicleAirFlag::DestinationTooFar)) AirportGoToNextPosition(v);

	return true;
}

bool Aircraft::Tick()
{
	if (!this->IsNormalAircraft()) return true;

	PerformanceAccumulator framerate(PFE_GL_AIRCRAFT);

	this->tick_counter++;

	if (!this->vehstatus.Test(VehState::Stopped)) this->running_ticks++;

	if (this->subtype == AIR_HELICOPTER) HelicopterTickHandler(this);

	this->current_order_time++;

	for (uint i = 0; i != 2; i++) {
		/* stop if the aircraft was deleted */
		if (!AircraftEventHandler(this, i)) return false;
	}

	return true;
}


/**
 * Returns aircraft's target station if v->target_airport
 * is a valid station with airport.
 * @param v vehicle to get target airport for
 * @return pointer to target station, nullptr if invalid
 */
Station *GetTargetAirportIfValid(const Aircraft *v)
{
	assert(v->type == VEH_AIRCRAFT);

	Station *st = Station::GetIfValid(v->targetairport);
	if (st == nullptr) return nullptr;

	return st->airport.tile == INVALID_TILE ? nullptr : st;
}

/**
 * Updates the status of the Aircraft heading or in the station
 * @param st Station been updated
 */
void UpdateAirplanesOnNewStation(const Station *st)
{
	/* only 1 station is updated per function call, so it is enough to get entry_point once */
	const AirportFTAClass *ap = st->airport.GetFTA();
	Direction rotation = st->airport.tile == INVALID_TILE ? DIR_N : st->airport.rotation;

	for (Aircraft *v : Aircraft::Iterate()) {
		if (!v->IsNormalAircraft() || v->targetairport != st->index) continue;
		assert(v->state == FLYING);

		Order *o = &v->current_order;
		/* The aircraft is heading to a hangar, but the new station doesn't have one,
		 * or the aircraft can't land on the new station. Cancel current order. */
		if (o->IsType(OT_GOTO_DEPOT) && !o->GetDepotOrderType().Test(OrderDepotTypeFlag::PartOfOrders) && o->GetDestination() == st->index &&
				(!st->airport.HasHangar() || !CanVehicleUseStation(v, st))) {
			o->MakeDummy();
			SetWindowWidgetDirty(WC_VEHICLE_VIEW, v->index, WID_VV_START_STOP);
		}
		v->pos = v->previous_pos = AircraftGetEntryPoint(v, ap, rotation);
		UpdateAircraftCache(v);
	}

	/* Heliports don't have a hangar. Invalidate all go to hangar orders from all aircraft. */
	if (!st->airport.HasHangar()) RemoveOrderFromAllVehicles(OT_GOTO_DEPOT, st->index, true);
}
