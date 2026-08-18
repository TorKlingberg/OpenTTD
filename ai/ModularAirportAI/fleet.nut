/*
 * Aircraft: choosing them, buying them, and keeping them off airports that
 * would crash them.
 *
 * The last part is specific to modular airports and is not optional. A fast jet
 * using an airport that is not large-safe takes an elevated overrun crash roll
 * *regardless of the "plane crashes" setting* — it is the stock short-strip
 * behaviour, not the general crash rate. So the aircraft choice is gated on
 * what the airports at both ends of the route actually are.
 */

/** What an airport can host, read off the map. */
function AirportCapability(tile)
{
	local caps = { planes = false, jets = false, helis = false };
	if (!AIAirport.IsModularAirportTile(tile)) return caps;
	local station = AIStation.GetStationID(tile);
	if (!AIStation.IsValidStation(station)) return caps;

	local tiles = AITileList_StationType(station, AIStation.STATION_AIRPORT);
	foreach (t, _ in tiles) {
		if (!AIAirport.IsModularAirportTile(t)) continue;
		local p = AIAirport.GetModularPiece(t);
		if (IsStandPiece(p)) caps.planes = true;
		if (IsHelipadPiece(p)) caps.helis = true;
	}
	/* Stock-compatible modular handling lets helicopters use ordinary stands
	 * when an airport has no dedicated helipad. A fixed-wing-capable endpoint is
	 * therefore also a helicopter-capable endpoint. */
	if (caps.planes) caps.helis = true;
	caps.jets = caps.planes && AIAirport.GetModularAirportSafety(tile) == AIAirport.MS_OK;
	return caps;
}

/**
 * Pick an aircraft for a route.
 *
 * `allow_jets` false means both ends are not large-safe, so big planes are off
 * the table however good their economics look.
 */
function ChooseAircraft(allow_jets, want_heli, budget, distance)
{
	local engines = AIEngineList(AIVehicle.VT_AIR);
	local best = -1, best_score = -1;

	foreach (e, _ in engines) {
		if (!AIEngine.IsBuildable(e)) continue;
		local type = AIEngine.GetPlaneType(e);
		local is_heli = (type == AIAirport.PT_HELICOPTER);
		if (want_heli != is_heli) continue;
		if (!is_heli && !allow_jets && type == AIAirport.PT_BIG_PLANE) continue;

		local price = AIEngine.GetPrice(e);
		if (price <= 0 || price > budget) continue;

		/* Range matters once it exists: an aircraft that cannot reach the far end
		 * accepts the orders, then sits in the hangar forever — no error, no
		 * movement, and nothing in the airport logs, because it never starts
		 * taxiing at all.
		 *
		 * `distance` must come from AIOrder.GetOrderDistance, not from a map
		 * distance. The unit is explicitly unspecified and is not tiles, so
		 * comparing a Manhattan distance against it silently passes routes the
		 * aircraft cannot fly. */
		local range = AIEngine.GetMaximumOrderDistance(e);
		if (range > 0 && distance > range) continue;

		local capacity = AIEngine.GetCapacity(e);
		local speed = AIEngine.GetMaxSpeed(e);
		local running = AIEngine.GetRunningCost(e);
		if (capacity <= 0 || speed <= 0) continue;

		/* Passenger-miles per year against what it costs to run for a year.
		 * Capacity times speed is the throughput of one aircraft; running cost
		 * is the recurring bill; price is amortised roughly over a decade so a
		 * large aircraft is not rejected for being expensive when it earns its
		 * price back many times over. */
		local yearly_cost = running + price / 10;
		if (yearly_cost <= 0) continue;
		local score = capacity * speed * 100 / yearly_cost;
		if (score > best_score) { best_score = score; best = e; }
	}
	return best;
}

/**
 * Buy an aircraft at `hangar` and set it flying between two stations.
 * Returns the VehicleID or -1.
 */
