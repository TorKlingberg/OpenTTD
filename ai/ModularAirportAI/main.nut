require("util.nut");
require("layout.nut");
require("fit.nut");
require("sites.nut");
require("build.nut");
require("fleet.nut");
require("grow.nut");
require("selftest.nut");

/* Keep this much in the bank rather than spending down to zero: an airport with
 * no aircraft earns nothing, and a fleet with no maintenance money bleeds. */
const CASH_RESERVE = 20000;

/* Routes shorter than this are not worth flying. */
const MIN_ROUTE_DISTANCE = 25;

class ModularAirportAI extends AIController
{
	blacklist = null;   ///< origins that failed to build, so we stop retrying them
	max_airports = 12;
	variety = 2;
	grow_cursor = 0;    ///< round-robin over airports, so none is starved
	pax_cargo = -1;
	town_search_stats = "not run yet";

	function Start()
	{
		this.blacklist = {};
		AICompany.SetName(UniqueCompanyName("ModularAirportAI"));
		this.max_airports = AIController.GetSetting("max_airports");
		this.variety = AIController.GetSetting("variety");

		/* Let the game renew ageing aircraft. Without this a long game quietly
		 * decays: aircraft grow old, break down more, and throughput slides even
		 * though the airports and routes are unchanged — over thirty years that
		 * cost about a fifth of the movements. */
		AICompany.SetAutoRenewStatus(true);
		AICompany.SetAutoRenewMonths(-6);
		AICompany.SetAutoRenewMoney(100000);

		if (AIController.GetSetting("selftest") == 1) {
			RunSelfTest();
			while (true) this.Sleep(1000);
		}

		AILog.Info("start: year=" + AIDate.GetYear(AIDate.GetCurrentDate())
		           + " max_airports=" + this.max_airports + " variety=" + this.variety);

		local last_report = 0;
		while (true) {
			this.ManageLoan();

			local airports = OurAirports();
			local aircraft = AIVehicleList().Count();

			/* An airport with no aircraft is a monthly bill and nothing else, so
			 * the fleet leads and the network follows: buy first, build second.
			 *
			 * Holding back new airports whenever the fleet looks thin deadlocks,
			 * though. Two airports too close together to be worth a route leave
			 * the fleet permanently thin and no aircraft ever buyable, so the AI
			 * stops building the very airports that would give it somewhere to
			 * fly. Only defer building when the fleet actually grew. */
			local bought = (airports.len() >= 2) ? this.TryExpandFleet(airports) : 0;
			local fleet_is_thin = bought > 0 && aircraft + bought < airports.len() * 2;
			if (airports.len() < this.max_airports && !fleet_is_thin) {
				this.TryBuildAirport(airports);
				airports = OurAirports();
			}
			this.TryGrowAirports(airports);

			local year = AIDate.GetYear(AIDate.GetCurrentDate());
			if (year != last_report) {
				last_report = year;
				this.Report(year);
				RetireLosers();
				/* A site that was unaffordable or too cramped last year may not
				 * be this year: the budget grows, and with it the scale of
				 * layout worth trying. Blacklisting has to expire or the AI
				 * talks itself out of the whole map within a decade. */
				this.blacklist = {};
			}

			this.Sleep(50);
		}
	}

	/**
	 * Borrow while expanding, repay when the money is idle. Airports are
	 * expensive and earn nothing until an aircraft flies to them, so being
	 * unable to buy the aircraft after building the airport is the worst
	 * outcome available.
	 */
	function ManageLoan()
	{
		local balance = AICompany.GetBankBalance(AICompany.COMPANY_SELF);
		local loan = AICompany.GetLoanAmount();
		if (balance < CASH_RESERVE && loan < AICompany.GetMaxLoanAmount()) {
			AICompany.SetLoanAmount(AICompany.GetMaxLoanAmount());
		} else if (loan > 0 && balance > loan + CASH_RESERVE * 4) {
			AICompany.SetLoanAmount(0);
		}
	}

