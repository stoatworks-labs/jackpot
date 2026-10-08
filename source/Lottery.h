#pragma once

#include "Common.h"
#include "Controls.h"

#include <atomic>
#include <string>
#include <vector>

/**
    The lottery drum: numbered balls tossed by air in a glass sphere, drawn one
    at a time up a tube at the top.

    ## The one idea, here

    The balls are identical. Nothing in the air, the glass or the other balls
    knows which number is printed on which, so the draw is simulated honestly
    and then the numbers are dealt out: the ball the tube caught first is
    painted with the first number wanted, and so on, and the rest get the rest.
    It is a permutation of the paint, applied from the instant the balls enter
    the drum -- which is why the drum is empty between draws: last draw's balls
    drain out of the bottom when Play is pressed, and the new set drops in from
    the loading chute, so no number is ever seen to change.

    ## The physics

    Spheres in a sphere: gravity, an air jet up the middle from the blower in
    the floor with a slow seeded swirl, quadratic air drag, impulses ball on
    ball and ball on glass with restitution and friction (which is what spins
    them, and the printed numbers with them). The tube opens at a scheduled time;
    the first ball into its mouth after that is drawn and carried up and along
    the rack.
*/
namespace jackpot::lottery
{
constexpr double kDrum      = 0.24;  ///< m: the glass's inner radius
constexpr double kBall      = 0.021; ///< m
constexpr double kMouth     = 0.034; ///< m: the tube's mouth radius at the top
constexpr double kRackZ     = kDrum + 0.10;
constexpr double kRackX0    = 0.075;
constexpr double kRackPitch = 0.047;

struct BallState
{
	V3 x;
	Quat q;
	int number = 0;
	bool shown = false;
};

struct Request
{
	int balls        = 49;
	int draw         = 6;
	Result result    = Result::Random;
	int bet          = 7;
	uint32_t seed    = 0;
	uint32_t play    = 0;
	double duration  = 12.0;
	double air       = 2.0;  ///< the jet's lift at its core, in g
	std::vector< BallState > previousDrum;///< last draw's balls, drained first
	std::vector< int > previousRack;      ///< and the drawn ones, rolled off the rack

	bool noWarp     = false;
	bool biasedDraw = false;
	bool identityPermutation = false;///< negative control: the paint as the balls were numbered
	double restitution = -1.0;
};

struct Simulation
{
	int balls = 0;
	std::vector< float > keys;  ///< per key, per ball: x y z qw qx qy qz
	std::vector< double > enter;///< sim seconds each ball enters the drum
	std::vector< int > captured;///< physical ball indices, in draw order
	std::vector< double > caughtAt;
	std::vector< V3 > caughtFrom;
	double natural   = 0.0;
	double maxOverlap = 0.0;    ///< m: the deepest ball-into-ball any step left
	double maxEscape  = 0.0;    ///< m: the furthest any ball got outside the glass
	static constexpr double kKeyRate = 120.0;
};

/// The numbers a draw shows, in order.
std::vector< int > DrawNumbers( const Request& r );

Simulation Simulate( const Request& r, double mix, double gap, const std::atomic< bool >* cancel = nullptr );

struct Plan
{
	Request request;
	Simulation sim;
	Playback playback;
	Outcome outcome;
	std::vector< int > numbers;  ///< drawn, in order
	std::vector< int > paint;    ///< per physical ball, its number
	double drain   = 0.0;        ///< sim seconds the old balls take to drain
	double mix     = 3.0;
	double gap     = 1.4;
	double ms      = 0.0;
	bool idle      = false;
	static constexpr double kRise = 0.35, kRoll = 0.45;

	/// Every ball to draw at playback seconds: the drum's, the drawn ones on
	/// their way or on the rack, and last draw's on their way out.
	std::vector< BallState > Balls( double seconds ) const;
	/// How far the tube's mouth is open, 0..1 (for the drawing).
	double Air( double seconds ) const;
};

Plan MakePlan( const Request& r, const std::atomic< bool >* cancel = nullptr );
/// No draw: an empty drum, last draw's balls on the rack.
Plan MakeRest( const Request& r );

/// The UK lotto's ball colours by number: white, blue, pink, green, yellow, purple.
int ColourBand( int number );

} // namespace jackpot::lottery
