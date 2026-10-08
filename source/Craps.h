#pragma once

#include "Common.h"
#include "Controls.h"
#include "Geometry.h"
#include "Physics.h"

#include <array>
#include <atomic>
#include <string>
#include <vector>

/**
    The craps table: two dice thrown the length of the felt, off the rubber
    pyramids of the back wall, onto the layout.

    The dice are polyhedral's, engine and all (`Geometry`, `Physics`, copied
    from stoatworks-labs/polyhedral): rigid cubes, sequential impulses, split
    impulse, an energy guard. The planner here is new and smaller than
    polyhedral's `Roll`: the throw comes from the shooter's end, must reach the
    back wall (a throw that does not is thrown again, as the stickman would
    call it), and the back wall is a field of square rubber pyramids -- a plane
    whose contact normal tilts toward whichever pyramid face the corner of a
    die meets (`Plane::pyramidPitch`, added to the copied Physics).

    The one idea, polyhedral's: the dice are face-transitive, so each is drawn
    as R(t) * S, with S the symmetry carrying the wanted face to the one the
    physics left on top.

    ## The game

    ## Meeting Spin Time

    Dice settle in 0.6 to 1 s however hard they are thrown (polyhedral found the
    same), so the throw's speed is NOT chosen from the duration -- a throw hard
    enough to fill four seconds rebounds the length of the table. Every throw
    is a stickman's ordinary one, 1.9 to 2.5 m/s, and the duration is met by the
    shooter's hold after the sweep plus at most a gentle slow motion (0.6x):
    `hold` is the pause that makes the rest of the warp up.

    Come-out: 7 or 11 wins, 2, 3 or 12 is craps, anything else is the point
    (the puck goes ON). With a point: the point again wins, a 7 is a seven-out,
    anything else is no decision. Result reads against that state -- see README.

    World axes are polyhedral's: y up, the table at y = 0. The back wall stands
    at z = kBackWall; the shooter's end is toward +z.
*/
namespace jackpot::craps
{
constexpr double kBackWall  = -0.20;
constexpr double kNearWall  = 0.34;
constexpr double kSideWall  = 0.30;
constexpr double kPyramidPitch  = 0.020;
constexpr double kPyramidHeight = 0.006;

struct Pose
{
	V3 x;
	Quat q;
};

struct Request
{
	Result result    = Result::Random;
	int fixed        = 7;
	int point        = 0;   ///< 0: come-out
	uint32_t seed    = 0;
	uint32_t play    = 0;
	double duration  = 3.0;
	bool pyramids    = true;
	std::array< Pose, 2 > previous {};///< the dice from the last roll, swept away first
	bool havePrevious = false;

	bool identitySymmetry = false;
	bool noWarp           = false;
	double restitution    = -1.0;///< negative control (> 1); < 0 is the felt's own
};

/// The totals a roll will show, per die.
std::array< int, 2 > DrawDice( const Request& r );

/// What a total means with this point: +1 win, -1 lose, 0 no decision.
int Decide( int point, int total );
/// The point after a roll.
int PointAfter( int point, int total );

struct Track
{
	std::vector< Pose > keys;
	int result   = 1;
	int target   = 0;///< the face that must show
	int landed   = 0;///< the face the physics left on top
	M3 S;
	double settle = 0.0;
};

struct Plan
{
	Request request;
	std::array< Track, 2 > dice;
	std::array< int, 2 > values {};
	int total       = 7;
	int pointAfter  = 0;
	Playback playback;
	Outcome outcome;
	double sweep    = 0.35;///< sim seconds the stick sweeps the old dice away
	double hold     = 0.0; ///< sim seconds the shooter holds the dice after the sweep
	double natural  = 0.0; ///< dice seconds from release to rest
	int trials      = 0;
	int missedWall  = 0;   ///< throws rejected for falling short of the back wall
	int cocked      = 0;
	bool hitWall    = false;
	double speed    = 0.0;
	double ms       = 0.0;
	bool idle       = false;
	dice::physics::Stats stats;
	static constexpr double kKeyRate = 240.0;

	/// Die i's pose at playback seconds, with its symmetry applied: what is drawn.
	bool DieAt( int i, double seconds, V3& x, M3& r ) const;
	/// The previous roll's die i, being swept: false once it is gone.
	bool OldDieAt( int i, double seconds, V3& x, M3& r ) const;
};

Plan MakePlan( const Request& r, const std::atomic< bool >* cancel = nullptr );
Plan MakeRest( const Request& r );

/// The world a throw lands in: table, walls, the pyramids. For the checks.
dice::physics::World MakeWorld( const Request& r );

} // namespace jackpot::craps