	/**
	 * Pick a town we do not serve and try to put an airport by it.
	 *
	 * Scale rises with the bank balance, so the AI opens routes with something
	 * small and cheap and only builds the big families once it can afford both
	 * the airport and the aircraft to justify it.
	 */
	function TryBuildAirport(airports)
	{
		local budget = AICompany.GetBankBalance(AICompany.COMPANY_SELF) - CASH_RESERVE;
		if (budget < 60000) return;

		local town = this.PickUnservedTown(airports, FundsAvailable());
		if (town < 0) return;

		/* Size the airport for the town it serves, then cap it by what we can
		 * raise rather than by what happens to be in the bank. Sizing off the
		 * instantaneous balance produces nothing but minimal airstrips forever:
		 * the balance is low precisely because the AI is spending, so it never
		 * observes itself being able to afford anything better. */
		local funds = FundsAvailable();
		local pop = AITown.GetPopulation(town);
		local scale = 0;
		if (pop > 700) scale = 1;
		if (pop > 1800) scale = 2;
		if (pop > 3500) scale = 3;
		if (funds < 200000 && scale > 0) scale = 0;
		else if (funds < 450000 && scale > 1) scale = 1;
		else if (funds < 900000 && scale > 2) scale = 2;

		/* Jets are only worth designing for once we could actually buy one. */
		local want_large_safe = funds > 250000;

		/* FindSiteNearTown already falls back from large-safe to whatever fits,
		 * and from real airports down to strips and heliports. */
		local site = FindSiteNearTown(town, scale, want_large_safe, this.variety, budget, this.blacklist);
		if (site == null) {
			AILog.Info("no site near " + AITown.GetName(town) + " (scale " + scale + "): " + SiteSearchStats());
			this.blacklist[AITown.GetLocation(town)] <- true;
			return;
		}

		local station = BuildSite(site);
		if (station < 0) {
			this.blacklist[site.tile] <- true;
			return;
		}
	}

	/**
	 * A town with no airport of ours nearby, preferring big ones.
	 *
	 * Once the map is covered and money has piled up, fall back to towns that
	 * still have room for a second airport. A town takes two, and a large town
	 * generates far more traffic than one small airport at its edge can carry —
	 * so densifying beats leaving millions in the bank.
	 */
	function PickUnservedTown(airports, funds)
	{
		local towns = AITownList();
		towns.Valuate(AITown.GetPopulation);
		towns.Sort(AIList.SORT_BY_VALUE, AIList.SORT_DESCENDING);

		local shortlist = [];
		local second_best = [];
		/* Once the company is rich, coverage matters more than the immediate
		 * return from each new endpoint. The old fixed cutoff of 200 made every
		 * smaller town permanently invisible, however much idle cash the company
		 * had or how high max_airports was set. */
		local min_pop = funds > 1500000 ? 80 : 200;
		local too_small = 0, blacklisted = 0, no_capacity = 0, served = 0;
		foreach (town, pop in towns) {
			if (pop < min_pop) { too_small++; continue; }
			local loc = AITown.GetLocation(town);
			if (loc in this.blacklist) { blacklisted++; continue; }
			local taken = false;
			foreach (a in airports) {
				/* A nearby airport is not necessarily this town's airport. On maps
				 * with closely packed towns the old 15-tile circle let one station
				 * mark several neighbours as served forever. Use the station's town
				 * association, which is also what the game's airport limit uses. */
				if (AIStation.GetNearestTown(a.station) == town) { taken = true; break; }
			}
			if (!taken) {
				/* Do not spend a full terrain search on a town whose shared airport
				 * allowance has already been consumed by other companies. */
				if (AITown.GetAllowedNoise(town) < 1) { no_capacity++; continue; }
				if (shortlist.len() < 6) shortlist.append(town);
				continue;
			}
			served++;
			/* Room for one more here, and big enough to be worth it. */
			if (pop > 2000 && AITown.GetAllowedNoise(town) >= 1 && second_best.len() < 4) {
				second_best.append(town);
			}
		}
		if (shortlist.len() == 0 && funds > 1500000) shortlist = second_best;
		this.town_search_stats = "min_pop=" + min_pop
		                       + " candidates=" + shortlist.len()
		                       + " served=" + served
		                       + " no_capacity=" + no_capacity
		                       + " blacklisted=" + blacklisted
		                       + " too_small=" + too_small;
		if (shortlist.len() == 0) return -1;
		/* Some randomness so two instances of this AI do not fight over the
		 * same town, and so successive games do not look identical. */
		local pick = this.variety > 0 ? AIBase.RandRange(shortlist.len()) : 0;
		return shortlist[pick];
	}

	/**
	 * Add aircraft where they will do the most good: the route between two of
	 * our airports with the fewest aircraft on it.
	 */
	/**
	 * Improve one existing airport per pass.
	 *
	 * One at a time on purpose: each build suspends the script, and spreading
	 * the work out keeps the AI responsive to whatever else needs doing.
	 */
	function TryGrowAirports(airports)
	{
		if (airports.len() == 0) return;
		local funds = FundsAvailable();
		if (funds < 60000) return;
		if (this.pax_cargo < 0) this.pax_cargo = PassengerCargo();

		local i = this.grow_cursor % airports.len();
		this.grow_cursor = (this.grow_cursor + 1) % airports.len();
		local a = airports[i];

		local what = GrowAirport(a.station, a.tile, funds, this.pax_cargo);
		if (what != null) {
			AILog.Info("grew " + AIStation.GetName(a.station) + ": " + what
			           + " (safety now " + AIAirport.GetModularAirportSafety(a.tile) + ")");
		}
	}

