#include "Shaders.h"

namespace jackpot::shaders
{
std::string Assemble( std::initializer_list< const char* > pieces )
{
	std::string out;
	for( const char* piece : pieces )
		out += piece;
	return out;
}

const char* const kVersion = "#version 410 core\n";

const char* const kQuadVertex = R"(
layout( location = 0 ) in vec4 vPosition;
layout( location = 1 ) in vec2 vUV;
out vec2 uv;
void main()
{
	gl_Position = vPosition;
	uv          = vUV;
}
)";

//===========================================================================
// The library.
//===========================================================================
const char* const kCommon = R"(
in vec2 uv;
out vec4 fragColour;

const float PI = 3.14159265358979;

const int BACK_NONE = 0;
const int BACK_FELT = 1;
const int BACK_DARK = 2;

uniform vec2  Resolution;
uniform float Zoom;
uniform float Time;        //plugin seconds, for lamps; frame-relative, never the host's float clock
uniform int   BackdropKind;
uniform vec3  Felt;        //linear
uniform vec3  Accent;      //linear
uniform float Lights;      //0..1
uniform float LightAngle;  //radians, the key light's azimuth
uniform int   IsEffect;
uniform float Mix;
uniform vec2  ClipScale;   //the input's used fraction of its texture (FFGL hardware padding)
uniform sampler2D Clip;
uniform int   TestFlat;    //jptest: no lighting, no glass, no lamps

//= mirrored in Maths.h, Pcg(). Integer only: the same on every GPU.
uint pcg( uint v )
{
	uint state = v * 747796405u + 2891336453u;
	uint word  = ( ( state >> ( ( state >> 28u ) + 4u ) ) ^ state ) * 277803737u;
	return ( word >> 22u ) ^ word;
}

float hash2( ivec2 c )
{
	return float( pcg( uint( c.x ) ^ pcg( uint( c.y ) + 0x9e3779b9u ) ) ) * ( 1.0 / 4294967296.0 );
}

float hash3( ivec3 c )
{
	uint h = pcg( uint( c.x ) ^ pcg( uint( c.y ) ^ pcg( uint( c.z ) + 0x9e3779b9u ) ) );
	return float( h ) * ( 1.0 / 4294967296.0 );
}

float valueNoise2( vec2 p )
{
	ivec2 i = ivec2( floor( p ) );
	vec2 f  = fract( p );
	vec2 u  = f * f * ( 3.0 - 2.0 * f );
	return mix( mix( hash2( i ), hash2( i + ivec2( 1, 0 ) ), u.x ), mix( hash2( i + ivec2( 0, 1 ) ), hash2( i + ivec2( 1, 1 ) ), u.x ), u.y );
}

float fbm2( vec2 p )
{
	float sum = 0.0, amp = 0.5;
	for( int o = 0; o < 4; ++o )
	{
		sum += amp * valueNoise2( p );
		p    = p * 2.03 + vec2( 17.1, 3.7 );
		amp *= 0.5;
	}
	return sum;
}

//The canvas: y up, the frame's height is 1 at Zoom 1, origin at the middle.
vec2 canvasPoint( vec2 fragment )
{
	return ( fragment - 0.5 * Resolution ) / ( Resolution.y * Zoom );
}

float canvasPixel()
{
	return 1.0 / ( Resolution.y * Zoom );
}

//-- 2D distances ------------------------------------------------------------
float sdCircle( vec2 p, float r )
{
	return length( p ) - r;
}

float sdBox( vec2 p, vec2 b, float r )
{
	vec2 d = abs( p ) - b + r;
	return length( max( d, 0.0 ) ) + min( max( d.x, d.y ), 0.0 ) - r;
}

float sdSegment( vec2 p, vec2 a, vec2 b, float r )
{
	vec2 pa = p - a, ba = b - a;
	float h = clamp( dot( pa, ba ) / dot( ba, ba ), 0.0, 1.0 );
	return length( pa - ba * h ) - r;
}

//smoothstep with its edges in either order. GLSL leaves smoothstep undefined
//when edge0 >= edge1 (4.10 section 8.3); Apple's driver happens to do the
//obvious thing, which is no promise about another rasteriser.
float sstep( float e0, float e1, float x )
{
	float t = clamp( ( x - e0 ) / ( e1 - e0 ), 0.0, 1.0 );
	return t * t * ( 3.0 - 2.0 * t );
}

