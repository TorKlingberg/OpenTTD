/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/**
 * @file modular_airport_holding.cpp Holding-loop and approach geometry for modular airports.
 *
 * Extracted from modular_airport_cmd.cpp. Builds the Dubins-curve holding loop a
 * fixed-wing aircraft flies while waiting to land, the computed helicopter
 * landing/takeoff tiles, and the per-tick holding-waypoint targets for both.
 * The public entry points are declared in modular_airport_cmd.h.
 */

#include "stdafx.h"
#include "aircraft.h"
#include "landscape.h"
#include "station_base.h"
#include "station_map.h"
#include "debug.h"
#include "core/fixedpoint_func.hpp"
#include "airport_ground_pathfinder.h"
#include "modular_airport_cmd.h"

#include "table/airporttile_ids.h"

#include "safeguards.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <vector>

struct DubinsArc {
	int64_t cx;
	int64_t cy;
	int64_t r;
	int64_t a0;
	int64_t sweep; // CCW positive, CW negative
};

struct DubinsSeg {
	bool is_arc;
	DubinsArc arc;
	int64_t x0;
	int64_t y0;
	int64_t x1;
	int64_t y1;
};

struct DubinsPath {
	std::vector<DubinsSeg> segs;
	int64_t length;
	bool valid;
};

static void DirToVecFixed(Direction d, int64_t &dx, int64_t &dy)
{
	/* Precomputed normalized vectors for the 8 directions in 16.16.
	 * Order matches Direction enum: DIR_N..DIR_NW.
	 * Raw (dx,dy) per direction: {-1,-1}, {-1,0}, {-1,1}, {0,1}, {1,1}, {1,0}, {1,-1}, {0,-1}
	 * Diagonal entries are 1/sqrt(2) * 65536 = 46341. Cardinal entries are 65536.
	 */
	static const int64_t v_dx[] = {-46341, -65536, -46341,      0,  46341,  65536,  46341,      0};
	static const int64_t v_dy[] = {-46341,      0,  46341,  65536,  46341,      0, -46341, -65536};
	dx = v_dx[d % 8];
	dy = v_dy[d % 8];
}

static void AddWaypoint(std::vector<ModularHoldingLoop::Waypoint> &out, int64_t x, int64_t y, int64_t cx, int64_t cy)
{
	const int ix = FP16Round(x + cx);
	const int iy = FP16Round(y + cy);
	if (!out.empty() && out.back().x == ix && out.back().y == iy) return;
	out.push_back({ix, iy});
}

static void AppendFallbackRectLoopWaypoints(std::vector<ModularHoldingLoop::Waypoint> &out, int min_x, int min_y, int max_x, int max_y)
{
	const int loop_x0 = (min_x - MODULAR_HOLDING_MARGIN_TILES) * TILE_SIZE + TILE_SIZE / 2;
	const int loop_y0 = (min_y - MODULAR_HOLDING_MARGIN_TILES) * TILE_SIZE + TILE_SIZE / 2;
	const int loop_x1 = (max_x + MODULAR_HOLDING_MARGIN_TILES) * TILE_SIZE + TILE_SIZE / 2;
	const int loop_y1 = (max_y + MODULAR_HOLDING_MARGIN_TILES) * TILE_SIZE + TILE_SIZE / 2;
	const int cx = (loop_x0 + loop_x1) / 2;
	const int cy = (loop_y0 + loop_y1) / 2;

	out.clear();
	AddWaypoint(out, FP16FromInt(loop_x0), FP16FromInt(loop_y0), 0, 0);
	AddWaypoint(out, FP16FromInt(cx), FP16FromInt(loop_y0), 0, 0);
	AddWaypoint(out, FP16FromInt(loop_x1), FP16FromInt(loop_y0), 0, 0);
	AddWaypoint(out, FP16FromInt(loop_x1), FP16FromInt(cy), 0, 0);
	AddWaypoint(out, FP16FromInt(loop_x1), FP16FromInt(loop_y1), 0, 0);
	AddWaypoint(out, FP16FromInt(cx), FP16FromInt(loop_y1), 0, 0);
	AddWaypoint(out, FP16FromInt(loop_x0), FP16FromInt(loop_y1), 0, 0);
	AddWaypoint(out, FP16FromInt(loop_x0), FP16FromInt(cy), 0, 0);
}