function BuyAircraft(hangar, engine, from_tile, to_tile)
{
	local v = AIVehicle.BuildVehicle(hangar, engine);
	if (!AIVehicle.IsValidVehicle(v)) {
		AILog.Warning("could not buy aircraft: " + AIError.GetLastErrorString());
		return -1;
	}
	/* Both tiles must be station tiles that are not hangars, or AppendOrder
	 * quietly makes a depot order out of them and the aircraft never flies. */
	if (AIAirport.IsModularAirportTile(from_tile) && IsHangarPiece(AIAirport.GetModularPiece(from_tile))) {
		AILog.Error("refusing to order to a hangar tile at " + TileStr(from_tile));
		AIVehicle.SellVehicle(v);
		return -1;
	}
	/* Aircraft are fast; waiting for a full load usually costs more in idle
	 * time than it gains in payload, so run them as a shuttle. */
	if (!AIOrder.AppendOrder(v, from_tile, AIOrder.OF_NONE)
	 || !AIOrder.AppendOrder(v, to_tile, AIOrder.OF_NONE)) {
		AILog.Warning("could not set orders: " + AIError.GetLastErrorString());
		AIVehicle.SellVehicle(v);
		return -1;
	}
	local started = AIVehicle.StartStopVehicle(v);
	AILog.Info("  start=" + started + " err=" + AIError.GetLastErrorString()
	           + " stopped_in_depot=" + AIVehicle.IsStoppedInDepot(v)
	           + " built_at=" + TileStr(hangar));

	/* Read the orders back. "Valid-looking orders that the aircraft ignores" is
	 * the failure this AI keeps rediscovering, and it is invisible unless the
	 * kind of each order is checked explicitly. */
	local kinds = "";
	for (local i = 0; i < AIOrder.GetOrderCount(v); i++) {
		local dest = AIOrder.GetOrderDestination(v, i);
		local kind = "?";
		if (AIOrder.IsGotoStationOrder(v, i)) kind = "station";
		else if (AIOrder.IsGotoDepotOrder(v, i)) kind = "DEPOT";
		else if (AIOrder.IsGotoWaypointOrder(v, i)) kind = "waypoint";
		kinds += " " + i + ":" + kind + "@" + TileStr(dest);
	}
	AILog.Info("  orders" + kinds
	           + " range=" + AIEngine.GetMaximumOrderDistance(engine)
	           + " dist=" + AIOrder.GetOrderDistance(AIVehicle.VT_AIR, from_tile, to_tile)
	           + " state=" + AIVehicle.GetState(v));
	return v;
}

/** Sell aircraft that have stopped in a hangar (sent there to be retired). */
function SellStoppedAircraft()
{
	local sold = 0;
	local list = AIVehicleList();
	foreach (v, _ in list) {
		if (AIVehicle.GetVehicleType(v) != AIVehicle.VT_AIR) continue;
		if (AIVehicle.IsStoppedInDepot(v)) {
			if (AIVehicle.SellVehicle(v)) sold++;
		}
	}
	return sold;
}

/**
 * Sell aircraft that spent a whole year losing money.
 *
 * Retiring one takes two passes — send it to a hangar, sell it once it arrives
 * — and the two passes must not be gated on the same test, because parking the
 * aircraft destroys the evidence that condemned it. A halted aircraft never
 * accumulates running_ticks, so its profit stops moving: the year rolls over,
 * profit_this_year resets to 0, profit_last_year follows a year later, and a
 * profit-based guard in front of the sell can never be true again. The
 * aircraft then sits stopped in the hangar for the rest of the game, still
 * counting against its stations' aircraft ceilings.
 *
 * So the arrival check (SellStoppedAircraft) runs monthly, while the condemnation
 * check below runs at the year boundary. Nothing else in this AI stops a vehicle,
 * so "stopped in a depot" means "this function put it there"; anything that parks
 * aircraft for another reason must not use a plain SendVehicleToDepot, or it will
 * find them sold.
 */
function RetireLosers()
{
	local sold = SellStoppedAircraft();
	local list = AIVehicleList();
	foreach (v, _ in list) {
		if (AIVehicle.GetVehicleType(v) != AIVehicle.VT_AIR) continue;
		if (AIVehicle.IsStoppedInDepot(v)) continue;
		if (AIVehicle.GetAge(v) < 730) continue;
		/* Last year only. This runs at the year boundary, moments after
		 * profit_this_year was reset, so this year's figure is a day or two of
		 * running cost with no delivery income yet — negative for most aircraft
		 * in flight, and evidence of nothing. */
		if (AIVehicle.GetProfitLastYear(v) >= 0) continue;
		AIVehicle.SendVehicleToDepot(v);
	}
	return sold;
}

/** Aircraft currently flying to or from a station. */
function VehiclesServingStation(station)
{
	local n = 0;
	local list = AIVehicleList_Station(station);
	foreach (_, __ in list) n++;
	return n;
}
