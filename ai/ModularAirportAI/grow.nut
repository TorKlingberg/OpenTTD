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
 * Most growth goes through BuildModularAirportTile one tile at a time, which is
 * not atomic the way placing a layout is. Every such step therefore has to
 * leave the airport working on its own. Legacy runways are the exception: the
 * area-upgrade command converts their complete operating surface atomically.
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
function ExpansionTiles(station, want_apron_adjacent, want_taxi_adjacent = false)
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
		/* A stand or hangar must hang off an apron tile. */
		if (want_apron_adjacent && piece != AIAirport.MP_APRON) continue;
		/* A new stand/helipad has to hang off something aircraft can taxi along. */
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
 * Sort candidates to prioritize those that minimize expanding the airport's
 * bounding rectangle.
 */
function SortByBoundingBoxImpact(station, candidates)
{
	local tiles = AITileList_StationType(station, AIStation.STATION_AIRPORT);
	local minx = 99999, miny = 99999, maxx = -1, maxy = -1;
	foreach (t, _ in tiles) {
		if (!AIAirport.IsModularAirportTile(t)) continue;
		local x = AIMap.GetTileX(t), y = AIMap.GetTileY(t);
		if (x < minx) minx = x;
		if (y < miny) miny = y;
		if (x > maxx) maxx = x;
		if (y > maxy) maxy = y;
	}
	if (maxx < 0) return candidates;
	local cur_area = (maxx - minx + 1) * (maxy - miny + 1);

	local scored = [];
	foreach (t in candidates) {
		local x = AIMap.GetTileX(t), y = AIMap.GetTileY(t);
		local nx0 = (x < minx) ? x : minx;
		local nx1 = (x > maxx) ? x : maxx;
		local ny0 = (y < miny) ? y : miny;
		local ny1 = (y > maxy) ? y : maxy;
		local new_area = (nx1 - nx0 + 1) * (ny1 - ny0 + 1);
		scored.append({ tile = t, delta = new_area - cur_area });
	}
	scored.sort(function (a, b) {
		if (a.delta < b.delta) return -1;
		if (a.delta > b.delta) return 1;
		return 0;
	});
	local out = [];
	foreach (s in scored) out.append(s.tile);
	return out;
}

/**
 * Maximum Manhattan distance from any airport tile to its nearest hangar.
 */
function MaxTileDistanceToAnyHangar(station)
{
	local tiles = AITileList_StationType(station, AIStation.STATION_AIRPORT);
	local hangars = [], airport_tiles = [];
	foreach (t, _ in tiles) {
		if (!AIAirport.IsModularAirportTile(t)) continue;
		airport_tiles.append(t);
		if (IsHangarPiece(AIAirport.GetModularPiece(t))) hangars.append(t);
	}
	if (hangars.len() == 0) return 999;
	local max_d = 0;
	foreach (t in airport_tiles) {
		local min_h = 999;
		local tx = AIMap.GetTileX(t), ty = AIMap.GetTileY(t);
		foreach (h in hangars) {
			local hx = AIMap.GetTileX(h), hy = AIMap.GetTileY(h);
			local d = (tx > hx ? tx - hx : hx - tx) + (ty > hy ? ty - hy : hy - ty);
			if (d < min_h) min_h = d;
		}
		if (min_h > max_d) max_d = min_h;
	}
	return max_d;
}

/**
 * Add a second hangar to an existing airport, ensuring it faces an apron tile.
 */
