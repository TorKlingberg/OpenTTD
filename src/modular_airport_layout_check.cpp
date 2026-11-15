/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file modular_airport_layout_check.cpp Checks a modular airport layout for shapes that can deadlock. */

/*
 * The reservation code guarantees that aircraft never collide and never stop where
 * they may not wait. It cannot guarantee that a waiting aircraft ever moves again:
 * that depends on the layout. A waiting aircraft holds the safe stop it stands on, so
 * a deadlock is a ring of aircraft each standing where the next one needs to go, and
 * whether such a ring can form is a property of where the safe stops lie on the routes
 * aircraft take. See "Layouts that can deadlock" in skills/reservations-design.md.
 *
 * Routes are the ones the ground pathfinder returns on an empty airport, which is what
 * aircraft follow when nothing is in their way; alternatives taken round a blocked tile
 * are not considered. The pathfinder reads its crossing-required cache, as it does for
 * real traffic, but nothing here writes to it or to any other game state: the result
 * feeds the station window's airport stats only.
 */

#include "stdafx.h"
#include "station_base.h"
#include "airport.h"
#include "airport_ground_pathfinder.h"
#include "modular_airport_cmd.h"
#include "table/airporttile_ids.h"
#include <algorithm>
#include <iterator>
#include <map>
#include <set>
#include <vector>

#include "safeguards.h"

namespace {

using TileList = std::vector<TileIndex>;

/** Ordering of tiles by index. */
bool TileLess(TileIndex a, TileIndex b) { return a.base() < b.base(); }

/** Sort @p tiles and drop the duplicates. */
void SortUnique(TileList &tiles)
{
	std::sort(tiles.begin(), tiles.end(), TileLess);
	tiles.erase(std::unique(tiles.begin(), tiles.end()), tiles.end());
}

/** Whether @p tile is in the sorted list @p sorted. */
bool Contains(const TileList &sorted, TileIndex tile)
{
	return std::binary_search(sorted.begin(), sorted.end(), tile, TileLess);
}

/**
 * The route an aircraft takes from @p from to @p to with nothing in its way.
 * @param crossing Whether a route to a runway goal may cross another runway. A route to
 *                 anything else falls back to crossing on its own.
 */
AirportGroundPath FindRoute(const Station *st, TileIndex from, TileIndex to, bool crossing = false)
{
	/* Fixed-wing throughout: helicopters fly to and from where they park. */
	return FindAirportGroundPath(st, from, to, nullptr, crossing, false, GroundPathRestriction::FixedWing);
}

/** A route to a takeoff end, and whether it was allowed to cross another runway. */
struct TakeoffRoute {
	AirportGroundPath path;
	bool crossing;
};

/**
 * The routes an aircraft on @p from may take to a takeoff end, in the order the game
 * asks: every end without crossing a runway, and only when none can be reached that
 * way, every end with.
 */
std::vector<TakeoffRoute> FindTakeoffRoutes(const Station *st, TileIndex from, const TileList &takeoff_ends)
{
	std::vector<TakeoffRoute> routes;
	for (bool crossing : {false, true}) {
		for (TileIndex end : takeoff_ends) {
			AirportGroundPath path = FindRoute(st, from, end, crossing);
			if (path.found) routes.push_back({std::move(path), crossing});
		}
		if (!routes.empty()) break;
	}
	return routes;
}

/**
 * Whether @p to can be reached from @p from without taxiing across any tile of
 * @p closed. Both ends are exempt.
 */
bool ReachesAvoiding(const Station *st, TileIndex from, TileIndex to, const TileList &closed)
{
	if (from == to) return true;

	std::set<uint32_t> seen{from.base()};
	TileList open{from};
	while (!open.empty()) {
		const TileIndex tile = open.back();
		open.pop_back();
		for (TileIndex next : GetAirportGroundNeighbours(st, tile, GroundPathRestriction::FixedWing, to, true)) {
			if (next == to) return true;
			if (Contains(closed, next)) continue;
			if (seen.insert(next.base()).second) open.push_back(next);
		}
	}
	return false;
}

/** Index of the first safe stop on @p path after its start: where the aircraft rests first. */
size_t FirstSafeStop(const Station *st, const AirportGroundPath &path)
{
	for (size_t i = 1; i < path.tiles.size(); i++) {
		if (IsModularSafeStopTile(st, path.tiles[i], path.tiles.back())) return i;
	}
	return path.tiles.size() - 1;
}

/** Runway tiles on @p path from its first safe stop on. */
std::ptrdiff_t RunwayTilesPastStop(const Station *st, const AirportGroundPath &path)
{
	return std::count_if(path.tiles.begin() + FirstSafeStop(st, path), path.tiles.end(), [st](TileIndex tile) {
		const ModularAirportTileData *data = st->airport.GetModularTileData(tile);
		return data != nullptr && IsModularRunwayPiece(data->piece_type);
	});
}

/**
 * Whether a departure taking @p shortest can get round the shared queue tiles when an
 * arrival holds one, by the retry the game makes (FindReservableRoute): ban the held
 * tile and look again. That retry happens only when the route from where the aircraft
 * rests is refused, so the shared tile must be the first safe stop of @p shortest: an
 * aircraft already resting on an earlier queue tile does not look for another route.
 * The game takes a detour only when it is at most MAX_ROUTE_DETOUR_TILES longer and
 * leaves no more runway beyond its first safe stop, and the ban only lasts to that stop,
 * so a route that rejoins a shared tile further on is no way round. All the shared
 * tiles are banned together, as arrivals may be waiting on every one of them, and other
 * stands are banned too: they may be parked on.
 * @param st The station.
 * @param from Where the departure starts.
 * @param queue The shared tile on @p shortest being judged.
 * @param shared All shared queue tiles, sorted.
 * @param stands All stands of the airport, sorted.
 * @param shortest The route the departure takes when nothing is in its way.
 * @param crossing Whether @p shortest was allowed to cross another runway.
 */
bool HasWayRound(const Station *st, TileIndex from, TileIndex queue, const TileList &shared, const TileList &stands, const AirportGroundPath &shortest, bool crossing)
{
	if (shortest.tiles[FirstSafeStop(st, shortest)] != queue) return false;

	const TileIndex to = shortest.tiles.back();
	TileList avoid = shared;
	for (TileIndex stand : stands) {
		if (stand != from && stand != to) avoid.push_back(stand);
	}
	const AirportGroundPath path = FindAirportGroundPath(st, from, to, nullptr, crossing, false, GroundPathRestriction::FixedWing, avoid);
	if (!path.found || path.tiles.size() > shortest.tiles.size() + MAX_ROUTE_DETOUR_TILES) return false;
	if (RunwayTilesPastStop(st, path) > RunwayTilesPastStop(st, shortest)) return false;

	return std::ranges::none_of(path.tiles, [&](TileIndex tile) {
		return Contains(shared, tile) || (tile != from && tile != to && Contains(stands, tile));
	});
}

/** The queue tiles of the taxi graph: who follows whom, and who uses them for what. */
struct QueueUse {
	std::map<uint32_t, std::set<uint32_t>> next; ///< Queue tile -> queue tiles some route reaches next.
	TileList arrivals;   ///< Queue tiles on a route from a runway to parking.
	TileList departures; ///< Queue tiles on a route from parking to a runway.