	/**
	 * Buy aircraft while there is both money and an under-served route.
	 * Returns how many were bought, which the caller uses to tell "the fleet is
	 * catching up" from "the fleet cannot grow at all".
	 */
	function TryExpandFleet(airports)
	{
		local bought = 0;
		for (local i = 0; i < 3; i++) {
			if (!this.BuyOneAircraft(airports)) break;
			bought++;
		}
		return bought;
	}

	function BuyOneAircraft(airports)
	{
		local budget = AICompany.GetBankBalance(AICompany.COMPANY_SELF) - CASH_RESERVE;
		if (budget < 30000) return false;
		if (AIVehicleList().Count() >= AIGameSettings.GetValue("vehicle.max_aircraft") - 1) return false;

		if (this.pax_cargo < 0) this.pax_cargo = PassengerCargo();

		/* Route choice follows demand: the passengers standing at the two ends
		 * with nothing to fly on. An earlier version ranked routes by spare stand
		 * capacity instead, which meant the AI kept feeding whichever airport
		 * happened to have been built biggest rather than whichever one people
		 * were waiting at.
		 *
		 * Capacity is not part of the ranking, only a veto: an airport already at
		 * its ceiling cannot use another aircraft, and one more would queue in the
		 * air and let the reservation system do the rest of the damage. When the
		 * veto is what stops a route with a queue behind it, GrowAirport sees the
		 * same two facts and builds. */
		local routes = [];
		for (local i = 0; i < airports.len(); i++) {
			for (local j = i + 1; j < airports.len(); j++) {
				local a = airports[i], b = airports[j];
				local map_dist = AIMap.DistanceManhattan(a.tile, b.tile);
				if (map_dist < MIN_ROUTE_DISTANCE) continue;

				if (VehiclesServingStation(a.station) >= AircraftCeiling(a.station)) continue;
				if (VehiclesServingStation(b.station) >= AircraftCeiling(b.station)) continue;

				local caps_a = AirportCapability(a.tile);
				local caps_b = AirportCapability(b.tile);
				local want_heli = !(caps_a.planes && caps_b.planes) && caps_a.helis && caps_b.helis;
				if (!want_heli && !(caps_a.planes && caps_b.planes)) continue;

				local allow_jets = caps_a.jets && caps_b.jets;
				/* Order distance, not map distance: the two are in different units
				 * and only this one may be compared with an engine's range. Reject an
				 * infeasible pair here so it cannot hide every route ranked below it. */
				local order_dist = AIOrder.GetOrderDistance(AIVehicle.VT_AIR, a.order_tile, b.order_tile);
				local engine = ChooseAircraft(allow_jets, want_heli, budget, order_dist);
				if (engine < 0) continue;

				local hangar = AIAirport.GetHangarOfAirport(a.tile);
				if (hangar < 0) hangar = AIAirport.GetHangarOfAirport(b.tile);
				if (hangar < 0) continue;

				/* Distance breaks ties between equally busy pairs and is worth a
				 * little on its own, since a longer leg earns more per trip. */
				local need = AIStation.GetCargoWaiting(a.station, this.pax_cargo)
				           + AIStation.GetCargoWaiting(b.station, this.pax_cargo)
				           + map_dist;
				routes.append({ a = a, b = b, need = need, engine = engine,
				                hangar = hangar, allow_jets = allow_jets });
			}
		}
		if (routes.len() == 0) return false;
		routes.sort(function (a, b) {
			if (a.need < b.need) return 1;
			if (a.need > b.need) return -1;
			return 0;
		});

		/* A route can become unavailable while BuildVehicle suspends the script.
		 * Continue down the ranked list instead of letting that one stale choice
		 * stop fleet growth for the whole network. */
		foreach (route in routes) {
			local v = BuyAircraft(route.hangar, route.engine, route.a.order_tile, route.b.order_tile);
			if (v < 0) continue;
			local ptype = AIEngine.GetPlaneType(route.engine);
			AILog.Info("aircraft " + AIEngine.GetName(route.engine)
			           + " [" + (ptype == AIAirport.PT_BIG_PLANE ? "big" :
			                     ptype == AIAirport.PT_HELICOPTER ? "heli" : "small") + "]"
			           + " on " + AIStation.GetName(route.a.station) + " <-> " + AIStation.GetName(route.b.station)
			           + (route.allow_jets ? "" : " (no jets: an end is not large-safe)"));
			return true;
		}
		return false;
	}

