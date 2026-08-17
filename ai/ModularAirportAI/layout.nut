/*
 * The layout generator.
 *
 * Pure functions from parameters to a Grid. No API calls that touch the world,
 * so everything here can be exercised offline by selftest.nut.
 *
 * The design follows AAAHogEx's station factories rather than ChooChoo's
 * hand-written blueprints: a small number of *families*, each parameterised, so
 * one family covers a two-stand airstrip and a ten-stand international airport.
 * Variety comes from choosing among families, sampling their parameters, and
 * from cosmetic choices that do not affect function.
 */

/* Tiles the three-tile small terminal occupies, along X from its west end. */
const SMALL_TERMINAL_WIDTH = 3;

/* Three tiles are the engine's operational floor for a runway, but the AI's
 * smallest useful legacy field is deliberately one tile longer. */
const MIN_LEGACY_RUNWAY_LENGTH = 4;

enum Family {
	STRIP,     ///< Small grass runway, a couple of stands. Buildable from year zero.
	LINEAR,    ///< One large runway, apron spine, a row of stands behind it.
	PIER,      ///< One large runway, stands on a finger reaching away from it.
	DUAL,      ///< Two runways, landing and takeoff split, stands in the middle.
	APRON,     ///< One large runway with an open apron block of stands.
	HELIPORT,  ///< Helipads only. No runway, so no fixed-wing traffic.
}

const FAMILY_COUNT = 6;

function FamilyName(family)
{
	switch (family) {
		case Family.STRIP:    return "strip";
		case Family.LINEAR:   return "linear";
		case Family.PIER:     return "pier";
		case Family.DUAL:     return "dual";
		case Family.APRON:    return "apron";
		case Family.HELIPORT: return "heliport";
	}
	return "?";
}

/** The large terminal variants, which are interchangeable except to look at. */
function BigTerminalVariants()
{
	return [AIAirport.MP_TERMINAL, AIAirport.MP_TERMINAL_ALT,
	        AIAirport.MP_TERMINAL_OTHER, AIAirport.MP_TERMINAL_ROUND];
}

/** Purely decorative pieces to scatter on tiles nothing else wants. */
function CosmeticVariants()
{
	/* Keep both radar surfaces in the pool. This deliberately gives radar two
	 * looks without making the very tall radio mast the de-facto visual theme. */
	return [AIAirport.MP_RADAR_GRASS, AIAirport.MP_RADAR,
	        AIAirport.MP_FLAG_GRASS, AIAirport.MP_LOW_TERMINAL,
	        AIAirport.MP_RADIO_TOWER, AIAirport.MP_GRASS];
}

/**
 * Default parameters for a family. Callers override what they care about;
 * RandomParams fills the rest with variety.
 */
function DefaultParams()
{
	return {
		runway_length = 6,   ///< tiles, including both ends
		stands        = 3,
		helipads      = 0,
		large_safe    = true,
		hangar_at_end = true,  ///< hangar at the low-x end of the service row
		terminal      = AIAirport.MP_TERMINAL,
		helipad_style = 0,   ///< 0 MP_HELIPAD, 1 plain "H", 2 rooftop heliport
		pier_depth    = 3,
		pier_double   = true,  ///< stands on both sides of the pier spine
		cosmetics     = 2,   ///< how many decorative tiles to try to add
		small_terminal = false, ///< try to fit the three-tile terminal into the decoration
		extra_terminals = 0, ///< optional additional full-size terminal buildings
		strip_compact = false, ///< four-tile strip with stands directly on the runway
		cosmetic_kind = AIAirport.MP_RADAR_GRASS,
		apron_rows    = 2,
		mirror        = false,
	};
}

/* ------------------------------------------------------------------------- */

/**
 * A minimal grass airfield: small runway, apron, a couple of stands, a small
 * hangar. Small runways are not large-safe by construction, so this is for
 * light aircraft and for the years before 1955 when the modern pieces do not
 * exist yet.
 */