	/** Record the queue tiles of one route, in the order it passes them. */
	void AddRoute(const Station *st, const AirportGroundPath &path, TileList *users)
	{
		TileIndex prev = INVALID_TILE;
		for (TileIndex tile : path.tiles) {
			if (!IsOneWayTaxiTile(st, tile)) continue;
			if (users != nullptr) users->push_back(tile);
			this->next[tile.base()];
			if (prev != INVALID_TILE) this->next[prev.base()].insert(tile.base());
			prev = tile;
		}
	}

	/** The queue tiles that lie on a cycle of the follows-relation. */
	TileList TilesOnCycles() const
	{
		/* A tile is on a cycle exactly when it can get back to itself. The graph has a
		 * node per one-way tile of one airport, so the plain search is plenty. */
		TileList on_cycle;
		for (const auto &[start, first] : this->next) {
			std::set<uint32_t> seen;
			std::vector<uint32_t> open(first.begin(), first.end());
			bool found = false;
			while (!open.empty() && !found) {
				const uint32_t tile = open.back();
				open.pop_back();
				if (tile == start) {
					found = true;
				} else if (seen.insert(tile).second) {
					const auto &follow = this->next.at(tile);
					open.insert(open.end(), follow.begin(), follow.end());
				}
			}
			if (found) on_cycle.push_back(TileIndex{start});
		}
		return on_cycle;
	}
};

} // namespace