//Coverage of a distance at the pixel's footprint.
float cover( float d, float px )
{
	return clamp( 0.5 - d / px, 0.0, 1.0 );
}

//Premultiplied "over".
vec4 over( vec4 top, vec4 under )
{
	return top + under * ( 1.0 - top.a );
}

vec4 layer( vec4 under, vec3 colour, float coverage )
{
	return over( vec4( colour * coverage, coverage ), under );
}

//The display transfer: the plugin lights in linear and writes display values.
vec3 toDisplay( vec3 c )
{
	return pow( max( c, 0.0 ), vec3( 1.0 / 2.2 ) );
}

vec3 toLinear( vec3 c )
{
	return pow( max( c, 0.0 ), vec3( 2.2 ) );
}

//A studio's worth of reflection for chrome and gold: a bright softbox above,
//a dim floor, a warm rim. `n` is a 3D normal, z toward the viewer.
vec3 studio( vec3 r )
{
	float up    = smoothstep( -0.2, 0.9, r.y );
	vec3 sky    = mix( vec3( 0.05, 0.05, 0.06 ), vec3( 0.95, 0.92, 0.88 ), up );
	float strip = smoothstep( 0.86, 0.98, r.y ) * ( 0.6 + 0.4 * sin( r.x * 6.0 ) );
	float side  = smoothstep( 0.5, 0.95, abs( r.x ) ) * 0.35;
	return sky * 0.7 + vec3( strip * 1.2 ) + vec3( 0.9, 0.6, 0.3 ) * side;
}

vec3 chrome( vec3 n, vec3 tint )
{
	vec3 r = reflect( vec3( 0.0, 0.0, -1.0 ), n );
	return studio( r ) * tint;
}

//The backdrop under a game: transparent (the clip, on the effect), felt, or
//a dark room with the casino's lights out of focus.
vec4 backdrop( vec2 p )
{
	if( BackdropKind == BACK_FELT )
	{
		float grain = fbm2( p * 900.0 ) * 0.08 + fbm2( p * 60.0 ) * 0.06;
		float vig   = 1.0 - 0.45 * dot( p, p );
		return vec4( Felt * ( 0.85 + grain ) * vig, 1.0 );
	}
	if( BackdropKind == BACK_DARK )
	{
		vec3 c = mix( vec3( 0.004, 0.003, 0.012 ), vec3( 0.02, 0.012, 0.03 ), smoothstep( -0.5, 0.6, p.y ) );
		//Bokeh: discs on a jittered grid, warm and cool.
		vec2 cell = p * 7.0;
		ivec2 id  = ivec2( floor( cell ) );
		for( int j = -1; j <= 1; ++j )
			for( int i = -1; i <= 1; ++i )
			{
				ivec2 k  = id + ivec2( i, j );
				float h  = hash2( k * 7 + ivec2( 3, 11 ) );
				if( h > 0.45 )
					continue;
				vec2 c0  = vec2( k ) + vec2( hash2( k + ivec2( 17, 0 ) ), hash2( k + ivec2( 0, 23 ) ) );
				float r  = 0.18 + 0.25 * hash2( k + ivec2( 5, 9 ) );
				float d  = length( cell - c0 ) - r;
				vec3 hue = mix( vec3( 1.0, 0.55, 0.18 ), vec3( 0.35, 0.45, 1.0 ), step( 0.3, h ) );
				float tw = 0.7 + 0.3 * sin( Time * ( 0.7 + 2.0 * h ) + 40.0 * h );
				c += hue * 0.05 * tw * ( sstep( 0.04, -0.04, d ) * ( 0.7 + 0.6 * smoothstep( -r, 0.0, d ) ) );
			}
		return vec4( c, 1.0 );
	}
	return vec4( 0.0 );
}

//Every game ends here: its premultiplied linear picture, over the clip on the
//effect, to display values.
vec4 finish( vec4 game )
{
	if( IsEffect == 1 )
	{
		vec4 clip     = texture( Clip, uv * ClipScale );
		vec3 clipLin  = toLinear( clip.rgb );
		vec4 under    = vec4( clipLin * clip.a, clip.a );
		vec4 composed = over( game, under );
		vec4 mixed    = mix( under, composed, Mix );
		vec3 straight = mixed.a > 0.0 ? mixed.rgb / mixed.a : vec3( 0.0 );
		return vec4( toDisplay( straight ) * mixed.a, mixed.a );
	}
	vec3 straight = game.a > 0.0 ? game.rgb / game.a : vec3( 0.0 );
	return vec4( toDisplay( straight ) * game.a, game.a );
}
)";