function GenerateStrip(params)
{
	local len = params.runway_length;
	if (len < MIN_LEGACY_RUNWAY_LENGTH) len = MIN_LEGACY_RUNWAY_LENGTH;
	local g = Grid(len, 3);
	local compact = params.strip_compact && len == 4 && params.small_terminal
	             && AIAirport.IsModularPieceAvailable(AIAirport.MP_SMALL_TERMINAL_3);

	g.Set(0, 2, AIAirport.MP_RUNWAY_SMALL_FAR_END, 0,
	      AIAirport.MRF_LANDING | AIAirport.MRF_TAKEOFF | AIAirport.MRF_DIR_LOW);
	for (local x = 1; x < len - 1; x++) {
		g.Set(x, 2, AIAirport.MP_RUNWAY_SMALL_MIDDLE, 0,
		      AIAirport.MRF_LANDING | AIAirport.MRF_TAKEOFF | AIAirport.MRF_DIR_LOW);
	}
	g.Set(len - 1, 2, AIAirport.MP_RUNWAY_SMALL_NEAR_END, 0,
	      AIAirport.MRF_LANDING | AIAirport.MRF_TAKEOFF | AIAirport.MRF_DIR_LOW);

	/* The compact 4x3 style spends its middle row on hangar/stands and lets them
	 * enter the runway directly. That leaves the back row for the old three-tile
	 * terminal. The other styles retain a separate apron spine. */
	if (!compact) {
		for (local x = 0; x < len; x++) g.Set(x, 1, AIAirport.MP_APRON);
	}

	/* The service row goes *above* the apron here, not below as in the other
	 * families, because a small hangar has only one graphic — the SE one — and
	 * must therefore face SE, onto the apron at (x, y+1). Facing it any other way
	 * would work mechanically and look wrong on screen. */
	local service_y = compact ? 1 : 0;
	g.Set(0, service_y, AIAirport.MP_SMALL_HANGAR, FACE_SE);
	local placed = 0;
	for (local x = 1; x < len && placed < params.stands; x++) {
		g.Set(x, service_y, AIAirport.MP_STAND, 0, 0, placed >= 2);
		placed++;
	}
	if (compact) g.SetWide(1, 0, AIAirport.MP_SMALL_TERMINAL_3, SMALL_TERMINAL_WIDTH, true);
	return g.Normalise();
}

/**
 * One large runway with a parallel apron spine and a row of stands behind it.
 * The workhorse: cheap, compact, and large-safe as soon as the runway reaches
 * six tiles.
 */
function GenerateLinear(params)
{
	local len = params.runway_length;
	if (len < 6) len = 6;
	local rwy = AIAirport.MRF_LANDING | AIAirport.MRF_TAKEOFF | AIAirport.MRF_DIR_LOW;
	local g = Grid(len, 4);

	g.Set(0, 0, AIAirport.MP_RUNWAY_END, 0, rwy);
	for (local x = 1; x < len - 1; x++) g.Set(x, 0, AIAirport.MP_RUNWAY, 0, rwy);
	g.Set(len - 1, 0, AIAirport.MP_RUNWAY_END, 0, rwy);

	for (local x = 0; x < len; x++) g.Set(x, 1, AIAirport.MP_APRON);

	/* Service row: hangar at one end, then stands, then the buildings that make
	 * the airport large-safe at the other end. Buildings go last because they
	 * are not taxiable and must never sit between the hangar and the apron. */
	local cursor = params.hangar_at_end ? 0 : len - 1;
	local step = params.hangar_at_end ? 1 : -1;
	g.Set(cursor, 2, AIAirport.MP_HANGAR, FACE_NW);
	cursor += step;

	local placed = 0;
	while (placed < params.stands && cursor >= 0 && cursor < len) {
		g.Set(cursor, 2, AIAirport.MP_STAND, 0, 0, placed >= 2);
		cursor += step;
		placed++;
	}
	if (params.large_safe) {
		/* Terminal first, so it lands against the last stand rather than behind
		 * the tower. Both are non-taxiable buildings, so the order is purely what
		 * it looks like from the ground. */
		if (cursor >= 0 && cursor < len) { g.Set(cursor, 2, params.terminal); cursor += step; }
		if (cursor >= 0 && cursor < len) {
			g.Set(cursor, 2, AIAirport.MP_TOWER); cursor += step;
		}
	}
	/* Helipads go in whatever is left of the service row: they only need the
	 * apron above them, which the whole row has. */
	if (params.helipads > 0) AddHelipadsAlongApron(g, params, 2, 0, len - 1);
	AddExtraTerminalFrontage(g, params, 3);
	return g.Normalise();
}

