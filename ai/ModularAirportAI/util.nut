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

/**
 * Which way a compound piece's tiles run: 0 along X, 1 along Y.
 *
 * A compound has one graphic per tile and the game expands it from its anchor,
 * so its own rotation is not a facing but a choice of axis, and only the two
 * values 0 and 1 mean anything to the build command. Layouts are authored along
 * X; a quarter-turn is what produces the Y form.
 */
function CompoundAxis(cell)
{
	return cell.rot % 2;
}

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

/**
 * Pieces that read as a terminal building — where passengers would come from.
 *
 * Wider than IsBigTerminalPiece, which answers a rules question (does this
 * satisfy MS_MISSING_BIG_TERMINAL). This one answers a looks question, so the
 * low terminal and the three-tile small terminal count too.
 */
function IsTerminalBuildingPiece(piece)
{
	switch (piece) {
		case AIAirport.MP_TERMINAL:
		case AIAirport.MP_TERMINAL_ALT:
		case AIAirport.MP_TERMINAL_OTHER:
		case AIAirport.MP_TERMINAL_ROUND:
		case AIAirport.MP_LOW_TERMINAL:
		case AIAirport.MP_SMALL_TERMINAL_3:
			return true;
	}
	return false;
}

/**
 * Pieces drawn two ways, chosen by the parity of their own rotation.
 *
 * Not a facing: nothing enters these, so the two forms are purely which way the
 * building is turned to the viewer -- the fire station's appliance bay, the car
 * park's entrance ramp. Worth rolling per tile so two of the same building in
 * one airport do not read as a copy-paste.
 */
function PieceHasMirroredForm(piece)
{
	return piece == AIAirport.MP_FIRE_STATION || piece == AIAirport.MP_CAR_PARK;
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
		case AIAirport.MP_SMALL_TERMINAL_3:
		case AIAirport.MP_EMPTY:
		case AIAirport.MP_FIRE_STATION:
		case AIAirport.MP_CARGO_TERMINAL:
		case AIAirport.MP_FUEL_FARM:
		case AIAirport.MP_CAR_PARK:
			return true;
	}
	return false;
}

/** Whether a piece is a real airport feature and not just empty ground. */
function IsNonEmptyAirportPiece(piece)
{
	return piece != AIAirport.MP_EMPTY && piece != AIAirport.MP_GRASS;
}

/**
 * The piece that closes a hole in the bounding rectangle, or null if this
 * climate/date offers neither kind of bare ground.
 */
function InfillGroundPiece()
{
	if (AIAirport.IsModularPieceAvailable(AIAirport.MP_EMPTY)) return AIAirport.MP_EMPTY;
	if (AIAirport.IsModularPieceAvailable(AIAirport.MP_GRASS)) return AIAirport.MP_GRASS;
	return null;
}

/**
 * The piece for a hole well inside the field, where apron joins the taxiway
 * network up rather than leaving a pocket. Falls back to bare ground.
 */
function InfillApronPiece()
{
	if (AIAirport.IsModularPieceAvailable(AIAirport.MP_APRON)) return AIAirport.MP_APRON;
	return InfillGroundPiece();
}

/**
 * Maximum Manhattan distance from any functional cell to its nearest hangar.
 *
 * Compound filler and bounding-box infill are skipped: neither is a place an
 * aircraft ever taxis to, so neither should be able to demand another hangar.
 */
