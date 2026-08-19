/*
 * Fitting a generated layout onto ground that is not a clean rectangle.
 *
 * This is where the modular advantage is actually cashed in. A stock airport
 * needs its whole W×H rectangle flat, clear and at one height; a modular one
 * needs only the tiles it occupies. So rather than rejecting a site because the
 * family's bounding box does not fit, drop the parts of the layout that hang
 * over the edge and check what is left is still an airport.
 *
 * Cells carry an `optional` flag for exactly this. Required cells — the runway,
 * the spine that reaches the hangar, the first couple of stands — make the fit
 * fail. Optional ones — extra stands, decoration, spare apron — get trimmed.
 */

/**
 * Try to fit a grid into a region, given as a table whose keys are
 * Grid.Key(x, y) for every usable cell.
 *
 * A table rather than a predicate closure on purpose: Squirrel resolves a bare
 * identifier inside a nested function against `this` before the enclosing
 * scope, so a mask closure over the caller's locals fails at run time rather
 * than compile time. Building the set up front also means the caller only has
 * to test the tiles the layout actually wants.
 *
 * Coordinates are not renormalised: the returned grid keeps the input's
 * coordinate system, so the caller's origin tile stays valid.
 *
 * Returns the trimmed grid, or null when no valid airport survives.
 */
function FitGridToMask(grid, allowed)
{
	local g = grid.Clone();

	/* Anything required that falls outside the region is fatal. Optional cells go,
	 * and a multi-tile piece goes whole: two thirds of a building is not a
	 * building, and the game would refuse to place it that way in any case. */
	foreach (c in g.Ordered()) {
		if (g.Key(c.x, c.y) in allowed) continue;
		if (!c.optional) return null;
		g.RemoveWhole(c.x, c.y);
	}

	/* Trimming can strand a hangar that used to face an apron. Re-aim it at
	 * an adjacent apron before giving up on the layout. */
	RepairHangars(g);

	/* Trimming can also cut a stand off from the taxiway network. Those are
	 * cheaper to drop than to route around. */
	PruneUnreachable(g);
	RepairHangars(g);

	/* Re-fill holes inside the trimmed bounding box if allowed ground permits. */
	local minx = 9999, miny = 9999, maxx = -1, maxy = -1;
	foreach (c in g.Ordered()) {
		if (c.x < minx) minx = c.x;
		if (c.y < miny) miny = c.y;
		if (c.x > maxx) maxx = c.x;
		if (c.y > maxy) maxy = c.y;
	}
	if (maxx >= minx && maxy >= miny) {
		/* Decide every hole against the trimmed design, then place. Deciding and
		 * placing in one pass would let a hole that just became apron count as a
		 * neighbour for the next hole, so a large gap would grow a chain of apron
		 * across itself in whatever order the scan happened to run. */
		local plan = [];
		for (local y = miny; y <= maxy; y++) {
			for (local x = minx; x <= maxx; x++) {
				if (g.Get(x, y) != null) continue;
				if (!(g.Key(x, y) in allowed)) continue;
				local non_empty = 0;
				foreach (d in [[0, 1], [0, -1], [1, 0], [-1, 0]]) {
					local n = g.Get(x + d[0], y + d[1]);
					if (n != null && IsNonEmptyAirportPiece(n.piece)) non_empty++;
				}
				plan.append({ x = x, y = y, apron = non_empty >= 3 });
			}
		}
		local plain = InfillGroundPiece();
		local dense = InfillApronPiece();
		foreach (h in plan) {
			local want = h.apron ? dense : plain;
			if (want != null) g.SetInfill(h.x, h.y, want);
		}
	}

	/* Infill can put an apron in front of a hangar that had nothing to face when
	 * RepairHangars last ran, so give it that chance before validating. */
	RepairHangars(g);

	if (ValidateGrid(g) != null) return null;
	return g;
}

/** Point each hangar at an adjacent apron tile, if one exists. */
function RepairHangars(grid)
{
	foreach (c in grid.Ordered()) {
		if (!IsHangarPiece(c.piece)) continue;
		local off = FaceOffset(c.rot);
		local n = grid.Get(c.x + off[0], c.y + off[1]);
		if (n != null && n.piece == AIAirport.MP_APRON) continue;
		local rot_options = (c.piece == AIAirport.MP_SMALL_HANGAR)
			? [FACE_SE] : [FACE_SE, FACE_NE, FACE_NW, FACE_SW];
		foreach (r in rot_options) {
			local d = FaceOffset(r);
			local m = grid.Get(c.x + d[0], c.y + d[1]);
			if (m == null || m.piece != AIAirport.MP_APRON) continue;
			c.rot = r;
			break;
		}
	}
}

