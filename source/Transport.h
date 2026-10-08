#pragma once

#include <cmath>

/**
    Resolume's transport, as FFGL 2.1 gives it: `SetBeatInfo( bpm, barPhase )`.
    The SDK defaults the BPM to 120; nothing in the fleet has measured how
    often Resolume calls it or whether `barPhase` is a usable downbeat (pattern's
    note), so this keeps its own phase from the BPM and the host's elapsed
    time and RE-ALIGNS to the host's barPhase whenever one arrives, trusting
    it as a 0..1 position in a 4/4 bar. No GL, so --offline checks it.

    Clock Sync asks one question: how long until the next beat (or bar)
    boundary that is at least `lead` seconds away? The dose or shake is then
    sized (or, for the chameleon, timed) so the colour change lands on it.
*/
namespace jackpot
{
class Transport
{
public:
	static constexpr double kBeatsPerBar = 4.0;

	void SetBeatInfo( double bpm, double barPhase )
	{
		if( bpm > 0.0 && std::isfinite( bpm ) )
			this->bpm = bpm;
		if( std::isfinite( barPhase ) )
		{
			hostBarPhase = barPhase - std::floor( barPhase );
			hasHostPhase = true;
		}
	}

	/// Advance by the frame's HOST dt (real seconds). The phase runs at the BPM;
	/// a host bar phase overrides it.
	void Advance( double dtHostSeconds )
	{
		if( hasHostPhase )
		{
			barPhase     = hostBarPhase;
			hasHostPhase = false;
		}
		else
		{
			barPhase += dtHostSeconds * bpm / 60.0 / kBeatsPerBar;
			barPhase -= std::floor( barPhase );
		}
	}

	double Bpm() const
	{
		return bpm;
	}
	double BarPhase() const
	{
		return barPhase;
	}
	double BeatSeconds() const
	{
		return 60.0 / bpm;
	}
	double BarSeconds() const
	{
		return kBeatsPerBar * 60.0 / bpm;
	}

	/// Real seconds to the next beat (bar = false) or bar boundary at least
	/// `lead` seconds ahead.
	double SecondsToNext( bool bar, double lead ) const
	{
		const double period = bar ? BarSeconds() : BeatSeconds();
		const double phase  = bar ? barPhase : ( barPhase * kBeatsPerBar - std::floor( barPhase * kBeatsPerBar ) );
		double wait         = ( 1.0 - phase ) * period;
		while( wait < lead )
			wait += period;
		return wait;
	}

private:
	double bpm          = 120.0;
	double barPhase     = 0.0;
	double hostBarPhase = 0.0;
	bool hasHostPhase   = false;
};

} // namespace jackpot