/**
 * A pier: the stands hang off a spine running away from the runway rather than
 * lying alongside it. Narrower than LINEAR for the same stand count, which is
 * what makes it the family that fits between two hills.
 */
function GeneratePier(params)
{
	local len = params.runway_length;
	if (len < 6) len = 6;
	local depth = params.pier_depth;
	if (depth < 1) depth = 1;
	local rwy = AIAirport.MRF_LANDING | AIAirport.MRF_TAKEOFF | AIAirport.MRF_DIR_LOW;
	local g = Grid(len, 2 + depth + 1);

	g.Set(0, 0, AIAirport.MP_RUNWAY_END, 0, rwy);
	for (local x = 1; x < len - 1; x++) g.Set(x, 0, AIAirport.MP_RUNWAY, 0, rwy);
	g.Set(len - 1, 0, AIAirport.MP_RUNWAY_END, 0, rwy);
	for (local x = 0; x < len; x++) g.Set(x, 1, AIAirport.MP_APRON);

	/* Spine one tile in from the left, so stands fit on both sides. */
	local sx = 1;
	local placed = 0;
	for (local d = 0; d < depth; d++) {
		local y = 2 + d;
		g.Set(sx, y, AIAirport.MP_APRON, 0, 0, d >= 1);
		if (placed < params.stands) {
			g.Set(sx - 1, y, AIAirport.MP_STAND, 0, 0, placed >= 2);
			placed++;
		}
		if (params.pier_double && placed < params.stands) {
			g.Set(sx + 1, y, AIAirport.MP_STAND, 0, 0, placed >= 2);
			placed++;
		}
	}

	/* Hangar closes the end of the spine, facing back up it. */
	local hy = 2 + depth;
	g.Set(sx, hy, AIAirport.MP_HANGAR, FACE_NW);

	if (params.large_safe) {
		/* Buildings go on the far side of the runway row, well clear of the
		 * spine, so they can never block it. */
		g.Set(len - 1, 2, AIAirport.MP_TOWER);
		g.Set(len - 2, 2, params.terminal);
	}
	/* Helipads hang off the runway-parallel taxiway, between the pier spine and
	 * the buildings, where they cannot collide with either. */
	if (params.helipads > 0) AddHelipadsAlongApron(g, params, 2, sx + 2, len - 3);
	return g.Normalise();
}

/**
 * Two parallel runways with landing and takeoff split between them, stands in
 * the middle. Twice the runway capacity of LINEAR and the shape a busy airport
 * wants, but it needs a wide site and costs accordingly.
 */
