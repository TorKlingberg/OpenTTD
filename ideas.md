Change "Aircraft last month" to last year instead.

* Diagonal taxi

* New plane holding pattern logic: Rectangle around the whole airport. Imaginary line from each runway. Where it intersects the rectangle, planes can turn off to land. Will it look good?
* Remove the stock -> modular conversion logic. Just keep fixed json files for the converted versions. 

To help debug ground pathing and reservation issues:
* Special log when stale reservation is cleared by fallback sweep - done
* Draw reservation lines from planes
* A regression test that loads a save with several airports and many planes, and measures throughput after a year.

Pathing improvements:
* When a plane leaves a stand, it should un-reserve the stand as soon as it has left it, not wait until whole path is done.