//===========================================================================
// Text from the SDF atlas.
//===========================================================================
const char* const kText = R"(
const float SPREAD = 12.0;  //px, the atlas's distance range either side of an edge
const float ONEDGE = 128.0; //the byte on the outline
const int MAX_SPANS = 24;
const int MAX_CHARS = 192;

uniform sampler2D Atlas;
uniform vec4  GlyphBox[ 44 ];  //( originX, originY, inkX0, inkX1 ): atlas px, H units
uniform float GlyphAdv[ 44 ];  //H units
uniform float AtlasPx;         //atlas px per H
uniform vec2  AtlasSize;
uniform int   Chars[ MAX_CHARS ];//glyph indices; -1 a space
uniform ivec2 Span[ MAX_SPANS ]; //( first char, count ) per string
uniform vec2  SpanInk[ MAX_SPANS ];//( ink left, ink width ) in H, from the pen's start
uniform float Weight;          //H units of dilation

//Signed distance in H units, positive inside, at local point q (H units from
//the glyph's pen origin, y from the bottom of the capitals).
float glyphDistance( int g, vec2 q )
{
	vec2 a    = GlyphBox[ g ].xy + q * AtlasPx;
	vec2 cell = floor( GlyphBox[ g ].xy / 128.0 ) * 128.0;
	if( a.x < cell.x + 1.0 || a.y < cell.y + 1.0 || a.x > cell.x + 127.0 || a.y > cell.y + 127.0 )
		return -SPREAD / AtlasPx;
	float v = texture( Atlas, a / AtlasSize ).r * 255.0;
	return ( v - ONEDGE ) / ( ONEDGE / SPREAD ) / AtlasPx + Weight;
}

//A string centred on the origin, `h` tall: the distance in the units of p
//(positive inside). Far from the string's box it returns a large negative.
float textDistance( int span, vec2 p, float h )
{
	vec2 q      = p / h;
	vec2 ink    = SpanInk[ span ];
	//The ink's box, centred: x from -w/2, y from -0.5.
	vec2 local  = vec2( q.x + 0.5 * ink.y + ink.x, q.y + 0.5 );
	if( local.x < ink.x - 0.3 || local.x > ink.x + ink.y + 0.3 || local.y < -0.4 || local.y > 1.4 )
		return -1.0;
	float best  = -1.0;
	float pen   = 0.0;
	ivec2 s     = Span[ span ];
	for( int i = 0; i < s.y; ++i )
	{
		int g = Chars[ s.x + i ];
		if( g < 0 )
		{
			pen += 0.35;
			continue;
		}
		float x = local.x - pen;
		if( x > GlyphBox[ g ].z - 0.3 && x < GlyphBox[ g ].w + 0.3 )
			best = max( best, glyphDistance( g, vec2( x, local.y ) ) );
		pen += GlyphAdv[ g ];
	}
	return best * h;
}

//A whole number centred on the origin, `h` tall (digits only, up to 4).
float numberDistance( int value, vec2 p, float h )
{
	int digits[ 4 ];
	int n = 0;
	int v = max( value, 0 );
	for( int i = 0; i < 4; ++i )
	{
		digits[ 3 - i ] = v % 10;
		v /= 10;
		++n;
		if( v == 0 )
			break;
	}
	//Ink width: the first digit's left ink to the last digit's right ink.
	float pen = 0.0, lo = 0.0, hi = 0.0;
	for( int i = 0; i < n; ++i )
	{
		int g = digits[ 4 - n + i ];
		if( i == 0 )
			lo = GlyphBox[ g ].z;
		hi   = pen + GlyphBox[ g ].w;
		pen += GlyphAdv[ g ];
	}
	vec2 q     = p / h;
	vec2 local = vec2( q.x + 0.5 * ( hi - lo ) + lo, q.y + 0.5 );
	if( local.x < lo - 0.3 || local.x > hi + 0.3 || local.y < -0.4 || local.y > 1.4 )
		return -1.0;
	float best = -1.0;
	pen        = 0.0;
	for( int i = 0; i < n; ++i )
	{
		int g = digits[ 4 - n + i ];
		best  = max( best, glyphDistance( g, vec2( local.x - pen, local.y ) ) );
		pen  += GlyphAdv[ g ];
	}
	return best * h;
}
)";

