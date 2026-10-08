#pragma once

#include "Common.h"
#include "Controls.h"

#include <atomic>
#include <vector>

/**
    The roulette wheel.

    ## The one idea, here

    A rotor's frets are identical. Turn the rotor by a whole pocket and the
    world the ball moves through is exactly the same world, so the ball does
    exactly the same thing -- it only ends in a pocket with a different number
    PAINTED in it. So the ball is simulated honestly, the pocket it ends in is
    read, and the numbered ring is turned by the k pockets that put the wanted
    number there. The physics never sees the integer part of the rotor's angle
    in pockets: the frets are placed from its fractional part alone, so the
    claim is exact by construction (`jptest --roulette` holds a trajectory to
    the bit against the rotor turned by whole pockets, and the negative control
    turns it by half a pocket and must see it change).

    The k-pocket turn would show as the ring jumping when Play is pressed. It
    does not, because the croupier spins the rotor by hand before releasing the
    ball: that spin-up is scripted, not simulated, and takes the ring from where
    it was to wherever the plan needs it, smoothly, a turn or so later.

    ## The ball

    A point mass carrying the ball's radius, on a surface of revolution z = h(r):

      - the ball track, rising outward at 14 degrees to the vertical lip;
      - the stator cone, 20 degrees, with eight diamond deflectors;
      - the rotor's apron, the numbered band;
      - the pockets: a flat floor between two steep steps, split by the frets.

    Gravity, the surface's normal, Coulomb friction against the lip, rolling
    resistance on the stator, slip friction on the turning rotor, air drag.
    On the track the ball is held against the lip for as long as v^2 / r
    exceeds g tan(14 deg) and no longer: it leaves when v^2 = g r tan(alpha),
    which `jptest --roulette` measures. Frets and diamonds are struck with a
    restitution; a diamond's sloped faces throw the ball up.
*/
namespace jackpot::roulette
{
constexpr double kBallRadius = 0.0095;
constexpr double kLip        = 0.395;///< m: the ball track's outer wall
constexpr double kTrackIn    = 0.335;///< m: where the track meets the stator
constexpr double kRotorOut   = 0.255;///< m: the rotor's edge
constexpr double kPocketOut  = 0.232;///< m: the pockets' outer step
constexpr double kPocketIn   = 0.195;///< m: ... and inner
constexpr double kDiamondR   = 0.300;///< m: the deflectors' radius
constexpr double kTrackSlope = 0.24932800284318071;///< tan( 14 deg )
constexpr double kStatorSlope = 0.36397023426620234;///< tan( 20 deg )
constexpr double kApronSlope = 0.25;
constexpr double kPocketDepth = 0.014;
constexpr double kFretHeight  = 0.014;///< above the pocket floor: flush with the rims (AGENTS.md)
constexpr double kStep        = 0.004;///< m: the width of a pocket's steep steps
constexpr int kDiamonds       = 8;
constexpr double kGravity     = 9.81;

/// The wheel's numbers in pocket order, anticlockwise from pocket 0. 37 is 00.
const std::vector< int >& Numbers( RouletteWheel wheel );
/// 0 green, 1 red, 2 black.
int Colour( int number );
std::string Name( int number );

/// The surface height under the ball's centre, its slope, and the rotor's
/// share of it (true over the apron and pockets).
double Height( double r );
double Slope( double r );
bool OnRotor( double r );

struct Request
{
	RouletteWheel wheel  = RouletteWheel::European;
	Result result        = Result::Random;
	int bet              = 17;  ///< Fixed Number: the player's straight-up bet (37 is 00)
	uint32_t seed        = 0;
	uint32_t play        = 0;
	double duration      = 8.0; ///< playback seconds from Play to the ball at rest
	double rotorSpeed    = 2.5; ///< rad/s once spun, anticlockwise
	bool deflectors      = true;
	double paintNow      = 0.0; ///< the numbered ring's angle when Play is pressed
	double paintSpeedNow = 0.0; ///< ... and its angular velocity

	bool noShift    = false;///< negative control: never turn the ring (the physics' own pocket shows)
	bool noWarp     = false;
	bool biasedDraw = false;///< negative control: Random draws Below( N - 1 )
};

/// The physical state the simulation starts from, as `jptest` drives it.
struct Launch
{
	double ballSpeed  = 3.0;///< m/s along the lip, clockwise
	double ballAngle  = 0.0;///< rad
	double fretPhase  = 0.0;///< the rotor's angle in pockets, FRACTIONAL part only, at release
	int fretTurns     = 0;  ///< ... and its whole pockets (the physics must not see these)
	double rotorSpeed = 2.5;
	bool deflectors   = true;
	int pockets       = 37;
	double fretNudge  = 0.0;///< negative control: frets moved by this many pockets, fraction and all
};

struct Simulation
{
	std::vector< V3 > keys;     ///< ball centre at kKeyRate, from release
	double natural   = 0.0;     ///< s from release to rest
	bool settled     = false;
	int pocket       = 0;       ///< the pocket the ball rests in, in the rotor's frame (its pocket 0 at RotorPockets)
	double departSpeed = 0.0;   ///< m/s when the lip last pushed
	double departRadius = 0.0;  ///< m, the ball's centre then
	double departTime  = 0.0;
	int fretHits = 0, diamondHits = 0;
	double restAngleInRotor = 0.0;///< rad: the ball's angle relative to the rotor at rest
	double restRadius = 0.0, restHeight = 0.0;
	static constexpr double kKeyRate = 240.0;
};

/// The rotor's physical angle in pockets after `seconds` from release:
/// launch phase plus the turning. (Whole pockets included, for drawing.)
double RotorPockets( const Launch& launch, double seconds );

/// Simulate one release to rest (or to the time limit).
Simulation Simulate( const Launch& launch, const std::atomic< bool >* cancel = nullptr );

struct Plan
{
	Request request;
	Launch launch;
	Simulation sim;
	Playback playback;
	Outcome outcome;
	int pockets     = 37;
	int shift       = 0;    ///< k: the numbered ring is the frets turned by this many pockets
	int wanted      = 0;    ///< pocket index (in Numbers order) of the number shown
	double spinUp   = 1.0;  ///< sim seconds of the croupier's hand before release
	double paintFrom = 0.0, paintSpeedFrom = 0.0;///< the ring when Play was pressed
	double paintAtRelease = 0.0;///< ... at release (unwrapped)
	double paintOffset = 0.0;///< ring angle minus the frets' physical angle, rad
	int trials      = 0;
	double ms       = 0.0;
	bool idle       = false;

	/// The numbered ring's angle at playback seconds (and after: it keeps turning).
	double PaintAngle( double seconds ) const;
	double PaintSpeed( double seconds ) const;
	/// The ball's centre at playback seconds; false before release.
	bool Ball( double seconds, V3& at ) const;
};

/// The pocket (index in Numbers order) a play will show: Result read
/// against the bet, Random uniform over the pockets.
int Wanted( const Request& r );

Plan MakePlan( const Request& r, const std::atomic< bool >* cancel = nullptr );
/// No throw: the ring where it is, turning on, no ball.
Plan MakeRest( const Request& r );

} // namespace jackpot::roulette