static DubinsPath ComputeDubins(int64_t x1, int64_t y1, int64_t hdx1, int64_t hdy1,
		int64_t x2, int64_t y2, int64_t hdx2, int64_t hdy2, int64_t radius)
{
	DubinsPath best = {};
	best.valid = false;
	best.length = INT64_MAX;

	if (radius <= 0) return best;

	const int64_t th1 = FP16Atan2(hdy1, hdx1);
	const int64_t th2 = FP16Atan2(hdy2, hdx2);
	const int64_t dx = FP16Div(x2 - x1, radius);
	const int64_t dy = FP16Div(y2 - y1, radius);
	const int64_t d = FP16Sqrt(FP16Mul(dx, dx) + FP16Mul(dy, dy));
	if (d < FP16_EPSILON) return best;

	const int64_t theta = FP16Atan2(dy, dx);
	const int64_t alpha = FP16NormalizeAngle2Pi(th1 - theta);
	const int64_t beta = FP16NormalizeAngle2Pi(th2 - theta);

	struct Candidate {
		int64_t t;
		int64_t p;
		int64_t q;
		char types[3];
		bool valid;
	};

	auto eval_lsl = [&]() -> Candidate {
		Candidate c = {};
		c.types[0] = 'L'; c.types[1] = 'S'; c.types[2] = 'L';
		const int64_t p2 = (FP16_1 << 1) + FP16Mul(d, d) - (FP16Cos(alpha - beta) << 1) + (FP16Mul(d, FP16Sin(alpha) - FP16Sin(beta)) << 1);
		if (p2 < 0) return c;
		const int64_t tmp0 = d + FP16Sin(alpha) - FP16Sin(beta);
		const int64_t tmp1 = FP16Atan2(FP16Cos(beta) - FP16Cos(alpha), tmp0);
		c.t = FP16NormalizeAngle2Pi(-alpha + tmp1);
		c.p = FP16Sqrt(p2);
		c.q = FP16NormalizeAngle2Pi(beta - tmp1);
		c.valid = true;
		return c;
	};

	auto eval_rsr = [&]() -> Candidate {
		Candidate c = {};
		c.types[0] = 'R'; c.types[1] = 'S'; c.types[2] = 'R';
		const int64_t p2 = (FP16_1 << 1) + FP16Mul(d, d) - (FP16Cos(alpha - beta) << 1) + (FP16Mul(d, -FP16Sin(alpha) + FP16Sin(beta)) << 1);
		if (p2 < 0) return c;
		const int64_t tmp0 = d - FP16Sin(alpha) + FP16Sin(beta);
		const int64_t tmp1 = FP16Atan2(FP16Cos(alpha) - FP16Cos(beta), tmp0);
		c.t = FP16NormalizeAngle2Pi(alpha - tmp1);
		c.p = FP16Sqrt(p2);
		c.q = FP16NormalizeAngle2Pi(-beta + tmp1);
		c.valid = true;
		return c;
	};

	auto eval_lsr = [&]() -> Candidate {
		Candidate c = {};
		c.types[0] = 'L'; c.types[1] = 'S'; c.types[2] = 'R';
		const int64_t p2 = -(FP16_1 << 1) + FP16Mul(d, d) + (FP16Cos(alpha - beta) << 1) + (FP16Mul(d, FP16Sin(alpha) + FP16Sin(beta)) << 1);
		if (p2 < 0) return c;
		const int64_t p = FP16Sqrt(p2);
		const int64_t tmp2 = FP16Atan2(-FP16Cos(alpha) - FP16Cos(beta), d + FP16Sin(alpha) + FP16Sin(beta)) - FP16Atan2(-(FP16_1 << 1), p);
		c.t = FP16NormalizeAngle2Pi(-alpha + tmp2);
		c.p = p;
		c.q = FP16NormalizeAngle2Pi(-beta + tmp2);
		c.valid = true;
		return c;
	};

	auto eval_rsl = [&]() -> Candidate {
		Candidate c = {};
		c.types[0] = 'R'; c.types[1] = 'S'; c.types[2] = 'L';
		const int64_t p2 = -(FP16_1 << 1) + FP16Mul(d, d) + (FP16Cos(alpha - beta) << 1) - (FP16Mul(d, FP16Sin(alpha) + FP16Sin(beta)) << 1);
		if (p2 < 0) return c;
		const int64_t p = FP16Sqrt(p2);
		const int64_t tmp2 = FP16Atan2(FP16Cos(alpha) + FP16Cos(beta), d - FP16Sin(alpha) - FP16Sin(beta)) - FP16Atan2((FP16_1 << 1), p);
		c.t = FP16NormalizeAngle2Pi(alpha - tmp2);
		c.p = p;
		c.q = FP16NormalizeAngle2Pi(beta - tmp2);
		c.valid = true;
		return c;
	};

	const std::array<Candidate, 4> candidates = {eval_rsr(), eval_lsl(), eval_rsl(), eval_lsr()};

	for (const Candidate &cand : candidates) {
		if (!cand.valid) continue;
		const int64_t cand_len = FP16Mul(cand.t + cand.p + cand.q, radius);
		if (cand_len >= best.length) continue;

		std::vector<DubinsSeg> segs;
		segs.reserve(3);
		int64_t px = x1;
		int64_t py = y1;
		int64_t th = th1;

		auto append_arc = [&](char turn_type, int64_t arc_angle) {
			if (arc_angle <= FP16_EPSILON) return;

			const bool left = (turn_type == 'L');
			const int64_t cx = left ? (px - FP16Mul(radius, FP16Sin(th))) : (px + FP16Mul(radius, FP16Sin(th)));
			const int64_t cy = left ? (py + FP16Mul(radius, FP16Cos(th))) : (py - FP16Mul(radius, FP16Cos(th)));
			const int64_t a0 = FP16Atan2(py - cy, px - cx);
			const int64_t sweep = left ? arc_angle : -arc_angle;
			const int64_t a1 = a0 + sweep;
			const int64_t nx = cx + FP16Mul(radius, FP16Cos(a1));
			const int64_t ny = cy + FP16Mul(radius, FP16Sin(a1));

			DubinsSeg seg = {};
			seg.is_arc = true;
			seg.arc = {cx, cy, radius, a0, sweep};
			seg.x0 = px;
			seg.y0 = py;
			seg.x1 = nx;
			seg.y1 = ny;
			segs.push_back(seg);

			px = nx;
			py = ny;
			th = FP16NormalizeAngle2Pi(th + sweep);
		};

		auto append_straight = [&](int64_t dist) {
			if (dist <= FP16_EPSILON) return;
			const int64_t nx = px + FP16Mul(dist, FP16Cos(th));
			const int64_t ny = py + FP16Mul(dist, FP16Sin(th));
			DubinsSeg seg = {};
			seg.is_arc = false;
			seg.x0 = px;
			seg.y0 = py;
			seg.x1 = nx;
			seg.y1 = ny;
			segs.push_back(seg);
			px = nx;
			py = ny;
		};

		append_arc(cand.types[0], cand.t);
		append_straight(FP16Mul(cand.p, radius));
		append_arc(cand.types[2], cand.q);

		best.segs = std::move(segs);
		best.length = cand_len;
		best.valid = true;
	}

	return best;
}

