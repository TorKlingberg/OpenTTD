/*
 * Turning a chosen Site into an actual airport.
 *
 * New airports go in through PlaceModularAirportLayout, which is atomic: it
 * preflights the whole placement and either builds all of it or none. That
 * sidesteps the hard part of ChooChoo's task tree — there is no half-built
 * airport to roll back, and no window in which the script suspends between two
 * tiles of the same airport and the world changes underneath it.
 */

/**
 * Build the airport described by a Site.
 *
 * Returns the StationID, or -1. Verifies afterwards rather than trusting the
 * command: an airport that builds and does not work is the failure mode worth
 * guarding against, so read the safety status back off the map.
 */
/**
 * Re-check a site immediately before committing to it.
 *
 * A site search spans months of game time — the script suspends on every API
 * call — so by the time a site is chosen the town may have built a house on it,
 * and the region cache the search used is long stale. Re-reading ~30 tiles is
 * far cheaper than a failed build, which costs the attempt and teaches nothing.
 */
function RevalidateSite(site)
{
	local base = -1;
	foreach (t in site.Tiles()) {
		if (!AIMap.IsValidTile(t)) return false;
		if (!AITile.IsBuildable(t)) return false;
		local z = AITile.GetMaxHeight(t);
		if (base < 0) base = z;
		else if (z != base) return false;

		/* Adjoining an existing station makes the command try to join it, which
		 * fails for a new airport next to someone else's station. */
		foreach (d in [1, -1, AIMap.GetMapSizeX(), -AIMap.GetMapSizeX()]) {
			local n = t + d;
			if (AIMap.IsValidTile(n) && AITile.IsStationTile(n)) return false;
		}
	}

	/* The local authority that matters is the one closest to the airport, which
	 * is not always the town the search started from. A town takes two airports
	 * and no more, so asking the wrong one lets a doomed build through. */
	local town = AITile.GetClosestTown(site.tile);
	if (AITown.IsValidTown(town) && AITown.GetAllowedNoise(town) < 1) return false;

	return true;
}

function BuildSite(site)
{
	if (!RevalidateSite(site)) return -1;
	local layout = site.grid.ToLayout();
	/* Always rotation 0: the grid was turned by Grid.Rotate before it was fitted,
	 * so it is already in world orientation. Asking the command to rotate instead
	 * produces an airport whose aircraft never leave the hangar. */
	local ok = AIAirport.PlaceModularAirportLayout(
		site.tile, AIStation.STATION_NEW, 0, site.grid.w, site.grid.h, layout);
	if (!ok) {
		AILog.Warning("build failed at " + TileStr(site.tile) + ": " + AIError.GetLastErrorString()
		              + " (" + DescribeBuildFailure(site) + ")");
		return -1;
	}

	/* Find a tile of what we just built and read the station off it. */
	local tiles = site.Tiles();
	local station = AIStation.STATION_INVALID;
	local anchor = -1;
	foreach (t in tiles) {
		if (!AIAirport.IsModularAirportTile(t)) continue;
		local st = AIStation.GetStationID(t);
		if (AIStation.IsValidStation(st)) { station = st; anchor = t; break; }
	}
	if (!AIStation.IsValidStation(station)) {
		AILog.Error("built at " + TileStr(site.tile) + " but found no station");
		return -1;
	}

	local safety = AIAirport.GetModularAirportSafety(anchor);
	AILog.Info("built " + FamilyName(site.family) + " at " + TileStr(site.tile)
	           + " rot=" + site.rot
	           + " tiles=" + site.grid.Count()
	           + " stands=" + CountPieces(site.grid, IsStandPiece)
	           + " safety=" + safety
	           + (site.trimmed > 0 ? " trimmed=" + site.trimmed : "")
	           + " -> " + AIStation.GetName(station));
	foreach (row in site.grid.AsciiRows()) AILog.Info("    plan |" + row + "|");
	DumpBuiltAirport(station, "   ");
	return station;
}

/**
 * Why a placement was probably refused.
 *
 * PlaceModularAirportLayout reports most refusals as ERR_UNKNOWN, so the usual
 * suspects have to be checked by hand: a neighbouring station the layout would
 * have to join, ground that stopped being buildable while the script was
 * suspended, and the town's own limits.
 */
