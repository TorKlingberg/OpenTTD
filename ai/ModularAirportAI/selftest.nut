/*
 * Offline exercise of the layout generator.
 *
 * Dumps every family across a range of parameters as ASCII, together with what
 * the preview API says about each. Turn on the "selftest" setting and run
 * headless; nothing is built, so it is safe to run on any map.
 *
 * The point is that a layout can be legal, score well, and still not work — see
 * ValidateGrid in util.nut. Every generated grid goes through both checks here,
 * and a family that ever produces an invalid grid is a bug, not bad luck.
 */

function DumpGrid(label, grid)
{
	local problem = ValidateGrid(grid);
	local layout = grid.ToLayout();
	local safety = AIAirport.GetModularLayoutSafety(layout);
	local max_h_dist = MaxDistanceToHangar(grid);

	AILog.Info(label + "  " + grid.w + "x" + grid.h
	           + " tiles=" + grid.Count()
	           + " runway=" + LongestLargeRunway(grid)
	           + " stands=" + CountPieces(grid, IsStandPiece)
	           + " hangars=" + CountPieces(grid, IsHangarPiece)
	           + " max_h_dist=" + max_h_dist
	           + " safety=" + safety
	           + " catchment=" + AIAirport.GetModularLayoutCatchmentRadius(layout)
	           + " noise=" + AIAirport.GetModularLayoutNoiseLevel(layout)
	           + " upkeep=" + AIAirport.GetModularLayoutMonthlyMaintenanceCost(layout)
	           + " planes=" + AIAirport.GetModularLayoutAcceptsPlanes(layout)
	           + " heli=" + AIAirport.GetModularLayoutHasHelipad(layout)
	           + (problem == null ? "" : "   *** INVALID: " + problem + " ***"));
	foreach (row in grid.AsciiRows()) AILog.Info("    |" + row + "|");
	return problem == null;
}

