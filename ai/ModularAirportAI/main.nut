require("util.nut");
require("layout.nut");
require("fit.nut");
require("selftest.nut");

class ModularAirportAI extends AIController
{
	function Start()
	{
		AICompany.SetName(UniqueCompanyName("ModularAirportAI"));

		if (AIController.GetSetting("selftest") == 1) {
			RunSelfTest();
			while (true) this.Sleep(1000);
		}

		while (true) this.Sleep(100);
	}

	function Save() { return {}; }
	function Load(version, data) { }
}

/** Company names must be unique, so add a suffix when the plain name is taken. */
function UniqueCompanyName(base)
{
	if (AICompany.SetName(base)) return base;
	for (local i = 2; i < 20; i++) {
		local name = base + " " + i;
		if (AICompany.SetName(name)) return name;
	}
	return base;
}