function TryAddSecondHangar(station)
{
	if (!AIAirport.IsModularPieceAvailable(AIAirport.MP_HANGAR)) return false;
	local tiles = AITileList_StationType(station, AIStation.STATION_AIRPORT);
	local aprons = {};
	foreach (t, _ in tiles) {
		if (AIAirport.IsModularAirportTile(t) && AIAirport.GetModularPiece(t) == AIAirport.MP_APRON) {
			aprons[t] <- true;
		}
	}
	if (aprons.len() == 0) return false;

	local candidates = ExpansionTiles(station, true, false);
	candidates = SortByBoundingBoxImpact(station, candidates);
	foreach (t in candidates) {
		local tx = AIMap.GetTileX(t), ty = AIMap.GetTileY(t);
		foreach (rot in [FACE_NW, FACE_SE, FACE_NE, FACE_SW]) {
			local off = FaceOffset(rot);
			local n = AIMap.GetTileIndex(tx + off[0], ty + off[1]);
			if (n in aprons) {
				if (AIAirport.BuildModularAirportTile(t, AIAirport.MP_HANGAR, rot, station)) {
					return true;
				}
			}
		}
	}
	return false;
}

/**
 * Fill unbuilt tiles inside the station's bounding rectangle.
 *
 * Places apron tiles if 3+ direct neighbors are non-empty airport tiles;
 * otherwise places empty airport ground.
 */