static void SampleDubinsPath(const DubinsPath &path, int64_t step_px, std::vector<ModularHoldingLoop::Waypoint> &out, int64_t cx, int64_t cy)
{
	if (!path.valid || step_px <= 0) return;

	for (const DubinsSeg &seg : path.segs) {
		if (seg.is_arc) {
			const int64_t radius = seg.arc.r;
			const int64_t sweep_abs = std::abs(seg.arc.sweep);
			if (radius <= 0 || sweep_abs <= FP16_EPSILON) continue;

			const int64_t step_ang = FP16Div(step_px, radius);
			if (step_ang <= 0) continue;
			const int count = (int)(sweep_abs / step_ang);
			for (int i = 1; i <= count; ++i) {
				const int64_t step = (seg.arc.sweep >= 0) ? (i * step_ang) : -(i * step_ang);
				const int64_t a = seg.arc.a0 + step;
				if (std::abs(a - (seg.arc.a0 + seg.arc.sweep)) <= FP16_EPSILON) break;
				const int64_t x = seg.arc.cx + FP16Mul(radius, FP16Cos(a));
				const int64_t y = seg.arc.cy + FP16Mul(radius, FP16Sin(a));
				AddWaypoint(out, x, y, cx, cy);
			}
		} else {
			/* Straight segment: only emit the endpoint. */
			AddWaypoint(out, seg.x1, seg.y1, cx, cy);
		}
	}
}

struct GateInfo {
	TileIndex runway_tile;
	int gate_x;
	int gate_y;
	int threshold_x;
	int threshold_y;
	Direction approach_dir;
	int64_t hdx;
	int64_t hdy;
	int rel_x; ///< gate_x - center_x (integer, for deterministic angular sort)
	int rel_y; ///< gate_y - center_y (integer, for deterministic angular sort)
	std::vector<GateInfo> members; ///< colocated group members (empty = solo gate)
};

static void GatherAndSortGates(const Station *st, std::vector<GateInfo> &gates)
{
	gates.clear();
	if (st->airport.modular_tile_data == nullptr) return;

	int min_x = INT_MAX;
	int min_y = INT_MAX;
	int max_x = INT_MIN;
	int max_y = INT_MIN;
	for (const ModularAirportTileData &data : *st->airport.modular_tile_data) {
		const int tx = static_cast<int>(TileX(data.tile));
		const int ty = static_cast<int>(TileY(data.tile));
		min_x = std::min(min_x, tx);
		min_y = std::min(min_y, ty);
		max_x = std::max(max_x, tx);
		max_y = std::max(max_y, ty);
	}
	if (min_x == INT_MAX) return;

	/* Use doubled coordinates to avoid fractional center (integer-exact for deterministic sorting). */
	const int center_2x = (min_x + max_x) * TILE_SIZE + TILE_SIZE;
	const int center_2y = (min_y + max_y) * TILE_SIZE + TILE_SIZE;

	for (const ModularAirportTileData &data : *st->airport.modular_tile_data) {
		if (!IsModularRunwayPiece(data.piece_type)) continue;
		if (!IsModularRunwayEndPiece(data.piece_type)) continue;

		/* Skip runways that are too short to be usable. */
		{
			std::vector<TileIndex> rwy;
			if (!GetContiguousModularRunwayTiles(st, data.tile, rwy) || (int)rwy.size() < MIN_RUNWAY_LENGTH_TILES) continue;
		}

		const uint8_t flags = GetRunwayFlags(st, data.tile);
		if ((flags & RUF_LANDING) == 0) continue;

		const bool is_low = IsRunwayEndLow(st, data.tile);
		if (is_low && (flags & RUF_DIR_HIGH) == 0) continue;
		if (!is_low && (flags & RUF_DIR_LOW) == 0) continue;

		int approach_x = 0;
		int approach_y = 0;
		GetModularLandingApproachPoint(st, data.tile, &approach_x, &approach_y);
		const int threshold_x = TileX(data.tile) * TILE_SIZE + TILE_SIZE / 2;
		const int threshold_y = TileY(data.tile) * TILE_SIZE + TILE_SIZE / 2;
		const Direction approach_dir = GetRunwayApproachDirection(st, data.tile);

		int64_t hdx, hdy;
		DirToVecFixed(approach_dir, hdx, hdy);

		GateInfo gate = {};
		gate.runway_tile = data.tile;
		gate.gate_x = approach_x;
		gate.gate_y = approach_y;
		gate.threshold_x = threshold_x;
		gate.threshold_y = threshold_y;
		gate.approach_dir = approach_dir;
		gate.hdx = hdx;
		gate.hdy = hdy;
		gate.rel_x = approach_x * 2 - center_2x;
		gate.rel_y = approach_y * 2 - center_2y;
		gates.push_back(gate);
	}

	/* Colocate parallel same-direction gates before sorting.
	 * Nearby gates with the same approach direction are merged into a single
	 * representative at their midpoint, so the Dubins path treats them as one
	 * flyover.  The original gates are stored in representative.members so
	 * ComputeModularHoldingLoop can create per-runway Gate entries. */
	static constexpr int COLOCATE_LATERAL_MAX_PX = 5 * TILE_SIZE;
	static constexpr int COLOCATE_ALONG_MAX_PX   = 3 * TILE_SIZE;
	for (size_t i = 0; i < gates.size(); ++i) {
		for (size_t j = i + 1; j < gates.size(); ) {
			if (gates[i].approach_dir != gates[j].approach_dir) { ++j; continue; }
			const int dx = gates[j].threshold_x - gates[i].threshold_x;
			const int dy = gates[j].threshold_y - gates[i].threshold_y;
			const int64_t along   = std::abs(dx * gates[i].hdx + dy * gates[i].hdy);
			const int64_t lateral = std::abs(dx * (-gates[i].hdy) + dy * gates[i].hdx);

			if (lateral > FP16FromInt(COLOCATE_LATERAL_MAX_PX) || along > FP16FromInt(COLOCATE_ALONG_MAX_PX)) { ++j; continue; }

			/* Absorb gate j into gate i's group. */
			if (gates[i].members.empty()) gates[i].members.push_back(gates[i]); // include self
			gates[i].members.push_back(gates[j]);
			gates.erase(gates.begin() + j);
		}

		/* Recompute representative to midpoint of all members. */
		if (!gates[i].members.empty()) {
			int sum_gx = 0, sum_gy = 0;
			for (const auto &m : gates[i].members) { sum_gx += m.gate_x; sum_gy += m.gate_y; }
			const int n = static_cast<int>(gates[i].members.size());
			gates[i].gate_x = sum_gx / n;
			gates[i].gate_y = sum_gy / n;
			gates[i].rel_x = gates[i].gate_x * 2 - center_2x;
			gates[i].rel_y = gates[i].gate_y * 2 - center_2y;
		}
	}

	/* Deterministic angular sort using integer quadrant + cross-product (no transcendental functions).
	 * This avoids floating-point non-determinism that could cause multiplayer desyncs. */
	auto Quadrant = [](int x, int y) -> int {
		if (x > 0 && y >= 0) return 0;
		if (x <= 0 && y > 0) return 1;
		if (x < 0 && y <= 0) return 2;
		return 3;
	};
	std::sort(gates.begin(), gates.end(), [&Quadrant](const GateInfo &a, const GateInfo &b) {
		int qa = Quadrant(a.rel_x, a.rel_y);
		int qb = Quadrant(b.rel_x, b.rel_y);
		if (qa != qb) return qa < qb;
		/* Same quadrant: use cross-product for deterministic ordering. */
		int64_t cross = static_cast<int64_t>(a.rel_x) * b.rel_y - static_cast<int64_t>(a.rel_y) * b.rel_x;
		if (cross != 0) return cross > 0;
		return a.runway_tile.base() < b.runway_tile.base();
	});
}