function GenerateDual(params)
{
	local len = params.runway_length;
	if (len < 6) len = 6;
	local g = Grid(len, 6);

	local land = AIAirport.MRF_LANDING | AIAirport.MRF_DIR_LOW;
	local take = AIAirport.MRF_TAKEOFF | AIAirport.MRF_DIR_HIGH;

	g.Set(0, 0, AIAirport.MP_RUNWAY_END, 0, land);
	for (local x = 1; x < len - 1; x++) g.Set(x, 0, AIAirport.MP_RUNWAY, 0, land);
	g.Set(len - 1, 0, AIAirport.MP_RUNWAY_END, 0, land);

	g.Set(0, 4, AIAirport.MP_RUNWAY_END, 0, take);
	for (local x = 1; x < len - 1; x++) g.Set(x, 4, AIAirport.MP_RUNWAY, 0, take);
	g.Set(len - 1, 4, AIAirport.MP_RUNWAY_END, 0, take);

	for (local x = 0; x < len; x++) {
		g.Set(x, 1, AIAirport.MP_APRON);
		g.Set(x, 3, AIAirport.MP_APRON);
	}

	local cursor = 0;
	g.Set(cursor++, 2, AIAirport.MP_HANGAR, FACE_NW);
	local placed = 0;
	while (placed < params.stands && cursor < len) {
		g.Set(cursor++, 2, AIAirport.MP_STAND, 0, 0, placed >= 2);
		placed++;
	}
	if (params.large_safe) {
		/* Terminal against the stands, tower behind it; see GenerateLinear. */
		if (cursor < len) g.Set(cursor++, 2, params.terminal);
		if (cursor < len) g.Set(cursor++, 2, AIAirport.MP_TOWER);
	}
	/* Anything left in the middle row stays apron so the two taxiways connect
	 * around the buildings rather than dead-ending at them. */
	while (cursor < len) {
		g.Set(cursor++, 2, AIAirport.MP_APRON, 0, 0, true);
	}
	AddExtraTerminalFrontage(g, params, 5);
	return g.Normalise();
}

/**
 * One runway with an open block of apron and stands. The simplest shape and the
 * one that degrades most gracefully when the site is an odd shape, because
 * almost every tile past the runway is optional.
 *
 * Note the throughput cost: a wide open apron is a single atomic FREE_MOVE
 * segment, so it serialises. Good for a quiet airport, not for a busy one.
 */
function GenerateApron(params)
{
	local len = params.runway_length;
	if (len < 6) len = 6;
	local rows = params.apron_rows;
	if (rows < 1) rows = 1;
	local rwy = AIAirport.MRF_LANDING | AIAirport.MRF_TAKEOFF | AIAirport.MRF_DIR_LOW;
	local g = Grid(len, 1 + rows + 2);

	g.Set(0, 0, AIAirport.MP_RUNWAY_END, 0, rwy);
	for (local x = 1; x < len - 1; x++) g.Set(x, 0, AIAirport.MP_RUNWAY, 0, rwy);
	g.Set(len - 1, 0, AIAirport.MP_RUNWAY_END, 0, rwy);
	for (local x = 0; x < len; x++) g.Set(x, 1, AIAirport.MP_APRON);

	local placed = 0;
	for (local r = 0; r < rows; r++) {
		local y = 2 + r;
		for (local x = 0; x < len; x++) {
			if (placed < params.stands) {
				g.Set(x, y, AIAirport.MP_STAND, 0, 0, placed >= 2);
				placed++;
			} else {
				g.Set(x, y, AIAirport.MP_APRON, 0, 0, true);
			}
		}
	}

	local hy = 1 + rows;
	g.Set(0, hy, AIAirport.MP_HANGAR, FACE_NW);
	if (params.large_safe) {
		g.Set(len - 1, hy, AIAirport.MP_TOWER, 0, 0, false);
		g.Set(len - 2, hy, params.terminal, 0, 0, false);
	}
	if (params.helipads > 0) AddHelipadsAlongApron(g, params, hy, 1, len - 3);
	AddExtraTerminalFrontage(g, params, hy + 1);
	return g.Normalise();
}

/**
 * Helipads and a hangar, no runway. Tiny, cheap, and the only thing that fits
 * some sites at all — but it carries helicopters only.
 */
