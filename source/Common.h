#pragma once

#include "Maths.h"

#include <algorithm>
#include <cstdint>
#include <string>

/**
    What every game's plan shares: a seeded stream of draws, the playback clock
    that lands a simulation on Spin Time, and the outcome it shows.

    ## Landing on Spin Time

    Every game is simulated at its own natural pace -- a roulette ball takes as
    long as friction says it takes -- and then PLAYED so that it comes to rest
    exactly at the duration asked for (Spin Time, or the time to the beat or
    bar it was sized for). Playback maps playback seconds onto simulated seconds
    with a rate that falls linearly from `rateStart` to `rateEnd`; the mean rate
    is the warp, natural / duration. This is polyhedral's `Plan::SimTime`,
    unchanged. A plan whose natural time is close to the duration (the planners
    aim for that first, by choosing the launch) plays at nearly 1x.
*/
namespace jackpot
{
using dice::Hash;
using dice::kPi;
using dice::M3;
using dice::Quat;
using dice::Unit;
using dice::V3;

/// A stream of 32-bit draws from one seed, for one purpose.
struct Stream
{
	uint32_t state;
	uint32_t counter = 0;
	explicit Stream( uint32_t seed ) : state( seed ) {}
	uint32_t Next()
	{
		return Hash( state, counter++ );
	}
	double Uniform()
	{
		return Unit( Next() );
	}
	double Range( double lo, double hi )
	{
		return lo + ( hi - lo ) * Uniform();
	}
	/// Uniform in [0, n), no modulo bias.
	int Below( int n )
	{
		if( n <= 1 )
			return 0;
		return static_cast< int >( dice::Below( static_cast< uint32_t >( n ), [ this ]() { return Next(); } ) );
	}
	/// Index drawn in proportion to `weights` (non-negative integers).
	template< typename Container >
	int Weighted( const Container& weights )
	{
		int total = 0;
		for( int w : weights )
			total += w;
		int u = Below( total );
		int i = 0;
		for( int w : weights )
		{
			if( u < w )
				return i;
			u -= w;
			++i;
		}
		return i - 1;
	}
};

struct Playback
{
	double natural   = 0.0;///< simulated seconds from start to rest
	double duration  = 0.0;///< playback seconds from Play to rest
	double warp      = 1.0;///< natural / duration
	double rateStart = 1.0;///< simulated seconds per playback second at the start
	double rateEnd   = 1.0;///< ... and at rest

	/// Simulated seconds at `seconds` of playback, clamped to the play.
	double SimTime( double seconds ) const
	{
		const double t = std::max( 0.0, std::min( seconds, duration ) );
		if( duration <= 0.0 )
			return natural;
		return t * ( rateStart + 0.5 * ( rateEnd - rateStart ) * t / duration );
	}
	bool Finished( double seconds ) const
	{
		return seconds >= duration;
	}
};

/// The rate curve that plays `natural` simulated seconds in `duration`.
/// Slow motion (warp < 1) eases toward the end so the reveal lingers;
/// fast motion plays uniformly. `noWarp` plays at 1x (a negative control).
Playback MakePlayback( double natural, double duration, bool noWarp = false );

/// What a play shows, in the game's own terms.
struct Outcome
{
	int value        = 0;    ///< the number, total, segment value or symbol shown
	bool win         = false;///< the player's bet won (README "Results")
	bool jackpot     = false;///< ... and it was the top prize
	bool nearMiss    = false;
	int payout       = 0;    ///< credits, for the slot's meter
	std::string text;        ///< for the display and the log, e.g. "17 RED"
};

} // namespace jackpot
