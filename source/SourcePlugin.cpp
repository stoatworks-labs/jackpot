#include "Jackpot.h"

/**
    The source: a casino game on its own -- on felt, in a dark room, or on
    nothing, so the layer goes over whatever is under it.

    Listed directly in the JackpotSource target, not in jackpot_core: both
    plugins share the class and not the `CFFGLPluginInfo` below, and putting
    either registration in the shared library would register both plugins into
    both bundles. The core is an OBJECT library because this registers itself
    from a file-scope constructor nothing references (see CMakeLists.txt).

    `SW Jackpot` is ten characters; the FFGL name field is char[ 16 ] and not
    null-terminated. `oxbow probe` reads it back the way a host does.
*/
namespace
{
class JackpotSource : public jackpot::JackpotPlugin
{
public:
	JackpotSource() :
		JackpotPlugin( false )
	{
	}
};
} // namespace

static CFFGLPluginInfo PluginInfo(
	PluginFactory< JackpotSource >,// Create method
	"JP01",                        // Plugin unique ID of maximum length 4
	"SW Jackpot",                  // Plugin name
	2,                             // API major version number
	1,                             // API minor version number
	0,                             // Plugin major version number
	1,                             // Plugin minor version number
	FF_SOURCE,                     // Plugin type
	"The casino floor: a one-armed bandit, a roulette wheel, the money wheel, a craps table and the lottery drum, "
	"each simulated honestly and landing on a random result or the one you set at exactly Spin Time, or on the beat "
	"or the bar. Chips and coins shower on a win.",
	"Jackpot FFGL source"          // About
);

extern "C" const char* JackpotSourceBuildStamp()
{
	return "jackpot " JACKPOT_VERSION " source, built " __DATE__ " " __TIME__;
}
