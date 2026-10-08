#pragma once

#include "Common.h"
#include "Controls.h"

#include <atomic>
#include <string>
#include <vector>

/**
    The money wheel (Big Six): a big vertical wheel of 54 segments, a peg
    between each pair, and a leather clapper at the top that the pegs push past.

    ## What slows it is the clapper

    Two bodies: the wheel (angle, a bearing's friction) and the clapper (a
    torsion spring at its pivot, lightly damped). Each peg that reaches the
    clapper bends it until the tip slips over; the spring's energy is then lost
    in the clapper's ringing, not handed back. So the wheel loses about the same
    energy per peg however fast it turns, ticks slower and slower, and at the
    end may not have enough to bend the clapper past the last peg -- and rocks
    back. Contact is a stiff penalty between each peg (a disc) and the clapper
    (a capsule), stepped at 50 kHz.

    ## The one idea, here

    The 54 pegs are identical, so turning the painted segments by k is invisible
    to the physics: the wheel is simulated, the segment under the clapper read,
    and the paint turned to put the wanted one there. The turn is absorbed into
    the operator's pull on the wheel before it is let go, as roulette's is in
    the croupier's spin.
*/
namespace jackpot::wheel
{
constexpr int kSegments     = 54;
constexpr double kRimRadius = 0.90;
constexpr double kPegRadius = 0.86;  ///< pegs' centres
constexpr double kPegSize   = 0.008;
constexpr double kPivotY    = kPegRadius + 0.10;
constexpr double kClapperLength = 0.115;
constexpr double kClapperHalf   = 0.004;

/// The values round the wheel, segment 0 first, anticlockwise: 1 2 5 10 20,
/// 40 the joker, 45 the logo.
const std::vector< int >& Layout();
std::string Name( int value );

struct Request
{
	Result result      = Result::Random;
	int bet            = 20;  ///< Fixed Number: the value backed
	uint32_t seed      = 0;
	uint32_t play      = 0;
	double duration    = 8.0;
	double stiffness   = 4.0; ///< the clapper's spring, N m / rad
	double paintNow    = 0.0; ///< the painted wheel's angle when Play is pressed

	bool noShift = false;
	bool noWarp  = false;
	bool biasedDraw = false;
};

/// The wheel's angle is kept in segments as whole + phase, and only the phase
/// reaches the pegs: whole segments are invisible to the physics BY
/// CONSTRUCTION (roulette's frets are the same), which is what makes turning
/// the paint by k segments exact. `jptest --wheel` holds the phase and the
/// clapper bit-identical under any `turns`.
struct Release
{
	double speed     = 6.0;///< rad/s, anticlockwise
	double phase     = 0.0;///< the pegs' angle at release, in segments, 0..1
	long turns       = 0;  ///< ... and its whole segments (the physics must not see these)
	double stiffness = 4.0;
	double pegTurn   = 0.0;///< negative control: pegs turned by this many segments, the physics sees it

	double Angle() const
	{
		return ( static_cast< double >( turns ) + phase ) * 2.0 * kPi / kSegments;
	}
};

struct Simulation
{
	std::vector< double > angle;  ///< wheel angle at kKeyRate, rad (for drawing)
	std::vector< double > phase;  ///< ... its phase in segments, 0..1
	std::vector< long > whole;    ///< ... and whole segments
	std::vector< double > clapper;///< clapper deflection at kKeyRate, rad
	double natural  = 0.0;
	bool settled    = false;
	int segment     = 0;          ///< the wheel's segment under the clapper at rest, whole turns and all
	int pegsPassed  = 0;
	int reversals   = 0;          ///< the wheel rocked back
	std::vector< double > pegLoss;///< J taken by each peg passage (energy before minus after)
	double startEnergy = 0.0;
	static constexpr double kKeyRate = 240.0;
};

Simulation Simulate( const Release& release, const std::atomic< bool >* cancel = nullptr );

/// The segment a play will show (Result read against the layout).
int Wanted( const Request& r );
/// Joules the bearing takes while the wheel turns one segment.
double BearingWorkPerSegment();

/// The clapper's deflection that just clears the pegs at wheel angle a, with
/// the wheel turning in the direction `direction` (+1 or -1): for the hand pull.
double ClearingDeflection( double angle, double direction );

struct Plan
{
	Request request;
	Release release;
	Simulation sim;
	Playback playback;
	Outcome outcome;
	int shift        = 0;  ///< segments the paint is turned from the pegs
	int wanted       = 0;  ///< segment index (in Layout) shown
	double pull      = 1.0;///< sim seconds of the operator's pull
	double paintFrom = 0.0, paintAtRelease = 0.0, paintOffset = 0.0;
	int trials       = 0;
	double ms        = 0.0;
	bool idle        = false;

	double PaintAngle( double seconds ) const;
	double Clapper( double seconds ) const;
};

Plan MakePlan( const Request& r, const std::atomic< bool >* cancel = nullptr );
Plan MakeRest( const Request& r );

} // namespace jackpot::wheel
