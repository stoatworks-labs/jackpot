#include "Jackpot.h"

/**
    The effect: the same games over the clip -- and the clip itself can be a
    reel symbol (Symbols: Clip).

    See SourcePlugin.cpp for why this file is listed in its own target.
*/
namespace
{
class JackpotEffect : public jackpot::JackpotPlugin
{
public:
	JackpotEffect() :
		JackpotPlugin( true )
	{
	}
};
} // namespace

static CFFGLPluginInfo PluginInfo(
	PluginFactory< JackpotEffect >,// Create method
	"JP02",                        // Plugin unique ID of maximum length 4
	"SW Jackpot Over",             // Plugin name
	2,                             // API major version number
	1,                             // API minor version number
	0,                             // Plugin major version number
	1,                             // Plugin minor version number
	FF_EFFECT,                     // Plugin type
	"Casino games over the clip -- slots, roulette, the money wheel, craps, the lottery drum, and showers of chips "
	"and coins -- landing on a random result or the one you set at exactly Spin Time. Symbols: Clip puts the clip "
	"on the reels.",
	"Jackpot FFGL effect"          // About
);

extern "C" const char* JackpotEffectBuildStamp()
{
	return "jackpot " JACKPOT_VERSION " effect, built " __DATE__ " " __TIME__;
}
