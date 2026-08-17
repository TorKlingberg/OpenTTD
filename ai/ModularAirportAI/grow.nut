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

	/* Sound, or as sound as it can be made: add capacity where traffic asks.
	 *
	 * The trigger is a queue of waiting passengers, which is an indirect signal:
	 * BuyOneAircraft will not put more than two aircraft on a stand, so a stand is
	 * also how the AI raises the fleet cap on a route. That is what makes the
	 * queue respond to stands at all, and gating growth on the current fleet
	 * instead — tried, measured — costs about a seventh of total throughput
	 * because it stops the fleet before it starts.
	 *
	 * What it needs is a ceiling rather than a different trigger. Left uncapped it
	 * chased a queue that a growing town refills faster than any number of stands
	 * can drain, and built airports with nine of them. */
	local waiting = AIStation.GetCargoWaiting(station, pax_cargo);
	local stands = CountAirportPieces(station, IsStandPiece);
	if (waiting > 60 + stands * 25 && stands < StandCap(station) && funds > 80000) {
		if (AddPiece(station, AIAirport.MP_STAND, true, true)) {
			return "added a stand (" + waiting + " waiting, " + (stands + 1) + " stands)";
		}
	}

	/* A second runway is the step that actually raises throughput once the
	 * stands are keeping up: a runway is reserved atomically along its whole
	 * length, so one runway serialises every landing and departure however many
	 * stands feed it. */
	if (waiting > 120 + stands * 30 && funds > 400000 && CountRunways(station) < 2) {
		local added = TryAddRunway(station);
		if (added != null) return added + " (" + waiting + " waiting)";
	}
	return null;
}

/**
 * The most stands worth having here.
 *
 * A runway is reserved along its whole length, so every landing and departure at
 * a one-runway airport goes through the same lock however many stands feed it.
 * Six stands already allow twelve aircraft on one runway under the two-per-stand
 * rule in BuyOneAircraft, which is more than it can cycle; past that the extra
 * tiles buy upkeep, taxi distance and a longer queue for the same runway. A
 * second runway roughly doubles what the ground can absorb.
 *
 * Airports whose only runway is a grass strip count zero here and cap at three,
 * which suits them: they are the AI's cheap opening move, not its hubs.
 */
function StandCap(station)
{
	return 3 + 3 * CountRunways(station);
}

/** Large runways at this airport, counted from their end pieces. */
function CountRunways(station)
{
	local ends = 0;
	local tiles = AITileList_StationType(station, AIStation.STATION_AIRPORT);
	foreach (t, _ in tiles) {
		if (!AIAirport.IsModularAirportTile(t)) continue;
		if (AIAirport.GetModularPiece(t) == AIAirport.MP_RUNWAY_END) ends++;
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
 * Tiles that touch a hangar.
 *
 * A stand beside the hangar shortens every servicing trip and the first trip a
 * newly built aircraft makes. It is a preference and not a rule: these tiles are
 * tried first and the airport still grows wherever it can.
 */
function TilesTouchingHangar(station)
{
	local out = {};
	local tiles = AITileList_StationType(station, AIStation.STATION_AIRPORT);
	foreach (t, _ in tiles) {
		if (!AIAirport.IsModularAirportTile(t)) continue;
		if (!IsHangarPiece(AIAirport.GetModularPiece(t))) continue;
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
 * `near_hangar` reorders the candidates rather than filtering them, so it costs
 * nothing when no tile beside the hangar is free.
 */
function AddPiece(station, piece, taxi_adjacent, near_hangar = false)
{
	if (!AIAirport.IsModularPieceAvailable(piece)) return false;
	local candidates = ExpansionTiles(station, taxi_adjacent);
	if (candidates.len() == 0) return false;

	if (near_hangar) {
		local touching = TilesTouchingHangar(station);
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