function GenerateHeliport(params)
{
	local pads = params.helipads;
	if (pads < 1) pads = 1;

	/* Apron spine down the middle with pads either side, so every pad touches
	 * the network however many there are. A single-sided variant exists purely
	 * so two heliports on one map do not look like the same building. */
	local both = params.pier_double;
	local rows = both ? (pads + 1) / 2 : pads;
	local g = Grid(3, rows + 1);

	local placed = 0;
	for (local r = 0; r < rows; r++) {
		g.Set(1, r, AIAirport.MP_APRON);
		if (placed < pads) { g.Set(0, r, HelipadPiece(params), 0, 0, placed >= 1); placed++; }
		if (both && placed < pads) { g.Set(2, r, HelipadPiece(params), 0, 0, placed >= 1); placed++; }
	}
	g.Set(1, rows, AIAirport.MP_APRON);
	g.Set(0, rows, AIAirport.MP_HANGAR, FACE_SW);
	return g.Normalise();
}

/** Helipads come in three looks with identical behaviour. */
function HelipadPiece(params)
{
	switch (params.helipad_style) {
		case 1: return AIAirport.MP_HELIPAD_PLAIN;
		case 2: return AIAirport.MP_HELIPORT;
	}
	return AIAirport.MP_HELIPAD;
}

/**
 * Hang helipads off a row of apron. Helipads are destinations, not through
 * routes, so each one only needs a single apron neighbour — which makes the
 * runway-parallel taxiway the natural place to put them.
 */
function AddHelipadsAlongApron(grid, params, row, from_x, to_x)
{
	local placed = 0;
	for (local x = from_x; x <= to_x && placed < params.helipads; x++) {
		if (grid.Get(x, row) != null) continue;
		local above = grid.Get(x, row - 1);
		if (above == null || !IsThroughTaxiable(above.piece)) continue;
		grid.Set(x, row, HelipadPiece(params), 0, 0, true);
		placed++;
	}
	return placed;
}

/**
 * Put optional full-size terminals on an outer frontage row.
 *
 * Some dense families deliberately fill every interior cell, leaving the
 * decorator nowhere to express a larger terminal complex. Their Grid already
 * has one spare outer row; this seeds that row only where the terminal touches
 * the airport, and the fitter remains free to drop it on cramped ground.
 */
function AddExtraTerminalFrontage(grid, params, row)
{
	if (params.extra_terminals <= 0 || row < 0 || row >= grid.h) return;
	local terminals = [];
	foreach (piece in BigTerminalVariants()) {
		if (AIAirport.IsModularPieceAvailable(piece)) terminals.append(piece);
	}
	if (terminals.len() == 0) return;

	local candidates = [];
	for (local x = 0; x < grid.w; x++) {
		if (grid.Get(x, row) != null) continue;
		local touches = (grid.Get(x, row - 1) != null)
		             || (grid.Get(x - 1, row) != null)
		             || (grid.Get(x + 1, row) != null);
		if (touches) candidates.append(x);
	}
	for (local i = 0; i < params.extra_terminals && i < candidates.len() && terminals.len() > 0; i++) {
		local terminal_index = AIBase.RandRange(terminals.len());
		grid.Set(candidates[i], row, terminals[terminal_index], 0, 0, true);
		terminals.remove(terminal_index);
	}
}

/**
 * Decorate the gaps: radar, windsock, a radio tower, a low terminal, grass.
 *
 * None of these does anything. They exist so that two airports of the same
 * family at the same size do not look like the same building twice, and so an
 * airport reads as a place rather than a runway with a shed. Every one is
 * optional, so on cramped ground the fitter drops the decoration before it
 * touches anything that matters.
 *
 * They are also all non-taxiable, which is why they only ever go in cells the
 * layout left empty — dropping one onto a route would strand aircraft, and
 * ValidateGrid would reject the layout anyway.
 */
