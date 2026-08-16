/*
 * Finding ground to put an airport on.
 *
 * A stock airport needs its whole W×H rectangle flat, clear and at one height.
 * A modular one needs only the tiles it actually occupies, and those tiles may
 * be sloped as long as their *maximum* corner is at the same height — sloped
 * ones just get a foundation. So this searches for one-level regions of
 * arbitrary shape and lets fit.nut trim the layout to them, rather than
 * scanning for free rectangles the way a stock-airport AI has to.
 */

/**
 * Which rotations a layout may be built in.
 *
 * The AI rotates layouts itself and always places at rotation 0 (see
 * Grid.Rotate). Legacy small runway pieces are axis-locked, so a layout
 * containing one can only be turned by half-turns.
 */
function AllowedRotations(grid)
{
	foreach (c in grid.Ordered()) {
		if (IsSmallRunwayPiece(c.piece)) return [0, 2];
	}
	return [0, 1, 2, 3];
}

/**
 * Can a modular airport tile go here?
 *
 * Deliberately does not require SLOPE_FLAT. Requiring it would throw away most
 * of the reason to build modular at all; the game's actual rule is that every
 * tile's maximum height matches, and it puts a foundation under the rest.
 */
function IsUsableAirportTile(tile, base_height)
{
	if (!AIMap.IsValidTile(tile)) return false;
	if (!AITile.IsBuildable(tile)) return false;
	if (AITile.GetMaxHeight(tile) != base_height) return false;
	return true;
}

/**
 * Read the terrain around a town once, into a table of tile -> height for the
 * tiles an airport could use.
 *
 * The search tries many layouts at many origins at four rotations, and the
 * terrain lookups dominate: done naively that is tens of thousands of API calls
 * per town, which in a suspending script costs game *years*. Scanning the
 * region once turns the inner loop into table lookups.
 */
function ScanRegion(centre, radius)
{
	local region = {};
	local cx = AIMap.GetTileX(centre), cy = AIMap.GetTileY(centre);
	local x0 = cx - radius, x1 = cx + radius;
	local y0 = cy - radius, y1 = cy + radius;
	if (x0 < 1) x0 = 1;
	if (y0 < 1) y0 = 1;
	if (x1 > AIMap.GetMapSizeX() - 2) x1 = AIMap.GetMapSizeX() - 2;
	if (y1 > AIMap.GetMapSizeY() - 2) y1 = AIMap.GetMapSizeY() - 2;

	for (local y = y0; y <= y1; y++) {
		for (local x = x0; x <= x1; x++) {
			local t = AIMap.GetTileIndex(x, y);
			if (!AITile.IsBuildable(t)) continue;
			region[t] <- AITile.GetMaxHeight(t);
		}
	}
	return region;
}

/**
 * A site: an origin tile, a rotation, and the fitted layout that goes there.
 */
class Site
{
	tile = 0;
	rot = 0;
	grid = null;
	town = -1;
	score = 0;
	family = -1;
	trimmed = 0;   ///< cells dropped to fit the ground, i.e. how cramped the site was

	constructor(tile_, rot_, grid_, town_, family_)
	{
		this.tile = tile_;
		this.rot = rot_;
		this.grid = grid_;
		this.town = town_;
		this.family = family_;
		this.score = 0;
	}

	/**
	 * World tiles this site would occupy.
	 *
	 * The grid is already in world orientation — rotation happened in
	 * Grid.Rotate before fitting — so local coordinates are simply offsets from
	 * the origin tile.
	 */
	function Tiles()
	{
		local out = [];
		local ox = AIMap.GetTileX(this.tile), oy = AIMap.GetTileY(this.tile);
		foreach (c in this.grid.Ordered()) {
			out.append(AIMap.GetTileIndex(ox + c.x, oy + c.y));
		}
		return out;
	}
}

/**
 * Try to place one already-rotated grid at one origin.
 *
 * Returns a fitted Site, or null. The set handed to the fitter is the real
 * terrain test, so whatever comes back is known to sit on ground the build
 * command will accept.
 */
function TryFit(grid, origin, rot, town, region)
{
	local ox = AIMap.GetTileX(origin), oy = AIMap.GetTileY(origin);
	if (ox + grid.w > AIMap.GetMapSizeX() - 1) return null;
	if (oy + grid.h > AIMap.GetMapSizeY() - 1) return null;
	if (!(origin in region)) return null;

	/* Test only the tiles the layout wants, not its whole bounding box: on a
	 * ragged site most of the box is irrelevant. */
	local base = region[origin];
	local allowed = {};
	foreach (c in grid.Ordered()) {
		local t = AIMap.GetTileIndex(ox + c.x, oy + c.y);
		if (!(t in region)) continue;
		if (region[t] != base) continue;
		allowed[grid.Key(c.x, c.y)] <- true;
	}

	local fitted = FitGridToMask(grid, allowed);
	if (fitted == null) return null;
	local site = Site(origin, rot, fitted, town, -1);
	site.trimmed = grid.Count() - fitted.Count();
	return site;
}

