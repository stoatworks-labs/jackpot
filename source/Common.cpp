#include "Common.h"

#include <algorithm>

namespace jackpot
{
Playback MakePlayback( double natural, double duration, bool noWarp )
{
	Playback p;
	p.natural  = std::max( 0.0, natural );
	p.duration = std::max( 0.05, duration );
	p.warp     = noWarp ? 1.0 : p.natural / p.duration;
	if( noWarp || p.warp >= 1.0 )
		p.rateStart = p.rateEnd = p.warp;
	else
	{
		//rate( t ) = c ( 1 - k t / D ): its mean is c ( 1 - k / 2 ), which must be
		//the warp. polyhedral's curve: k grows as the play falls short of the
		//duration and stops at 0.35, so the end lingers without freezing.
		const double k = std::clamp( 0.5 * ( 1.0 - p.warp ), 0.0, 0.35 );
		p.rateStart    = p.warp / ( 1.0 - 0.5 * k );
		p.rateEnd      = p.rateStart * ( 1.0 - k );
	}
	if( noWarp )
		p.duration = p.natural;//played at 1x, it ends when it ends
	return p;
}

} // namespace jackpot
