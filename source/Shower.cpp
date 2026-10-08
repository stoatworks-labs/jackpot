#include "Shower.h"

#include <algorithm>
#include <cmath>

namespace jackpot
{
namespace
{
constexpr double kGravity    = 9.81;
constexpr double kFriction   = 0.40;
constexpr double kChipBounce = 0.35, kCoinBounce = 0.45;
constexpr double kSpinDamp   = 0.15;   ///< 1/s while touching: the felt
constexpr double kSettleSpeed = 0.012; ///< m/s
constexpr double kSettleSpin  = 2.0;   ///< rad/s
constexpr double kSettleFor   = 0.25;  ///< s

/// Chip denominations' share of a shower: white, red, green, black, purple, orange.
const int kColourWeights[ 6 ] = { 3, 6, 5, 3, 2, 1 };

double Smooth( double x )
{
	x = std::clamp( x, 0.0, 1.0 );
	return x * x * ( 3.0 - 2.0 * x );
}

Quat RandomRotation( Stream& s )
{
	const double u1 = s.Uniform(), u2 = s.Uniform(), u3 = s.Uniform();
	const double a = std::sqrt( 1.0 - u1 ), b = std::sqrt( u1 );
	return dice::Normalise( Quat { a * std::sin( 2 * kPi * u2 ), a * std::cos( 2 * kPi * u2 ), b * std::sin( 2 * kPi * u3 ), b * std::cos( 2 * kPi * u3 ) } );
}

/// Per unit mass: the disc's body-frame principal moments (or, as a negative
/// control, a ball's).
void Moments( const Piece& p, double& axial, double& across, bool ball = false )
{
	const double h = 2.0 * p.half;
	axial          = ball ? 0.4 * p.radius * p.radius : 0.5 * p.radius * p.radius;
	across         = ball ? axial : 0.25 * p.radius * p.radius + h * h / 12.0;
}

M3 InverseInertiaWorld( const Piece& p, const M3& r, bool ball )
{
	double axial = 0.0, across = 0.0;
	Moments( p, axial, across, ball );
	M3 inv;
	inv.m[ 0 ][ 0 ] = 1.0 / across;
	inv.m[ 1 ][ 1 ] = 1.0 / across;
	inv.m[ 2 ][ 2 ] = 1.0 / axial;
	return r * inv * Transpose( r );
}

double Kinetic( const Piece& p, const M3& r, bool ball )
{
	double axial = 0.0, across = 0.0;
	Moments( p, axial, across, ball );
	const V3 wb = Transpose( r ) * p.w;
	return 0.5 * Dot( p.v, p.v ) + 0.5 * ( across * ( wb.x * wb.x + wb.y * wb.y ) + axial * wb.z * wb.z );
}
} // namespace

double Shower::MetresPerUnit( const Piece& ) const
{
	return kChipMetres / std::max( 1e-4, settings.size );
}

double Shower::FloorAt( double x, double z ) const
{
	if( !settings.pile )
		return -1e9;
	if( heights.empty() )
		return kFloor;
	const int ix = static_cast< int >( std::floor( ( x + kGridHalfX ) / ( 2.0 * kGridHalfX ) * kGrid ) );
	const int iz = static_cast< int >( std::floor( ( z + kGridHalfZ ) / ( 2.0 * kGridHalfZ ) * kGrid ) );
	if( ix < 0 || iz < 0 || ix >= kGrid || iz >= kGrid )
		return kFloor;
	return kFloor + heights[ static_cast< size_t >( iz * kGrid + ix ) ];
}

void Shower::Clear()
{
	pieces.clear();
	heights.assign( static_cast< size_t >( kGrid * kGrid ), 0.0f );
	remaining = owed = carry = 0.0;
}

void Shower::Start( double seconds, double scale )
{
	remaining = std::max( remaining, seconds );
	rateScale = scale;
}

void Shower::Stop()
{
	remaining = 0.0;
}

Piece Shower::Spawn()
{
	Stream s( Hash( seed, static_cast< uint32_t >( spawned ), 0x5b0eu ) );
	++spawned;
	Piece p;
	const bool coin = settings.pieces == Pieces::Coins || ( settings.pieces == Pieces::Mixed && s.Below( 2 ) == 0 );
	p.kind          = coin ? PieceKind::Coin : PieceKind::Chip;
	p.radius        = 0.5 * settings.size * ( coin ? 0.615 : 1.0 ) * s.Range( 0.97, 1.03 );
	p.half          = p.radius * ( coin ? 0.075 : 0.085 );
	p.colour        = s.Weighted( kColourWeights );
	p.q             = RandomRotation( s );
	const double g  = kGravity / MetresPerUnit( p );
	const double a  = settings.aspect;
	const V3 axis   = dice::Normalise( V3 { s.Range( -1, 1 ), s.Range( -1, 1 ), s.Range( -1, 1 ) } );
	switch( settings.pattern )
	{
	case Pattern::Fountain:
	{
		const double up   = std::sqrt( 2.0 * g * s.Range( 0.55, 0.95 ) );
		const double lean = s.Range( -0.38, 0.38 );
		p.x               = { s.Range( -0.02, 0.02 ), -0.52, s.Range( -0.03, 0.03 ) };
		p.v               = { up * std::sin( lean ), up * std::cos( lean ), s.Range( -0.15, 0.15 ) * up };
		p.w               = axis * s.Range( 8.0, 30.0 );
		break;
	}
	case Pattern::Burst:
	{
		const V3 dir = dice::Normalise( V3 { s.Range( -1, 1 ), s.Range( -0.4, 1 ), s.Range( -1, 1 ) } );
		const double speed = std::sqrt( g * 0.5 ) * s.Range( 0.7, 1.4 );
		p.x = { s.Range( -0.02, 0.02 ), 0.05 + s.Range( -0.02, 0.02 ), 0.0 };
		p.v = dir * speed;
		p.w = axis * s.Range( 10.0, 40.0 );
		break;
	}
	case Pattern::Pour:
	{
		const double fall = std::sqrt( g * 0.05 );
		p.x = { 0.18 * a + s.Range( -0.015, 0.015 ), 0.56 + p.radius, s.Range( -0.01, 0.01 ) };
		p.v = { s.Range( -0.06, 0.06 ) * fall, -fall, s.Range( -0.06, 0.06 ) * fall };
		p.w = axis * s.Range( 2.0, 10.0 );
		break;
	}
	case Pattern::Rain:
	default:
		p.x = { s.Range( -0.55, 0.55 ) * a, 0.56 + 2.0 * p.radius + s.Range( 0.0, 0.08 ), s.Range( -0.3, 0.3 ) };
		p.v = { s.Range( -0.1, 0.1 ), -s.Range( 0.0, 0.4 ) * std::sqrt( g * 0.1 ), s.Range( -0.05, 0.05 ) };
		p.w = axis * s.Range( 5.0, 25.0 );
		break;
	}
	return p;
}

void Shower::Settle( Piece& p )
{
	//Laid flat, turned about the vertical as it lay, onto the pile.
	const M3 r     = ToMatrix( p.q );
	const V3 n     = r * V3 { 0, 0, 1 };
	const V3 up    = { 0, n.y >= 0.0 ? 1.0 : -1.0, 0 };
	worstSnap      = std::max( worstSnap, std::acos( std::clamp( std::fabs( n.y ), 0.0, 1.0 ) ) * 180.0 / kPi );
	const M3 flat  = dice::Align( n, up ) * r;
	p.q            = dice::FromMatrix( flat );
	p.x.y          = FloorAt( p.x.x, p.x.z ) + p.half;
	p.v = p.w      = {};
	p.resting      = true;
	if( heights.empty() )
		heights.assign( static_cast< size_t >( kGrid * kGrid ), 0.0f );
	const double top = p.x.y + p.half - kFloor;
	for( int iz = 0; iz < kGrid; ++iz )
		for( int ix = 0; ix < kGrid; ++ix )
		{
			const double cx = -kGridHalfX + ( ix + 0.5 ) * 2.0 * kGridHalfX / kGrid;
			const double cz = -kGridHalfZ + ( iz + 0.5 ) * 2.0 * kGridHalfZ / kGrid;
			if( ( cx - p.x.x ) * ( cx - p.x.x ) + ( cz - p.x.z ) * ( cz - p.x.z ) <= p.radius * p.radius * 0.8 )
			{
				float& h = heights[ static_cast< size_t >( iz * kGrid + ix ) ];
				h        = std::max( h, static_cast< float >( top ) );
			}
		}
}

void Shower::StepPiece( Piece& p )
{
	if( p.resting )
		return;
	const double dt  = kDt;
	const double mpu = MetresPerUnit( p );
	const double g   = kGravity / mpu;

	//Air: quadratic drag, heavier face-on than edge-on.
	M3 r              = ToMatrix( p.q );
	const V3 n        = r * V3 { 0, 0, 1 };
	const double speed = Length( p.v );
	if( !settings.noDrag && speed > 1e-9 )
	{
		const double face = std::fabs( Dot( n, p.v ) ) / speed;
		const double kFace = p.kind == PieceKind::Coin ? kCoinFace : kChipFace;
		const double k     = ( kEdge + ( kFace - kEdge ) * face ) * mpu;
		p.v -= p.v * ( std::min( 0.5, k * speed * dt ) );
	}
	p.v.y -= g * dt;

	//Torque-free rotation with the gyroscopic term, in the body frame.
	double axial = 0.0, across = 0.0;
	Moments( p, axial, across, settings.ballInertia );
	V3 wb        = Transpose( r ) * p.w;
	const V3 L   = { across * wb.x, across * wb.y, axial * wb.z };
	const V3 gyro = settings.noGyro ? V3 {} : Cross( wb, L );
	wb           = wb - V3 { gyro.x / across, gyro.y / across, gyro.z / axial } * dt;
	p.w          = r * wb;

	p.x += p.v * dt;
	const Quat dq = { 0.0, p.w.x, p.w.y, p.w.z };
	const Quat d  = dq * p.q;
	p.q           = dice::Normalise( Quat { p.q.w + 0.5 * dt * d.w, p.q.x + 0.5 * dt * d.x, p.q.y + 0.5 * dt * d.y, p.q.z + 0.5 * dt * d.z } );
	r             = ToMatrix( p.q );

	if( !settings.pile )
		return;

	//Contacts: the lowest point of each rim, and the lower face's rim at
	//three points when nearly flat (a disc lying down rests on a circle).
	const V3 axis  = r * V3 { 0, 0, 1 };
	V3 points[ 5 ];
	int count      = 0;
	const V3 upY   = { 0, 1, 0 };
	V3 down        = upY * -1.0 - axis * Dot( upY * -1.0, axis );
	const double dl = Length( down );
	const double tilt = std::fabs( Dot( axis, upY ) );
	if( dl > 1e-6 )
	{
		down = down * ( 1.0 / dl );
		points[ count++ ] = p.x + down * p.radius + axis * p.half;
		points[ count++ ] = p.x + down * p.radius - axis * p.half;
	}
	if( tilt > 0.9 )
	{
		const V3 e1   = r * V3 { 1, 0, 0 }, e2 = r * V3 { 0, 1, 0 };
		const double s = Dot( axis, upY ) > 0.0 ? -1.0 : 1.0;
		for( int k = 0; k < 3; ++k )
		{
			const double a = 2.0 * kPi * k / 3.0 + 0.3;
			points[ count++ ] = p.x + ( e1 * std::cos( a ) + e2 * std::sin( a ) ) * p.radius + axis * ( s * p.half );
		}
	}

	struct C
	{
		V3 r;
		double depth, target, mass, jn = 0.0;
		V3 t1, t2;
		double m1, m2, j1 = 0.0, j2 = 0.0;
	};
	C contacts[ 5 ];
	int used         = 0;
	const M3 invI    = InverseInertiaWorld( p, r, settings.ballInertia );
	const double bounce = settings.restitution >= 0.0 ? settings.restitution : ( p.kind == PieceKind::Coin ? kCoinBounce : kChipBounce );
	for( int i = 0; i < count; ++i )
	{
		const double floor = FloorAt( points[ i ].x, points[ i ].z );
		const double depth = floor - points[ i ].y;
		if( depth <= 0.0 )
			continue;
		C& c         = contacts[ used++ ];
		c.r          = points[ i ] - p.x;
		c.depth      = depth;
		const V3 rn  = Cross( c.r, upY );
		c.mass       = 1.0 / ( 1.0 + Dot( rn, invI * rn ) );
		const V3 vp  = p.v + Cross( p.w, c.r );
		const double vn = vp.y;
		c.target     = ( -vn > 0.05 / mpu * 3.0 ) ? -bounce * vn : 0.0;
		c.t1         = { 1, 0, 0 };
		c.t2         = { 0, 0, 1 };
		const V3 r1 = Cross( c.r, c.t1 ), r2 = Cross( c.r, c.t2 );
		c.m1 = 1.0 / ( 1.0 + Dot( r1, invI * r1 ) );
		c.m2 = 1.0 / ( 1.0 + Dot( r2, invI * r2 ) );
	}
	if( used == 0 )
	{
		p.still = 0.0;
		return;
	}
	++contactSteps;
	const double before = Kinetic( p, r, settings.ballInertia );
	for( int it = 0; it < 6; ++it )
		for( int i = 0; i < used; ++i )
		{
			C& c         = contacts[ i ];
			const V3 vp  = p.v + Cross( p.w, c.r );
			const double dj = c.mass * ( c.target - vp.y );
			const double jn = std::max( 0.0, c.jn + dj );
			const double applied = jn - c.jn;
			c.jn         = jn;
			p.v += upY * applied;
			p.w += invI * Cross( c.r, upY * applied );
			//Friction, clamped to the cone, each tangent in turn.
			for( int axisIndex = 0; axisIndex < 2; ++axisIndex )
			{
				const V3 t     = axisIndex == 0 ? c.t1 : c.t2;
				double& acc    = axisIndex == 0 ? c.j1 : c.j2;
				const double m = axisIndex == 0 ? c.m1 : c.m2;
				const V3 vq    = p.v + Cross( p.w, c.r );
				const double lim = kFriction * c.jn;
				const double jt  = std::clamp( acc - m * Dot( vq, t ), -lim, lim );
				const double app = jt - acc;
				acc              = jt;
				p.v += t * app;
				p.w += invI * Cross( c.r, t * app );
			}
		}
	const double after = Kinetic( p, r, settings.ballInertia );
	if( after > before * ( 1.0 + 1e-9 ) + 1e-12 )
	{
		++energyRises;
		worstRise = std::max( worstRise, ( after - before ) / std::max( before, 1e-12 ) );
	}
	//Out of the felt, without adding energy: the position alone.
	double deepest = 0.0;
	for( int i = 0; i < used; ++i )
		deepest = std::max( deepest, contacts[ i ].depth );
	p.x.y += deepest * 0.8;
	p.w = p.w * ( 1.0 - kSpinDamp * dt );

	//At rest, and flat: onto the pile.
	if( Length( p.v ) * mpu < kSettleSpeed && Length( p.w ) < kSettleSpin && tilt > 0.96 )
		p.still += dt;
	else
		p.still = 0.0;
	if( p.still >= kSettleFor )
		Settle( p );
}

void Shower::Advance( double dt, bool continuous )
{
	dt = std::clamp( dt, 0.0, 0.1 );
	if( heights.empty() )
		heights.assign( static_cast< size_t >( kGrid * kGrid ), 0.0f );
	if( remaining > 0.0 || continuous )
	{
		owed += settings.rate * ( continuous ? 1.0 : rateScale ) * dt;
		remaining = std::max( 0.0, remaining - dt );
	}
	while( owed >= 1.0 )
	{
		owed -= 1.0;
		if( static_cast< int >( pieces.size() ) < kMaxPieces )
			pieces.push_back( Spawn() );
	}

	carry += dt;
	while( carry >= kDt )
	{
		for( Piece& p : pieces )
			StepPiece( p );
		carry -= kDt;
	}

	//Off the frame: gone. Too many lying about: the oldest fade.
	int resting = 0;
	for( const Piece& p : pieces )
		resting += p.resting ? 1 : 0;
	int excess = resting - kMaxResting;
	for( Piece& p : pieces )
	{
		if( !p.resting )
			continue;
		if( excess > 0 || !settings.pile )
		{
			p.fade = std::min( p.fade, 1.0 ) - 2.0 * dt;
			--excess;
		}
	}
	const double edge = 0.5 * settings.aspect + 0.4;
	pieces.erase( std::remove_if( pieces.begin(), pieces.end(),
	                              [ & ]( const Piece& p ) { return p.x.y < -1.0 || std::fabs( p.x.x ) > edge || p.fade <= 0.0; } ),
	              pieces.end() );
	(void)Smooth;
}

} // namespace jackpot