const ModularHoldingLoop &GetModularHoldingLoop(const Station *st)
{
	if (st->airport.modular_holding_loop_dirty || st->airport.modular_holding_loop == nullptr) {
		if (st->airport.modular_holding_loop == nullptr) {
			st->airport.modular_holding_loop = new ModularHoldingLoop();
		}
		ComputeModularHoldingLoop(st, *st->airport.modular_holding_loop);
		st->airport.modular_holding_loop_dirty = false;
	}
	return *st->airport.modular_holding_loop;
}

/**
 * Whether @p data is a tile a helicopter may touch down on and park on: an
 * apron/taxiway piece that is neither a one-way queueing corridor nor wedged
 * against a building.
 *
 * One-way tiles can never be used as a helicopter pad.  A one-way tile is a
 * queueing corridor: a helicopter parked on one blocks every aircraft behind
 * it, and cannot leave except along the corridor's flow direction — which
 * typically feeds a runway rather than a stand, so a helicopter heading for a
 * terminal has no legal move at all.  That deadlocks the corridor permanently
 * rather than merely slowing it down.
 */
static bool IsModularHeliParkableApron(const Station *st, const ModularAirportTileData &data)
{
	if (!IsApronOrTaxiwayPiece(data.piece_type)) return false;
	if (data.one_way_taxi) return false;

	/* Check 8-directional adjacency for buildings. */
	static const TileIndexDiff neighbors[] = {
		TileDiffXY(1, 0), TileDiffXY(-1, 0), TileDiffXY(0, 1), TileDiffXY(0, -1),
		TileDiffXY(1, 1), TileDiffXY(1, -1), TileDiffXY(-1, 1), TileDiffXY(-1, -1),
	};
	for (TileIndexDiff diff : neighbors) {
		const ModularAirportTileData *nd = st->airport.GetModularTileData(data.tile + diff);
		if (nd != nullptr && IsModularBuildingPiece(nd->piece_type)) return false;
	}

	return true;
}

/**
 * Whether some hangar on this airport can be reached from @p from by ground.
 *
 * Answers yes/no only, and probes with update_cache=false, because the path *cost* is
 * not usable for a layout-derived decision. FindAirportGroundPath consults the saved
 * crossing-required cache, which live traffic mutates: once a pair is learned, that pair
 * takes the crossing pass and reports a different cost for an unchanged layout. Ranking
 * candidates by cost would therefore depend on when the computation happened, and this
 * cache is recomputed lazily per client, so two clients could pick different tiles.
 * Reachability is free of that — a hangar goal always falls through to the crossing pass,
 * so "found" is the same either way. Writing is suppressed for the same reason the
 * pathfinder's own probe convention suppresses it (see airport_ground_pathfinder.cpp):
 * a probe must not insert keys into saved state that no aircraft ever asked for.
 */
