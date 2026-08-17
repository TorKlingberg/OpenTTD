/*
 * Shared vocabulary for modular airport layouts.
 *
 * Everything here is pure: no API calls that touch the world, so the layout
 * generator can be exercised without a game running (see selftest.nut).
 */

/* Layout array stride, mirroring AIAirport.MLF_STRIDE. */
const LAYOUT_STRIDE = 8;

/* Directions a hangar can face, in the modular convention 0=SE, 1=NE, 2=NW, 3=SW.
 * OpenTTD axes: +X is SW, -X is NE, +Y is SE, -Y is NW. */
const FACE_SE = 0;
const FACE_NE = 1;
const FACE_NW = 2;
const FACE_SW = 3;

/** Tile offset a hangar with the given facing exits onto. */
function FaceOffset(face)
{
	switch (face) {
		case FACE_SE: return [0, 1];
		case FACE_NE: return [-1, 0];
		case FACE_NW: return [0, -1];
		case FACE_SW: return [1, 0];
	}
	return [0, 0];
}

/** Turn a four-bit direction mask by r quarter-turns. */
function RotateDirMask(mask, r)
{
	local out = 0;
	for (local i = 0; i < 4; i++) {
		if (mask & (1 << i)) out = out | (1 << ((i + r) & 3));
	}
	return out;
}

/** The facing a hangar needs to exit onto (tx, ty) from (x, y), or -1 if not adjacent. */
function FaceTowards(x, y, tx, ty)
{
	if (tx == x && ty == y + 1) return FACE_SE;
	if (tx == x - 1 && ty == y) return FACE_NE;
	if (tx == x && ty == y - 1) return FACE_NW;
	if (tx == x + 1 && ty == y) return FACE_SW;
	return -1;
}

/**
 * Pieces aircraft may taxi *across* on their way somewhere else. Stands count:
 * the ground pathfinder charges a penalty for crossing a non-goal stand but
 * allows it. Hangars and helipads do not: they are destinations only, which is
 * why they must sit next to something in this set.
 */
function IsThroughTaxiable(piece)
{
	switch (piece) {
		case AIAirport.MP_APRON:
		case AIAirport.MP_STAND:
		case AIAirport.MP_RUNWAY:
		case AIAirport.MP_RUNWAY_END:
		case AIAirport.MP_RUNWAY_SMALL_MIDDLE:
		case AIAirport.MP_RUNWAY_SMALL_NEAR_END:
		case AIAirport.MP_RUNWAY_SMALL_FAR_END:
			return true;
	}
	return false;
}

/** Pieces aircraft stop at to load and unload. */
function IsStandPiece(piece)
{
	return piece == AIAirport.MP_STAND;
}

function IsHangarPiece(piece)
{
	return piece == AIAirport.MP_HANGAR || piece == AIAirport.MP_SMALL_HANGAR;
}

function IsHelipadPiece(piece)
{
	return piece == AIAirport.MP_HELIPAD
	    || piece == AIAirport.MP_HELIPAD_PLAIN
	    || piece == AIAirport.MP_HELIPORT;
}

/** Pieces that make up a large runway; the only kind fast jets can safely use. */
function IsLargeRunwayPiece(piece)
{
	return piece == AIAirport.MP_RUNWAY || piece == AIAirport.MP_RUNWAY_END;
}

function IsSmallRunwayPiece(piece)
{
	return piece == AIAirport.MP_RUNWAY_SMALL_MIDDLE
	    || piece == AIAirport.MP_RUNWAY_SMALL_NEAR_END
	    || piece == AIAirport.MP_RUNWAY_SMALL_FAR_END;
}

function IsRunwayPiece(piece)
{
	return IsLargeRunwayPiece(piece) || IsSmallRunwayPiece(piece);
}

/** Pieces that satisfy MS_MISSING_BIG_TERMINAL. */
function IsBigTerminalPiece(piece)
{
	switch (piece) {
		case AIAirport.MP_TERMINAL:
		case AIAirport.MP_TERMINAL_ALT:
		case AIAirport.MP_TERMINAL_OTHER:
		case AIAirport.MP_TERMINAL_ROUND:
			return true;
	}
	return false;
}

