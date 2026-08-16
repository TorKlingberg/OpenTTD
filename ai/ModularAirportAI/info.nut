class ModularAirportAI extends AIInfo
{
	function GetAuthor()      { return "Tor Klingberg"; }
	function GetName()        { return "ModularAirportAI"; }
	function GetShortName()   { return "MDAP"; }
	function GetDescription() { return "Designs and builds modular airports tile by tile, rather than picking a stock airport type."; }
	function GetVersion()     { return 1; }
	function GetDate()        { return "2026-08-16"; }
	function CreateInstance() { return "ModularAirportAI"; }
	function GetAPIVersion()  { return "16"; }
	function UseAsRandomAI()  { return true; }

	function GetSettings()
	{
		AddSetting({
			name = "max_airports",
			description = "Maximum number of airports to build",
			min_value = 1, max_value = 40,
			easy_value = 6, medium_value = 12, hard_value = 20, custom_value = 12,
			flags = CONFIG_INGAME
		});
		AddSetting({
			name = "variety",
			description = "How much layout variety to aim for (0 = always pick the highest scoring layout)",
			min_value = 0, max_value = 3,
			easy_value = 2, medium_value = 2, hard_value = 2, custom_value = 2,
			flags = CONFIG_INGAME
		});
		AddLabels("variety", {
			_0 = "None: always the best layout",
			_1 = "Some",
			_2 = "Plenty",
			_3 = "Maximum: near-random among workable layouts"
		});
		AddSetting({
			name = "selftest",
			description = "Dump generated layouts to the debug log at startup and stop",
			easy_value = 0, medium_value = 0, hard_value = 0, custom_value = 0,
			flags = CONFIG_BOOLEAN | CONFIG_INGAME | CONFIG_DEVELOPER
		});
	}
}

RegisterAI(ModularAirportAI());