function DecorateGrid(grid, params)
{
	if (params.cosmetics <= 0 && params.extra_terminals <= 0 && !params.small_terminal) return;

	/* Collect the empty cells that touch the airport, so decoration clusters
	 * against the buildings instead of floating in a field. */
	local spots = [];
	for (local y = 0; y < grid.h; y++) {
		for (local x = 0; x < grid.w; x++) {
			if (grid.Get(x, y) != null) continue;
			local touching = 0, frontage = 0;
			foreach (d in [[0, 1], [0, -1], [1, 0], [-1, 0]]) {
				local neighbour = grid.Get(x + d[0], y + d[1]);
				if (neighbour == null) continue;
				touching++;
				if (IsStandPiece(neighbour.piece) || IsTerminalBuildingPiece(neighbour.piece)) frontage++;
			}
			if (touching > 0) spots.append([x, y, touching, frontage]);
		}
	}
	if (spots.len() == 0) return;

	/* Buildings belong against stands and existing terminals. Within an equally
	 * good frontage, prefer enclosed gaps over lone corner tiles. */
	spots.sort(function (a, b) {
		if (a[3] < b[3]) return 1;
		if (a[3] > b[3]) return -1;
		if (a[2] < b[2]) return 1;
		if (a[2] > b[2]) return -1;
		return 0;
	});

	/* Only roll pieces that exist in the current year. Previously a pre-1955
	 * radar made GridIsAvailable reject the entire otherwise-legacy airfield. */
	local kinds = [];
	foreach (piece in CosmeticVariants()) {
		if (AIAirport.IsModularPieceAvailable(piece)) kinds.append(piece);
	}
	if (kinds.len() == 0) return;
	/* Scale with the airport. Every decorative tile is a real maintenance bill,
	 * so a two-stand airfield gets a windsock, not an avenue of radars. */
	local want = params.cosmetics;
	local cap = 1 + grid.Count() / 8;
	if (want > cap) want = cap;
	if (want > spots.len()) want = spots.len();

	/* The three-tile terminal, when the airport is big enough to be worth a
	 * frontage and the gaps happen to line up. It is the only piece here that
	 * needs a run of tiles rather than one, so it gets first refusal: fitting it
	 * after the single tiles have taken the good spots almost never works.
	 * SetWide refuses unless the whole run is free, so a failure costs nothing. */
	local has_small_terminal = false;
	foreach (c in grid.Ordered()) {
		if (c.piece == AIAirport.MP_SMALL_TERMINAL_3) { has_small_terminal = true; break; }
	}
	if (params.small_terminal && !has_small_terminal && grid.Count() >= 14
	 && AIAirport.IsModularPieceAvailable(AIAirport.MP_SMALL_TERMINAL_3)) {
		foreach (s in spots) {
			if (grid.SetWide(s[0], s[1], AIAirport.MP_SMALL_TERMINAL_3, SMALL_TERMINAL_WIDTH, true)) {
				/* It is a building rather than clutter, so it is not held to the
				 * decoration budget — but it does spend it, and an airport that
				 * gets a terminal gets fewer radars. */
				want -= SMALL_TERMINAL_WIDTH;
				if (want < 0) want = 0;
				break;
			}
		}
	}

	local spot_index = 0;
	/* Large airports frequently get a second frontage building, and the biggest
	 * can get two. They are optional for fitting but intentionally not free: the
	 * visual scale costs the same construction and upkeep as a functional one. */
	local terminals = [];
	foreach (piece in BigTerminalVariants()) {
		if (AIAirport.IsModularPieceAvailable(piece)) terminals.append(piece);
	}
	local existing_big_terminals = 0;
	foreach (c in grid.Ordered()) {
		if (IsBigTerminalPiece(c.piece)) existing_big_terminals++;
	}
	/* One is the functional terminal in a large-safe family; anything beyond it
	 * has already fulfilled part of the optional target. A fallback layout with
	 * large_safe disabled has no such free first terminal. */
	local functional_terminals = params.large_safe && existing_big_terminals > 0 ? 1 : 0;
	local extras_left = params.extra_terminals - (existing_big_terminals - functional_terminals);
	if (extras_left < 0) extras_left = 0;
	for (local i = 0; i < extras_left && terminals.len() > 0; i++) {
		while (spot_index < spots.len() && grid.Get(spots[spot_index][0], spots[spot_index][1]) != null) spot_index++;
		if (spot_index >= spots.len()) break;
		local s = spots[spot_index++];
		local terminal_index = AIBase.RandRange(terminals.len());
		grid.Set(s[0], s[1], terminals[terminal_index], 0, 0, true);
		terminals.remove(terminal_index);
		if (want > 0) want--;
	}

	for (local i = 0; i < want && kinds.len() > 0; i++) {
		while (spot_index < spots.len() && grid.Get(spots[spot_index][0], spots[spot_index][1]) != null) spot_index++;
		if (spot_index >= spots.len()) break;
		local s = spots[spot_index++];
		/* Draw without replacement. Independent rolls allowed a conspicuous radio
		 * mast to repeat several times while subtler windsocks and terminals lost
		 * the coin toss. One airport now gets a genuinely mixed frontage. */
		local kind_index = AIBase.RandRange(kinds.len());
		grid.Set(s[0], s[1], kinds[kind_index], 0, 0, true);
		kinds.remove(kind_index);
	}
}