//===========================================================================
// Shared by the scenes that have nothing else yet: the backdrop alone.
//===========================================================================
const char* const kBlank = R"(
void main()
{
	vec2 p = canvasPoint( gl_FragCoord.xy );
	fragColour = finish( backdrop( p ) );
}
)";


//===========================================================================
// Slots: the cabinet.
//===========================================================================
const char* const kSlots = R"(
//= mirrored in Jackpot.cpp, SlotSpan.
const int SPAN_MARQUEE = 0;
const int SPAN_CREDITS = 1;
const int SPAN_WIN     = 2;
const int SPAN_BANNER  = 3;
const int SPAN_SPIN    = 4;

const int STOPS        = 22;
const float PITCH      = 2.0 * PI / 22.0;
const float PHI_MAX    = 1.5 * 2.0 * PI / 22.0; //three stops in the window
const int SYM_CLIP     = 12;

uniform int   Reels;
uniform float ReelA[ 5 ];   //stop at the payline when the shutter opened
uniform float ReelB[ 5 ];   //... and closed
uniform int   StripSym[ 110 ];
uniform sampler2D Symbols;
uniform vec2  SymbolsSize;
uniform float Handle;       //0 up .. 1 down
uniform int   Credits;
uniform int   WinMeter;
uniform float WinGlow;      //0..1 pulse on the paying symbols
uniform int   Banner;       //0 none, 1 win, 2 jackpot
uniform float BannerAmount; //0..1
uniform int   Celebrate;    //0 idle, 1 win, 2 jackpot: what the lamps are doing

float cabinetHalf()
{
	return Reels == 5 ? 0.44 : 0.30;
}

//The premultiplied linear colour of symbol s at tile (u, v), at mip `lod`.
vec4 symbolTexel( int s, vec2 t, float lod )
{
	if( t.x < 0.0 || t.x > 1.0 || t.y < 0.0 || t.y > 1.0 || s <= 0 )
		return vec4( 0.0 );
	if( s == SYM_CLIP && IsEffect == 1 )
	{
		vec2 c = abs( t - 0.5 ) * 2.0;
		if( max( c.x, c.y ) > 0.82 )
			return vec4( toLinear( vec3( 1.0, 0.82, 0.23 ) ), 1.0 );
		vec4 clip = texture( Clip, ( ( t - 0.5 ) / 0.82 * 0.5 + 0.5 ) * ClipScale );
		return vec4( toLinear( clip.rgb ), 1.0 );
	}
	vec2 cell = vec2( float( s % 4 ), float( s / 4 ) );
	vec2 a    = ( cell + clamp( t, 0.004, 0.996 ) ) / 4.0;
	vec4 c    = textureLod( Symbols, a, lod );
	vec3 straight = c.a > 0.0 ? c.rgb / c.a : vec3( 0.0 );
	return vec4( toLinear( straight ) * c.a, c.a );
}

//One reel, at reel-local (x across -1..1, y up -1..1 of the window): the
//strip integrated over the shutter's travel.
vec3 reelColour( int reel, vec2 local, float reelWidth, float halfHeight, float px )
{
	float sinMax = sin( PHI_MAX );
	float phi    = asin( clamp( local.y * sinMax, -1.0, 1.0 ) );
	float radius = halfHeight / sinMax;
	float arc    = radius * PITCH;            //one stop's height on the drum
	float a      = ReelA[ reel ], b = ReelB[ reel ];
	float travel = abs( b - a );
	int taps     = clamp( int( travel * 14.0 ) + 1, 1, 28 );
	//Mip from the footprint: atlas px per canvas unit, times canvas per pixel,
	//foreshortened by the drum.
	float texels = float( 192 ) / ( arc * 0.9 ) * px / max( cos( phi ), 0.2 );
	float lod    = log2( max( texels, 1.0 ) );
	vec3 paper   = toLinear( vec3( 0.96, 0.94, 0.88 ) );
	vec3 sum     = vec3( 0.0 );
	for( int j = 0; j < taps; ++j )
	{
		float s  = mix( a, b, ( float( j ) + 0.5 ) / float( taps ) );
		float u  = s + phi / PITCH;
		float k  = floor( u + 0.5 );
		float f  = u - k;
		int stop = int( mod( k, float( STOPS ) ) );
		int sym  = StripSym[ reel * STOPS + stop ];
		vec2 t   = vec2( 0.5 + local.x * 0.5 * reelWidth / ( arc * 0.9 ), 0.5 + f / 0.9 );
		vec4 ink = symbolTexel( sym, t, lod );
		vec3 c   = ink.rgb + paper * ( 1.0 - ink.a );
		//The paying symbols pulse once the reels are at rest.
		if( WinGlow > 0.0 && abs( k - floor( b + 0.5 ) ) < 0.5 )
			c *= 1.0 + 0.6 * WinGlow;
		sum += c;
	}
	vec3 c = sum / float( taps );
	if( TestFlat == 1 )
		return c;
	//The drum: lit from above the window, falling off round the curve.
	float facing = cos( phi );
	float light  = 0.30 + 0.70 * pow( facing, 2.0 ) + 0.10 * smoothstep( 0.2, 0.9, local.y );
	return c * light;
}