/** Decorative pieces: no function, but they are what stops airports looking identical. */
function IsCosmeticPiece(piece)
{
	switch (piece) {
		case AIAirport.MP_RADIO_TOWER:
		case AIAirport.MP_RADAR:
		case AIAirport.MP_RADAR_GRASS:
		case AIAirport.MP_FLAG_GRASS:
		case AIAirport.MP_GRASS:
		case AIAirport.MP_LOW_TERMINAL:
		case AIAirport.MP_EMPTY:
			return true;
	}
	return false;
}

/** One character per piece, for the ASCII dumps in selftest.nut. */
function PieceChar(piece)
{
	switch (piece) {
		case AIAirport.MP_APRON:                 return "+";
		case AIAirport.MP_STAND:                 return "S";
		case AIAirport.MP_RUNWAY:                return "=";
		case AIAirport.MP_RUNWAY_END:            return "E";
		case AIAirport.MP_RUNWAY_SMALL_MIDDLE:   return "-";
		case AIAirport.MP_RUNWAY_SMALL_NEAR_END: return ">";
		case AIAirport.MP_RUNWAY_SMALL_FAR_END:  return "<";
		case AIAirport.MP_HANGAR:                return "H";
		case AIAirport.MP_SMALL_HANGAR:          return "h";
		case AIAirport.MP_HELIPAD:               return "X";
		case AIAirport.MP_HELIPAD_PLAIN:         return "x";
		case AIAirport.MP_HELIPORT:              return "O";
		case AIAirport.MP_TERMINAL:              return "B";
		case AIAirport.MP_TERMINAL_ALT:          return "B";
		case AIAirport.MP_TERMINAL_OTHER:        return "B";
		case AIAirport.MP_TERMINAL_ROUND:        return "B";
		case AIAirport.MP_LOW_TERMINAL:          return "b";
		case AIAirport.MP_TOWER:                 return "W";
		case AIAirport.MP_RADIO_TOWER:           return "R";
		case AIAirport.MP_RADAR:                 return "r";
		case AIAirport.MP_RADAR_GRASS:           return "r";
		case AIAirport.MP_FLAG_GRASS:            return "f";
		case AIAirport.MP_GRASS:                 return ",";
		case AIAirport.MP_EMPTY:                 return "_";
	}
	return "?";
}

/**
 * A rectangular grid of layout cells under construction.
 *
 * Cells are addressed in the family's own local coordinates. Rotation is not
 * applied here: PlaceModularAirportLayout rotates the whole layout on the way
 * in, so families only ever have to describe one orientation.
 *
 * Cells marked optional may be dropped by the fitter when the site is too
 * cramped for them; required cells make the whole layout fail instead. That
 * split is what lets one family serve both a wide-open field and a gap between
 * two hills.
 */
class Grid
{
	w = 0;
	h = 0;
	cells = null;

	constructor(width, height)
	{
		this.w = width;
		this.h = height;
		this.cells = {};
	}

	function Key(x, y) { return y * 1000 + x; }

	function Set(x, y, piece, rot = 0, rwy = 0, optional = false)
	{
		if (x < 0 || y < 0 || x >= this.w || y >= this.h) return;
		this.cells[this.Key(x, y)] <- {
			x = x, y = y, piece = piece, rot = rot, rwy = rwy,
			one_way = 0, taxi = 15, fence = 0, optional = optional
		};
	}

	function SetTaxi(x, y, dir_mask, one_way)
	{
		local c = this.Get(x, y);
		if (c == null) return;
		c.taxi = dir_mask;
		c.one_way = one_way ? 1 : 0;
	}

	function Get(x, y)
	{
		local k = this.Key(x, y);
		return (k in this.cells) ? this.cells[k] : null;
	}

	function Remove(x, y)
	{
		local k = this.Key(x, y);
		if (k in this.cells) delete this.cells[k];
	}

	function Count() { return this.cells.len(); }

	/** Every cell, in a stable order so builds are reproducible. */
	function Ordered()
	{
		local keys = [];
		foreach (k, _ in this.cells) keys.append(k);
		keys.sort();
		local out = [];
		foreach (k in keys) out.append(this.cells[k]);
		return out;
	}