/** Dispatch to a family generator. */
function GenerateLayout(family, params)
{
	local g;
	switch (family) {
		case Family.STRIP:    g = GenerateStrip(params); break;
		case Family.LINEAR:   g = GenerateLinear(params); break;
		case Family.PIER:     g = GeneratePier(params); break;
		case Family.DUAL:     g = GenerateDual(params); break;
		case Family.APRON:    g = GenerateApron(params); break;
		case Family.HELIPORT: g = GenerateHeliport(params); break;
		default:              g = GenerateLinear(params); break;
	}
	EnsureTowerIfNearlySafe(g);
	DecorateGrid(g, params);
	if (params.mirror) g = g.MirrorX().Normalise();
	return g;
}

/**
 * Add a control tower when it is the only thing between this layout and being
 * safe for fast jets.
 *
 * This happens by accident and often: fitting can trim a layout down to
 * something that still holds a six-tile runway and a big terminal, with nothing
 * else missing. One extra tile then converts an airport that gives jets an
 * elevated overrun crash roll into one that does not — much the cheapest safety
 * the generator can buy.
 */
function EnsureTowerIfNearlySafe(grid)
{
	local safety = AIAirport.GetModularLayoutSafety(grid.ToLayout());
	if (safety != AIAirport.MS_MISSING_TOWER) return;

	/* Somewhere empty that touches the airport, so it does not look dropped in
	 * a field. Non-taxiable, so it can go anywhere that is not a through route. */
	for (local y = 0; y < grid.h; y++) {
		for (local x = 0; x < grid.w; x++) {
			if (grid.Get(x, y) != null) continue;
			local touches = false;
			foreach (d in [[0, 1], [0, -1], [1, 0], [-1, 0]]) {
				if (grid.Get(x + d[0], y + d[1]) != null) { touches = true; break; }
			}
			if (!touches) continue;
			grid.Set(x, y, AIAirport.MP_TOWER);
			return;
		}
	}
}

/**
 * Sample a parameter set for a family.
 *
 * This is where variety comes from. The ranges are deliberately wide on things
 * that only change appearance (terminal variant, which end the hangar sits at,
 * mirroring) and narrow on things that change whether the airport works.
 */
