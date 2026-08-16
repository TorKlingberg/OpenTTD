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

	/* Sound, or as sound as it can be made: add capacity where traffic asks. */
	local waiting = AIStation.GetCargoWaiting(station, pax_cargo);
	local stands = CountAirportPieces(station, IsStandPiece);
	if (waiting > 150 + stands * 60 && funds > 80000) {
		if (AddPiece(station, AIAirport.MP_STAND, true)) return "added a stand (" + waiting + " waiting)";
	}
	return null;
}

/**
 * Add one piece to an existing airport.
 *
 * `taxi_adjacent` marks pieces aircraft must be able to reach — stands and
 * helipads. Buildings and decoration can go anywhere the airport touches.
 */
function AddPiece(station, piece, taxi_adjacent)
{
	if (!AIAirport.IsModularPieceAvailable(piece)) return false;
	local candidates = ExpansionTiles(station, taxi_adjacent);
	if (candidates.len() == 0) return false;

	foreach (t in candidates) {
		if (AIAirport.BuildModularAirportTile(t, piece, 0, station)) return true;
	}
	return false;
}