	/** Shrink-wrap to the occupied cells, so the placement footprint is tight. */
	function Normalise()
	{
		if (this.cells.len() == 0) return this;
		local minx = 9999, miny = 9999, maxx = -1, maxy = -1;
		foreach (_, c in this.cells) {
			if (c.x < minx) minx = c.x;
			if (c.y < miny) miny = c.y;
			if (c.x > maxx) maxx = c.x;
			if (c.y > maxy) maxy = c.y;
		}
		if (minx == 0 && miny == 0) {
			this.w = maxx + 1;
			this.h = maxy + 1;
			return this;
		}
		local moved = {};
		foreach (_, c in this.cells) {
			c.x -= minx;
			c.y -= miny;
			moved[this.Key(c.x, c.y)] <- c;
		}
		this.cells = moved;
		this.w = maxx - minx + 1;
		this.h = maxy - miny + 1;
		return this;
	}

	/** The flat integer array PlaceModularAirportLayout expects. */
	function ToLayout()
	{
		local out = [];
		foreach (c in this.Ordered()) {
			out.append(c.x);
			out.append(c.y);
			out.append(c.piece);
			out.append(c.rot);
			out.append(c.rwy);
			out.append(c.one_way);
			out.append(c.taxi);
			out.append(c.fence);
		}
		return out;
	}

	function Clone()
	{
		local g = Grid(this.w, this.h);
		foreach (_, c in this.cells) {
			g.cells[g.Key(c.x, c.y)] <- {
				x = c.x, y = c.y, piece = c.piece, rot = c.rot, rwy = c.rwy,
				one_way = c.one_way, taxi = c.taxi, fence = c.fence, optional = c.optional
			};
		}
		return g;
	}

	/** Mirror along X. Runway direction flags are unaffected; hangar facings are not. */
	function MirrorX()
	{
		local g = Grid(this.w, this.h);
		foreach (_, c in this.cells) {
			local nx = this.w - 1 - c.x;
			local rot = c.rot;
			if (IsHangarPiece(c.piece)) {
				if (rot == FACE_NE) rot = FACE_SW;
				else if (rot == FACE_SW) rot = FACE_NE;
			}
			g.cells[g.Key(nx, c.y)] <- {
				x = nx, y = c.y, piece = c.piece, rot = rot, rwy = c.rwy,
				one_way = c.one_way, taxi = c.taxi, fence = c.fence, optional = c.optional
			};
		}
		return g;
	}

	/**
	 * Rotate the whole layout by r quarter-turns, in the script's own hands.
	 *
	 * PlaceModularAirportLayout can rotate a layout itself, but airports built
	 * that way do not work: aircraft never leave the hangar. Measured, not
	 * assumed — the same layout authored vertically by hand and placed at
	 * rotation 0 flies, while rotations 1 and 3 of the horizontal original leave
	 * every aircraft parked forever. So the AI rotates here and always places at
	 * rotation 0.
	 *
	 * The transform mirrors RotateTemplateTile in modular_airport_template_cmd.cpp:
	 * positions turn, each piece's own rotation advances by r, direction masks
	 * rotate, and a runway's low/high flag flips when the coordinate order along
	 * its axis reverses.
	 */
	function Rotate(r)
	{
		r = r & 3;
		if (r == 0) return this.Clone();

		local out = Grid((r % 2 == 0) ? this.w : this.h, (r % 2 == 0) ? this.h : this.w);
		foreach (_, c in this.cells) {
			local nx, ny;
			switch (r) {
				case 1: nx = this.h - 1 - c.y; ny = c.x; break;
				case 2: nx = this.w - 1 - c.x; ny = this.h - 1 - c.y; break;
				case 3: nx = c.y;              ny = this.w - 1 - c.x; break;
			}

			local piece = c.piece;
			local rwy = c.rwy;
			if (IsRunwayPiece(piece)) {
				/* Low and high ends swap whenever the order of coordinates along
				 * the runway's own axis reverses. */
				local on_x_axis = (c.rot % 2) == 0;
				local reverse = on_x_axis ? (r == 2 || r == 3) : (r == 1 || r == 2);
				if (reverse) {
					local low = rwy & AIAirport.MRF_DIR_LOW;
					local high = rwy & AIAirport.MRF_DIR_HIGH;
					rwy = rwy & ~(AIAirport.MRF_DIR_LOW | AIAirport.MRF_DIR_HIGH);
					if (low != 0) rwy = rwy | AIAirport.MRF_DIR_HIGH;
					if (high != 0) rwy = rwy | AIAirport.MRF_DIR_LOW;
					/* The end pieces of a small runway are named for which end
					 * they sit at, so they swap with the ends. */
					if (piece == AIAirport.MP_RUNWAY_SMALL_NEAR_END) piece = AIAirport.MP_RUNWAY_SMALL_FAR_END;
					else if (piece == AIAirport.MP_RUNWAY_SMALL_FAR_END) piece = AIAirport.MP_RUNWAY_SMALL_NEAR_END;
				}
			}

			out.cells[out.Key(nx, ny)] <- {
				x = nx, y = ny, piece = piece, rot = (c.rot + r) & 3, rwy = rwy,
				one_way = c.one_way, taxi = RotateDirMask(c.taxi, r),
				fence = RotateDirMask(c.fence, r), optional = c.optional
			};
		}
		return out;
	}

