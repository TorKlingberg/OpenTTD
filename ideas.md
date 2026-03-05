* Change "Aircraft last month" to last year instead.

* Diagonal taxi

* New plane holding pattern logic: Rectangle around the whole airport. Imaginary line from each runway. Where it intersects the rectangle, planes can turn off to land. Will it look good?
* Remove the stock -> modular conversion logic. Just keep fixed json files for the converted versions. 

To help debug ground pathing and reservation issues:
* A regression test that loads a save with several airports and many planes, and measures throughput after a year.


* Planes on stand should turn to face terminal building if there is one
* Make sure multiplayer won't desync

* New menu for modular debug, while text labels and on/off buttons

Pathing improvements:
* For landing-only runways, only reserve the runway before landing. Unless planes need to cross landing runway sometimes (may to get to takeoff runway)
* Free all tiles as soon as plane has passed them?

* What if a plane decides to go for service in the middle of a free-move segemnt, and can't get a path to hangar?

* Cache for airport path-finding?

* Add a global landings+takeoffs counter that is logged and reset at the end of a year.