/**
 * Choose a family to try.
 *
 * Uniform choice would make one airport in six a heliport, which is a poor
 * trade: helicopters are slow, and a heliport cannot serve the fixed-wing
 * network at all. Keep it rare, and let it earn its place by being the only
 * thing that fits on sites too small for a runway.
 */
function PickFamily(families, scale)
{
	local weighted = [];
	foreach (f in families) {
		local weight = 4;
		/* A grass strip is the right answer when there is no money and no modern
		 * pieces, and the wrong one everywhere else — it can never be large-safe,
		 * so every jet using it takes an elevated crash roll. Let it fade out as
		 * the AI can afford better. */
		if (f == Family.STRIP) weight = (scale >= 2) ? 0 : (scale == 1 ? 1 : 4);
		if (f == Family.DUAL) weight = (scale >= 2) ? 3 : 1;
		for (local i = 0; i < weight; i++) weighted.append(f);
	}
	if (weighted.len() == 0) return families[AIBase.RandRange(families.len())];
	return weighted[AIBase.RandRange(weighted.len())];
}

/**
 * The families to try, in descending order of preference.
 *
 * Order matters more than weighting here. Terrain silently selects for whatever
 * is smallest: a grass strip fits where a six-tile runway does not, and a
 * heliport fits where neither does, so offering all of them at once produces a
 * map covered in the meanest airport that would fit. Trying the real airports
 * first and falling back only when nothing fits gives each site the best
 * airport its ground can carry — and keeps strips and heliports for the cramped
 * places where they are genuinely the right answer.
 */
function FamilyTiers()
{
	local available = AvailableFamilies();
	local modern = [], strip = [], heli = [];
	foreach (f in available) {
		if (f == Family.HELIPORT) heli.append(f);
		else if (f == Family.STRIP) strip.append(f);
		else modern.append(f);
	}
	local tiers = [];
	if (modern.len() > 0) tiers.append(modern);
	if (strip.len() > 0) tiers.append(strip);
	if (heli.len() > 0) tiers.append(heli);
	return tiers;
}

/**
 * How much traffic a site should attract: population inside the catchment,
 * discounted for distance from the town centre.
 */
function SiteDemand(site)
{
	local layout = site.grid.ToLayout();
	local radius = AIAirport.GetModularLayoutCatchmentRadius(layout);
	local centre = AITown.GetLocation(site.town);
	local dist = AIMap.DistanceManhattan(site.tile, centre);
	local pop = AITown.GetPopulation(site.town);

	local reach = pop;
	if (dist > radius) reach = pop * radius / dist;
	return reach;
}

/**
 * Search around a town for the best airport it can take right now.
 *
 * Generates candidate layouts, then tries each against a sample of origins and
 * all four rotations. The preview API costs nothing, so the expensive part is
 * the terrain test — hence the coarse stride over origins rather than trying
 * every tile.
 *
 * `variety` widens the pool the winner is drawn from: at 0 the best-scoring
 * candidate always wins, at 3 any workable candidate can.
 */
/* Counters for the last search, so a run that builds nothing can say why. */
_site_origins <- 0;
_site_nofit <- 0;
_site_rejected <- 0;

function SiteSearchStats()
{
	return "tried=" + _site_origins + " nofit=" + _site_nofit + " rejected=" + _site_rejected;
}

/**
 * Find a site near a town, taking the best airport the ground will carry.
 *
 * Works down the tiers in FamilyTiers, stopping at the first that fits. If a
 * large-safe design was wanted and no tier can provide one, it tries again
 * without that requirement rather than leaving the town unserved — but the
 * caller then knows not to send jets there.
 */
function FindSiteNearTown(town, scale, want_large_safe, variety, budget, blacklist)
{
	local tiers = FamilyTiers();
	if (want_large_safe) {
		foreach (tier in tiers) {
			local site = SearchSites(town, scale, true, variety, budget, blacklist, tier);
			if (site != null) return site;
		}
	}
	foreach (tier in tiers) {
		local site = SearchSites(town, scale, false, variety, budget, blacklist, tier);
		if (site != null) return site;
	}
	return null;
}