	function Report(year)
	{
		local airports = OurAirports();
		local safe = 0;
		foreach (a in airports) if (AirportIsLargeSafe(a.tile)) safe++;
		AILog.Info("--- " + year
		           + " airports=" + airports.len() + " (large-safe " + safe + ")"
		           + " aircraft=" + AIVehicleList().Count()
		           + " cash=" + AICompany.GetBankBalance(AICompany.COMPANY_SELF)
		           + " loan=" + AICompany.GetLoanAmount());

		/* An aircraft that never leaves its hangar is the failure this whole
		 * design is trying to avoid, and it looks identical to "no demand" from
		 * the outside. Say where they actually are. */
		if (this.pax_cargo < 0) this.pax_cargo = PassengerCargo();
		local worst = 0, worst_name = "-";
		foreach (a in airports) {
			local w = AIStation.GetCargoWaiting(a.station, this.pax_cargo);
			if (w > worst) { worst = w; worst_name = AIStation.GetName(a.station); }
		}
		AILog.Info("    busiest: " + worst_name + " " + worst + " waiting");
		AILog.Info("    town search: " + this.town_search_stats);

		local vl = AIVehicleList();
		local stuck = 0, moving = 0, parked = 0;
		foreach (v, _ in vl) {
			if (AIVehicle.IsStoppedInDepot(v)) parked++;
			if (AIVehicle.GetCurrentSpeed(v) > 0) moving++;
			else stuck++;
		}
		/* Aircraft halted in a hangar. RetireLosers parks the ones it is about to
		 * sell, so a small transient count is normal and a count that only ever
		 * grows means they are being parked and then not sold. */
		if (parked > 0) AILog.Info("    parked in hangar: " + parked + " of " + vl.Count());
		if (stuck > 0 && moving == 0 && vl.Count() > 0) {
			local v = vl.Begin();
			AILog.Warning("all " + vl.Count() + " aircraft stationary; first is at "
			              + TileStr(AIVehicle.GetLocation(v))
			              + " orders=" + AIOrder.GetOrderCount(v)
			              + " in_depot=" + AIVehicle.IsStoppedInDepot(v)
			              + " age=" + AIVehicle.GetAge(v)
			              + " profit=" + AIVehicle.GetProfitThisYear(v));
		}
	}

	function Save() { return {}; }
	function Load(version, data) { }
}

/**
 * Our modular airports, read from the game rather than remembered.
 *
 * Everything here is derivable from the map, so there is nothing to get out of
 * step across a save and load. Only what the map cannot tell us — which sites
 * we have already failed on — is worth keeping in the AI's own state.
 */
function OurAirports()
{
	local out = [];
	local list = AIStationList(AIStation.STATION_AIRPORT);
	foreach (st, _ in list) {
		local tile = AirportAnchorTile(st);
		if (tile < 0) continue;
		out.append({ station = st, tile = tile, order_tile = AirportOrderTile(st, tile) });
	}
	return out;
}

/** Any modular tile of a station, for the calls that want one. */
function AirportAnchorTile(station)
{
	local tiles = AITileList_StationType(station, AIStation.STATION_AIRPORT);
	foreach (t, _ in tiles) {
		if (AIAirport.IsModularAirportTile(t)) return t;
	}
	return -1;
}

/**
 * A tile of this airport that is safe to put in an order.
 *
 * AIOrder.AppendOrder decides between a station order and a *depot* order from
 * the tile it is given, so handing it a hangar tile silently produces "fly here
 * and stop" instead of "serve this airport". The aircraft then does exactly
 * that: it sits in the hangar forever, with valid-looking orders, no error, and
 * nothing in the airport logs because it never taxis at all.
 *
 * Which tile a station reports first depends on the layout — a hangar in the
 * airport's northernmost row is enough to trigger it — so pick deliberately.
 */
function AirportOrderTile(station, fallback)
{
	local tiles = AITileList_StationType(station, AIStation.STATION_AIRPORT);
	foreach (t, _ in tiles) {
		if (!AIAirport.IsModularAirportTile(t)) continue;
		local p = AIAirport.GetModularPiece(t);
		if (IsHangarPiece(p)) continue;
		if (IsStandPiece(p) || IsHelipadPiece(p) || p == AIAirport.MP_APRON) return t;
	}
	return fallback;
}

/**
 * What we could spend if we wanted to, counting unused loan capacity.
 *
 * The bank balance alone is a poor measure of how big an airport to build: it
 * is lowest exactly when expansion is going well.
 */
function FundsAvailable()
{
	return AICompany.GetBankBalance(AICompany.COMPANY_SELF)
	     + (AICompany.GetMaxLoanAmount() - AICompany.GetLoanAmount());
}

/** Company names must be unique, so add a suffix when the plain name is taken. */
function UniqueCompanyName(base)
{
	if (AICompany.SetName(base)) return base;
	for (local i = 2; i < 20; i++) {
		local name = base + " " + i;
		if (AICompany.SetName(name)) return name;
	}
	return base;
}