/**
 * Drop stands, helipads and spare apron that the taxiway network no longer
 * reaches. A stranded stand is the classic silly airport: it builds, it scores,
 * and no aircraft ever uses it.
 */
function PruneUnreachable(grid)
{
	/* Seed from a runway end, or from the apron a helipad touches. */
	local seed = null;
	foreach (c in grid.Ordered()) {
		if (c.piece == AIAirport.MP_RUNWAY_END
		 || c.piece == AIAirport.MP_RUNWAY_SMALL_NEAR_END
		 || c.piece == AIAirport.MP_RUNWAY_SMALL_FAR_END) { seed = c; break; }
	}
	if (seed == null) {
		foreach (c in grid.Ordered()) {
			if (IsThroughTaxiable(c.piece)) { seed = c; break; }
		}
	}
	if (seed == null) return;

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

	local doomed = [];
	foreach (c in grid.Ordered()) {
		if (IsStandPiece(c.piece)) {
			if (!(grid.Key(c.x, c.y) in seen)) { doomed.append(c); continue; }
			local has_apron = false;
			foreach (d in [[0, 1], [0, -1], [1, 0], [-1, 0]]) {
				local n = grid.Get(c.x + d[0], c.y + d[1]);
				if (n != null && n.piece == AIAirport.MP_APRON && (grid.Key(n.x, n.y) in seen)) {
					has_apron = true;
					break;
				}
			}
			if (!has_apron) { doomed.append(c); continue; }
		}
		if (IsThroughTaxiable(c.piece)) {
			if (!(grid.Key(c.x, c.y) in seen)) doomed.append(c);
			continue;
		}
		if (IsHelipadPiece(c.piece)) {
			local ok = false;
			foreach (d in [[0, 1], [0, -1], [1, 0], [-1, 0]]) {
				local n = grid.Get(c.x + d[0], c.y + d[1]);
				if (n != null && (grid.Key(n.x, n.y) in seen)) { ok = true; break; }
			}
			if (!ok) doomed.append(c);
		}
	}
	foreach (c in doomed) grid.Remove(c.x, c.y);
}

/**
 * How good a layout is, as a single number.
 *
 * Deliberately not just throughput: catchment and stands drive revenue, upkeep
 * and tile count drive cost, and being large-safe is close to a hard
 * requirement once fast jets exist because the alternative is losing aircraft
 * to overrun crashes.
 */
function ScoreGrid(grid, want_large_safe)
{
	local layout = grid.ToLayout();
	local safety = AIAirport.GetModularLayoutSafety(layout);
	local stands = CountPieces(grid, IsStandPiece);
	local helipads = CountPieces(grid, IsHelipadPiece);
	local catchment = AIAirport.GetModularLayoutCatchmentRadius(layout);
	local upkeep = AIAirport.GetModularLayoutMonthlyMaintenanceCost(layout);

	local score = 0;
	score += stands * 60;
	score += helipads * 25;
	/* Mild on purpose, and bounded: a terminal is one or two tiles long, so only
	 * one or two stands can touch it and counting further would just be a proxy
	 * for "small airport". At most a third of one stand's worth, which breaks
	 * ties between trims of the same layout without ever choosing a smaller one.
	 * An earlier unbounded version at 15 a stand cost about 6% of total
	 * throughput by quietly shifting which layouts won. */
	local touching = StandsTouchingTerminals(grid);
	score += (touching > 2 ? 2 : touching) * 10;
	score += catchment * 40;
	score += LongestLargeRunway(grid) * 8;
	/* Bounding-box infill improves the visual footprint but has no operational
	 * value, whether it came out as empty ground or as apron. Do not let it make
	 * the same functional design score worse: count only the design proper. */
	local functional_tiles = 0;
	local minx = 9999, miny = 9999, maxx = -1, maxy = -1;
	foreach (c in grid.Ordered()) {
		if (!c.infill) functional_tiles++;
		if (c.x < minx) minx = c.x;
		if (c.y < miny) miny = c.y;
		if (c.x > maxx) maxx = c.x;
		if (c.y > maxy) maxy = c.y;
	}
	local bbox_area = (maxx >= minx && maxy >= miny) ? (maxx - minx + 1) * (maxy - miny + 1) : 0;
	score -= functional_tiles * 6;
	score -= upkeep / 8;
	/* Penalty for spreading out over a larger bounding rectangle */
	score -= bbox_area * 2;

	if (want_large_safe) {
		if (safety == AIAirport.MS_OK) score += 400;
		else score -= 600;
	}
	return score;
}