static bool IsModularHangarReachableFrom(const Station *st, TileIndex from)
{
	for (const ModularAirportTileData &data : *st->airport.modular_tile_data) {
		if (!IsModularHangarPiece(data.piece_type)) continue;
		if (FindAirportGroundPath(st, from, data.tile, nullptr, false, false).found) return true;
	}
	return false;
}

/**
 * Manhattan distance in tiles from @p from to the nearest hangar on this airport.
 * Pure layout, so it ranks candidates identically on every client and at every moment.
 * @return The distance, or INT_MAX when the airport has no hangar.
 */
static int ModularNearestHangarDistance(const Station *st, TileIndex from)
{
	int best = INT_MAX;
	for (const ModularAirportTileData &data : *st->airport.modular_tile_data) {
		if (!IsModularHangarPiece(data.piece_type)) continue;
		const int dist = abs(static_cast<int>(TileX(data.tile)) - static_cast<int>(TileX(from))) +
		                 abs(static_cast<int>(TileY(data.tile)) - static_cast<int>(TileY(from)));
		if (dist < best) best = dist;
	}
	return best;
}

bool IsModularPadWithHangarAccess(const Station *st, TileIndex tile)
{
	EnsureModularHeliTilesValid(st);
	const std::vector<TileIndex> &pads = st->airport.modular_hangar_reachable_pads;
	return std::find(pads.begin(), pads.end(), tile) != pads.end();
}

/**
 * Compute where a helicopter heading for a hangar should touch down when the
 * airport's helipads cannot get it to one.
 *
 * A rooftop heliport (`APT_HELIPORT`) has no taxiable neighbour at all, so a
 * helicopter that lands on one for servicing is stranded: the terminal handler
 * finds no reachable hangar, falls through to the departure ladder, finds no
 * reachable runway either, and lifts off vertically — then picks the very same
 * pad on the next approach, because helicopters only ever consider helipads.
 * The result is a helicopter bobbing up and down over the pad forever.
 *
 * Where that is the case the pads are simply not usable for this trip, so
 * precompute a touchdown tile that *can* reach a hangar: the apron nearest one,
 * or failing that a stand. Layout-derived like the other computed heli tiles,
 * so it rides the same dirty flag — which means every input must be layout-pure,
 * see IsModularHangarReachableFrom for the trap there.
 *
 * Stays INVALID_TILE in every ordinary layout — no helipads (the plain computed
 * heli tile already puts the helicopter on the taxi network), no hangar
 * (nothing to reach), or at least one pad that reaches a hangar.
 */
static void ComputeModularHeliServiceTile(const Station *st)
{
	st->airport.modular_heli_service_tile = INVALID_TILE;
	st->airport.modular_hangar_reachable_pads.clear();

	if (st->airport.modular_tile_data == nullptr || st->airport.modular_tile_data->empty()) return;

	bool has_helipad = false;
	bool has_hangar = false;
	for (const ModularAirportTileData &data : *st->airport.modular_tile_data) {
		if (IsModularHelipadPiece(data.piece_type)) has_helipad = true;
		if (IsModularHangarPiece(data.piece_type)) has_hangar = true;
	}
	if (!has_helipad || !has_hangar) return;

	/* Record which pads can actually get a helicopter to a hangar. The landing scan
	 * cannot work this out for itself: it runs once per flying tick per aircraft, and an
	 * A* per pad there is exactly the cost its cheap euclidean pad scoring exists to
	 * avoid. Without the filter a depot-bound helicopter can pick a cut-off pad at an
	 * airport that also has a connected one — it lands, cannot taxi to the hangar, lifts
	 * off, and scores the same pad best again. */
	for (const ModularAirportTileData &data : *st->airport.modular_tile_data) {
		if (!IsModularHelipadPiece(data.piece_type)) continue;
		if (IsModularHangarReachableFrom(st, data.tile)) st->airport.modular_hangar_reachable_pads.push_back(data.tile);
	}

	/* At least one usable pad, so the ordinary helipad flow works once filtered. */
	if (!st->airport.modular_hangar_reachable_pads.empty()) return;

	TileIndex best_apron = INVALID_TILE;
	int best_apron_dist = INT_MAX;
	TileIndex best_stand = INVALID_TILE;
	int best_stand_dist = INT_MAX;

	for (const ModularAirportTileData &data : *st->airport.modular_tile_data) {
		/* Aprons before stands: the helicopter taxis straight off to the hangar,
		 * so it need not consume a parking spot a fixed-wing aircraft wants. */
		const bool is_stand = (data.piece_type == APT_STAND || data.piece_type == APT_STAND_1);
		if (!is_stand && !IsModularHeliParkableApron(st, data)) continue;

		if (!IsModularHangarReachableFrom(st, data.tile)) continue;
		const int dist = ModularNearestHangarDistance(st, data.tile);

		if (is_stand) {
			if (dist < best_stand_dist) {
				best_stand_dist = dist;
				best_stand = data.tile;
			}
		} else if (dist < best_apron_dist) {
			best_apron_dist = dist;
			best_apron = data.tile;
		}
	}

	st->airport.modular_heli_service_tile = (best_apron != INVALID_TILE) ? best_apron : best_stand;
	Debug(misc, 2, "[ModAp] Station {} heli service tile: {} (no helipad reaches a hangar)",
		st->index,
		st->airport.modular_heli_service_tile == INVALID_TILE ? 0 : st->airport.modular_heli_service_tile.base());
}

