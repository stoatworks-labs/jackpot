#include "Physics.h"

#include <algorithm>
#include <cmath>

namespace dice::physics
{
namespace
{
constexpr double kMargin    = 1.0e-5;///< m: a probe this close counts as touching (resting contact)
constexpr double kWakeSpeed = 0.02;  ///< m/s: an approach this fast wakes a sleeping die

V3 AnyPerpendicular( V3 n )
{
	const V3 a = std::fabs( n.x ) < 0.6 ? V3 { 1, 0, 0 } : V3 { 0, 1, 0 };
	return Normalise( Cross( n, a ) );
}
} // namespace

double World::InverseMass( int body ) const
{
	if( body < 0 || bodies[ static_cast< size_t >( body ) ].asleep )
		return 0.0;
	return 1.0 / bodies[ static_cast< size_t >( body ) ].solid->mass;
}

M3 World::InverseInertia( int body ) const
{
	if( body < 0 || bodies[ static_cast< size_t >( body ) ].asleep )
		return M3::Zero();
	const M3& r = rotations[ static_cast< size_t >( body ) ];
	return r * bodies[ static_cast< size_t >( body ) ].solid->inverseInertia * Transpose( r );
}

V3 World::VelocityAt( int body, V3 r, bool pseudo ) const
{
	if( body < 0 )
		return {};
	const size_t i = static_cast< size_t >( body );
	if( pseudo )
		return pv[ i ] + Cross( pw[ i ], r );
	return bodies[ i ].v + Cross( bodies[ i ].w, r );
}

void World::ApplyImpulse( int body, V3 impulse, V3 r, bool pseudo )
{
	if( body < 0 || bodies[ static_cast< size_t >( body ) ].asleep )
		return;
	const size_t i     = static_cast< size_t >( body );
	const double im    = 1.0 / bodies[ i ].solid->mass;
	const V3 dw        = InverseInertia( body ) * Cross( r, impulse );
	if( pseudo )
	{
		pv[ i ] += impulse * im;
		pw[ i ] += dw;
	}
	else
	{
		bodies[ i ].v += impulse * im;
		bodies[ i ].w += dw;
	}
}

void World::Collect()
{
	contacts.clear();
	const size_t n = bodies.size();

	//-------------------------------------------------------------------
	// Body against the table and the walls: the deepest point of a convex
	// body against a plane is always a vertex, so vertices are enough.
	//-------------------------------------------------------------------
	for( size_t i = 0; i < n; ++i )
	{
		const Body& body = bodies[ i ];
		if( body.asleep )
			continue;
		const M3& r = rotations[ i ];
		for( const Plane& plane : planes )
		{
			//Cheap reject: the whole die is well clear of this plane.
			if( Dot( plane.normal, body.x ) - plane.offset > body.solid->circumradius + plane.pyramidHeight + kMargin )
				continue;
			for( const V3& vertex : body.solid->vertices )
			{
				const V3 p = body.x + r * vertex;
				double gap = Dot( plane.normal, p ) - plane.offset;
				V3 normal  = plane.normal;
				if( plane.pyramidPitch > 0.0 )
				{
					//The pyramid face under the vertex: height falls linearly from
					//the apex to the cell's edge; its normal leans toward the edge.
					const double a  = Dot( p, plane.u ) / plane.pyramidPitch;
					const double b  = Dot( p, plane.w ) / plane.pyramidPitch;
					const double fa = a - std::floor( a + 0.5 ), fb = b - std::floor( b + 0.5 );
					const double m  = std::max( std::fabs( fa ), std::fabs( fb ) );
					const double h  = plane.pyramidHeight * ( 1.0 - 2.0 * m );
					const double s  = 2.0 * plane.pyramidHeight / plane.pyramidPitch;
					normal          = Normalise( plane.normal + ( std::fabs( fa ) > std::fabs( fb ) ? plane.u * ( s * ( fa > 0 ? 1.0 : -1.0 ) )
					                                                                               : plane.w * ( s * ( fb > 0 ? 1.0 : -1.0 ) ) ) );
					gap             = ( gap - h ) * Dot( normal, plane.normal );
				}
				if( gap >= kMargin )
					continue;
				Contact c;
				c.a        = static_cast< int >( i );
				c.point    = p;
				c.normal   = normal;
				c.depth    = gap;
				c.friction = plane.friction;
				c.restitution = plane.restitution;
				c.table    = plane.table;
				contacts.push_back( c );
			}
		}
	}

	//-------------------------------------------------------------------
	// Die against die: each body's probes (vertices, edge midpoints) against
	// the other's planes, both ways round, and the insphere backstop.
	//-------------------------------------------------------------------
	for( size_t i = 0; i < n; ++i )
		for( size_t j = i + 1; j < n; ++j )
		{
			const Body& a = bodies[ i ];
			const Body& b = bodies[ j ];
			if( a.asleep && b.asleep )
				continue;
			const V3 between = a.x - b.x;
			const double d   = Length( between );
			if( d > a.solid->circumradius + b.solid->circumradius + kMargin )
				continue;

			for( int pass = 0; pass < 2; ++pass )
			{
				const size_t pi    = pass == 0 ? i : j;//whose probes
				const size_t qi    = pass == 0 ? j : i;//whose planes
				const Body& probe  = bodies[ pi ];
				const Body& shape  = bodies[ qi ];
				const M3& rp       = rotations[ pi ];
				const M3& rq       = rotations[ qi ];
				const M3 rqT       = Transpose( rq );
				for( const V3& local : probe.solid->vertices )
				{
					const V3 world = probe.x + rp * local;
					const V3 inQ   = rqT * ( world - shape.x );
					if( Length( inQ ) > shape.solid->circumradius + kMargin )
						continue;
					double worst = -1e30;
					int face     = 0;
					for( size_t k = 0; k < shape.solid->faces.size(); ++k )
					{
						const double s = Dot( shape.solid->faces[ k ].normal, inQ ) - shape.solid->faces[ k ].offset;
						if( s > worst )
						{
							worst = s;
							face  = static_cast< int >( k );
						}
					}
					if( worst >= kMargin )
						continue;
					Contact c;
					c.a        = static_cast< int >( pi );
					c.b        = static_cast< int >( qi );
					c.point    = world;
					c.normal   = rq * shape.solid->faces[ static_cast< size_t >( face ) ].normal;
					c.depth    = worst;
					c.friction = settings.dieFriction;
					c.restitution = settings.restitution;
					contacts.push_back( c );
				}
			}

			//Edge across edge: two dice meeting edge to edge, neither's vertex
			//inside the other. For each pair of edges, the closest points; if
			//each lies inside the other die, the edges have passed through each
			//other and the separation along their common normal is the depth.
			{
				const M3& ra = rotations[ i ];
				const M3& rb = rotations[ j ];
				auto inside = [ & ]( const Body& body, const M3& r, V3 world ) {
					const V3 local = Transpose( r ) * ( world - body.x );
					double worst   = -1e30;
					for( const geo::Face& f : body.solid->faces )
						worst = std::max( worst, Dot( f.normal, local ) - f.offset );
					return worst;
				};
				for( const auto& ea : a.solid->edges )
				{
					const V3 a0 = a.x + ra * a.solid->vertices[ static_cast< size_t >( ea[ 0 ] ) ];
					const V3 a1 = a.x + ra * a.solid->vertices[ static_cast< size_t >( ea[ 1 ] ) ];
					if( Length( ( a0 + a1 ) * 0.5 - b.x ) > b.solid->circumradius + 0.5 * Length( a1 - a0 ) )
						continue;
					for( const auto& eb : b.solid->edges )
					{
						const V3 b0 = b.x + rb * b.solid->vertices[ static_cast< size_t >( eb[ 0 ] ) ];
						const V3 b1 = b.x + rb * b.solid->vertices[ static_cast< size_t >( eb[ 1 ] ) ];
						const V3 da = a1 - a0, db = b1 - b0, w0 = a0 - b0;
						const double aa = Dot( da, da ), bb = Dot( db, db ), ab = Dot( da, db );
						const double d1 = Dot( da, w0 ), d2 = Dot( db, w0 );
						const double den = aa * bb - ab * ab;
						if( den < 1e-18 )
							continue;//parallel: a face or a vertex contact covers it
						const double s = std::clamp( ( ab * d2 - bb * d1 ) / den, 0.0, 1.0 );
						const double u = std::clamp( ( aa * d2 - ab * d1 ) / den, 0.0, 1.0 );
						if( s <= 0.0 || s >= 1.0 || u <= 0.0 || u >= 1.0 )
							continue;//an end: a vertex, which the probes handle
						const V3 pa = a0 + da * s, pb = b0 + db * u;
						if( inside( b, rb, pa ) >= kMargin || inside( a, ra, pb ) >= kMargin )
							continue;
						V3 n = Normalise( Cross( da, db ) );
						if( Dot( n, a.x - b.x ) < 0.0 )
							n = -n;
						Contact c;
						c.a           = static_cast< int >( i );
						c.b           = static_cast< int >( j );
						c.point       = ( pa + pb ) * 0.5;
						c.normal      = n;
						c.depth       = std::min( Dot( pa - pb, n ), 0.0 );
						c.friction    = settings.dieFriction;
						c.restitution = settings.restitution;
						contacts.push_back( c );
					}
				}
			}

			//The inspheres overlap only when the dice really do: a backstop
			//for anything deeper than one step's travel.
			const double inner = a.solid->inradius + b.solid->inradius;
			if( d < inner && d > 1e-12 )
			{
				Contact c;
				c.a        = static_cast< int >( i );
				c.b        = static_cast< int >( j );
				c.normal   = between * ( 1.0 / d );
				c.point    = b.x + c.normal * b.solid->inradius;
				c.depth    = d - inner;
				c.friction = settings.dieFriction;
				c.restitution = settings.restitution;
				contacts.push_back( c );
			}
		}

	stats.contacts += static_cast< long >( contacts.size() );
}

void World::Prepare( bool elastic )
{
	for( Contact& c : contacts )
	{
		const V3 xa = bodies[ static_cast< size_t >( c.a ) ].x;
		c.ra        = c.point - xa;
		c.rb        = c.b >= 0 ? c.point - bodies[ static_cast< size_t >( c.b ) ].x : V3 {};

		const double ima = InverseMass( c.a ), imb = InverseMass( c.b );
		const M3 iia = InverseInertia( c.a ), iib = InverseInertia( c.b );
		auto effective = [ & ]( V3 dir ) {
			double k = ima + imb;
			k += Dot( dir, Cross( iia * Cross( c.ra, dir ), c.ra ) );
			if( c.b >= 0 )
				k += Dot( dir, Cross( iib * Cross( c.rb, dir ), c.rb ) );
			return k > 0.0 ? 1.0 / k : 0.0;
		};

		const V3 rel     = VelocityAt( c.a, c.ra, false ) - VelocityAt( c.b, c.rb, false );
		const double vn0 = Dot( rel, c.normal );
		const V3 slide   = rel - c.normal * vn0;
		c.t1             = Length( slide ) > 1e-9 ? Normalise( slide ) : AnyPerpendicular( c.normal );
		c.t2             = Cross( c.normal, c.t1 );

		c.normalMass   = effective( c.normal );
		c.tangentMass1 = effective( c.t1 );
		c.tangentMass2 = effective( c.t2 );
		c.target       = ( elastic && vn0 < -settings.bounceSpeed ) ? -c.restitution * vn0 : 0.0;
		c.jn = c.jt1 = c.jt2 = c.jp = 0.0;
	}
}

void World::SolveVelocities()
{
	for( int iteration = 0; iteration < settings.iterations; ++iteration )
		for( Contact& c : contacts )
		{
			V3 rel        = VelocityAt( c.a, c.ra, false ) - VelocityAt( c.b, c.rb, false );
			double lambda = c.normalMass * ( c.target - Dot( rel, c.normal ) );
			const double jn = std::max( c.jn + lambda, 0.0 );
			lambda          = jn - c.jn;
			c.jn            = jn;
			ApplyImpulse( c.a, c.normal * lambda, c.ra, false );
			ApplyImpulse( c.b, c.normal * -lambda, c.rb, false );

			const double limit = c.friction * c.jn;
			for( int axis = 0; axis < 2; ++axis )
			{
				const V3 t       = axis == 0 ? c.t1 : c.t2;
				double& acc      = axis == 0 ? c.jt1 : c.jt2;
				const double m   = axis == 0 ? c.tangentMass1 : c.tangentMass2;
				rel              = VelocityAt( c.a, c.ra, false ) - VelocityAt( c.b, c.rb, false );
				double dj        = -m * Dot( rel, t );
				const double nj  = std::clamp( acc + dj, -limit, limit );
				dj               = nj - acc;
				acc              = nj;
				ApplyImpulse( c.a, t * dj, c.ra, false );
				ApplyImpulse( c.b, t * -dj, c.rb, false );
			}
		}
}

void World::SolvePositions()
{
	const size_t n = bodies.size();
	pv.assign( n, V3 {} );
	pw.assign( n, V3 {} );
	for( int iteration = 0; iteration < settings.iterations / 2; ++iteration )
		for( Contact& c : contacts )
		{
			const double push = std::max( -c.depth - settings.slop, 0.0 );
			if( push <= 0.0 )
				continue;
			const double bias = settings.correction * push / settings.dt;
			const V3 rel      = VelocityAt( c.a, c.ra, true ) - VelocityAt( c.b, c.rb, true );
			double lambda     = c.normalMass * ( bias - Dot( rel, c.normal ) );
			const double jp   = std::max( c.jp + lambda, 0.0 );
			lambda            = jp - c.jp;
			c.jp              = jp;
			ApplyImpulse( c.a, c.normal * lambda, c.ra, true );
			ApplyImpulse( c.b, c.normal * -lambda, c.rb, true );
		}
}

void World::Step()
{
	const double dt = settings.dt;
	const size_t n  = bodies.size();
	++stats.steps;

	rotations.resize( n );
	for( size_t i = 0; i < n; ++i )
		rotations[ i ] = bodies[ i ].Rotation();

	//-------------------------------------------------------------------
	// 1. Gravity, and the gyroscopic term (zero but for the d10).
	//-------------------------------------------------------------------
	for( size_t i = 0; i < n; ++i )
	{
		Body& b = bodies[ i ];
		if( b.asleep )
			continue;
		b.v.y -= settings.gravity * dt;
		const M3& r      = rotations[ i ];
		const M3 inertia = r * b.solid->inertia * Transpose( r );
		b.w -= InverseInertia( static_cast< int >( i ) ) * Cross( b.w, inertia * b.w ) * dt;
	}

	//-------------------------------------------------------------------
	// 2. Contacts; a die hit hard enough wakes.
	//-------------------------------------------------------------------
	Collect();
	for( const Contact& c : contacts )
	{
		if( c.b < 0 )
			continue;
		Body& a = bodies[ static_cast< size_t >( c.a ) ];
		Body& b = bodies[ static_cast< size_t >( c.b ) ];
		if( a.asleep == b.asleep )
			continue;
		const V3 ra      = c.point - a.x;
		const V3 rb      = c.point - b.x;
		const double vn  = Dot( ( a.v + Cross( a.w, ra ) ) - ( b.v + Cross( b.w, rb ) ), c.normal );
		if( vn < -kWakeSpeed )
		{
			a.asleep = b.asleep = false;
			a.still = b.still = 0.0;
		}
	}

	//-------------------------------------------------------------------
	// 3. The velocity solve, guarded: it may not create kinetic energy.
	//-------------------------------------------------------------------
	std::vector< V3 > savedV( n ), savedW( n );
	for( size_t i = 0; i < n; ++i )
	{
		savedV[ i ] = bodies[ i ].v;
		savedW[ i ] = bodies[ i ].w;
	}
	const double before = KineticEnergy();
	Prepare( true );
	SolveVelocities();
	double after = KineticEnergy();
	if( after > before )
	{
		if( settings.energyGuard && after > before * ( 1.0 + 1e-12 ) + 1e-18 )
		{
			stats.worstRejectedGain = std::max( stats.worstRejectedGain, after - before );
			for( size_t i = 0; i < n; ++i )
			{
				bodies[ i ].v = savedV[ i ];
				bodies[ i ].w = savedW[ i ];
			}
			Prepare( false );
			SolveVelocities();
			after = KineticEnergy();
			++stats.inelasticRetries;
		}
	}
	stats.lastSolveBefore = before;
	stats.lastSolveAfter  = after;
	if( after > before )
		stats.worstKeptGain = std::max( stats.worstKeptGain, ( after - before ) / std::max( before, 1e-30 ) );

	//-------------------------------------------------------------------
	// 4. Penetration out by pseudo-velocity.
	//-------------------------------------------------------------------
	SolvePositions();

	//-------------------------------------------------------------------
	// 5. Integrate.
	//-------------------------------------------------------------------
	touching.assign( n, false );
	std::vector< bool > onTable( n, false );
	for( const Contact& c : contacts )
	{
		touching[ static_cast< size_t >( c.a ) ] = true;
		if( c.b >= 0 )
			touching[ static_cast< size_t >( c.b ) ] = true;
		if( c.table )
			onTable[ static_cast< size_t >( c.a ) ] = true;
	}
	for( size_t i = 0; i < n; ++i )
	{
		Body& b = bodies[ i ];
		if( b.asleep )
			continue;
		//Free flight: v was advanced by g dt already, and x += v dt + g dt^2 / 2
		//is the exact parabola. A body in contact had gravity answered by the
		//solve, and moves by what is left.
		V3 move = b.v;
		if( !touching[ i ] )
			move.y += 0.5 * settings.gravity * dt;
		b.x += ( move + pv[ i ] ) * dt;

		const V3 spin = b.w + pw[ i ];
		const Quat dq = Quat { 0.0, spin.x, spin.y, spin.z } * b.q;
		b.q           = Normalise( Quat { b.q.w + 0.5 * dt * dq.w, b.q.x + 0.5 * dt * dq.x, b.q.y + 0.5 * dt * dq.y,
		                                  b.q.z + 0.5 * dt * dq.z } );

		//Felt and polyhedral rolling lose energy that a frictional point
		//contact does not model; this is that, and only while on the table.
		if( onTable[ i ] )
			b.w = b.w * ( 1.0 / ( 1.0 + settings.rollDamping * dt ) );

		//-------------------------------------------------------------------
		// Sleep.
		//-------------------------------------------------------------------
		if( touching[ i ] && Length( b.v ) < settings.sleepSpeed && Length( b.w ) < settings.sleepSpin )
			b.still += dt;
		else
			b.still = 0.0;
		if( b.still >= settings.sleepTime )
		{
			b.asleep = true;
			b.v      = {};
			b.w      = {};
		}
	}
}

double World::KineticEnergy() const
{
	double e = 0.0;
	for( size_t i = 0; i < bodies.size(); ++i )
	{
		const Body& b = bodies[ i ];
		if( b.asleep )
			continue;
		const M3 r = rotations.size() == bodies.size() ? rotations[ i ] : b.Rotation();
		const M3 inertia = r * b.solid->inertia * Transpose( r );
		e += 0.5 * b.solid->mass * Dot( b.v, b.v ) + 0.5 * Dot( b.w, inertia * b.w );
	}
	return e;
}

double World::MechanicalEnergy() const
{
	double e = KineticEnergy();
	for( const Body& b : bodies )
		e += b.solid->mass * settings.gravity * b.x.y;
	return e;
}

bool World::AllAsleep() const
{
	for( const Body& b : bodies )
		if( !b.asleep )
			return false;
	return true;
}

} // namespace dice::physics
