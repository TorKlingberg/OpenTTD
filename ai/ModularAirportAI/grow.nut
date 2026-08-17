/*
 * Growing airports that already exist.
 *
 * This is the reason a modular-airport AI is worth writing. A stock-airport AI
 * faces a step function: to enlarge an airport it must demolish it, lose the
 * station, lose the catchment, and rebuild. A modular one adds a tile.
 *
 * So the AI opens routes with something small and cheap, and pays for capacity
 * only when traffic justifies it. Maintenance is derived from the pieces, so
 * nothing is spent on a terminal building that no passenger has yet arrived to
 * use.
 *
 * Growth goes through BuildModularAirportTile one tile at a time, which is not
 * atomic the way placing a layout is. Every step therefore has to leave the
 * airport working on its own: only pieces that cannot break taxi routing are
 * ever added, and they are only added to ground already checked.
 */

/** The cargo id for passengers, which is what airports mostly move. */
function PassengerCargo()
{
	local list = AICargoList();
	foreach (c, _ in list) {
		if (AICargo.HasCargoClass(c, AICargo.CC_PASSENGERS)) return c;
	}
	return 0;
}

/**
 * Empty tiles this airport could expand onto.
 *
 * Only tiles adjacent to the airport, buildable, and at the airport's own
 * height — a modular airport occupies exactly one height level, so anything
 * else is refused by the build command.
 */
function ExpansionTiles(station, want_taxi_adjacent)
{
	local tiles = AITileList_StationType(station, AIStation.STATION_AIRPORT);
	local base = -1;
	local ours = {};
	foreach (t, _ in tiles) {
		if (!AIAirport.IsModularAirportTile(t)) continue;
		if (base < 0) base = AITile.GetMaxHeight(t);
		ours[t] <- AIAirport.GetModularPiece(t);
	}
	if (base < 0) return [];

	local out = [];
	local seen = {};
	foreach (t, piece in ours) {
		/* A new stand has to hang off something aircraft can taxi along, or it
		 * is a stand no aircraft can reach — which builds, scores and never
		 * gets used. */
		if (want_taxi_adjacent && !IsThroughTaxiable(piece)) continue;

		local x = AIMap.GetTileX(t), y = AIMap.GetTileY(t);
		foreach (d in [[0, 1], [0, -1], [1, 0], [-1, 0]]) {
			local n = AIMap.GetTileIndex(x + d[0], y + d[1]);
			if (!AIMap.IsValidTile(n)) continue;
			if (n in ours || n in seen) continue;
			if (AITile.IsStationTile(n)) continue;
			if (!AITile.IsBuildable(n)) continue;
			if (AITile.GetMaxHeight(n) != base) continue;
			seen[n] <- true;
			out.append(n);
		}
	}
	return out;
}

/**
 * Try to improve one airport. Returns a description of what changed, or null.
 *
 * Ordered by value per tile. Making an airport large-safe comes first and by a
 * wide margin: until it is, every fast jet that lands there takes an elevated
 * overrun crash roll regardless of the "plane crashes" setting, so one tile can
 * be the difference between a working route and a stream of write-offs.
 */
function GrowAirport(station, tile, funds, pax_cargo)
{
	local safety = AIAirport.GetModularAirportSafety(tile);

	local missing_runway = safety & (AIAirport.MS_MISSING_LANDING_RUNWAY | AIAirport.MS_MISSING_TAKEOFF_RUNWAY);

	/* A tower and a terminal are only worth buying for an airport that can
	 * become large-safe. A grass strip cannot: it has no large runway, and one
	 * is not addable a tile at a time — that needs six contiguous clear tiles in
	 * a line, which is a rebuild. Adding the buildings anyway would spend real
	 * money to move the safety mask from 15 to 12, which changes nothing about
	 * what may safely land there. */
	if (safety != AIAirport.MS_OK && safety >= 0 && missing_runway == 0) {
		if (safety & AIAirport.MS_MISSING_TOWER) {
			if (AddPiece(station, AIAirport.MP_TOWER, false)) return "added a control tower";
		}
		if (safety & AIAirport.MS_MISSING_BIG_TERMINAL) {
			local variants = BigTerminalVariants();
			local piece = variants[AIBase.RandRange(variants.len())];
			if (AddPiece(station, piece, false)) return "added a terminal building";
		}
		return null;
	}

	/* Sound, or as sound as it can be made. Now the question is whether it is big
	 * enough, and that is two questions:
	 *
	 *   1. Is there demand it is not carrying? A queue of waiting passengers.
	 *   2. Is more aeroplane the answer, or is the airport itself the limit?
	 *
	 * Buying aircraft is much the cheaper answer and BuyOneAircraft reaches for it
	 * first, so an airport that is still below its ceiling needs nothing built:
	 * the next aircraft will take the queue. Building only makes sense once the
	 * ground is what is holding the route back.
	 *
	 * That also says *what* to build. The ceiling is the lower of the stand and
	 * runway limits, so whichever of the two produced it is the thing to add, and
	 * the AI stops guessing between a stand and a runway. */
	local waiting = AIStation.GetCargoWaiting(station, pax_cargo);
	if (waiting <= MIN_QUEUE_TO_GROW) return null;

	local serving = VehiclesServingStation(station);
	if (serving < AircraftCeiling(station)) return null;

	local slots = ParkingSlots(station);
	local runways = CountRunways(station);
	local stand_bound = PLANES_PER_STAND * slots <= PLANES_PER_RUNWAY * runways;

	if (stand_bound) {
		if (funds > 80000 && AddPiece(station, AIAirport.MP_STAND, true, true)) {
			return "added a stand (" + waiting + " waiting, " + serving + " aircraft on "
			       + slots + " stands)";
		}
		return null;
	}

	/* Runway-bound: a runway is reserved along its whole length, so this is the
	 * step that actually raises throughput once the stands are keeping up. Capped
	 * at two because a third would need the landing and takeoff split to be worth
	 * anything, and that is a different design. */
	if (funds > 400000 && runways < 2) {
		local added = TryAddRunway(station);
		if (added != null) {
			return added + " (" + waiting + " waiting, " + serving + " aircraft on "
			       + runways + " runway)";
		}
	}
	return null;
}