	function AsciiRows()
	{
		local rows = [];
		for (local y = 0; y < this.h; y++) {
			local line = "";
			for (local x = 0; x < this.w; x++) {
				local c = this.Get(x, y);
				line += (c == null) ? " " : PieceChar(c.piece);
			}
			rows.append(line);
		}
		return rows;
	}
}

/**
 * Check a grid is an airport rather than a pile of tiles.
 *
 * The script API deliberately does not answer this — GetModularLayoutSafety
 * checks composition (is there a tower, a big terminal, a long enough runway)
 * and never checks that the pieces connect. A layout can report MS_OK with its
 * hangar walled in behind the tower, build without error, and leave every
 * aircraft parked forever. So the generator has to prove connectivity itself.
 *
 * How many stands in this grid touch a hangar.
 *
 * Worth a nudge in the layout score, not a rule: aircraft go to the hangar to be
 * built and to be serviced, and a stand next to it makes both trips short. It
 * has to stay a nudge, because insisting on it would collapse the pier and apron
 * families, where the hangar closes one end of a run of stands and only the
 * nearest one or two can ever touch it.
 */
function StandsTouchingHangars(grid)
{
	local n = 0;
	foreach (c in grid.Ordered()) {
		if (!IsStandPiece(c.piece)) continue;
		foreach (d in [[0, 1], [0, -1], [1, 0], [-1, 0]]) {
			local nb = grid.Get(c.x + d[0], c.y + d[1]);
			if (nb != null && IsHangarPiece(nb.piece)) { n++; break; }
		}
	}
	return n;
}

/**
 * Returns null when the grid is sound, or a string naming the first problem.
 */
