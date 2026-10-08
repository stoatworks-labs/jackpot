#pragma once

#include "Common.h"
#include "Controls.h"

#include <array>
#include <vector>

/**
    The one-armed bandit.

    ## The pull decides; the reels are a display

    Since the 1980s a slot machine's result has been decided by its random
    number generator the instant the handle is pulled. The reels are then
    driven by stepper motors to show it. Inge Telnaes's patent (US 4,448,419,
    1984) is the reason the jackpot can be rare while the reel still carries a
    seven on every pass: the RNG draws a stop on a VIRTUAL reel of many more
    stops than the physical one, and a table maps virtual stops onto physical
    ones -- unevenly. The jackpot symbol gets one virtual stop; the blanks either
    side of it get many. So the seven is seen sailing past just above or below
    the line far more often than its physical share would allow.

    That is this module, honestly: 22 physical stops (11 symbols, 11 blanks),
    a 64-stop virtual reel weighted as above (`Strip: Virtual`), or every
    physical stop equal (`Strip: Physical`). `jptest --nearmiss` measures the
    near-miss rate from the table and against the physical strip.

    ## The reels

    The plan is a schedule, not a simulation: the stops are the outcome and the
    reels are driven to them. Each reel kicks back as the handle drops, runs up
    to speed, holds a constant speed (chosen per reel so the distance it covers
    lands its stop exactly), slows to a creep, and is caught by the detent --
    a damped spring whose ratio is Reel Bounce. Reel i comes to rest a fixed
    gap before reel i + 1, and the last at exactly the duration.
*/
namespace jackpot::slots
{
constexpr int kStops    = 22;///< physical stops per reel
constexpr int kVirtual  = 64;///< virtual stops per reel
constexpr int kMaxReels = 5;
constexpr int kRows     = 3; ///< rows in the window: the payline is the middle one

enum Symbol : int
{
	Blank = 0,
	Cherry,
	Lemon,
	Orange,
	Plum,
	Bell,
	Melon,
	Bar,
	DoubleBar,
	TripleBar,
	Seven,
	Diamond,
	ClipSymbol,///< the effect's input, on the Clip set
	kSymbolCount
};

const char* SymbolName( int symbol );

/// One reel's physical strip and its virtual-reel weights.
struct Reel
{
	std::array< int, kStops > symbol {}; ///< per physical stop
	std::array< int, kStops > weight {}; ///< virtual stops mapped to each physical stop; sums to kVirtual
	int seven = 0;                       ///< the jackpot symbol's stop
};

/// The strips of a set, reel by reel (five; three-reel play uses the first three).
const std::array< Reel, kMaxReels >& Reels( SymbolSet set );

/// Credits paid for `stops` on the payline (0: nothing).
int Pays( SymbolSet set, int reels, const int* stops );
bool IsJackpot( SymbolSet set, int reels, const int* stops );
/// Sevens on the line on reels 1 and 2, and reel 3's seven one stop above or below it.
bool IsNearMiss( SymbolSet set, int reels, const int* stops );

struct Request
{
	SymbolSet set      = SymbolSet::Fruit;
	int reels          = 3;
	Strip strip        = Strip::Virtual;
	Result result      = Result::Random;
	int fixed          = 10;
	uint32_t seed      = 0;
	uint32_t play      = 0;
	double duration    = 3.0; ///< playback seconds from the pull to the last reel at rest
	double damping     = 0.6; ///< the detent's damping ratio
	std::array< double, kMaxReels > from {};///< each reel's stop before the pull

	bool uniformVirtual = false;///< negative control: a virtual reel with no weighting
};

/// Draw the stops a play shows, as the Result asks.
std::array< int, kMaxReels > DrawStops( const Request& r );

struct ReelMotion
{
	double from     = 0.0;///< stop at the payline before the pull
	double target   = 0.0;///< ... at rest: from + whole laps + the distance to the drawn stop
	double start    = 0.0;///< playback seconds: the reel begins to move
	double rampUp   = 0.2;
	double speed    = 40.0;///< stops per second, at the run
	double slowAt   = 0.0;///< begins to slow
	double creep    = 10.0;///< stops per second when the detent catches
	double catchAt  = 0.0;///< the detent catches
	double restAt   = 0.0;///< at rest (snapped exactly on the stop)
	double omega    = 75.0;///< the detent spring's natural frequency, rad/s
	double zeta     = 0.6;
	double kick     = 0.18;///< stops wound back as the handle drops

	/// The stop at the payline (unwrapped: it keeps counting across laps).
	double Position( double t ) const;
};

struct Plan
{
	Request request;
	std::array< int, kMaxReels > stops {};///< physical stop on the payline, per reel
	std::array< ReelMotion, kMaxReels > reels {};
	Playback playback;
	Outcome outcome;
	double pull = 0.3;///< seconds the handle takes to go down
	bool idle   = false;

	/// Position of reel i at playback seconds.
	double Position( int i, double seconds ) const;
	/// The handle's angle, 0 (up) to 1 (down), at playback seconds.
	double Handle( double seconds ) const;
};

Plan MakePlan( const Request& r );
/// The reels at rest showing `r.from`, no pull.
Plan MakeRest( const Request& r );

} // namespace jackpot::slots