/* Below this, the queue is ordinary turnover rather than unmet demand and
 * nothing needs building. */
const MIN_QUEUE_TO_GROW = 60;

/*
 * How much aeroplane an airport can work.
 *
 * Two resources bind, and which one binds is the whole question when deciding
 * what to build next:
 *
 *  - Stands. An aircraft has to park somewhere to load. More than a few sharing
 *    a stand and they spend their time waiting for it rather than flying.
 *  - Runways. A runway is reserved along its whole length, so every landing and
 *    every departure at the airport goes through that one lock, however many
 *    stands feed it.
 *
 * Both numbers are aircraft *serving the station*, which is what
 * VehiclesServingStation counts, and an aircraft on a two-airport route is
 * counted at both ends.
 */
const PLANES_PER_STAND  = 3;
const PLANES_PER_RUNWAY = 10;

/** Parking slots: stands, plus helipads for the helicopters that use them. */
function ParkingSlots(station)
{
	return CountAirportPieces(station, IsStandPiece)
	     + CountAirportPieces(station, IsHelipadPiece);
}

/**
 * The most aircraft this airport can usefully have on it.
 *
 * The lower of the two ceilings, because the tighter resource is the one that
 * decides. Which one it is also says what to build: see GrowAirport.
 */
function AircraftCeiling(station)
{
	local by_stands = PLANES_PER_STAND * ParkingSlots(station);
	local by_runway = PLANES_PER_RUNWAY * CountRunways(station);
	return (by_stands < by_runway) ? by_stands : by_runway;
}

/**
 * Runways at this airport, counted from their end pieces.
 *
 * Grass strips count: they cannot take fast jets, but they land and launch the
 * light aircraft that use them through the same single lock, so they size an
 * airport's throughput exactly the same way.
 */
function CountRunways(station)
{
	local ends = 0;
	local tiles = AITileList_StationType(station, AIStation.STATION_AIRPORT);
	foreach (t, _ in tiles) {
		if (!AIAirport.IsModularAirportTile(t)) continue;
		local p = AIAirport.GetModularPiece(t);
		if (p == AIAirport.MP_RUNWAY_END
		 || p == AIAirport.MP_RUNWAY_SMALL_NEAR_END
		 || p == AIAirport.MP_RUNWAY_SMALL_FAR_END) ends++;
	}
	return ends / 2;
}

/**
 * Lay a second runway alongside an airport that has outgrown its first.
 *
 * Looks for a straight run of clear tiles at the airport's own height with at
 * least one tile touching something aircraft can already taxi on, so the new
 * runway joins the existing network by construction rather than by hope.
 *
 * The new runway takes both landing and takeoff rather than splitting the two.
 * Splitting is better for throughput, but it means a window in which the old
 * runway is landing-only; if anything then goes wrong before the new one is
 * finished, the airport cannot launch an aircraft at all.
 */
