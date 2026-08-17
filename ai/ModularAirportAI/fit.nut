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
	 * whatever it still touches before giving up on the layout. */
	RepairHangars(g);

	/* Trimming can also cut a stand off from the taxiway network. Those are
	 * cheaper to drop than to route around. */
	PruneUnreachable(g);
	RepairHangars(g);

	if (ValidateGrid(g) != null) return null;
	return g;
}

/** Point each hangar at an adjacent through-taxiable tile, if one exists. */
function RepairHangars(grid)
{
	foreach (c in grid.Ordered()) {
		if (!IsHangarPiece(c.piece)) continue;
		local off = FaceOffset(c.rot);
		local n = grid.Get(c.x + off[0], c.y + off[1]);
		if (n != null && IsThroughTaxiable(n.piece)) continue;
		foreach (d in [[0, 1], [0, -1], [1, 0], [-1, 0]]) {
			local m = grid.Get(c.x + d[0], c.y + d[1]);
			if (m == null || !IsThroughTaxiable(m.piece)) continue;
			c.rot = FaceTowards(c.x, c.y, m.x, m.y);
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
	/* Mild on purpose, and bounded: only one or two stands can touch a hangar in
	 * any sane layout, so counting further would just be a proxy for "small
	 * airport". At most a third of one stand's worth, which breaks ties between
	 * trims of the same layout without ever choosing a smaller one. An earlier
	 * unbounded version at 15 a stand cost about 6% of total throughput by
	 * quietly shifting which layouts won. */
	local touching = StandsTouchingHangars(grid);
	score += (touching > 2 ? 2 : touching) * 10;
	score += catchment * 40;
	score += LongestLargeRunway(grid) * 8;
	score -= grid.Count() * 6;
	score -= upkeep / 8;

	if (want_large_safe) {
		if (safety == AIAirport.MS_OK) score += 400;
		else score -= 600;
	}
	return score;
}
