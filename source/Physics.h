#pragma once

#include "Geometry.h"
#include "Maths.h"

#include <vector>

/**
    Rigid dice on a table: gravity, contacts, friction, restitution, sleep.

    Small and exact rather than general. Every body is a convex polyhedron with
    a known plane set, so a contact is either a vertex of one body that has
    crossed a plane of the other -- the table, a wall, a face of another die --
    or two dice's edges that have passed through each other (each edge's
    closest point inside the other die), pushed apart along the edges' common
    normal. Vertices and edges are the whole of convex-polyhedron contact; an
    insphere test backs both up for anything deeper than one step's travel.

    ## The step (960 Hz, four steps per keyframe at 240 Hz)

    1. Gravity into the velocities; the gyroscopic term for the d10, the one
       solid whose inertia is not isotropic.
    2. Contacts from the current positions.
    3. Sequential impulses: normal impulses accumulated and clamped >= 0, the
       restitution target taken from the approach speed before the solve,
       Coulomb friction clamped to mu times the normal impulse.
    4. **Split impulse**: penetration is pushed out with pseudo-velocities that
       move the positions and are then thrown away. Baumgarte's way (a bias in
       the real velocity) pushes the die out by giving it kinetic energy, which
       is a bounce nobody threw.
    5. Integration. A body touching nothing moves by v dt + g dt^2 / 2, which is
       exact for a parabola; `ditest --physics` holds free flight to it.

    **Contact impulses never add kinetic energy.** Per-point restitution at
    several simultaneous contacts can, in some orientations; when a solve leaves
    more kinetic energy than it found, it is solved again inelastically. The
    world counts how often (`Stats::inelasticRetries`).
*/
namespace dice::physics
{
/// A static half-space boundary: the allowed side is dot( normal, p ) >= offset.
struct Plane
{
	V3 normal;
	double offset   = 0.0;
	double friction = 0.4;
	double restitution = 0.4;
	bool table      = false;///< the floor, as opposed to a wall
	/// Rubber pyramids on the plane (a craps table's back wall): square
	/// pyramids `pyramidPitch` across and `pyramidHeight` tall, in the plane's
	/// axes u and w. Zero pitch is a flat plane. (Jackpot's addition.)
	double pyramidPitch  = 0.0;
	double pyramidHeight = 0.0;
	V3 u, w;
};

struct Body
{
	const geo::Solid* solid = nullptr;
	V3 x;       ///< centre of mass, world
	V3 v;       ///< m/s
	V3 w;       ///< rad/s, world
	Quat q;     ///< body to world
	bool asleep = false;
	double still = 0.0;///< seconds below the sleep thresholds

	M3 Rotation() const
	{
		return ToMatrix( q );
	}
};

struct Settings
{
	double dt           = 1.0 / 960.0;
	double gravity      = 9.81;
	double restitution  = 0.5; ///< die on die (each plane has its own)
	double tableFriction = 0.3;
	double wallFriction = 0.2;
	double wallRestitution = 0.8;///< the walls are the frame's edges, not a tray: lively
	double dieFriction  = 0.30;
	int iterations      = 16;
	double rollDamping  = 0.6;   ///< per second, angular, while touching the table: felt and edges
	double bounceSpeed  = 0.04;  ///< m/s: slower approaches do not bounce (resting contact)
	double sleepSpeed   = 0.004; ///< m/s
	double sleepSpin    = 0.35;  ///< rad/s
	double sleepTime    = 0.12;  ///< s
	double correction   = 0.3;   ///< fraction of the penetration removed per step
	double slop         = 2.0e-5;///< m of penetration left alone (keeps resting contacts warm)
	bool energyGuard    = true;  ///< re-solve inelastically if a solve gained kinetic energy
};

struct Stats
{
	long steps              = 0;
	long contacts           = 0;
	long inelasticRetries   = 0;
	double worstRejectedGain = 0.0;///< J: the most kinetic energy an elastic solve tried to add (and was redone)
	double worstKeptGain     = 0.0;///< relative: the most a KEPT solve added, (after - before) / before
	double lastSolveBefore   = 0.0;
	double lastSolveAfter    = 0.0;
};

class World
{
public:
	Settings settings;
	std::vector< Plane > planes;
	std::vector< Body > bodies;
	Stats stats;

	void Step();

	double KineticEnergy() const;
	/// Kinetic plus gravitational potential (zero at the table), J.
	double MechanicalEnergy() const;
	bool AllAsleep() const;
	bool Touching( size_t body ) const
	{
		return body < touching.size() && touching[ body ];
	}

private:
	struct Contact
	{
		int a = -1, b = -1;///< b = -1: a static plane
		V3 point;
		V3 normal;         ///< from b toward a
		double depth = 0.0;///< negative: penetrating
		double friction = 0.4;
		double restitution = 0.4;
		V3 ra, rb;
		V3 t1, t2;
		double normalMass = 0.0, tangentMass1 = 0.0, tangentMass2 = 0.0;
		double target = 0.0;///< the normal speed the solve aims for
		double jn = 0.0, jt1 = 0.0, jt2 = 0.0, jp = 0.0;
		bool table = false;
	};

	void Collect();
	void Prepare( bool elastic );
	void SolveVelocities();
	void SolvePositions();

	double InverseMass( int body ) const;
	M3 InverseInertia( int body ) const;
	V3 VelocityAt( int body, V3 r, bool pseudo ) const;
	void ApplyImpulse( int body, V3 impulse, V3 r, bool pseudo );

	std::vector< Contact > contacts;
	std::vector< M3 > rotations;
	std::vector< V3 > pv, pw;///< split-impulse pseudo-velocities
	std::vector< bool > touching;
};

} // namespace dice::physics