function MaxDistanceToHangar(grid)
{
	local hangars = [];
	foreach (c in grid.Ordered()) {
		if (IsHangarPiece(c.piece)) hangars.append(c);
	}
	if (hangars.len() == 0) return 999;

	local max_d = 0;
	foreach (c in grid.Ordered()) {
		if (c.filler || c.infill) continue;
		local min_h = 999;
		foreach (h in hangars) {
			local d = (c.x > h.x ? c.x - h.x : h.x - c.x) + (c.y > h.y ? c.y - h.y : h.y - c.y);
			if (d < min_h) min_h = d;
		}
		if (min_h > max_d) max_d = min_h;
	}
	return max_d;
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
		case AIAirport.MP_SMALL_TERMINAL_3:      return "t";
		case AIAirport.MP_TOWER:                 return "W";
		case AIAirport.MP_RADIO_TOWER:           return "R";
		case AIAirport.MP_RADAR:                 return "r";
		case AIAirport.MP_RADAR_GRASS:           return "r";
		case AIAirport.MP_FLAG_GRASS:            return "f";
		case AIAirport.MP_GRASS:                 return ",";
		case AIAirport.MP_EMPTY:                 return "_";
		case AIAirport.MP_FIRE_STATION:          return "F";
		case AIAirport.MP_CARGO_TERMINAL:        return "C";
		case AIAirport.MP_FUEL_FARM:             return "U";
		case AIAirport.MP_CAR_PARK:              return "P";
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
			one_way = 0, taxi = 15, fence = 0, optional = optional,
			span = 1, filler = false, infill = false
		};
	}

	/**
	 * Place a cell that only exists to close a hole in the bounding rectangle.
	 *
	 * Marked so scoring can tell it apart from the design proper: infill has no
	 * operational value, so it must not make an otherwise identical functional
	 * design score better or worse.
	 */
	function SetInfill(x, y, piece)
	{
		this.Set(x, y, piece, 0, 0, true);
		local c = this.Get(x, y);
		if (c != null) c.infill = true;
	}

	/**
	 * Place a piece that occupies `span` tiles along X from (x, y).
	 *
	 * Layouts are authored along X only. A compound laid along Y is what a
	 * quarter-turn in Rotate produces, and it is told apart by its own rotation
	 * (see CompoundAxis) rather than by a separate field.
	 *
	 * The extra tiles become filler cells: they hold the ground so nothing else
	 * is placed on top, and ToLayout skips them, because the game expands the
	 * compound from its anchor itself. Nothing is placed at all unless the whole
	 * footprint is free and inside the grid — a compound is all or nothing.
	 * Returns whether it went down.
	 */
	function SetWide(x, y, piece, span, optional = false)
	{
		if (x < 0 || y < 0 || y >= this.h || x + span > this.w) return false;
		for (local i = 0; i < span; i++) {
			if (this.Get(x + i, y) != null) return false;
		}
		this.Set(x, y, piece, 0, 0, optional);
		this.Get(x, y).span = span;
		for (local i = 1; i < span; i++) {
			this.Set(x + i, y, piece, 0, 0, optional);
			local f = this.Get(x + i, y);
			f.span = span;
			f.filler = true;
		}
		return true;
	}

	/** Does this grid hold anything wider than a single tile? */
	function HasWidePiece()
	{
		foreach (_, c in this.cells) {
			if (c.span > 1) return true;
		}
		return false;
	}

	/**
	 * Whether every compound here still runs along X, the axis layouts author.
	 *
	 * Rotate's one legal turn for a compound depends on this: it is the X run
	 * that a quarter-turn carries onto Y with its tiles still in anchor order.
	 */
	function CompoundsOnAuthoringAxis()
	{
		foreach (_, c in this.cells) {
			if (c.span > 1 && CompoundAxis(c) != 0) return false;
		}
		return true;
	}

	/**
	 * Remove a cell, and with it the rest of the compound it belongs to.
	 *
	 * Half a building is worse than none: the graphics only join up as a set, and
	 * the game would have refused to place them piecemeal anyway.
	 */
	function RemoveWhole(x, y)
	{
		local c = this.Get(x, y);
		if (c == null) return;
		if (c.span <= 1) { this.Remove(x, y); return; }

		/* Walk back to the anchor, then clear the whole run. A rotated layout
		 * holds its compounds along Y, so follow the run's own axis rather than
		 * assuming the authoring one. */
		local dx = (CompoundAxis(c) == 0) ? 1 : 0;
		local dy = 1 - dx;
		local ax = x, ay = y;
		while (ax - dx >= 0 && ay - dy >= 0) {
			local prev = this.Get(ax - dx, ay - dy);
			if (prev == null || prev.span != c.span || !this.Get(ax, ay).filler) break;
			ax -= dx;
			ay -= dy;
		}
		for (local i = 0; i < c.span; i++) this.Remove(ax + dx * i, ay + dy * i);
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
			/* The game expands a compound from its anchor, so only the anchor is
			 * described here; its filler cells exist to reserve the ground. */
			if (c.filler) continue;
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
				one_way = c.one_way, taxi = c.taxi, fence = c.fence, optional = c.optional,
				span = c.span, filler = c.filler, infill = c.infill
			};
		}
		return g;
	}

	/** Mirror along X, including the direction encoded by runway flags and end pieces. */
	function MirrorX()
	{
		local g = Grid(this.w, this.h);
		foreach (_, c in this.cells) {
			/* A compound is not mirrored, only moved: its tiles have one graphic
			 * each and only join up in anchor order. Its run [x, x+span-1] maps to
			 * [w-span-x, w-1-x], so the anchor lands at w-span-x and the filler
			 * cells are rebuilt from there rather than mirrored individually.
			 * Mirroring happens while the layout is still in the orientation it
			 * was authored in, so every compound here runs along X; the Y form
			 * only ever comes out of Rotate, which runs after this. */
			if (c.filler) continue;
			local nx = this.w - c.span - c.x;
			local rot = c.rot;
			local piece = c.piece;
			local rwy = c.rwy;
			if (IsHangarPiece(c.piece)) {
				if (rot == FACE_NE) rot = FACE_SW;
				else if (rot == FACE_SW) rot = FACE_NE;
			}
			/* Mirroring reverses coordinate order along an X-axis runway. Preserve
			 * its operational direction and keep the perspective-specific small
			 * runway end graphics at their canonical low/high ends. */
			if (IsRunwayPiece(piece) && (rot % 2) == 0) {
				local low = rwy & AIAirport.MRF_DIR_LOW;
				local high = rwy & AIAirport.MRF_DIR_HIGH;
				rwy = rwy & ~(AIAirport.MRF_DIR_LOW | AIAirport.MRF_DIR_HIGH);
				if (low != 0) rwy = rwy | AIAirport.MRF_DIR_HIGH;
				if (high != 0) rwy = rwy | AIAirport.MRF_DIR_LOW;
				if (piece == AIAirport.MP_RUNWAY_SMALL_NEAR_END) piece = AIAirport.MP_RUNWAY_SMALL_FAR_END;
				else if (piece == AIAirport.MP_RUNWAY_SMALL_FAR_END) piece = AIAirport.MP_RUNWAY_SMALL_NEAR_END;
			}
			g.cells[g.Key(nx, c.y)] <- {
				x = nx, y = c.y, piece = piece, rot = rot, rwy = rwy,
				one_way = c.one_way, taxi = c.taxi, fence = c.fence, optional = c.optional,
				span = c.span, filler = false, infill = c.infill
			};
			for (local i = 1; i < c.span; i++) {
				g.cells[g.Key(nx + i, c.y)] <- {
					x = nx + i, y = c.y, piece = piece, rot = rot, rwy = rwy,
					one_way = c.one_way, taxi = c.taxi, fence = c.fence, optional = c.optional,
					span = c.span, filler = true, infill = c.infill
				};
			}
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

		/* A compound piece has one graphic per tile and the game expands it from
		 * its anchor towards +X or +Y, so a turn is only expressible when the
		 * tiles keep their order. For the X-axis run that layouts author, that is
		 * the single quarter-turn onto Y: a half-turn or a three-quarter turn
		 * would need the same three graphics in the opposite order, which there
		 * is no form for. AllowedRotations offers only 0 and 1 for such a layout,
		 * so this is a guard rather than a branch that runs. */
		if (this.HasWidePiece() && (r != 1 || !this.CompoundsOnAuthoringAxis())) return this.Clone();

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

			/* A hangar's rotation is a facing and wraps at four; a compound's is an
			 * axis and only 0 and 1 mean anything to the build command. */
			local rot = (c.span > 1) ? ((c.rot + r) & 1) : ((c.rot + r) & 3);

			out.cells[out.Key(nx, ny)] <- {
				x = nx, y = ny, piece = piece, rot = rot, rwy = rwy,
				one_way = c.one_way, taxi = RotateDirMask(c.taxi, r),
				fence = RotateDirMask(c.fence, r), optional = c.optional,
				span = c.span, filler = c.filler, infill = c.infill
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
 * How many stands in this grid touch a terminal building.
 *
 * Worth a nudge in the layout score, not a rule: an aircraft parked against the
 * terminal is where the passengers actually are, and a stand next to the round
 * terminal grows a jetway onto it. It has to stay a nudge, because insisting on
 * it would collapse the pier family, whose stands hang off a spine with the
 * buildings well clear of it, and that is the family that fits tight sites.
 */
function StandsTouchingTerminals(grid)
{
	local n = 0;
	foreach (c in grid.Ordered()) {
		if (!IsStandPiece(c.piece)) continue;
		foreach (d in [[0, 1], [0, -1], [1, 0], [-1, 0]]) {
			local nb = grid.Get(c.x + d[0], c.y + d[1]);
			if (nb != null && IsTerminalBuildingPiece(nb.piece)) { n++; break; }
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

	/* A compound piece is described to the game by its anchor alone, which the
	 * game then expands along the axis the anchor's own rotation names. So the
	 * anchor must be at the low end of a contiguous run of its own filler cells
	 * along that axis, or the tiles the game places and the tiles this grid
	 * reserved are not the same tiles. Mirroring, rotation and trimming all move
	 * these around, which is why it is checked rather than assumed. */
	local wide_cells = 0, spanned = 0;
	foreach (c in grid.Ordered()) {
		if (c.span <= 1) continue;
		wide_cells++;
		if (c.filler) continue;
		spanned += c.span;
		local dx = (CompoundAxis(c) == 0) ? 1 : 0;
		local dy = 1 - dx;
		for (local i = 1; i < c.span; i++) {
			local f = grid.Get(c.x + dx * i, c.y + dy * i);
			if (f == null || !f.filler || f.piece != c.piece || f.span != c.span) {
				return "compound piece broken at " + c.x + "," + c.y;
			}
		}
	}
	if (wide_cells != spanned) return "compound piece tile without its anchor";

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
		local has_apron = false;
		foreach (d in [[0, 1], [0, -1], [1, 0], [-1, 0]]) {
			local n = grid.Get(c.x + d[0], c.y + d[1]);
			if (n != null && n.piece == AIAirport.MP_APRON) { has_apron = true; break; }
		}
		if (!has_apron) return "stand at " + c.x + "," + c.y + " has no adjacent apron";
	}

	/* Hangars and helipads are endpoints, so they need an adjacent tile on the
	 * network — and in front of a hangar must be an apron. */
	foreach (c in hangars) {
		local off = FaceOffset(c.rot);
		local n = grid.Get(c.x + off[0], c.y + off[1]);
		if (n == null || n.piece != AIAirport.MP_APRON) {
			return "hangar at " + c.x + "," + c.y + " does not face an apron";
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
		if (c.piece == AIAirport.MP_RUNWAY_SMALL_FAR_END
		 || c.piece == AIAirport.MP_RUNWAY_SMALL_NEAR_END) {
			local dx = (c.rot % 2) == 0 ? 1 : 0;
			local dy = (c.rot % 2) == 0 ? 0 : 1;
			local before = grid.Get(c.x - dx, c.y - dy);
			local after = grid.Get(c.x + dx, c.y + dy);
			local has_before = before != null && IsSmallRunwayPiece(before.piece);
			local has_after = after != null && IsSmallRunwayPiece(after.piece);
			if (c.piece == AIAirport.MP_RUNWAY_SMALL_FAR_END && (has_before || !has_after)) {
				return "small runway far end is not at the low end";
			}
			if (c.piece == AIAirport.MP_RUNWAY_SMALL_NEAR_END && (!has_before || has_after)) {
				return "small runway near end is not at the high end";
			}
		}
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