/**
 * Compute the best helicopter landing/takeoff tile for a modular airport without helipads.
 * If any helipad exists, both tiles are set to INVALID_TILE (use real helipads).
 * Otherwise, prefer an apron tile not adjacent to buildings, closest to airport center.
 * Fallback: runway ends with appropriate flags.
 */
static void ComputeModularHeliTiles(const Station *st)
{
	st->airport.modular_heli_landing_tile = INVALID_TILE;
	st->airport.modular_heli_takeoff_tile = INVALID_TILE;

	if (st->airport.modular_tile_data == nullptr || st->airport.modular_tile_data->empty()) return;

	/* If the airport has helipads, helicopters use those directly via
	 * FindModularLandingTarget — no computed tile needed. */
	for (const ModularAirportTileData &data : *st->airport.modular_tile_data) {
		if (IsModularHelipadPiece(data.piece_type)) return;
	}

	/* Compute bounding box center. */
	int min_x = INT_MAX, min_y = INT_MAX, max_x = INT_MIN, max_y = INT_MIN;
	for (const ModularAirportTileData &data : *st->airport.modular_tile_data) {
		int tx = static_cast<int>(TileX(data.tile));
		int ty = static_cast<int>(TileY(data.tile));
		min_x = std::min(min_x, tx);
		min_y = std::min(min_y, ty);
		max_x = std::max(max_x, tx);
		max_y = std::max(max_y, ty);
	}
	int center_x = (min_x + max_x) / 2;
	int center_y = (min_y + max_y) / 2;

	/* Step 2: Find best apron/taxiway tile a helicopter may park on. */
	TileIndex best_apron = INVALID_TILE;
	int best_apron_dist = INT_MAX;

	for (const ModularAirportTileData &data : *st->airport.modular_tile_data) {
		if (!IsModularHeliParkableApron(st, data)) continue;

		int tx = static_cast<int>(TileX(data.tile));
		int ty = static_cast<int>(TileY(data.tile));
		int dist = abs(tx - center_x) + abs(ty - center_y);
		if (dist < best_apron_dist) {
			best_apron_dist = dist;
			best_apron = data.tile;
		}
	}

	if (best_apron != INVALID_TILE) {
		st->airport.modular_heli_landing_tile = best_apron;
		st->airport.modular_heli_takeoff_tile = best_apron;
		Debug(misc, 2, "[ModAp] Station {} computed heli tile: apron {}", st->index, best_apron.base());
		return;
	}

	/* Step 3: Fallback — find runway ends with appropriate flags. */
	TileIndex best_landing = INVALID_TILE;
	int best_landing_dist = INT_MAX;
	TileIndex best_takeoff = INVALID_TILE;
	int best_takeoff_dist = INT_MAX;

	for (const ModularAirportTileData &data : *st->airport.modular_tile_data) {
		if (!IsModularRunwayPiece(data.piece_type)) continue;

		if (!IsModularRunwayEndPiece(data.piece_type)) continue;

		/* Check minimum runway length. */
		std::vector<TileIndex> rwy;
		if (!GetContiguousModularRunwayTiles(st, data.tile, rwy) || (int)rwy.size() < MIN_RUNWAY_LENGTH_TILES) continue;

		uint8_t flags = GetRunwayFlags(st, data.tile);
		int tx = static_cast<int>(TileX(data.tile));
		int ty = static_cast<int>(TileY(data.tile));
		int dist = abs(tx - center_x) + abs(ty - center_y);

		if ((flags & RUF_LANDING) && dist < best_landing_dist) {
			best_landing_dist = dist;
			best_landing = data.tile;
		}
		if ((flags & RUF_TAKEOFF) && dist < best_takeoff_dist) {
			best_takeoff_dist = dist;
			best_takeoff = data.tile;
		}
	}

	st->airport.modular_heli_landing_tile = best_landing;
	st->airport.modular_heli_takeoff_tile = best_takeoff;
	Debug(misc, 2, "[ModAp] Station {} computed heli tile: landing={} takeoff={} (runway fallback)",
		st->index,
		best_landing == INVALID_TILE ? 0 : best_landing.base(),
		best_takeoff == INVALID_TILE ? 0 : best_takeoff.base());
}

/**
 * Whether a modular airport's layout contains a hangar piece. See the declaration in
 * station_base.h for why the spec cannot answer this.
 *
 * Deliberately takes the Airport rather than the Station: Airport::HasHangar is a hot
 * inline with no Station to hand, and the answer needs nothing but the tile data.
 */
bool ModularAirportHasHangar(const Airport &ap)
{
	if (ap.modular_has_hangar_dirty) {
		bool found = false;
		if (ap.modular_tile_data != nullptr) {
			for (const ModularAirportTileData &data : *ap.modular_tile_data) {
				if (IsModularHangarPiece(data.piece_type)) {
					found = true;
					break;
				}
			}
		}
		ap.modular_has_hangar = found;
		ap.modular_has_hangar_dirty = false;
	}
	return ap.modular_has_hangar;
}

void EnsureModularHeliTilesValid(const Station *st)
{
	if (!st->airport.modular_heli_tiles_dirty) return;
	ComputeModularHeliTiles(st);
	ComputeModularHeliServiceTile(st);
	st->airport.modular_heli_tiles_dirty = false;
}