std::vector<ModularLayoutIssue> CheckModularAirportLayout(const Station *st)
{
	std::vector<ModularLayoutIssue> issues;
	if (st == nullptr || st->airport.modular_tile_data == nullptr) return issues;

	TileList landing_ends, takeoff_ends, stands, hangars;
	for (const ModularAirportTileData &data : *st->airport.modular_tile_data) {
		if (data.piece_type == APT_STAND || data.piece_type == APT_STAND_1) {
			stands.push_back(data.tile);
		} else if (IsModularHangarPiece(data.piece_type)) {
			hangars.push_back(data.tile);
		} else if (IsModularRunwayEndPiece(data.piece_type)) {
			if (IsModularLandingEnd(st, data.tile)) landing_ends.push_back(data.tile);
			if (IsModularTakeoffEnd(st, data.tile)) takeoff_ends.push_back(data.tile);
		}
	}

	/* Half an airport is not a broken one. Without somewhere to land, somewhere to take
	 * off from and somewhere to park there is no traffic to get stuck, and the player
	 * can see as much. */
	if (landing_ends.empty() || takeoff_ends.empty() || (stands.empty() && hangars.empty())) return issues;

	SortUnique(stands);
	TileList parking = stands;
	parking.insert(parking.end(), hangars.begin(), hangars.end());

	QueueUse queues;
	/** A route from parking to a takeoff end. */
	struct Departure {
		TileIndex from;
		TakeoffRoute route;
	};
	std::vector<Departure> departures;
	for (TileIndex tile : parking) {
		bool reached = false;
		for (TileIndex end : landing_ends) {
			const AirportGroundPath path = FindRoute(st, end, tile);
			if (!path.found) continue;
			reached = true;
			queues.AddRoute(st, path, &queues.arrivals);
		}

		bool leaves = false;
		bool leaves_clear = false;
		/* Only a departure from a stand frees a stand. A hangar holds any number of
		 * aircraft, so nothing ever waits for one to leave it. One that has left and
		 * rests on a queue tile on its way could hold up a stand's departure there,
		 * but that departure goes on from the tile the same way, so it is counted
		 * in its own right. */
		const bool from_stand = Contains(stands, tile);
		for (TakeoffRoute &route : FindTakeoffRoutes(st, tile, takeoff_ends)) {
			const AirportGroundPath &path = route.path;
			leaves = true;
			queues.AddRoute(st, path, from_stand ? &queues.departures : nullptr);
			leaves_clear = leaves_clear || ReachesAvoiding(st, tile, path.tiles.back(), stands);
			if (from_stand) departures.push_back({tile, std::move(route)});
		}

		if (!reached) issues.push_back({ModularLayoutProblem::Unreachable, tile});
		if (!leaves) issues.push_back({ModularLayoutProblem::NoWayOut, tile});
		/* A stand that is the only way through gets parked on, and then it is no way
		 * through at all: two aircraft that each need the other's stand never move.
		 * Only one waiting on a stand holds a stand. A stand is no safe stop for an
		 * aircraft passing over it, so an arrival reserves its way past any stand it
		 * crosses before it lands, and one leaving a hangar before it leaves the
		 * hangar, which takes any number of aircraft. Neither waits on a stand. */
		if (from_stand && leaves && !leaves_clear) issues.push_back({ModularLayoutProblem::BehindStand, tile});
	}

	/* Aircraft also taxi between stands and hangars, and those routes can close a loop
	 * the runway routes leave open. One bound from a hangar for a stand waits for that
	 * stand just as an arrival from a runway does, so it queues like one. */
	for (TileIndex stand : stands) {
		/* The hangar an aircraft on the stand is sent to, as FindFreeModularHangar picks
		 * it: the cheapest route, the first such hangar on a tie. */
		TileIndex nearest = INVALID_TILE;
		int nearest_cost = 0;
		for (TileIndex hangar : hangars) {
			const AirportGroundPath to_hangar = FindRoute(st, stand, hangar);
			if (to_hangar.found) {
				queues.AddRoute(st, to_hangar, nullptr);
				if (nearest == INVALID_TILE || to_hangar.cost < nearest_cost) {
					nearest = hangar;
					nearest_cost = to_hangar.cost;
				}
			}
			const AirportGroundPath from_hangar = FindRoute(st, hangar, stand);
			if (from_hangar.found) queues.AddRoute(st, from_hangar, &queues.arrivals);
		}
		if (nearest != INVALID_TILE && !ReachesAvoiding(st, stand, nearest, stands)) {
			issues.push_back({ModularLayoutProblem::BehindStand, stand});
		}
	}

	for (TileIndex tile : queues.TilesOnCycles()) issues.push_back({ModularLayoutProblem::QueueLoop, tile});

	SortUnique(queues.arrivals);
	SortUnique(queues.departures);
	TileList shared;
	std::ranges::set_intersection(queues.arrivals, queues.departures, std::back_inserter(shared), TileLess);
	for (TileIndex tile : shared) {
		/* Arrivals waiting here for a stand jam the airport only when what would free the
		 * stand, a departure, has to come through here too. A departure with a way round
		 * takes it when the tile is held. */
		const bool forced = std::ranges::any_of(departures, [&](const Departure &dep) {
			const AirportGroundPath &path = dep.route.path;
			if (std::ranges::find(path.tiles, tile) == path.tiles.end()) return false;
			return !HasWayRound(st, dep.from, tile, shared, stands, path, dep.route.crossing);
		});
		if (forced) issues.push_back({ModularLayoutProblem::SharedQueue, tile});
	}

	std::sort(issues.begin(), issues.end(), [](const ModularLayoutIssue &a, const ModularLayoutIssue &b) {
		if (a.tile != b.tile) return a.tile.base() < b.tile.base();
		return a.problem < b.problem;
	});
	issues.erase(std::unique(issues.begin(), issues.end()), issues.end());
	return issues;
}

const std::vector<ModularLayoutIssue> &GetModularAirportLayoutIssues(const Station *st)
{
	const Airport &airport = st->airport;
	if (airport.modular_layout_issues_dirty) {
		airport.modular_layout_issues = CheckModularAirportLayout(st);
		airport.modular_layout_issues_dirty = false;
	}
	return airport.modular_layout_issues;
}