function SearchSites(town, scale, want_large_safe, variety, budget, blacklist, families)
{
	_site_origins = 0;
	_site_nofit = 0;
	_site_rejected = 0;
	local centre = AITown.GetLocation(town);
	if (families.len() == 0) return null;

	local candidates = [];
	local tries = 4 + variety * 2;
	for (local i = 0; i < tries; i++) {
		local family = PickFamily(families, scale);
		local params = RandomParams(family, scale);
		if (!want_large_safe) params.large_safe = false;
		local grid = GenerateLayout(family, params);
		if (ValidateGrid(grid) != null) continue;
		if (!GridIsAvailable(grid)) continue;

		/* Turn the layout here rather than asking the build command to do it, and
		 * treat each orientation as its own candidate. Rotating changes which
		 * sites a design fits — a runway that will not fit east-west often fits
		 * north-south — so this is where cramped ground gets its options. */
		local rotations = AllowedRotations(grid);
		local rot = rotations[AIBase.RandRange(rotations.len())];
		local turned = grid.Rotate(rot);
		if (ValidateGrid(turned) != null) continue;
		candidates.append([family, turned, rot]);
	}
	if (candidates.len() == 0) return null;

	/* Deliberately no pruning by score here. Scoring favours big layouts, and
	 * big layouts are the ones that do not fit — pruning to the best candidates
	 * before touching terrain throws away exactly the small designs that a
	 * cramped site needs. */
	local cx = AIMap.GetTileX(centre), cy = AIMap.GetTileY(centre);
	local radius = 12;
	local stride = 2;
	/* The region has to cover where the *layouts* reach, not just where their
	 * origins sit, or every tile past the edge reads as unbuildable and nothing
	 * ever fits. */
	local region = ScanRegion(centre, radius + 18);
	local found = [];

	for (local oy = cy - radius; oy <= cy + radius; oy += stride) {
		if (oy < 1 || oy >= AIMap.GetMapSizeY() - 1) continue;
		for (local ox = cx - radius; ox <= cx + radius; ox += stride) {
			if (ox < 1 || ox >= AIMap.GetMapSizeX() - 1) continue;
			local origin = AIMap.GetTileIndex(ox, oy);
			if (!(origin in region)) continue;
			if (origin in blacklist) continue;

			foreach (cand in candidates) {
				_site_origins++;
				local site = TryFit(cand[1], origin, cand[2], town, region);
				if (site == null) { _site_nofit++; continue; }
				site.family = cand[0];
				if (!AcceptableSite(site, want_large_safe, budget)) { _site_rejected++; continue; }
				site.score = ScoreGrid(site.grid, want_large_safe)
				           + SiteDemand(site) / 4
				           - AIMap.DistanceManhattan(site.tile, centre) * 12;
				found.append(site);
			}
		}
	}
	if (found.len() == 0) return null;

	found.sort(function (a, b) {
		if (a.score < b.score) return 1;
		if (a.score > b.score) return -1;
		return 0;
	});
	local pool = 1 + variety * variety;
	if (pool > found.len()) pool = found.len();
	return found[AIBase.RandRange(pool)];
}

/**
 * Reject sites that would produce an airport not worth having.
 *
 * This is the "don't build silly things" gate. Trimming can leave a layout
 * legal but pointless — one stand, or a large-safe design that lost its tower
 * to the site edge and would send jets into an elevated crash roll.
 */
function AcceptableSite(site, want_large_safe, budget)
{
	local layout = site.grid.ToLayout();
	local stands = CountPieces(site.grid, IsStandPiece);
	local helipads = CountPieces(site.grid, IsHelipadPiece);

	if (stands == 0 && helipads == 0) return false;
	if (stands > 0 && !AIAirport.GetModularLayoutAcceptsPlanes(layout)) return false;
	if (want_large_safe && AIAirport.GetModularLayoutSafety(layout) != AIAirport.MS_OK) return false;

	/* The town has to tolerate the airport, or the build fails at the last step.
	 *
	 * GetAllowedNoise means two different things depending on a setting. With
	 * station_noise_level on it is a noise budget to compare the layout against;
	 * with it off — the default — it is "2 minus the airports this town already
	 * has", which is a count, and comparing a noise level against it rejects
	 * everything bigger than a single helipad. */
	if (AIGameSettings.GetValue("station_noise_level") != 0) {
		if (AIAirport.GetModularLayoutNoiseLevel(layout) > AITown.GetAllowedNoise(site.town)) return false;
	} else {
		if (AITown.GetAllowedNoise(site.town) < 1) return false;
	}

	/* Upkeep has to be payable while the route gets going. Half a year of it is
	 * the right order: an airport that cannot pay for itself before its first
	 * aircraft arrives is a mistake, but demanding two years of reserve rejects
	 * every layout above a grass strip. */
	local upkeep = AIAirport.GetModularLayoutMonthlyMaintenanceCost(layout);
	if (upkeep > 0 && budget > 0 && upkeep * 6 > budget) return false;

	return true;
}
