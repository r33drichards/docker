#include "inspircd.h"
class ModuleBoundedProbe final : public Module, public Timer
{
	unsigned tick = 0;

public:
	ModuleBoundedProbe()
		: Module(VF_NONE, "Synthetic bounded logger integration probe.")
		, Timer(1, true)
	{
	}
	void init() override { ServerInstance->Timers.AddTimer(this); }
	bool Tick() override
	{
		++tick;
		for (unsigned index = 0; index < 50; ++index)
			ServerInstance->Logs.Normal("BOUNDED_PROBE", "probe {} {} {}", tick, index, std::string(120, 'x'));
		ServerInstance->Logs.Debug("BOUNDED_PROBE", "TOP-SECRET-DEBUG-AUTH");
		ServerInstance->Logs.RawIO("USERINPUT", "TOP-SECRET-PASS");
		ServerInstance->Logs.Normal("USERINPUT", "TOP-SECRET-WRONG-LEVEL");
		return true;
	}
};
MODULE_INIT(ModuleBoundedProbe)