//Lamp state for bulb `i` of a row: 0..1.
float lampOn( int i )
{
	if( Lights <= 0.0 )
		return 0.0;
	if( Celebrate == 2 )
		return step( 0.45, hash2( ivec2( i, int( Time * 14.0 ) ) ) );
	if( Celebrate == 1 )
		return float( ( i + int( Time * 5.0 ) ) % 2 == 0 );
	return float( ( i + int( Time * 7.0 ) ) % 4 == 0 );
}

//A row of `count` bulbs from a to b: the nearest bulb's disc, and the glow of
//it and its two neighbours (one bulb's glow alone would stop dead halfway to
//the next).
void lampRow( inout vec4 col, vec2 q, vec2 a, vec2 b, int count, int base, float px )
{
	vec2 ab  = b - a;
	float t  = clamp( dot( q - a, ab ) / dot( ab, ab ), 0.0, 1.0 );
	int near = int( floor( t * float( count - 1 ) + 0.5 ) );
	vec3 warm = toLinear( vec3( 1.0, 0.85, 0.45 ) );
	float glow = 0.0;
	for( int k = -1; k <= 1; ++k )
	{
		int i = near + k;
		if( i < 0 || i >= count )
			continue;
		vec2 c   = a + ab * ( float( i ) / float( count - 1 ) );
		float d  = length( q - c );
		float on = TestFlat == 1 ? 0.0 : lampOn( i + base ) * Lights;
		if( k == 0 )
			col = layer( col, mix( warm * 0.18, warm * 3.0, on ), cover( d - 0.0085, px ) );
		glow += on * exp( -d * d / 0.0004 );
	}
	col.rgb += warm * 0.5 * glow * col.a;
}
)";