void ComputeModularHoldingLoop(const Station *st, ModularHoldingLoop &loop)
{
	int min_x = INT_MAX;
	int min_y = INT_MAX;
	int max_x = INT_MIN;
	int max_y = INT_MIN;

	if (st->airport.modular_tile_data != nullptr && !st->airport.modular_tile_data->empty()) {
		for (const ModularAirportTileData &data : *st->airport.modular_tile_data) {
			const int tx = static_cast<int>(TileX(data.tile));
			const int ty = static_cast<int>(TileY(data.tile));
			min_x = std::min(min_x, tx);
			min_y = std::min(min_y, ty);
			max_x = std::max(max_x, tx);
			max_y = std::max(max_y, ty);
		}
	} else {
		const int tx = static_cast<int>(TileX(st->xy));
		const int ty = static_cast<int>(TileY(st->xy));
		min_x = max_x = tx;
		min_y = max_y = ty;
	}

	loop.waypoints.clear();
	loop.gates.clear();

	std::vector<GateInfo> gates;
	GatherAndSortGates(st, gates);
	if (gates.empty()) {
		AppendFallbackRectLoopWaypoints(loop.waypoints, min_x, min_y, max_x, max_y);
		return;
	}

	/* Use station center as relative origin for fixed-point math to prevent overflow. */
	const int center_px_x = (min_x + max_x) * TILE_SIZE / 2 + TILE_SIZE / 2;
	const int center_px_y = (min_y + max_y) * TILE_SIZE / 2 + TILE_SIZE / 2;
	const int64_t cx = FP16FromInt(center_px_x);
	const int64_t cy = FP16FromInt(center_px_y);

	const int64_t overshoot_px = FP16FromInt(MODULAR_HOLDING_OVERSHOOT_TILES * TILE_SIZE);
	const int64_t radius_px = FP16FromInt(MODULAR_HOLDING_TURN_RADIUS_TILES * TILE_SIZE);
	const int64_t sample_px = FP16FromInt(MODULAR_HOLDING_SAMPLE_INTERVAL_PX);

	/* Build the loop.  GatherAndSortGates has already colocated parallel
	 * same-direction gates into groups (representative at midpoint, originals
	 * in .members).  Each sorted entry becomes one waypoint + Dubins segment. */
	for (size_t i = 0; i < gates.size(); ++i) {
		const GateInfo &cur = gates[i];
		const GateInfo &next = gates[(i + 1) % gates.size()];

		/* Create Gate entries: if this is a colocated group, expand members;
		 * otherwise create a single Gate from the representative. */
		const uint32_t shared_wp = static_cast<uint32_t>(loop.waypoints.size());
		if (!cur.members.empty()) {
			for (const auto &m : cur.members) {
				ModularHoldingLoop::Gate gate = {};
				gate.runway_tile = m.runway_tile;
				gate.wp_index = shared_wp;
				gate.approach_x = m.gate_x;
				gate.approach_y = m.gate_y;
				gate.threshold_x = m.threshold_x;
				gate.threshold_y = m.threshold_y;
				gate.approach_dir = m.approach_dir;
				loop.gates.push_back(gate);
			}
		} else {
			ModularHoldingLoop::Gate gate = {};
			gate.runway_tile = cur.runway_tile;
			gate.wp_index = shared_wp;
			gate.approach_x = cur.gate_x;
			gate.approach_y = cur.gate_y;
			gate.threshold_x = cur.threshold_x;
			gate.threshold_y = cur.threshold_y;
			gate.approach_dir = cur.approach_dir;
			loop.gates.push_back(gate);
		}

		/* Waypoint and Dubins use the representative's (midpoint) coordinates. */
		const int64_t cur_gx = FP16FromInt(cur.gate_x) - cx;
		const int64_t cur_gy = FP16FromInt(cur.gate_y) - cy;
		AddWaypoint(loop.waypoints, cur_gx, cur_gy, cx, cy);

		/* Overshoot: endpoint only (no dense intermediates on straights). */
		const int64_t ex = cur_gx + FP16Mul(cur.hdx, overshoot_px);
		const int64_t ey = cur_gy + FP16Mul(cur.hdy, overshoot_px);
		AddWaypoint(loop.waypoints, ex, ey, cx, cy);

		const int64_t next_gx = FP16FromInt(next.gate_x) - cx;
		const int64_t next_gy = FP16FromInt(next.gate_y) - cy;

		DubinsPath path = ComputeDubins(ex, ey, cur.hdx, cur.hdy,
				next_gx, next_gy, next.hdx, next.hdy,
				radius_px);
		if (!path.valid) {
			path = ComputeDubins(ex, ey, cur.hdx, cur.hdy,
					next_gx, next_gy, next.hdx, next.hdy,
					radius_px >> 1);
		}
		if (!path.valid) {
			/* Fallback straight line: no intermediates needed. */
			continue;
		}

		SampleDubinsPath(path, sample_px, loop.waypoints, cx, cy);
	}
}

/**
 * Find the waypoint in the holding loop closest to the aircraft's current pixel position.
 * Position-based (not time-based) so it works correctly regardless of aircraft speed,
 * and avoids the "ghost laps the plane" problem that occurs with phase-driven targeting.
 */
uint32_t GetNearestModularHoldingWaypoint(const Aircraft *v, const ModularHoldingLoop &loop)
{
	const uint32_t n_wp = static_cast<uint32_t>(loop.waypoints.size());
	if (n_wp == 0) return 0;
	uint32_t nearest = 0;
	int64_t min_d2 = INT64_MAX;
	for (uint32_t i = 0; i < n_wp; ++i) {
		const int64_t dx = v->x_pos - loop.waypoints[i].x;
		const int64_t dy = v->y_pos - loop.waypoints[i].y;
		const int64_t d2 = dx * dx + dy * dy;
		if (d2 < min_d2) { min_d2 = d2; nearest = i; }
	}
	return nearest;
}

