Ever since I first played Transport Tycoon in the '90s I've wanted airports to be more interesting, and since I first encountered OpenTTD 20 years ago I've dreamt of modular airports. Now I've gone and done it.

Features:
* Manually build your airport by placing runways, stands, aprons (taxiways), hangars, terminals, etc.
* Planes automatically find their way taxi-ing on the ground, similar to road vehicles.
* To get started you can place modular version of the stock airports and then modify them
* Expand your airport over time
* Save your designs and templates
* Chose the direction of runways, and mark them as takeoff or landing only

It is AI coded. I have written C++ professionally, but I have not read all the code changes for this. It wasn't a single prompt though: I have worked on this over a couple of months in my spare time. This is a fork of OpenTTD, and I don't expect it to ever be merged into the main game. If any patch set maintainer is interested I'd be happy to work with them.

The hardest part, as expected, was getting planes to taxi around the airports without getting stuck when it's busy. I won't promise it's 100% deadlock free, but it works quite well.

The basic rule is that planes will not land or leave a stand or hangar until it can reserve a path to where it's going next. Aprons can be marked one-way, and one-way apron also serve as a place for planes to queue on the way to  or from a runway.

Tips for building efficient airports:
* Make sure there are aprons (taxiways) so planes can get between runways, loading stands and hangars.
* Avoid layouts where planes have to cross a loading stand to get somewhere.
* Having to cross one runway to get to an other, like the Metropolitan airport blocks it during crossing.
* Use one-way aprons to create queues for planes that just landed or are waiting to take off.
* To be safe for large planes an airport needs a 6-tile paved runway, a control tower and a big terminal building.
* Adding a helipad or two really speed up helicopter operations and keep them out of the way of fixed wing planes.
* Added fences to stop planes from taking bad shortcuts.

Build cost, maintenance, noise, catchment area and such roughly match the stock airports.

There are no new graphics. It uses the existing airport graphics. That means some limitations, like the old runway and hangar only exists in one direction. Most elements of the stock airports are available to build, except the diagonal taxiways of the City airport.

There is also a new AI player, ModularAirportAI, that builds modular airports. It has a set of functional templates that it can rotate and tweak. It also expands airports over time with new runways and stands, but isn't very good at placing them efficiently. It doesn't build ground vehicles and is not competitive with AAAHogEx.


Web demo: https://torklingberg.github.io/OpenTTD/
Builds: 
Source: https://github.com/TorKlingberg/OpenTTD