function ValidateGrid(grid)
{
	local stands = [], hangars = [], helipads = [], runway_ends = [], through = [];
	foreach (c in grid.Ordered()) {
		if (IsStandPiece(c.piece)) stands.append(c);
		if (IsHangarPiece(c.piece)) hangars.append(c);
		if (IsHelipadPiece(c.piece)) helipads.append(c);
		if (IsThroughTaxiable(c.piece)) through.append(c);
		if (c.piece == AIAirport.MP_RUNWAY_END
		 || c.piece == AIAirport.MP_RUNWAY_SMALL_NEAR_END
		 || c.piece == AIAirport.MP_RUNWAY_SMALL_FAR_END) runway_ends.append(c);
	}

	if (stands.len() == 0 && helipads.len() == 0) return "no stand or helipad";
	if (hangars.len() == 0) return "no hangar";
	if (stands.len() > 0 && runway_ends.len() == 0) return "stands but no runway";

	/* Flood the through-taxiable network from one runway end (or, for a pure
	 * heliport, from whatever a helipad touches). */
	local seed = null;
	if (runway_ends.len() > 0) {
		seed = runway_ends[0];
	} else {
		foreach (p in helipads) {
			foreach (d in [[0, 1], [0, -1], [1, 0], [-1, 0]]) {
				local n = grid.Get(p.x + d[0], p.y + d[1]);
				if (n != null && IsThroughTaxiable(n.piece)) { seed = n; break; }
			}
			if (seed != null) break;
		}
		if (seed == null) return "heliport has no apron";
	}

	local seen = {};
	local queue = [seed];
	seen[grid.Key(seed.x, seed.y)] <- true;
	while (queue.len() > 0) {
		local c = queue.pop();
		foreach (d in [[0, 1], [0, -1], [1, 0], [-1, 0]]) {
			local n = grid.Get(c.x + d[0], c.y + d[1]);
			if (n == null || !IsThroughTaxiable(n.piece)) continue;
			local k = grid.Key(n.x, n.y);
			if (k in seen) continue;
			seen[k] <- true;
			queue.append(n);
		}
	}

	/* Every runway end must be on the same network: a second runway the aircraft
	 * cannot reach is worse than no second runway. */
	foreach (c in runway_ends) {
		if (!(grid.Key(c.x, c.y) in seen)) return "runway end at " + c.x + "," + c.y + " unreachable";
	}
	foreach (c in stands) {
		if (!(grid.Key(c.x, c.y) in seen)) return "stand at " + c.x + "," + c.y + " unreachable";
	}

	/* Hangars and helipads are endpoints, so they need an adjacent tile on the
	 * network — and a hangar must actually face it. */
	foreach (c in hangars) {
		local off = FaceOffset(c.rot);
		local n = grid.Get(c.x + off[0], c.y + off[1]);
		if (n == null || !IsThroughTaxiable(n.piece)) {
			return "hangar at " + c.x + "," + c.y + " faces nothing";
		}
		if (!(grid.Key(n.x, n.y) in seen)) return "hangar at " + c.x + "," + c.y + " unreachable";
	}
	foreach (c in helipads) {
		local ok = false;
		foreach (d in [[0, 1], [0, -1], [1, 0], [-1, 0]]) {
			local n = grid.Get(c.x + d[0], c.y + d[1]);
			if (n != null && IsThroughTaxiable(n.piece) && (grid.Key(n.x, n.y) in seen)) { ok = true; break; }
		}
		if (!ok) return "helipad at " + c.x + "," + c.y + " unreachable";
	}

	/* Runway flags must be a combination the game will accept. */
	foreach (c in grid.Ordered()) {
		if (!IsRunwayPiece(c.piece)) continue;
		if ((c.rwy & (AIAirport.MRF_LANDING | AIAirport.MRF_TAKEOFF)) == 0) {
			return "runway at " + c.x + "," + c.y + " does neither landing nor takeoff";
		}
		local dir = c.rwy & (AIAirport.MRF_DIR_LOW | AIAirport.MRF_DIR_HIGH);
		if (dir != AIAirport.MRF_DIR_LOW && dir != AIAirport.MRF_DIR_HIGH) {
			return "runway at " + c.x + "," + c.y + " has no single direction";
		}
	}

	/* An airport with stands must be able to both take off and land. */
	if (stands.len() > 0) {
		local can_land = false, can_take_off = false;
		foreach (c in grid.Ordered()) {
			if (!IsRunwayPiece(c.piece)) continue;
			if (c.rwy & AIAirport.MRF_LANDING) can_land = true;
			if (c.rwy & AIAirport.MRF_TAKEOFF) can_take_off = true;
		}
		if (!can_land) return "no runway allows landing";
		if (!can_take_off) return "no runway allows takeoff";
	}

	return null;
}

/** Longest contiguous run of large runway pieces, in tiles. */
function LongestLargeRunway(grid)
{
	local best = 0;
	for (local y = 0; y < grid.h; y++) {
		local run = 0;
		for (local x = 0; x < grid.w; x++) {
			local c = grid.Get(x, y);
			if (c != null && IsLargeRunwayPiece(c.piece)) { run++; if (run > best) best = run; }
			else run = 0;
		}
	}
	for (local x = 0; x < grid.w; x++) {
		local run = 0;
		for (local y = 0; y < grid.h; y++) {
			local c = grid.Get(x, y);
			if (c != null && IsLargeRunwayPiece(c.piece)) { run++; if (run > best) best = run; }
			else run = 0;
		}
	}
	return best;
}

function CountPieces(grid, pred)
{
	local n = 0;
	foreach (c in grid.Ordered()) if (pred(c.piece)) n++;
	return n;
}