bool IsHoldingGateActive(uint32_t aircraft_wp, uint32_t gate_wp, uint32_t n_wp)
{
	if (n_wp == 0) return false;
	const uint32_t diff = (aircraft_wp + n_wp - gate_wp) % n_wp;
	return diff == 0 || diff == (n_wp - 1);
}

void GetModularHoldingWaypointTarget(Aircraft *v, const Station *st, int *target_x, int *target_y, uint32_t *wp_index)
{
	const ModularHoldingLoop &loop = GetModularHoldingLoop(st);
	const uint32_t n_wp = static_cast<uint32_t>(loop.waypoints.size());
	if (n_wp == 0) {
		TileIndex target = st->airport.tile;
		if (target == INVALID_TILE) target = st->xy;
		*target_x = TileX(target) * TILE_SIZE + TILE_SIZE / 2;
		*target_y = TileY(target) * TILE_SIZE + TILE_SIZE / 2;
		if (wp_index != nullptr) *wp_index = 0;
		return;
	}

	static constexpr uint32_t LOOKAHEAD = 2; ///< Waypoints ahead of base to steer toward (sparse waypoints).
	/* ADVANCE_DIST_SQ: when the aircraft is within this squared-pixel distance of the
	 * current target waypoint, advance the base index by one.  Advancing early (before
	 * reaching the exact pixel) prevents the aircraft from catching the target and
	 * circling it while waiting for the ghost clock to tick. */
	static constexpr int ADVANCE_DIST    = TILE_SIZE * 3; // 3 tiles
	static constexpr int ADVANCE_DIST_SQ = ADVANCE_DIST * ADVANCE_DIST;

	/* Initialise or reinitialise the per-aircraft base index from the time-based ghost.
	 * The ghost spreads aircraft around the whole loop via their vehicle-index offset, so
	 * planes that arrive at the airport at different times start at different loop phases
	 * and visit both runway gates.  UINT32_MAX (or a stale index >= n_wp) means the
	 * index needs to be set. */
	if (v->modular_holding_wp_index == UINT32_MAX || v->modular_holding_wp_index >= n_wp) {
		const uint64_t offset   = static_cast<uint64_t>(v->index.base() % n_wp) * MODULAR_HOLDING_TICKS_PER_WP;
		const uint64_t phase    = TimerGameTick::counter + offset;
		v->modular_holding_wp_index = static_cast<uint32_t>((phase / MODULAR_HOLDING_TICKS_PER_WP) % n_wp);
	}

	/* Advance the base index whenever the aircraft closes within ADVANCE_DIST of the
	 * current lookahead target.  This decouples progress from the ghost clock and means
	 * the target moves continuously as the aircraft flies, preventing it from catching
	 * a fixed point and circling it. */
	const uint32_t target_wp = (v->modular_holding_wp_index + LOOKAHEAD) % n_wp;
	const int64_t tdx = v->x_pos - loop.waypoints[target_wp].x;
	const int64_t tdy = v->y_pos - loop.waypoints[target_wp].y;
	if (tdx * tdx + tdy * tdy <= ADVANCE_DIST_SQ) {
		v->modular_holding_wp_index = (v->modular_holding_wp_index + 1) % n_wp;
	}

	/* Recompute target after possible advance. */
	const uint32_t final_target_wp = (v->modular_holding_wp_index + LOOKAHEAD) % n_wp;
	*target_x = loop.waypoints[final_target_wp].x;
	*target_y = loop.waypoints[final_target_wp].y;
	if (wp_index != nullptr) *wp_index = v->modular_holding_wp_index;
}

void GetModularHeliHoldingTarget(Aircraft *v, const Station *st, int *target_x, int *target_y)
{
	TileIndex hold_center = INVALID_TILE;
	EnsureModularHeliTilesValid(st);
	if (st->airport.modular_heli_landing_tile != INVALID_TILE) {
		hold_center = st->airport.modular_heli_landing_tile;
	} else {
		hold_center = FindModularLandingTarget(st, v);
	}
	if (hold_center == INVALID_TILE) hold_center = st->xy;

	const int center_x = TileX(hold_center) * TILE_SIZE + TILE_SIZE / 2;
	const int center_y = TileY(hold_center) * TILE_SIZE + TILE_SIZE / 2;
	static constexpr int HOLD_SQUARE_SIZE_TILES = 6;
	const int offset = (HOLD_SQUARE_SIZE_TILES * TILE_SIZE) / 2;

	static constexpr int ADVANCE_DIST = TILE_SIZE;
	static constexpr int ADVANCE_DIST_SQ = ADVANCE_DIST * ADVANCE_DIST;

	struct HoldPoint {
		int x;
		int y;
	};

	const HoldPoint points[8] = {
		{center_x - offset, center_y - offset},
		{center_x,          center_y - offset},
		{center_x + offset, center_y - offset},
		{center_x + offset, center_y},
		{center_x + offset, center_y + offset},
		{center_x,          center_y + offset},
		{center_x - offset, center_y + offset},
		{center_x - offset, center_y},
	};

	if (v->modular_holding_wp_index == UINT32_MAX || v->modular_holding_wp_index >= 8) {
		v->modular_holding_wp_index = v->index.base() % 8;
	}

	uint32_t idx = v->modular_holding_wp_index;
	const int dx = v->x_pos - points[idx].x;
	const int dy = v->y_pos - points[idx].y;
	if (dx * dx + dy * dy <= ADVANCE_DIST_SQ) {
		idx = (idx + 1) % 8;
		v->modular_holding_wp_index = idx;
	}

	*target_x = points[idx].x;
	*target_y = points[idx].y;
}
