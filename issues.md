From an other agent airport_type_modular.md

Modular airports currently have type AT_SMALL from enum AirportTypes. This is weird and sometimes causes bugs. 
I think they should have their own type, or maybe be AT_INVALID. 
They are all different, so can't have a shared fixed spec you look up like stock airports.

I get that this is a complicated change, but I think it's important, so please understand the code fully before commiting to a plan.

We either need to make sure AT_MODULAR's spec is ok for all possible airports, or make sure they are not looked up for modular airports.

Noise and maintenance cost needs proper calculation for modular airports. I want something where if you re-build the stock airports as modular you get the same results. Like already done for build cost and catchment area.

Maybe suitable to fix at the same time, or separately if that's better: After you have a placed a modular airport template, or built a stock airport with "Build as modular", the airport should not in any way be different from if you built the same layout manually. I don't mean bit-exact, but they shouldn't have some different flag. I suspect currently they are differnt in some ways, so please check this.

Check that HasHangar() and all sister methods return correct answers for all possible modular airports.