function TryAddRunway(station)
{
	local tiles = AITileList_StationType(station, AIStation.STATION_AIRPORT);
	local ours = {}, taxiable = {};
	local minx = 99999, miny = 99999, maxx = -1, maxy = -1, base = -1;
	foreach (t, _ in tiles) {
		if (!AIAirport.IsModularAirportTile(t)) continue;
		local x = AIMap.GetTileX(t), y = AIMap.GetTileY(t);
		if (base < 0) base = AITile.GetMaxHeight(t);
		if (x < minx) minx = x;
		if (y < miny) miny = y;
		if (x > maxx) maxx = x;
		if (y > maxy) maxy = y;
		ours[t] <- true;
		if (IsThroughTaxiable(AIAirport.GetModularPiece(t))) taxiable[t] <- true;
	}
	if (base < 0) return null;

	local length = 6;
	foreach (axis in [[1, 0], [0, 1]]) {
		for (local y = miny - 3; y <= maxy + 3; y++) {
			for (local x = minx - 3; x <= maxx + 3; x++) {
				local run = [], touches = false, ok = true;
				for (local i = 0; i < length && ok; i++) {
					local tx = x + axis[0] * i, ty = y + axis[1] * i;
					if (tx < 1 || ty < 1 || tx >= AIMap.GetMapSizeX() - 1 || ty >= AIMap.GetMapSizeY() - 1) { ok = false; break; }
					local t = AIMap.GetTileIndex(tx, ty);
					if ((t in ours) || AITile.IsStationTile(t) || !AITile.IsBuildable(t)
					 || AITile.GetMaxHeight(t) != base) { ok = false; break; }
					run.append(t);
					foreach (d in [[0, 1], [0, -1], [1, 0], [-1, 0]]) {
						local n = AIMap.GetTileIndex(tx + d[0], ty + d[1]);
						if (n in taxiable) touches = true;
					}
				}
				if (!ok || !touches) continue;

				/* Runway piece rotation encodes the axis: 0 along X, 1 along Y. */
				local rot = (axis[0] == 1) ? 0 : 1;
				local built = 0;
				for (local i = 0; i < run.len(); i++) {
					local piece = (i == 0 || i == run.len() - 1)
						? AIAirport.MP_RUNWAY_END : AIAirport.MP_RUNWAY;
					if (!AIAirport.BuildModularAirportTile(run[i], piece, rot, station)) break;
					built++;
				}
				if (built < run.len()) {
					/* Partial runways are only taxiable tiles, so nothing is broken,
					 * but say so — it means the ground moved under the preflight. */
					AILog.Warning("second runway stopped after " + built + " of " + run.len() + " tiles");
					return (built > 0) ? "added a partial second runway" : null;
				}
				AIAirport.SetModularRunwayFlags(run[0],
					AIAirport.MRF_LANDING | AIAirport.MRF_TAKEOFF | AIAirport.MRF_DIR_LOW);
				return "added a second runway";
			}
		}
	}
	return null;
}

/**
 * Tiles that touch a terminal building.
 *
 * A stand against the terminal is where the passengers are, and beside a round
 * terminal it grows a jetway onto it. It is a preference and not a rule: these
 * tiles are tried first and the airport still grows wherever it can.
 */
function TilesTouchingTerminal(station)
{
	local out = {};
	local tiles = AITileList_StationType(station, AIStation.STATION_AIRPORT);
	foreach (t, _ in tiles) {
		if (!AIAirport.IsModularAirportTile(t)) continue;
		if (!IsTerminalBuildingPiece(AIAirport.GetModularPiece(t))) continue;
		local x = AIMap.GetTileX(t), y = AIMap.GetTileY(t);
		foreach (d in [[0, 1], [0, -1], [1, 0], [-1, 0]]) {
			local n = AIMap.GetTileIndex(x + d[0], y + d[1]);
			if (AIMap.IsValidTile(n)) out[n] <- true;
		}
	}
	return out;
}

/**
 * Add one piece to an existing airport.
 *
 * `taxi_adjacent` marks pieces aircraft must be able to reach — stands and
 * helipads. Buildings and decoration can go anywhere the airport touches.
 * `near_terminal` reorders the candidates rather than filtering them, so it costs
 * nothing when no tile beside a terminal is free.
 */
function AddPiece(station, piece, taxi_adjacent, near_terminal = false)
{
	if (!AIAirport.IsModularPieceAvailable(piece)) return false;
	local candidates = ExpansionTiles(station, taxi_adjacent);
	if (candidates.len() == 0) return false;

	if (near_terminal) {
		local touching = TilesTouchingTerminal(station);
		local preferred = [], others = [];
		foreach (t in candidates) {
			if (t in touching) preferred.append(t);
			else others.append(t);
		}
		foreach (t in others) preferred.append(t);
		candidates = preferred;
	}

	foreach (t in candidates) {
		if (AIAirport.BuildModularAirportTile(t, piece, 0, station)) return true;
	}
	return false;
}