function TryFillEmptyBounds(station)
{
	local has_empty = AIAirport.IsModularPieceAvailable(AIAirport.MP_EMPTY);
	local has_apron = AIAirport.IsModularPieceAvailable(AIAirport.MP_APRON);
	local has_grass = AIAirport.IsModularPieceAvailable(AIAirport.MP_GRASS);
	if (!has_empty && !has_apron && !has_grass) return false;

	local tiles = AITileList_StationType(station, AIStation.STATION_AIRPORT);
	local minx = 99999, miny = 99999, maxx = -1, maxy = -1, base = -1;
	local ours = {};
	foreach (t, _ in tiles) {
		if (!AIAirport.IsModularAirportTile(t)) continue;
		local x = AIMap.GetTileX(t), y = AIMap.GetTileY(t);
		if (base < 0) base = AITile.GetMaxHeight(t);
		if (x < minx) minx = x;
		if (y < miny) miny = y;
		if (x > maxx) maxx = x;
		if (y > maxy) maxy = y;
		ours[t] <- AIAirport.GetModularPiece(t);
	}
	if (base < 0 || maxx < 0) return false;

	local built_any = false;
	for (local y = miny; y <= maxy; y++) {
		for (local x = minx; x <= maxx; x++) {
			local t = AIMap.GetTileIndex(x, y);
			if (!AIMap.IsValidTile(t)) continue;
			if (t in ours) continue;
			if (AITile.IsStationTile(t)) continue;
			if (!AITile.IsBuildable(t)) continue;
			if (AITile.GetMaxHeight(t) != base) continue;

			local touches = false;
			local non_empty = 0;
			foreach (d in [[0, 1], [0, -1], [1, 0], [-1, 0]]) {
				local n = AIMap.GetTileIndex(x + d[0], y + d[1]);
				if (n in ours) {
					touches = true;
					if (IsNonEmptyAirportPiece(ours[n])) non_empty++;
				}
			}
			if (!touches) continue;

			local piece = (non_empty >= 3 && has_apron) ? AIAirport.MP_APRON :
			              (has_empty ? AIAirport.MP_EMPTY : AIAirport.MP_GRASS);
			if (AIAirport.BuildModularAirportTile(t, piece, 0, station)) {
				ours[t] <- piece;
				built_any = true;
			}
		}
	}
	return built_any;
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
	local has_legacy = AirportHasLegacyPieces(station);

	/* A three-tile terminal is part of the character of an old airfield, even
	 * though it buys no capacity. Try it before modernising away the evidence
	 * that this was a grass strip. The compound placement is atomic, so cramped
	 * airports simply decline it without leaving half a building. */
	if (has_legacy && funds > 100000 && AIBase.RandRange(8) == 0
	 && !AirportHasPiece(station, AIAirport.MP_SMALL_TERMINAL_3)) {
		if (TryAddSmallTerminal(station)) return "added an old three-tile terminal";
	}

	/* Modernisation is intentionally occasional, but a runway is one operating
	 * surface and is converted as one atomic job. This avoids the incoherent
	 * half-grass, half-paved state that tile-at-a-time conversion produced. */
	if (has_legacy && funds > 180000 && AIBase.RandRange(32) == 0) {
		local upgraded = TryUpgradeLegacyPiece(station);
		if (upgraded != null) return upgraded;
	}

	local missing_runway = safety & (AIAirport.MS_MISSING_LANDING_RUNWAY | AIAirport.MS_MISSING_TAKEOFF_RUNWAY);

	/* A tower and a terminal are only worth buying once runway modernisation
	 * above has produced a six-tile paved runway. Adding the
	 * buildings earlier would spend real money to move the safety mask from 15
	 * to 12, which changes nothing about what may safely land there. */
	if (safety != AIAirport.MS_OK && safety >= 0 && missing_runway == 0) {
		if (safety & AIAirport.MS_MISSING_TOWER) {
			if (AddPiece(station, AIAirport.MP_TOWER, false)) {
				TryFillEmptyBounds(station);
				return "added a control tower";
			}
		}
		if (safety & AIAirport.MS_MISSING_BIG_TERMINAL) {
			local variants = BigTerminalVariants();
			local piece = variants[AIBase.RandRange(variants.len())];
			if (AddPiece(station, piece, false)) {
				TryFillEmptyBounds(station);
				return "added a terminal building";
			}
		}
		return null;
	}

	local slots = ParkingSlots(station);
	local runways = CountRunways(station);

	/* Once an airport gets large (many stands/runways or any tile > 10 from a hangar),
	 * add a second hangar so aircraft never have to taxi too far across the field. */
	local hangars_count = CountAirportPieces(station, IsHangarPiece);
	local max_h_dist = MaxTileDistanceToAnyHangar(station);
	local is_large = slots >= 5 || runways >= 2 || max_h_dist > 10;
	if (hangars_count < 2 && is_large && funds > 80000) {
		if (TryAddSecondHangar(station)) {
			TryFillEmptyBounds(station);
			return "added a second hangar (max hangar distance was " + max_h_dist + ")";
		}
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
	 * ground is what is holding the route back. */
	local waiting = AIStation.GetCargoWaiting(station, pax_cargo);
	if (waiting <= MIN_QUEUE_TO_GROW) {
		if (funds > 100000 && TryFillEmptyBounds(station)) {
			return "filled empty tiles in bounding rectangle";
		}
		return null;
	}

	local serving = VehiclesServingStation(station);
	if (serving < AircraftCeiling(station)) {
		if (funds > 100000 && TryFillEmptyBounds(station)) {
			return "filled empty tiles in bounding rectangle";
		}
		return null;
	}

	/* A heliport has no runway by design. Once its pads are full, grow the
	 * resource helicopters actually use instead of treating the absent runway
	 * as a bottleneck and trying to turn the heliport into an aerodrome. */
	if (runways == 0) {
		if (funds > 80000 && AddHelipad(station)) {
			TryFillEmptyBounds(station);
			return "added a helipad (" + waiting + " waiting, " + serving
			       + " helicopters on " + slots + " pads)";
		}
		return null;
	}

	local stand_bound = PLANES_PER_STAND * slots <= PLANES_PER_RUNWAY * runways;

	if (stand_bound) {
		if (funds > 80000 && AddPiece(station, AIAirport.MP_STAND, true, true)) {
			TryFillEmptyBounds(station);
			return "added a stand (" + waiting + " waiting, " + serving + " aircraft on "
			       + slots + " stands)";
		}
		return null;
	}

	/* Runway-bound: lengthen a runway sometimes before buying a whole second
	 * strip. Length is
	 * capped, because runway reservations cover the complete segment and an
	 * indefinitely long runway would reduce rather than increase throughput. */
	if (funds > 180000 && AIBase.RandRange(3) == 0) {
		local extended = TryExtendRunway(station, 8);
		if (extended != null) {
			TryFillEmptyBounds(station);
			return extended + " (" + waiting + " waiting, " + serving + " aircraft)";
		}
	}

	/* More strips, not just longer ones. The runway is the airport's single
	 * busiest lock, so a station that is still runway-bound with a strip already
	 * added is telling us it wants another one. The cap is there because each
	 * runway also has to be reachable and worth its upkeep, not because two is a
	 * natural limit. */
	if (funds > 400000 && runways < MAX_RUNWAYS) {
		local added = TryAddRunway(station);
		if (added != null) {
			TryFillEmptyBounds(station);
			return added + " (" + waiting + " waiting, " + serving + " aircraft on "
			       + runways + " runway)";
		}
	}

	if (funds > 80000 && TryFillEmptyBounds(station)) {
		return "filled empty tiles in bounding rectangle";
	}
	return null;
}

/** Whether a built airport contains one of the old grass-era pieces. */
function AirportHasLegacyPieces(station)
{
	local tiles = AITileList_StationType(station, AIStation.STATION_AIRPORT);
	foreach (t, _ in tiles) {
		if (!AIAirport.IsModularAirportTile(t)) continue;
		local p = AIAirport.GetModularPiece(t);
		if (IsSmallRunwayPiece(p) || p == AIAirport.MP_SMALL_HANGAR) return true;
	}
	return false;
}

/** Whether any tile of an airport reads back as a particular named piece. */
function AirportHasPiece(station, wanted)
{
	local tiles = AITileList_StationType(station, AIStation.STATION_AIRPORT);
	foreach (t, _ in tiles) {
		if (AIAirport.IsModularAirportTile(t) && AIAirport.GetModularPiece(t) == wanted) return true;
	}
	return false;
}

/** Try every plausible origin for the compound old terminal. */
function TryAddSmallTerminal(station)
{
	if (!AIAirport.IsModularPieceAvailable(AIAirport.MP_SMALL_TERMINAL_3)) return false;
	foreach (t in ExpansionTiles(station, false)) {
		if (AIAirport.BuildModularAirportTile(t, AIAirport.MP_SMALL_TERMINAL_3, 0, station)) return true;
	}
	return false;
}

/** Whether a tile is part of this station's legacy runway. */
function IsLegacyRunwayTile(station, tile)
{
	return AIMap.IsValidTile(tile)
	    && AIAirport.IsModularAirportTile(tile)
	    && AIStation.GetStationID(tile) == station
	    && IsSmallRunwayPiece(AIAirport.GetModularPiece(tile));
}

/**
 * The two ends of the legacy runway containing tile.
 *
 * Legacy runways are axis-locked along X, so walking left and right finds the
 * exact one-row area that the atomic upgrade command should convert.
 */
function LegacyRunwayEnds(station, tile)
{
	local y = AIMap.GetTileY(tile);
	local left_x = AIMap.GetTileX(tile), right_x = left_x;
	while (left_x > 0) {
		local next = AIMap.GetTileIndex(left_x - 1, y);
		if (!IsLegacyRunwayTile(station, next)) break;
		left_x--;
	}
	while (right_x + 1 < AIMap.GetMapSizeX()) {
		local next = AIMap.GetTileIndex(right_x + 1, y);
		if (!IsLegacyRunwayTile(station, next)) break;
		right_x++;
	}
	return [AIMap.GetTileIndex(left_x, y), AIMap.GetTileIndex(right_x, y)];
}

/** Upgrade one whole old runway, or one old hangar. */
function TryUpgradeLegacyPiece(station)
{
	if (!AIAirport.IsModularPieceAvailable(AIAirport.MP_RUNWAY)
	 && !AIAirport.IsModularPieceAvailable(AIAirport.MP_HANGAR)) return null;

	local candidates = [], seen_runways = {};
	local tiles = AITileList_StationType(station, AIStation.STATION_AIRPORT);
	foreach (t, _ in tiles) {
		if (!AIAirport.IsModularAirportTile(t)) continue;
		local p = AIAirport.GetModularPiece(t);
		if (IsSmallRunwayPiece(p)) {
			local ends = LegacyRunwayEnds(station, t);
			if (!(ends[0] in seen_runways)) {
				seen_runways[ends[0]] <- true;
				candidates.append([ends[0], ends[1], true]);
			}
		} else if (p == AIAirport.MP_SMALL_HANGAR) {
			candidates.append([t, t, false]);
		}
	}
	if (candidates.len() == 0) return null;

	local start = AIBase.RandRange(candidates.len());
	for (local i = 0; i < candidates.len(); i++) {
		local c = candidates[(start + i) % candidates.len()];
		if (c[2]) {
			if (!AIAirport.UpgradeModularAirportArea(c[0], c[1])) continue;
			return "paved the entire " + (AIMap.DistanceManhattan(c[0], c[1]) + 1)
			     + "-tile old runway";
		}
		if (AIAirport.UpgradeModularAirportTile(c[0])) return "upgraded the old hangar";
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
const PLANES_PER_RUNWAY = 8;

/**
 * How many runways one airport may grow to.
 *
 * PLANES_PER_RUNWAY is both the "add a runway" trigger and the fleet ceiling,
 * so the two numbers have to move together: lowering the first without raising
 * this one would not build more runways, it would just cap every airport
 * smaller.
 */
const MAX_RUNWAYS = 4;

/** Parking slots: stands, plus helipads for the helicopters that use them. */
function ParkingSlots(station)
{
	return CountAirportPieces(station, IsStandPiece)
	     + CountAirportPieces(station, IsHelipadPiece);
}

/** Add one of the interchangeable helipad styles as capacity. */
function AddHelipad(station)
{
	foreach (piece in [AIAirport.MP_HELIPAD,
	                   AIAirport.MP_HELIPAD_PLAIN,
	                   AIAirport.MP_HELIPORT]) {
		if (AddPiece(station, piece, true)) return true;
	}
	return false;
}

/** Pure capacity calculation, kept separate so the model can be self-tested. */
function AircraftCeilingForCounts(stands, helipads, runways)
{
	/* With no runway, only helicopters can operate and only helipads provide
	 * useful parking. In particular, a malformed stand-only airport must not be
	 * given helicopter capacity merely because it has parking tiles. */
	if (runways == 0) return PLANES_PER_STAND * helipads;

	local by_parking = PLANES_PER_STAND * (stands + helipads);
	local by_runway = PLANES_PER_RUNWAY * runways;
	return (by_parking < by_runway) ? by_parking : by_runway;
}

/**
 * The most aircraft this airport can usefully have on it.
 *
 * The lower of the two ceilings, because the tighter resource is the one that
 * decides. Which one it is also says what to build: see GrowAirport.
 */
function AircraftCeiling(station)
{
	return AircraftCeilingForCounts(
		CountAirportPieces(station, IsStandPiece),
		CountAirportPieces(station, IsHelipadPiece),
		CountRunways(station));
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

/** A map-safe orthogonal offset, or -1 at the map edge. */
function OffsetAirportTile(tile, dx, dy)
{
	local x = AIMap.GetTileX(tile) + dx;
	local y = AIMap.GetTileY(tile) + dy;
	if (x < 1 || y < 1 || x >= AIMap.GetMapSizeX() - 1 || y >= AIMap.GetMapSizeY() - 1) return -1;
	return AIMap.GetTileIndex(x, y);
}

/** Whether this tile continues this station's runway along the requested axis. */
function IsRunwayOnAxis(station, tile, rotation)
{
	if (tile < 0 || !AIAirport.IsModularAirportTile(tile)) return false;
	if (AIStation.GetStationID(tile) != station) return false;
	if (!IsRunwayPiece(AIAirport.GetModularPiece(tile))) return false;
	return (AIAirport.GetModularPieceRotation(tile) % 2) == (rotation % 2);
}

/**
 * Add one tile to the end of a runway shorter than max_length.
 *
 * Building beside an end invokes the engine's runway visual normaliser, which
 * turns the old end into a middle and the new tile into the new end. Flags are
 * inherited from the adjoining segment by the build command.
 */
function TryExtendRunway(station, max_length)
{
	local candidates = [];
	local tiles = AITileList_StationType(station, AIStation.STATION_AIRPORT);
	foreach (t, _ in tiles) {
		if (!AIAirport.IsModularAirportTile(t)) continue;
		local p = AIAirport.GetModularPiece(t);
		if (!IsRunwayPiece(p)) continue;

		local rot = AIAirport.GetModularPieceRotation(t);
		local ax = (rot % 2) == 0 ? 1 : 0;
		local ay = (rot % 2) == 0 ? 0 : 1;
		local lo = OffsetAirportTile(t, -ax, -ay);
		local hi = OffsetAirportTile(t,  ax,  ay);
		local has_lo = IsRunwayOnAxis(station, lo, rot);
		local has_hi = IsRunwayOnAxis(station, hi, rot);
		/* Exactly one runway neighbour identifies an end. */
		if (has_lo == has_hi) continue;

		local outward_x = has_lo ? ax : -ax;
		local outward_y = has_lo ? ay : -ay;
		local inward_x = -outward_x;
		local inward_y = -outward_y;
		local length = 1;
		local walk = OffsetAirportTile(t, inward_x, inward_y);
		while (IsRunwayOnAxis(station, walk, rot)) {
			length++;
			walk = OffsetAirportTile(walk, inward_x, inward_y);
		}
		if (length >= max_length) continue;

		local outside = OffsetAirportTile(t, outward_x, outward_y);
		if (outside < 0 || AITile.IsStationTile(outside) || !AITile.IsBuildable(outside)) continue;
		if (AITile.GetMaxHeight(outside) != AITile.GetMaxHeight(t)) continue;
		candidates.append({ tile = outside, rotation = rot, length = length,
		                    small = IsSmallRunwayPiece(p) });
	}
	if (candidates.len() == 0) return null;

	local start = AIBase.RandRange(candidates.len());
	for (local i = 0; i < candidates.len(); i++) {
		local c = candidates[(start + i) % candidates.len()];
		local piece = c.small ? AIAirport.MP_RUNWAY_SMALL_MIDDLE : AIAirport.MP_RUNWAY;
		if (AIAirport.BuildModularAirportTile(c.tile, piece, c.rotation, station)) {
			return "extended a runway to " + (c.length + 1) + " tiles";
		}
	}
	return null;
}

/**
 * Lay a second runway alongside an airport that has outgrown its first.
 *
 * Looks for a straight run of clear tiles at the airport's own height with at
 * least one tile touching something aircraft can already taxi on, so the new
 * runway joins the existing network by construction rather than by hope.
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
					AILog.Warning("extra runway stopped after " + built + " of " + run.len() + " tiles");
					return (built > 0) ? "added a partial extra runway" : null;
				}
				AIAirport.SetModularRunwayFlags(run[0],
					AIAirport.MRF_LANDING | AIAirport.MRF_TAKEOFF | AIAirport.MRF_DIR_LOW);
				return "added a runway";
			}
		}
	}
	return null;
}

/**
 * Tiles that touch a terminal building.
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
 * Stands and hangars must be placed adjacent to an apron tile.
 * Helipads must be taxi-adjacent.
 * Candidates are sorted to minimize bounding rectangle growth.
 */
function AddPiece(station, piece, want_taxi_or_apron, near_terminal = false)
{
	if (!AIAirport.IsModularPieceAvailable(piece)) return false;
	local want_apron = (piece == AIAirport.MP_STAND || piece == AIAirport.MP_HANGAR);
	local candidates = ExpansionTiles(station, want_apron, want_taxi_or_apron);
	if (candidates.len() == 0) return false;

	candidates = SortByBoundingBoxImpact(station, candidates);

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