function RandomParams(family, scale)
{
	local p = DefaultParams();
	local terms = BigTerminalVariants();
	local cosms = CosmeticVariants();

	p.terminal = terms[AIBase.RandRange(terms.len())];
	p.cosmetic_kind = cosms[AIBase.RandRange(cosms.len())];
	p.cosmetics = 1 + AIBase.RandRange(5);
	/* Roughly one airport in three, so it reads as a feature of some airports
	 * rather than as the house style. */
	p.small_terminal = AIBase.RandRange(3) == 0;
	p.hangar_at_end = AIBase.RandRange(2) == 0;
	p.mirror = AIBase.RandRange(2) == 0;
	p.helipad_style = AIBase.RandRange(3);
	p.pier_double = AIBase.RandRange(4) != 0;
	/* Extra full-size frontage is a feature of substantial airports. Scale-two
	 * layouts get one most of the time; scale three may get a second. */
	if (family != Family.STRIP && family != Family.HELIPORT && scale >= 2
	 && AIBase.RandRange(4) != 0) {
		p.extra_terminals = 1;
		if (scale >= 3 && AIBase.RandRange(2) == 0) p.extra_terminals++;
	}

	/* scale 0..3 drives how big an airport we are trying to afford. */
	switch (family) {
		case Family.STRIP:
			/* Three equally likely old-airfield characters:
			 *  - a genuinely minimal 4..5-tile strip;
			 *  - a conventional 6..8-tile strip with room along its frontage;
			 *  - a compact 4x3 layout whose stands open directly onto the runway,
			 *    leaving the row behind them for the three-tile terminal. */
			local strip_style = AIBase.RandRange(3);
			if (strip_style == 0) {
				p.runway_length = MIN_LEGACY_RUNWAY_LENGTH + AIBase.RandRange(2);
				p.stands = 2;
				p.small_terminal = false;
			} else if (strip_style == 1) {
				p.runway_length = 6 + AIBase.RandRange(3);
				p.stands = 2 + (scale > 1 ? AIBase.RandRange(2) : 0);
				p.small_terminal = AIBase.RandRange(3) != 0;
			} else {
				p.runway_length = 4;
				p.stands = 2;
				p.small_terminal = true;
				p.strip_compact = true;
			}
			p.large_safe = false;
			break;

		case Family.LINEAR:
			p.runway_length = 6 + scale + AIBase.RandRange(3);
			p.stands = 2 + scale + AIBase.RandRange(2);
			p.helipads = (AIBase.RandRange(4) == 0) ? 1 + AIBase.RandRange(2) : 0;
			break;

		case Family.PIER:
			p.runway_length = 6 + scale + AIBase.RandRange(3);
			p.pier_depth = 2 + AIBase.RandRange(2 + scale);
			p.stands = (p.pier_double ? 2 : 1) * p.pier_depth;
			p.helipads = (scale >= 2 && AIBase.RandRange(3) == 0) ? 1 + AIBase.RandRange(2) : 0;
			break;

		case Family.DUAL:
			p.runway_length = 7 + scale + AIBase.RandRange(3);
			p.stands = 3 + scale + AIBase.RandRange(3);
			break;

		case Family.APRON:
			p.runway_length = 6 + scale + AIBase.RandRange(2);
			p.apron_rows = 1 + AIBase.RandRange(2);
			p.stands = 2 + scale + AIBase.RandRange(3);
			p.helipads = (AIBase.RandRange(3) == 0) ? 1 + AIBase.RandRange(2) : 0;
			break;

		case Family.HELIPORT:
			p.helipads = 1 + AIBase.RandRange(4 + scale);
			p.large_safe = false;
			break;
	}
	return p;
}

/** Whether every piece in a grid can be built in the current year. */
function GridIsAvailable(grid)
{
	foreach (c in grid.Ordered()) {
		if (!AIAirport.IsModularPieceAvailable(c.piece)) return false;
	}
	return true;
}

/** The families worth trying right now, given the year. */
function AvailableFamilies()
{
	local out = [];
	/* MP_RUNWAY_END standing in for the whole modern set; they share a min year. */
	local modern = AIAirport.IsModularPieceAvailable(AIAirport.MP_RUNWAY_END)
	            && AIAirport.IsModularPieceAvailable(AIAirport.MP_TOWER);
	out.append(Family.STRIP);
	if (modern) {
		out.append(Family.LINEAR);
		out.append(Family.PIER);
		out.append(Family.DUAL);
		out.append(Family.APRON);
		if (AIAirport.IsModularPieceAvailable(AIAirport.MP_HELIPAD)) out.append(Family.HELIPORT);
	}
	return out;
}