/** Generate many layouts per family and check every one of them. */
function RunSelfTest()
{
	AILog.Info("=== ModularAirportAI layout self-test ===");
	AILog.Info("year=" + AIDate.GetYear(AIDate.GetCurrentDate()));
	AILog.Info("legend: = runway  E runway end  - < > small runway  + apron  S stand");
	AILog.Info("        T stand+terminal  P stand+pier  H hangar  h small hangar");
	AILog.Info("        X helipad  B terminal  b low terminal  t small terminal  W tower");
	AILog.Info("        r radar  f flag  , grass  _ empty");
	AILog.Info("        F fire station  C cargo terminal  U fuel farm  P car park");

	/* Calibration: the modular upkeep figure is directly comparable with a stock
	 * airport's, so print the stock ladder to give the generated numbers below a
	 * scale a human can judge. */
	AILog.Info("stock airport upkeep for comparison:"
	           + " small=" + AIAirport.GetMonthlyMaintenanceCost(AIAirport.AT_SMALL)
	           + " large=" + AIAirport.GetMonthlyMaintenanceCost(AIAirport.AT_LARGE)
	           + " metropolitan=" + AIAirport.GetMonthlyMaintenanceCost(AIAirport.AT_METROPOLITAN)
	           + " intercontinental=" + AIAirport.GetMonthlyMaintenanceCost(AIAirport.AT_INTERCON)
	           + " helistation=" + AIAirport.GetMonthlyMaintenanceCost(AIAirport.AT_HELISTATION));

	local families = [Family.STRIP, Family.LINEAR, Family.PIER,
	                  Family.DUAL, Family.APRON, Family.HELIPORT];
	local total = 0, bad = 0;

	/* A heliport deliberately has no runway. It must nevertheless admit
	 * helicopters, while a stand-only malformed airport must not.
	 *
	 * The last three pin which resource binds, because that is what decides what
	 * GrowAirport builds next: parking with few stands, the runway once they
	 * outgrow it, and parking again once a second runway lifts that lock. */
	local capacity_ok = AircraftCeilingForCounts(0, 2, 0) == 6
	                 && AircraftCeilingForCounts(2, 0, 0) == 0
	                 && AircraftCeilingForCounts(2, 0, 1) == 6
	                 && AircraftCeilingForCounts(4, 0, 1) == 8
	                 && AircraftCeilingForCounts(4, 0, 2) == 12;
	AILog.Info("capacity model: " + (capacity_ok ? "ok" : "*** INVALID ***"));
	if (!capacity_ok) bad++;

	/* Guard the useful minimum independently of RandomParams: even a caller
	 * asking for a shorter strip must get a complete four-tile runway. */
	local floor_params = DefaultParams();
	floor_params.runway_length = 1;
	floor_params.large_safe = false;
	local floor_grid = GenerateStrip(floor_params);
	local legacy_floor_ok = CountPieces(floor_grid, IsSmallRunwayPiece) >= MIN_LEGACY_RUNWAY_LENGTH;
	AILog.Info("legacy runway minimum: " + (legacy_floor_ok ? "ok" : "*** INVALID ***"));
	if (!legacy_floor_ok) bad++;

	foreach (family in families) {
		AILog.Info("");
		AILog.Info("--- family " + FamilyName(family) + " ---");
		for (local scale = 0; scale <= 3; scale++) {
			for (local rep = 0; rep < 3; rep++) {
				local params = RandomParams(family, scale);
				local grid = GenerateLayout(family, params);
				total++;
				local filled = true;
				if (AIAirport.IsModularPieceAvailable(AIAirport.MP_EMPTY)) {
					for (local y = 0; y < grid.h && filled; y++) {
						for (local x = 0; x < grid.w; x++) {
							if (grid.Get(x, y) == null) { filled = false; break; }
						}
					}
				}
				local max_h_dist = MaxDistanceToHangar(grid);
				local dist_ok = max_h_dist <= 10;
				local valid = DumpGrid(FamilyName(family) + " scale=" + scale + " rep=" + rep, grid);
				if (!filled) AILog.Info("    *** INVALID: generated bounds contain holes ***");
				if (!dist_ok) AILog.Info("    *** INVALID: max hangar distance " + max_h_dist + " > 10 ***");
				if (!valid || !filled || !dist_ok) bad++;
			}
		}
	}

	/* Every rotation AllowedRotations offers has to survive Rotate and come back
	 * a valid, buildable airport, because the site search takes whichever one it
	 * rolls and never re-asks. This is the only offline cover for the two turns
	 * that are not free: a compound piece, which only survives the quarter-turn
	 * that carries its run from X onto Y, and a hangar, whose rotation is the
	 * way it faces and which the small hangar cannot be drawn in for every value
	 * unless the new airport graphics are on. So the counts below depend on that
	 * setting, and both answers are correct. */
	AILog.Info("");
	AILog.Info("--- rotations ---");
	foreach (family in families) {
		local turns = 0, refused = 0;
		for (local scale = 0; scale <= 3; scale++) {
			local grid = GenerateLayout(family, RandomParams(family, scale));
			if (ValidateGrid(grid) != null) continue;
			foreach (r in AllowedRotations(grid)) {
				local turned = grid.Rotate(r);
				turns++;
				/* Rotate hands back an unturned clone when it cannot express the
				 * turn, so a layout that came back the wrong shape means
				 * AllowedRotations offered something Rotate would not do. */
				local want_w = (r % 2 == 0) ? grid.w : grid.h;
				local problem = ValidateGrid(turned);
				if (turned.w != want_w) problem = "rotation " + r + " not applied";
				if (problem == null && !GridIsAvailable(turned)) {
					problem = "rotation " + r + " is not buildable";
				}
				if (problem != null) {
					total++;
					bad++;
					refused++;
					AILog.Info(FamilyName(family) + " scale=" + scale + " rot=" + r
					           + "  *** INVALID: " + problem + " ***");
					foreach (row in turned.AsciiRows()) AILog.Info("    |" + row + "|");
				}
			}
		}
		AILog.Info(FamilyName(family) + ": " + turns + " rotations offered, " + refused + " bad");
	}

	/* The fitter is the part that has to work on cramped ground, so exercise it
	 * against a few deliberately awkward masks. */
	AILog.Info("");
	AILog.Info("--- fitting into constrained sites ---");
	local masks = [
		["full 12x6",      function (x, y) { return x < 12 && y < 6; }],
		["notch",          function (x, y) { return x < 12 && y < 6 && !(x >= 8 && y >= 3); }],
		["diagonal shore", function (x, y) { return x < 12 && y < 6 && (x + y) < 13; }],
		["narrow 10x3",    function (x, y) { return x < 10 && y < 3; }],
		["ragged",         function (x, y) { return x < 12 && y < 6 && !((x == 4 && y >= 2) || (x == 9 && y <= 1)); }],
	];
	foreach (m in masks) {
		local name = m[0], fn = m[1];
		local best = null, best_family = -1;
		foreach (family in families) {
			for (local scale = 3; scale >= 0; scale--) {
				local params = RandomParams(family, scale);
				local grid = GenerateLayout(family, params);
				local allowed = {};
				for (local y = 0; y < grid.h; y++) {
					for (local x = 0; x < grid.w; x++) {
						if (fn(x, y)) allowed[grid.Key(x, y)] <- true;
					}
				}
				local fitted = FitGridToMask(grid, allowed);
				if (fitted == null) continue;
				if (best == null || fitted.Count() > best.Count()) { best = fitted; best_family = family; }
			}
		}
		AILog.Info("");
		if (best == null) {
			AILog.Info("mask '" + name + "': nothing fits");
		} else {
			total++;
			if (!DumpGrid("mask '" + name + "' -> " + FamilyName(best_family), best)) bad++;
		}
	}

	AILog.Info("");
	AILog.Info("=== self-test done: " + total + " layouts, " + bad + " invalid ===");
}
