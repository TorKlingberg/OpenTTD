Ever since I first played Transport Tycoon in the '90s I've wanted airports to be more interesting, and since I first encountered OpenTTD 20 years ago I've dreamt of modular airports. Now I've gone and done it.

[b]Features:[/b]
[list]
[*]Manually build your airport by placing runways, stands, aprons (taxiways), hangars, terminals, etc.
[*]Planes automatically find their way while taxiing on the ground, much like road vehicles.
[*]To get started, you can place a modular version of a stock airport and then modify it.
[*]Expand your airport over time.
[*]Save your designs as templates.
[*]Choose the direction of runways and allow landings, takeoffs, or both.
[/list]

It is AI coded. I have written C++ professionally, but I have not read all the code changes for this. It wasn't a single prompt though: I have worked on this over a couple of months in my spare time. This is a fork of OpenTTD, and I don't expect it to ever be merged into the main game. If any patch set maintainer is interested I'd be happy to work with them.

The hardest part, as expected, was getting planes to taxi around airports without getting stuck when an airport is busy. I won't promise the system is 100% deadlock-free, but it works quite well.

The basic rule is that planes will not land or leave a stand or hangar until they can reserve a path to where they are going next. Aprons can be marked one-way, and one-way aprons also serve as places for planes to queue on the way to or from a runway.

[b]Tips for building efficient airports:[/b]
[list]
[*]Make sure there are aprons (taxiways) so planes can get between runways, loading stands, and hangars.
[*]Avoid layouts where planes have to cross a loading stand to get somewhere.
[*]Avoid making aircraft cross runways unnecessarily. As with the Metropolitan Airport, a crossing aircraft temporarily blocks the runway.
[*]Use one-way aprons to create queues for planes that have just landed or are waiting to take off.
[*]To be safe for large planes, an airport needs a 6-tile paved runway, a control tower, and a big terminal building.
[*]Adding a helipad or two really speeds up helicopter operations and keeps them out of the way of fixed-wing aircraft.
[*]Use edge fences to stop planes from taking bad shortcuts.
[/list]

Building costs, maintenance, noise, catchment areas, and similar factors roughly match the stock airports.

There are no new graphics. The system uses the existing airport graphics. That means there are some limitations: the old runway graphic only exists along one axis, and the old/small hangar graphic has only one facing. Most elements of the stock airports are available to build, except the diagonal taxiways of the City Airport.

There is also a new AI player, [b]ModularAirportAI[/b], that builds modular airports. It has a set of functional templates that it can rotate and tweak. It also expands airports over time with new runways and stands, but isn't very good at placing them efficiently. It doesn't build ground vehicles and is not competitive with AAAHogEx.

[b]This is an experimental fork.[/b] Savegames written by it cannot be opened in official OpenTTD, and multiplayer participants must use the same fork build.

I'm happy to take any feedback, bug reports or feature requests right here.

[url=https://github.com/TorKlingberg/OpenTTD/blob/master/docs/images/modular-airports/README.md]More screenshots[/url]

[url=https://torklingberg.github.io/OpenTTD/]Web demo[/url]
[url=https://github.com/TorKlingberg/OpenTTD/releases/tag/modular-airports-test-3]Builds[/url] (not signed, so Windows and especially macOS may refuse to run them)
[url=https://github.com/TorKlingberg/OpenTTD]Source[/url]