const char* const kSlotsMain = R"(
void main()
{
	vec2 p   = canvasPoint( gl_FragCoord.xy );
	float px = canvasPixel();
	float W  = cabinetHalf();
	vec2 q   = p - vec2( -0.035, 0.0 );
	vec4 col = backdrop( p );

	//The cabinet's shadow on whatever it stands in front of.
	float body = sdBox( q - vec2( 0.0, -0.02 ), vec2( W, 0.46 ), 0.05 );
	if( TestFlat == 0 && BackdropKind != BACK_NONE )
		col.rgb *= 1.0 - 0.55 * ( 1.0 - smoothstep( -0.01, 0.09, body + 0.02 * ( q.y + 0.5 ) ) );

	//-- the handle, behind the cabinet's edge where it pivots -----------------
	vec2 pivot   = vec2( W + 0.028, 0.04 );
	float theta  = mix( 0.12, PI - 0.25, Handle );
	float toward = sin( theta );                  //how far the knob has come toward the viewer
	float persp  = 1.0 + 0.45 * toward;
	vec2 knob    = pivot + vec2( 0.02 * toward, 0.27 * cos( theta ) );
	float armR   = 0.011 * mix( 1.0, persp, 0.6 );
	float arm    = sdSegment( q, pivot, knob, armR );
	float ball   = sdCircle( q - knob, 0.036 * persp );
	float hub    = sdBox( q - pivot - vec2( -0.01, 0.0 ), vec2( 0.03, 0.055 ), 0.02 );
	if( arm < px * 2.0 )
	{
		float across = clamp( dot( q - pivot, normalize( vec2( knob.y - pivot.y, pivot.x - knob.x ) ) ) / armR, -1.0, 1.0 );
		vec3 n       = vec3( across, 0.0, sqrt( 1.0 - across * across ) );
		col          = layer( col, TestFlat == 1 ? vec3( 0.7 ) : chrome( n, vec3( 0.9 ) ), cover( arm, px ) );
	}
	col = layer( col, TestFlat == 1 ? vec3( 0.4 ) : chrome( vec3( 0.6, 0.2, 0.77 ), vec3( 0.7 ) ), cover( hub, px ) );

	//-- the body ----------------------------------------------------------------
	if( body < px * 2.0 )
	{
		vec3 paint = Accent * ( 0.55 + 0.45 * smoothstep( -0.5, 0.5, q.y ) );
		//A sheen down the left of the curved front, and the chrome trim.
		paint     += Accent * 0.6 * exp( -pow( ( q.x + W * 0.55 ) / ( W * 0.18 ), 2.0 ) );
		float trim = smoothstep( -0.014, -0.010, body );
		//The trim's normal leans outward with the distance's gradient, so the
		//chrome turns smoothly round the corners.
		vec2 out2  = normalize( vec2( dFdx( body ), dFdy( body ) ) + vec2( 1e-7 ) );
		vec3 c     = mix( paint, chrome( normalize( vec3( out2 * 0.55, 0.83 ) ), vec3( 1.0 ) ), trim );
		col        = layer( col, TestFlat == 1 ? Accent : c, cover( body, px ) );
	}

	//-- the marquee ---------------------------------------------------------------
	vec2 mq      = q - vec2( 0.0, 0.335 );
	float panel  = sdBox( mq, vec2( W - 0.035, 0.092 ), 0.04 );
	if( panel < px * 2.0 )
	{
		//A sunburst behind the name: rays from below the panel.
		float ang   = atan( mq.x, mq.y + 0.25 );
		float rays  = 0.5 + 0.5 * sign( sin( ang * 24.0 + Time * 0.6 ) );
		vec3 back   = mix( toLinear( vec3( 0.55, 0.02, 0.05 ) ), toLinear( vec3( 0.85, 0.08, 0.1 ) ), rays );
		back       *= 0.7 + 0.5 * smoothstep( -0.09, 0.09, mq.y );
		if( Celebrate == 2 )
			back = mix( back, toLinear( vec3( 1.0, 0.85, 0.3 ) ), 0.35 + 0.35 * sin( Time * 18.0 ) );
		col = layer( col, TestFlat == 1 ? vec3( 0.5, 0.02, 0.05 ) : back, cover( panel, px ) );
		col = layer( col, toLinear( vec3( 0.9, 0.7, 0.2 ) ), cover( abs( panel + 0.005 ) - 0.004, px ) );
		float name = textDistance( SPAN_MARQUEE, mq - vec2( 0.0, -0.004 ), 0.085 );
		col        = layer( col, toLinear( vec3( 0.18, 0.02, 0.0 ) ), cover( -name - 0.006, px ) );
		vec3 gold  = mix( toLinear( vec3( 1.0, 0.93, 0.55 ) ), toLinear( vec3( 0.85, 0.5, 0.05 ) ), sstep( 0.04, -0.04, mq.y ) );
		col        = layer( col, gold, cover( -name, px ) );
	}

	//-- lamps round the marquee and down the sides ----------------------------
	{
		int across = Reels == 5 ? 22 : 15;
		lampRow( col, mq, vec2( -W + 0.06, 0.077 ), vec2( W - 0.06, 0.077 ), across, 0, px );
		lampRow( col, mq, vec2( -W + 0.06, -0.077 ), vec2( W - 0.06, -0.077 ), across, 31, px );
		lampRow( col, q, vec2( -W + 0.016, 0.22 ), vec2( -W + 0.016, -0.40 ), 14, 62, px );
		lampRow( col, q, vec2( W - 0.016, 0.22 ), vec2( W - 0.016, -0.40 ), 14, 93, px );
	}

	//-- the window and its reels --------------------------------------------------
	float Wi     = W - 0.045;
	vec2 wc      = vec2( 0.0, 0.06 );
	float window = sdBox( q - wc, vec2( Wi, 0.145 ), 0.015 );
	float bezel  = sdBox( q - wc, vec2( Wi + 0.018, 0.145 + 0.018 ), 0.03 );
	if( bezel < px * 2.0 )
	{
		vec2 bn = normalize( vec2( dFdx( bezel ), dFdy( bezel ) ) + 1e-6 );
		col     = layer( col, TestFlat == 1 ? vec3( 0.6 ) : chrome( normalize( vec3( bn * 0.7, 0.7 ) ), vec3( 0.95 ) ), cover( bezel, px ) );
	}
	if( window < px * 2.0 )
	{
		float gap = 0.008;
		float rw  = ( 2.0 * Wi - float( Reels + 1 ) * gap ) / float( Reels );
		vec3 c    = vec3( 0.01 );
		float x0  = -Wi + gap;
		int reel  = int( floor( ( q.x - x0 ) / ( rw + gap ) ) );
		if( reel >= 0 && reel < Reels )
		{
			float xc = x0 + rw * 0.5 + float( reel ) * ( rw + gap );
			vec2 local = vec2( ( q.x - xc ) / ( 0.5 * rw ), ( q.y - wc.y ) / 0.145 );
			if( abs( local.x ) <= 1.0 )
			{
				c = reelColour( reel, local, rw, 0.145, px );
				if( TestFlat == 0 )
					c *= 0.75 + 0.25 * ( 1.0 - pow( abs( local.x ), 6.0 ) );
			}
		}
		if( TestFlat == 0 )
		{
			//The window's own shadow on the drums, and the glass.
			c *= 0.55 + 0.45 * smoothstep( 0.0, 0.03, -window );
			float streak = sstep( 0.03, 0.0, abs( ( q.x - wc.x ) * 0.45 + ( q.y - wc.y ) - 0.06 ) );
			c += vec3( 0.05 ) * streak;
			//The payline.
			float line = abs( q.y - wc.y ) - 0.0028;
			c = mix( c, toLinear( vec3( 0.95, 0.08, 0.06 ) ) * ( 1.0 + WinGlow ), cover( line, px ) * 0.85 );
		}
		col = layer( col, c, cover( window, px ) );
	}
	//The payline's arrows, on the bezel.
	if( TestFlat == 0 )
	{
		//A triangle on each side bezel, its tip at the window's edge pointing in.
		vec2 aq    = vec2( abs( q.x ) - Wi, q.y - wc.y );
		float tri  = max( aq.x - 0.016, abs( aq.y ) - 0.7 * ( aq.x + 0.002 ) );
		col        = layer( col, toLinear( vec3( 0.95, 0.1, 0.05 ) ), cover( tri, px ) );
	}

	//-- the meters ------------------------------------------------------------------
	for( int m = 0; m < 2; ++m )
	{
		vec2 mc   = vec2( ( m == 0 ? -1.0 : 1.0 ) * W * 0.47, -0.17 );
		vec2 mp   = q - mc;
		float box = sdBox( mp, vec2( W * 0.42, 0.048 ), 0.012 );
		if( box > px * 2.0 )
			continue;
		col = layer( col, vec3( 0.006, 0.004, 0.004 ), cover( box, px ) );
		col = layer( col, toLinear( vec3( 0.6, 0.55, 0.45 ) ) * 0.6, cover( abs( box ) - 0.0018, px ) );
		float label = textDistance( m == 0 ? SPAN_CREDITS : SPAN_WIN, mp - vec2( 0.0, 0.028 ), 0.014 );
		col = layer( col, toLinear( vec3( 0.85, 0.8, 0.65 ) ), cover( -label, px ) );
		float digits = numberDistance( m == 0 ? Credits : WinMeter, mp - vec2( 0.0, -0.01 ), 0.042 );
		vec3 led     = toLinear( vec3( 1.0, 0.12, 0.06 ) );
		col          = layer( col, led * 2.0, cover( -digits, px ) );
	}

	//-- the buttons and the tray ------------------------------------------------------
	for( int b = 0; b < 3; ++b )
	{
		vec2 bc   = vec2( ( float( b ) - 1.0 ) * W * 0.5, -0.275 );
		float btn = sdCircle( q - bc, 0.026 );
		if( btn > px * 2.0 )
			continue;
		vec3 hue = b == 0 ? vec3( 0.9, 0.1, 0.08 ) : b == 1 ? vec3( 1.0, 0.65, 0.05 ) : vec3( 0.1, 0.75, 0.2 );
		vec2 bq  = ( q - bc ) / 0.026;
		vec3 n   = vec3( bq, sqrt( max( 0.0, 1.0 - dot( bq, bq ) ) ) );
		float lit = 0.5 + 0.5 * max( dot( n, normalize( vec3( -0.4, 0.6, 0.7 ) ) ), 0.0 );
		vec3 c    = toLinear( hue ) * lit + vec3( pow( max( n.z * 0.8 + n.y * 0.6, 0.0 ), 30.0 ) );
		col       = layer( col, TestFlat == 1 ? toLinear( hue ) : c, cover( btn, px ) );
	}
	{
		vec2 tc    = q - vec2( 0.0, -0.385 );
		float tray = sdBox( tc, vec2( W - 0.07, 0.048 ), 0.03 );
		if( tray < px * 2.0 )
		{
			float depth = clamp( -tray / 0.03, 0.0, 1.0 );
			vec3 n      = normalize( vec3( 0.0, mix( 0.8, -0.3, depth ) * sign( tc.y + 0.01 ), 0.6 ) );
			col         = layer( col, TestFlat == 1 ? vec3( 0.5 ) : chrome( n, vec3( 0.85 ) ) * mix( 1.0, 0.35, depth ), cover( tray, px ) );
		}
	}

	//-- the knob, in front of everything once it has come round toward us --------------
	//(Guarded: far from the knob its "normal" is unbounded and pow( ., 60 )
	//overflows to infinity, and infinity times zero coverage is NaN.)
	if( ball < px * 2.0 )
	{
		vec2 bq   = ( q - knob ) / ( 0.036 * persp );
		vec3 n    = vec3( bq, sqrt( max( 0.0, 1.0 - dot( bq, bq ) ) ) );
		vec3 red  = toLinear( vec3( 0.85, 0.04, 0.05 ) );
		float lit = 0.25 + 0.85 * max( dot( n, normalize( vec3( -0.4, 0.6, 0.7 ) ) ), 0.0 );
		vec3 c    = red * lit + vec3( 1.2 ) * pow( max( dot( n, normalize( vec3( -0.2, 0.5, 1.7 ) ) ), 0.0 ), 60.0 );
		col       = layer( col, TestFlat == 1 ? red : c, cover( ball, px ) );
	}

	//-- the banner over the window, once the reels stop on a win -----------------------
	if( Banner > 0 && BannerAmount > 0.0 && TestFlat == 0 )
	{
		float s    = mix( 0.6, 1.0, BannerAmount );
		vec2 bp    = ( q - wc - vec2( 0.0, 0.0 ) ) / s;
		float box  = sdBox( bp, vec2( Wi * 0.86, 0.07 ), 0.035 );
		float fade = BannerAmount;
		vec3 back  = Banner == 2 ? toLinear( vec3( 0.75, 0.05, 0.08 ) ) : toLinear( vec3( 0.06, 0.08, 0.35 ) );
		col        = layer( col, back, cover( box * s, px ) * fade * 0.92 );
		col        = layer( col, toLinear( vec3( 1.0, 0.82, 0.25 ) ), cover( ( abs( box ) - 0.004 ) * s, px ) * fade );
		float word = textDistance( SPAN_BANNER, bp, 0.075 );
		vec3 gold  = mix( toLinear( vec3( 1.0, 0.95, 0.6 ) ), toLinear( vec3( 0.95, 0.55, 0.05 ) ), sstep( 0.04, -0.04, bp.y ) );
		col        = layer( col, toLinear( vec3( 0.15, 0.03, 0.0 ) ), cover( ( -word - 0.006 ) * s, px ) * fade );
		col        = layer( col, gold * ( 1.0 + 0.3 * sin( Time * 9.0 ) ), cover( -word * s, px ) * fade );
	}

	fragColour = finish( col );
}
)";

//PLACEHOLDERS -- replaced game by game.
const char* const kRoulette      = "";
const char* const kRouletteMain  = R"(void main(){ fragColour = finish( backdrop( canvasPoint( gl_FragCoord.xy ) ) ); })";
const char* const kWheel         = R"(void main(){ fragColour = finish( backdrop( canvasPoint( gl_FragCoord.xy ) ) ); })";
const char* const kCraps         = "";
const char* const kCrapsMain     = R"(void main(){ fragColour = finish( backdrop( canvasPoint( gl_FragCoord.xy ) ) ); })";
const char* const kLottery       = R"(void main(){ fragColour = finish( backdrop( canvasPoint( gl_FragCoord.xy ) ) ); })";
const char* const kShowerVertex  = R"(layout( location = 0 ) in vec2 corner; void main(){ gl_Position = vec4( corner, 0.0, 1.0 ); })";
const char* const kShowerFragment = R"(void main(){ fragColour = vec4( 0.0 ); })";
const char* const kComposite     = R"(in vec2 uv; out vec4 fragColour; void main(){ fragColour = vec4( 0.0 ); })";

} // namespace jackpot::shaders