function DescribeBuildFailure(site)
{
	local reasons = [];
	local blocked = 0, wrong_height = 0, base = -1;
	local neighbour = false;

	foreach (t in site.Tiles()) {
		if (!AIMap.IsValidTile(t)) { reasons.append("off map"); break; }
		if (base < 0) base = AITile.GetMaxHeight(t);
		if (!AITile.IsBuildable(t)) blocked++;
		else if (AITile.GetMaxHeight(t) != base) wrong_height++;
		if (AITile.IsStationTile(t)) neighbour = true;
	}
	if (blocked > 0) reasons.append(blocked + " tiles not buildable");
	if (wrong_height > 0) reasons.append(wrong_height + " tiles at another height");
	if (neighbour) reasons.append("overlaps a station");
	if (AITown.GetAllowedNoise(site.town) < 1) reasons.append("town will take no more airports");
	if (site.grid.w > AIGameSettings.GetValue("station_spread")
	 || site.grid.h > AIGameSettings.GetValue("station_spread")) {
		reasons.append("wider than station_spread");
	}
	if (reasons.len() == 0) return "no obvious cause";

	local out = reasons[0];
	for (local i = 1; i < reasons.len(); i++) out += ", " + reasons[i];
	return out;
}

/**
 * Print an airport as it actually exists on the map.
 *
 * Read back rather than reprinted from the plan: the plan is in local
 * coordinates and the command rotates it, so this is the only view that shows
 * what aircraft will really have to taxi through. Every "it built fine but
 * nothing flies" bug so far has been visible here and nowhere else.
 */
function DumpBuiltAirport(station, label)
{
	local tiles = AITileList_StationType(station, AIStation.STATION_AIRPORT);
	local minx = 99999, miny = 99999, maxx = -1, maxy = -1;
	foreach (t, _ in tiles) {
		local x = AIMap.GetTileX(t), y = AIMap.GetTileY(t);
		if (x < minx) minx = x;
		if (y < miny) miny = y;
		if (x > maxx) maxx = x;
		if (y > maxy) maxy = y;
	}
	if (maxx < 0) return;

	AILog.Info(label + " as built (" + (maxx - minx + 1) + "x" + (maxy - miny + 1)
	           + " at " + minx + "," + miny + "):");
	local runways = "", stands = "";
	for (local y = miny; y <= maxy; y++) {
		local line = "";
		for (local x = minx; x <= maxx; x++) {
			local t = AIMap.GetTileIndex(x, y);
			if (!AIAirport.IsModularAirportTile(t)) { line += " "; continue; }
			local p = AIAirport.GetModularPiece(t);
			local ch = PieceChar(p);
			/* Show which way each hangar faces: 0=SE 1=NE 2=NW 3=SW. */
			if (IsHangarPiece(p)) ch = "" + AIAirport.GetModularPieceRotation(t);
			line += ch;
			if (p == AIAirport.MP_RUNWAY_END) {
				runways += " end@" + TileStr(t) + " flags=" + AIAirport.GetModularRunwayFlags(t);
			}
			if (IsStandPiece(p)) stands += " " + TileStr(t) + ":rot" + AIAirport.GetModularPieceRotation(t);
		}
		AILog.Info("    |" + line + "|");
	}
	AILog.Info("    runways:" + runways);
	AILog.Info("    stands: " + stands);
}

/** A tile as "x,y", for log lines that a human has to match up with the map. */
function TileStr(tile)
{
	return AIMap.GetTileX(tile) + "," + AIMap.GetTileY(tile);
}

/**
 * Whether an existing airport is safe for fast jets.
 *
 * Read from the map rather than remembered: growth changes it, and the whole
 * point of tracking it is deciding what aircraft may be sent there.
 */
function AirportIsLargeSafe(tile)
{
	if (!AIAirport.IsModularAirportTile(tile)) return false;
	return AIAirport.GetModularAirportSafety(tile) == AIAirport.MS_OK;
}

/** Count the stands of a built airport by walking its tiles. */
function CountAirportPieces(station, pred)
{
	local tiles = AITileList_StationType(station, AIStation.STATION_AIRPORT);
	local n = 0;
	foreach (t, _ in tiles) {
		if (!AIAirport.IsModularAirportTile(t)) continue;
		if (pred(AIAirport.GetModularPiece(t))) n++;
	}
	return n;
}
