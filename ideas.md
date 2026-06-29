* Diagonal taxi

* Remove the stock -> modular conversion logic. Just keep fixed json files for the converted versions. 


* Planes on stand should turn to face terminal building if there is one
* Make sure multiplayer won't desync

Pathing improvements:
* What if a plane decides to go for service in the middle of a free-move segemnt, and can't get a path to hangar?
* Cache for airport path-finding?


I'm thinking: When a plane at an airport is stopped by the user, it should lose reserved tiles other than the tile it's currently on (could be two tiles if it's in between two)
If it's on a runway it would still keep the whole runway reserved. When the plane is started again it would aquire