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

extern const char* const kVersion;
extern const char* const kQuadVertex;

std::vector< Program > Programs()
{
	const std::string quad = Assemble( { kVersion, kQuadVertex } );
	return {
		{ "slots", quad, Assemble( { kVersion, kCommon, kText, kSlots, kSlotsMain } ) },
		{ "roulette", quad, Assemble( { kVersion, kCommon, kText, kScene, kRoulette, kRouletteMain } ) },
		{ "wheel", quad, Assemble( { kVersion, kCommon, kText, kWheel } ) },
		{ "craps", quad, Assemble( { kVersion, kCommon, kText, kScene, kCraps, kCrapsMain } ) },
		{ "lottery", quad, Assemble( { kVersion, kCommon, kText, kScene, kLottery, kLotteryMain } ) },
		{ "blank", quad, Assemble( { kVersion, kCommon, kBlank } ) },
		{ "shower", Assemble( { kVersion, kShowerVertex } ), Assemble( { kVersion, kCommon, kText, kScene, kShowerFragment } ) },
		{ "composite", quad, Assemble( { kVersion, kComposite } ) },
	};
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

//Zero coverage leaves `under` exactly as it was, whatever `colour` holds: a
//highlight evaluated far from its shape can overflow (pow of a huge "normal"),
//and infinity times zero is NaN, which drew black wedges twice.
vec4 layer( vec4 under, vec3 colour, float coverage )
{
	return coverage > 0.0 ? over( vec4( colour * coverage, coverage ), under ) : under;
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
		//The clip arrives premultiplied (Resolume's DXV clips with alpha have
		//rgb <= a on 99.6% of pixels -- colourunder measured it), so it is
		//un-premultiplied before it is taken to linear.
		vec4 clip     = texture( Clip, uv * ClipScale );
		vec3 clipLin  = toLinear( clip.a > 0.0 ? clip.rgb / clip.a : vec3( 0.0 ) );
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

//A string stacked one character under another, each upright, centred across
//x = 0, the first character's cell starting at y = 0 and the rest below it,
//`h` tall each with `lead` between.
float stackedDistance( int span, vec2 p, float h, float lead )
{
	ivec2 s    = Span[ span ];
	float best = -1.0;
	int row    = int( floor( -p.y / ( h * lead ) ) );
	for( int k = row - 1; k <= row + 1; ++k )
	{
		if( k < 0 || k >= s.y )
			continue;
		int g = Chars[ s.x + k ];
		if( g < 0 )
			continue;
		float cy = -( float( k ) + 0.5 ) * h * lead;
		vec2 q   = vec2( p.x / h + 0.5 * ( GlyphBox[ g ].z + GlyphBox[ g ].w ), ( p.y - cy ) / h + 0.5 );
		best     = max( best, glyphDistance( g, q ) * h );
	}
	return best;
}

//-- the result board: the play's result in words, over the top of the frame ----
uniform int   ResultSpan;    //the span holding the words, -1 none
uniform float ResultAmount;  //0..1
uniform int   ResultTone;    //0 plain, 1 a win, 2 the top prize

//In frame units (the frame's height is 1, whatever Zoom is), so the board
//stays where it is when the game is framed closer.
vec4 resultBoard( vec4 col )
{
	if( ResultSpan < 0 || ResultAmount <= 0.0 )
		return col;
	vec2 q    = ( gl_FragCoord.xy - 0.5 * Resolution ) / Resolution.y - vec2( 0.0, 0.405 );
	float px  = 1.0 / Resolution.y;
	float h   = 0.058;
	float w   = 0.5 * SpanInk[ ResultSpan ].y * h + 0.045;
	float box = sdBox( q, vec2( w, 0.052 ), 0.026 );
	if( box > 4.0 * px )
		return col;
	vec3 back = ResultTone == 2 ? toLinear( vec3( 0.72, 0.04, 0.07 ) ) : ResultTone == 1 ? toLinear( vec3( 0.05, 0.07, 0.33 ) ) : vec3( 0.006 );
	col = layer( col, back, cover( box, px ) * 0.92 * ResultAmount );
	col = layer( col, toLinear( vec3( 1.0, 0.82, 0.25 ) ), cover( abs( box ) - 0.0025, px ) * ResultAmount );
	float word = textDistance( ResultSpan, q, h );
	col = layer( col, toLinear( vec3( 1.0, 0.94, 0.62 ) ), cover( -word, px ) * ResultAmount );
	return col;
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
		vec3 straight = clip.a > 0.0 ? clip.rgb / clip.a : vec3( 0.0 );
		return vec4( toLinear( straight ) * clip.a, clip.a );
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
	int nearest = int( floor( t * float( count - 1 ) + 0.5 ) );
	vec3 warm = toLinear( vec3( 1.0, 0.85, 0.45 ) );
	float glow = 0.0;
	for( int k = -1; k <= 1; ++k )
	{
		int i = nearest + k;
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

//===========================================================================
// The 3D games' shared pieces: the camera, the light, the primitives.
// World axes: z up, the camera to the south (-y) looking north and down.
//===========================================================================
const char* const kScene = R"(
uniform vec3  CamPos;
uniform vec3  CamRight;
uniform vec3  CamUp;
uniform vec3  CamForward;
uniform float CamFocal;   //1 / tan( half the vertical field of view )
uniform int   Samples;    //rays per pixel: 1; 2 means four at the pixels on an edge, one elsewhere; 4

//The surface the last shaded ray hit, by kind (and part, and pocket): a pixel
//whose 2x2 quad saw two different ones is on an edge (fwidth), and only those
//take four rays when Samples is 2.
float HitId = 0.0;

//The ray through a point of the frame (in pixels).
vec3 cameraRay( vec2 fragment )
{
	vec2 s = ( 2.0 * fragment - Resolution ) / Resolution.y;
	return normalize( CamForward * CamFocal + CamRight * s.x + CamUp * s.y );
}

//Toward the key light: Light Angle round, 50 degrees up.
vec3 keyLight()
{
	float e = 0.87266;
	return normalize( vec3( cos( LightAngle ) * cos( e ), sin( LightAngle ) * cos( e ), sin( e ) ) );
}

//The rotated-grid offsets for four rays (or the centre, for one).
vec2 sampleOffset( int i )
{
	vec2 o[ 4 ] = vec2[ 4 ]( vec2( 0.125, 0.375 ), vec2( 0.375, -0.125 ), vec2( -0.125, -0.375 ), vec2( -0.375, 0.125 ) );
	return o[ i ];
}

//-- primitives: the nearest t > 0, or -1 --------------------------------------
float hitSphere( vec3 ro, vec3 rd, vec3 c, float r )
{
	vec3 oc = ro - c;
	float b = dot( oc, rd );
	float h = b * b - ( dot( oc, oc ) - r * r );
	if( h < 0.0 )
		return -1.0;
	float t = -b - sqrt( h );
	return t > 0.0 ? t : -1.0;
}

//A capsule from a to b, radius r (Quilez).
float hitCapsule( vec3 ro, vec3 rd, vec3 a, vec3 b, float r )
{
	vec3 ba  = b - a;
	vec3 oa  = ro - a;
	float baba = dot( ba, ba );
	float bard = dot( ba, rd );
	float baoa = dot( ba, oa );
	float rdoa = dot( rd, oa );
	float oaoa = dot( oa, oa );
	float qa   = baba - bard * bard;
	float qb   = baba * rdoa - baoa * bard;
	float qc   = baba * oaoa - baoa * baoa - r * r * baba;
	float h    = qb * qb - qa * qc;
	if( h >= 0.0 )
	{
		float t = ( -qb - sqrt( h ) ) / qa;
		float y = baoa + t * bard;
		if( y > 0.0 && y < baba )
			return t > 0.0 ? t : -1.0;
		vec3 oc = ( y <= 0.0 ) ? oa : ro - b;
		qb      = dot( rd, oc );
		qc      = dot( oc, oc ) - r * r;
		h       = qb * qb - qc;
		if( h > 0.0 )
		{
			t = -qb - sqrt( h );
			return t > 0.0 ? t : -1.0;
		}
	}
	return -1.0;
}

//A capped cylinder from a to b, radius r (Quilez): (t, normal), t < 0 none.
vec4 hitCylinder( vec3 ro, vec3 rd, vec3 a, vec3 b, float r )
{
	vec3 ca    = b - a;
	vec3 oc    = ro - a;
	float caca = dot( ca, ca );
	float card = dot( ca, rd );
	float caoc = dot( ca, oc );
	float qa   = caca - card * card;
	float qb   = caca * dot( oc, rd ) - caoc * card;
	float qc   = caca * dot( oc, oc ) - caoc * caoc - r * r * caca;
	float h    = qb * qb - qa * qc;
	if( h < 0.0 )
		return vec4( -1.0 );
	h       = sqrt( h );
	float t = ( -qb - h ) / qa;
	float y = caoc + t * card;
	if( y > 0.0 && y < caca && t > 0.0 )
		return vec4( t, ( oc + t * rd - ca * y / caca ) / r );
	t = ( ( y < 0.0 ? 0.0 : caca ) - caoc ) / card;
	if( abs( qb + qa * t ) < h && t > 0.0 )
		return vec4( t, ca * sign( y ) / sqrt( caca ) );
	return vec4( -1.0 );
}

vec3 capsuleNormal( vec3 p, vec3 a, vec3 b, float r )
{
	vec3 ba = b - a;
	float h = clamp( dot( p - a, ba ) / dot( ba, ba ), 0.0, 1.0 );
	return ( p - a - ba * h ) / r;
}

//A box centred at the origin of its own frame, half-sizes h: (t, normal).
vec4 hitBox( vec3 ro, vec3 rd, vec3 h )
{
	vec3 m  = 1.0 / rd;
	vec3 n  = m * ro;
	vec3 k  = abs( m ) * h;
	vec3 t1 = -n - k;
	vec3 t2 = -n + k;
	float tN = max( max( t1.x, t1.y ), t1.z );
	float tF = min( min( t2.x, t2.y ), t2.z );
	if( tN > tF || tF < 0.0 || tN < 0.0 )
		return vec4( -1.0 );
	vec3 nor = -sign( rd ) * step( t1.yzx, t1.xyz ) * step( t1.zxy, t1.xyz );
	return vec4( tN, nor );
}

//How much a sphere shadows a point toward the light, softly (Quilez).
float sphereShadow( vec3 ro, vec3 rd, vec3 c, float r, float k )
{
	vec3 oc = ro - c;
	float b = dot( oc, rd );
	float h = b * b - ( dot( oc, oc ) - r * r );
	return b > 0.0 ? 1.0 : smoothstep( 0.0, 1.0, h * k / b );
}

//Ambient occlusion of a sphere on a surface (Quilez).
float sphereOcclusion( vec3 p, vec3 n, vec3 c, float r )
{
	vec3 d  = c - p;
	float l = length( d );
	return clamp( dot( n, d ) * r * r / ( l * l * l ), 0.0, 1.0 );
}

//Varnished wood at a point of the surface (metres, in the frame the wood turns
//with), its grain in rings round the origin. Cartesian, not polar: an angle
//wraps at pi, and the grain showed a seam there.
vec3 woodColour( vec2 q, vec3 base )
{
	float g    = fbm2( q * 28.0 );
	float ring = 0.5 + 0.5 * sin( length( q ) * 900.0 + g * 9.0 );
	return base * ( 0.72 + 0.28 * ring ) * ( 0.85 + 0.3 * g );
}

//Lit: the key light, a cool fill from the opposite side, a highlight with
//Fresnel, and the studio in polished things.
vec3 shadeSurface( vec3 albedo, vec3 n, vec3 rd, float shine, float shadow, float ao )
{
	vec3 L    = keyLight();
	vec3 F    = normalize( vec3( -L.x, -L.y, 0.6 ) );
	float dif = max( dot( n, L ), 0.0 ) * shadow;
	float fil = max( dot( n, F ), 0.0 ) * 0.25;
	vec3 h    = normalize( L - rd );
	float spe = pow( max( dot( n, h ), 0.0 ), mix( 16.0, 140.0, shine ) ) * shine * shadow;
	float fre = pow( 1.0 - max( dot( n, -rd ), 0.0 ), 5.0 );
	vec3 env  = studio( reflect( rd, n ).xzy ) * ( 0.04 + 0.5 * fre ) * shine;
	return albedo * ( 0.18 * ao + 0.95 * dif + fil * ao ) + vec3( spe ) * 1.6 + env * ao;
}
)";

//===========================================================================
// Roulette: the wheel. The bowl is a surface of revolution made of cones, each
// intersected exactly; the frets, diamonds, turret and ball are primitives.
//===========================================================================
const char* const kRoulette = R"(
//= mirrored in Roulette.h / Roulette.cpp (kProfile, the constants). The
//physics' profile is vertices 1..8; 0 (the turret's dome) and 9.. (the lip's
//drawn top and the bowl's rim) are drawn only. The physics' lip runs to 0.15 m;
//it is DRAWN to 0.08, above anything the ball reaches (jptest --roulette).
const float BALL_R     = 0.0095;
const float LIP        = 0.395;
const float TRACK_IN   = 0.335;
const float ROTOR_OUT  = 0.255;
const float POCKET_OUT = 0.232;
const float POCKET_IN  = 0.195;
const float DIAMOND_R  = 0.300;
const float STEP_W     = 0.004;
const float POCKET_D   = 0.014;
const float FRET_H     = 0.014;
const float FRET_HALF  = 0.001;
const float APRON_Z    = 0.00575;   //0.25 * ( ROTOR_OUT - POCKET_OUT )
const float STATOR_Z   = 0.034868;  //+ tan 20 * ( TRACK_IN - ROTOR_OUT )
const float TRACK_Z    = 0.049828;  //+ tan 14 * ( LIP - TRACK_IN )
const float TABLE_Z    = -0.06;
const float LIP_TOP    = 0.08;      //the lip as drawn
const int NV = 15;

uniform float RingAngle;     //the numbered ring (and the frets), rad
uniform int   Pockets;       //37 or 38
uniform int   PocketNumber[ 38 ];
uniform int   Deflectors;
uniform vec4  BallAt;        //xyz, w > 0 when the ball is on the wheel
uniform int   Highlight;     //the pocket the ball rests in, lit; -1 none

vec2 profileVertex( int i )
{
	vec2 v[ NV ] = vec2[ NV ]( vec2( 0.0, 0.045 ), vec2( 0.120, 0.0249 ), vec2( POCKET_IN - STEP_W, 0.0 ),
	                           vec2( POCKET_IN, -POCKET_D ), vec2( POCKET_OUT - STEP_W, -POCKET_D ), vec2( POCKET_OUT, 0.0 ),
	                           vec2( ROTOR_OUT, APRON_Z ), vec2( TRACK_IN, STATOR_Z ), vec2( LIP, TRACK_Z ),
	                           vec2( LIP, LIP_TOP ), vec2( 0.43, LIP_TOP ), vec2( 0.445, 0.074 ), vec2( 0.455, 0.055 ),
	                           vec2( 0.46, 0.035 ), vec2( 0.46, TABLE_Z ) );
	return v[ i ];
}

int numberColour( int n )
{
	if( n == 0 || n == 37 )
		return 0;
	int red[ 18 ] = int[ 18 ]( 1, 3, 5, 7, 9, 12, 14, 16, 18, 19, 21, 23, 25, 27, 30, 32, 34, 36 );
	for( int i = 0; i < 18; ++i )
		if( red[ i ] == n )
			return 1;
	return 2;
}

vec3 pocketPaint( int n )
{
	int c = numberColour( n );
	return c == 0 ? toLinear( vec3( 0.05, 0.42, 0.16 ) ) : c == 1 ? toLinear( vec3( 0.66, 0.05, 0.05 ) ) : toLinear( vec3( 0.045, 0.04, 0.04 ) );
}

//The bowl: the nearest cone (t, segment), each segment a frustum of the
//profile between two vertices.
vec2 hitBowl( vec3 ro, vec3 rd )
{
	float best = 1e9;
	float seg  = -1.0;
	for( int i = 0; i + 1 < NV; ++i )
	{
		vec2 a  = profileVertex( i ), b = profileVertex( i + 1 );
		float dr = b.x - a.x, dz = b.y - a.y;
		if( abs( dz ) < 1e-7 )
		{
			//A flat annulus.
			if( abs( rd.z ) < 1e-7 )
				continue;
			float t = ( a.y - ro.z ) / rd.z;
			vec2 q  = ro.xy + rd.xy * t;
			float r = length( q );
			if( t > 1e-4 && t < best && r >= min( a.x, b.x ) && r <= max( a.x, b.x ) )
			{
				best = t;
				seg  = float( i );
			}
			continue;
		}
		//r dz = a.r dz + ( z - a.z ) dr, squared.
		float P  = a.x * dz + ( ro.z - a.y ) * dr;
		float Q  = rd.z * dr;
		float dz2 = dz * dz;
		float A  = dz2 * dot( rd.xy, rd.xy ) - Q * Q;
		float B  = 2.0 * ( dz2 * dot( ro.xy, rd.xy ) - P * Q );
		float C  = dz2 * dot( ro.xy, ro.xy ) - P * P;
		float h  = B * B - 4.0 * A * C;
		if( h < 0.0 || abs( A ) < 1e-12 )
			continue;
		h = sqrt( h );
		for( int k = 0; k < 2; ++k )
		{
			float t = ( -B + ( k == 0 ? -h : h ) ) / ( 2.0 * A );
			if( t <= 1e-4 || t >= best )
				continue;
			float z  = ro.z + rd.z * t;
			float rc = ( P + Q * t ) / dz;
			if( rc < 0.0 || z < min( a.y, b.y ) - 1e-6 || z > max( a.y, b.y ) + 1e-6 )
				continue;
			best = t;
			seg  = float( i );
		}
	}
	return vec2( best, seg );
}

//The profile's normal on segment s, at a point.
vec3 bowlNormal( int s, vec3 p )
{
	vec2 a = profileVertex( s ), b = profileVertex( s + 1 );
	vec2 n = normalize( vec2( -( b.y - a.y ), b.x - a.x ) );
	float r = max( length( p.xy ), 1e-6 );
	return normalize( vec3( n.x * p.xy / r, n.y ) );
}

//The frets near a ray's path through the pocket band: (t, fret index, normal).
//The frets are boxes in their own radial frames at RingAngle + (k + 1/2) pocket,
//from the inner step's foot to the outer step's foot (Roulette.cpp).
vec4 hitFrets( vec3 ro, vec3 rd, float tMax, out vec3 normal )
{
	float pocket = 2.0 * PI / float( Pockets );
	float zTop   = -POCKET_D + FRET_H;
	//Where the ray is in the fret band: the angles at its ends there.
	float t0 = rd.z < 0.0 ? max( ( zTop - ro.z ) / rd.z, 0.0 ) : 0.0;
	float t1 = min( tMax, rd.z < 0.0 ? ( -POCKET_D - ro.z ) / rd.z : tMax );
	if( t1 <= t0 )
		return vec4( -1.0 );
	vec3 p0 = ro + rd * t0, p1 = ro + rd * t1;
	float a0 = ( atan( p0.y, p0.x ) - RingAngle ) / pocket - 0.5;
	float a1 = ( atan( p1.y, p1.x ) - RingAngle ) / pocket - 0.5;
	//The shorter way round between them.
	float span = a1 - a0;
	span -= float( Pockets ) * floor( span / float( Pockets ) + 0.5 );
	float lo = floor( min( a0, a0 + span ) ) - 1.0;
	int count = min( int( abs( span ) ) + 3, 8 );
	vec4 best = vec4( -1.0 );
	float bestT = 1e9;
	for( int j = 0; j < count; ++j )
	{
		float k  = lo + float( j );
		float fa = RingAngle + ( k + 0.5 ) * pocket;
		vec2 ax  = vec2( cos( fa ), sin( fa ) );
		vec2 ay  = vec2( -ax.y, ax.x );
		//The box's frame: x radial, y across, z up; centred on the fret.
		vec3 c   = vec3( ax * ( 0.5 * ( POCKET_IN + POCKET_OUT ) - STEP_W ), 0.5 * ( zTop - POCKET_D ) );
		vec3 o   = ro - c;
		vec3 lo3 = vec3( dot( o.xy, ax ), dot( o.xy, ay ), o.z );
		vec3 ld  = vec3( dot( rd.xy, ax ), dot( rd.xy, ay ), rd.z );
		vec4 h   = hitBox( lo3, ld, vec3( 0.5 * ( POCKET_OUT - POCKET_IN ), FRET_HALF, 0.5 * FRET_H ) );
		if( h.x > 0.0 && h.x < bestT )
		{
			bestT  = h.x;
			normal = vec3( ax * h.y + ay * h.z, h.w );
			best   = vec4( h.x, k, 0.0, 0.0 );
		}
	}
	return best;
}
)";

const char* const kRouletteMain = R"(
const float DIAMOND_Z = 0.0251287; //Height( DIAMOND_R ) + 0.003, mirrored in Roulette.cpp

//The diamonds: capsules on the stator, alternately along and across.
void diamondEnds( int i, out vec3 a, out vec3 b )
{
	float ang = ( float( i ) + 0.5 ) * 2.0 * PI / 8.0;
	vec3 c    = vec3( DIAMOND_R * cos( ang ), DIAMOND_R * sin( ang ), DIAMOND_Z );
	vec3 axis = ( i % 2 == 0 ) ? vec3( cos( ang ), sin( ang ), 0.0 ) : vec3( -sin( ang ), cos( ang ), 0.0 );
	a = c - axis * 0.010;
	b = c + axis * 0.010;
}

//The turret: a spindle, a cap and four arms, turning with the ring. Returns
//the nearest t and its normal.
float hitTurret( vec3 ro, vec3 rd, out vec3 n )
{
	float best = 1e9;
	vec3 a = vec3( 0.0, 0.0, 0.03 ), b = vec3( 0.0, 0.0, 0.082 );
	float t = hitCapsule( ro, rd, a, b, 0.011 );
	if( t > 0.0 && t < best )
	{
		best = t;
		n    = capsuleNormal( ro + rd * t, a, b, 0.011 );
	}
	vec3 cap = vec3( 0.0, 0.0, 0.092 );
	t        = hitSphere( ro, rd, cap, 0.016 );
	if( t > 0.0 && t < best )
	{
		best = t;
		n    = ( ro + rd * t - cap ) / 0.016;
	}
	for( int k = 0; k < 4; ++k )
	{
		float ang = RingAngle + ( float( k ) + 0.5 ) * 0.5 * PI;
		vec3 tip  = vec3( 0.074 * cos( ang ), 0.074 * sin( ang ), 0.074 );
		vec3 root = vec3( 0.0, 0.0, 0.074 );
		t         = hitCapsule( ro, rd, root, tip, 0.0045 );
		if( t > 0.0 && t < best )
		{
			best = t;
			n    = capsuleNormal( ro + rd * t, root, tip, 0.0045 );
		}
		t = hitSphere( ro, rd, tip, 0.0095 );
		if( t > 0.0 && t < best )
		{
			best = t;
			n    = ( ro + rd * t - tip ) / 0.0095;
		}
	}
	return best;
}

//Which numbered pocket an angle is in, and where across it (-1/2 .. 1/2).
int pocketAt( float phi, out float across )
{
	float u = ( phi - RingAngle ) / ( 2.0 * PI / float( Pockets ) );
	float k = floor( u + 0.5 );
	across  = u - k;
	return int( mod( k, float( Pockets ) ) );
}

//A pocket's number printed at local (x across, y outward) in metres.
float printedNumber( int n, vec2 q, float h )
{
	if( n != 37 )
		return numberDistance( n, q, h );
	float adv = GlyphAdv[ 0 ] * h * 0.5;
	return max( numberDistance( 0, q + vec2( adv, 0.0 ), h ), numberDistance( 0, q - vec2( adv, 0.0 ), h ) );
}

const int K_NONE = 0, K_BOWL = 1, K_FRET = 2, K_DIAMOND = 3, K_TURRET = 4, K_BALL = 5, K_TABLE = 6;

vec4 shadeRoulette( vec3 ro, vec3 rd )
{
	vec2 bowl = hitBowl( ro, rd );
	float t   = bowl.x;
	int kind  = bowl.y >= 0.0 ? K_BOWL : K_NONE;
	int seg   = int( bowl.y );
	vec3 n    = vec3( 0.0, 0.0, 1.0 );
	if( kind == K_BOWL )
		n = bowlNormal( seg, ro + rd * t );

	if( TestFlat != 2 )
	{
		vec3 fn;
		vec4 f = hitFrets( ro, rd, t, fn );
		if( f.x > 0.0 && f.x < t )
		{
			t    = f.x;
			kind = K_FRET;
			n    = fn;
		}
		if( Deflectors == 1 )
			for( int i = 0; i < 8; ++i )
			{
				vec3 a, b;
				diamondEnds( i, a, b );
				float d = hitCapsule( ro, rd, a, b, 0.006 );
				if( d > 0.0 && d < t )
				{
					t    = d;
					kind = K_DIAMOND;
					n    = capsuleNormal( ro + rd * d, a, b, 0.006 );
				}
			}
		vec3 tn;
		float tt = hitTurret( ro, rd, tn );
		if( tt < t )
		{
			t    = tt;
			kind = K_TURRET;
			n    = tn;
		}
		if( BallAt.w > 0.0 )
		{
			float bt = hitSphere( ro, rd, BallAt.xyz, BALL_R );
			if( bt > 0.0 && bt < t )
			{
				t    = bt;
				kind = K_BALL;
				n    = ( ro + rd * bt - BallAt.xyz ) / BALL_R;
			}
		}
	}
	if( kind == K_NONE && BackdropKind == BACK_FELT && rd.z < 0.0 )
	{
		t    = ( TABLE_Z - ro.z ) / rd.z;
		kind = K_TABLE;
	}
	HitId = float( kind ) * 1000.0 + float( seg ) * 40.0;
	if( kind == K_NONE )
		return vec4( 0.0 );

	vec3 p     = ro + rd * t;
	float r    = length( p.xy );
	float phi  = atan( p.y, p.x );
	if( kind == K_BOWL && seg >= 2 && seg <= 5 )
	{
		float across;
		HitId += float( pocketAt( phi, across ) );
	}
	vec3 albedo = vec3( 0.5 );
	float shine = 0.3;
	float ao    = 1.0;
	bool glow   = false;

	//The id picture (jptest): the number painted where each ray lands, in G.
	if( TestFlat == 2 )
	{
		if( kind == K_BOWL && seg >= 2 && seg <= 5 )
		{
			float across;
			int j = pocketAt( phi, across );
			return vec4( float( j ) / 64.0, float( PocketNumber[ j ] ) / 64.0, float( seg ) / 16.0, 1.0 );
		}
		return vec4( 0.0, 0.0, 0.0, 1.0 );
	}

	if( kind == K_BOWL )
	{
		vec2 inRotor = vec2( cos( -RingAngle ) * p.x - sin( -RingAngle ) * p.y, sin( -RingAngle ) * p.x + cos( -RingAngle ) * p.y );
		if( seg <= 1 )
		{
			albedo = woodColour( inRotor, toLinear( vec3( 0.62, 0.36, 0.16 ) ) );
			shine  = 0.75;
		}
		else if( seg <= 4 )
		{
			float across;
			int j  = pocketAt( phi, across );
			albedo = pocketPaint( PocketNumber[ j ] ) * 0.85;
			shine  = 0.35;
			//Darker into the corners where the floor meets the steps and frets.
			float wall = min( min( r - POCKET_IN, POCKET_OUT - STEP_W - r ), ( 0.5 - abs( across ) ) * r * 2.0 * PI / float( Pockets ) );
			ao         = 0.45 + 0.55 * smoothstep( 0.0, 0.008, wall );
			glow       = j == Highlight;
		}
		else if( seg == 5 )
		{
			float across;
			int j     = pocketAt( phi, across );
			int num   = PocketNumber[ j ];
			albedo    = pocketPaint( num );
			shine     = 0.6;
			float pkt = 2.0 * PI / float( Pockets );
			float gap = ( 0.5 - abs( across ) ) * pkt * r;
			vec3 gold = toLinear( vec3( 0.95, 0.75, 0.32 ) );
			float pxw = t / ( CamFocal * Resolution.y * 0.5 );
			albedo    = mix( albedo, gold, cover( gap - 0.0007, pxw ) );
			vec2 q    = vec2( -across * pkt * r, r - 0.5 * ( POCKET_OUT + ROTOR_OUT ) );
			float ink = printedNumber( num, q, 0.0105 );
			albedo    = mix( albedo, toLinear( vec3( 0.97, 0.95, 0.88 ) ), cover( -ink, pxw ) );
			glow      = j == Highlight;
		}
		else
		{
			vec3 base = seg == 6 ? toLinear( vec3( 0.28, 0.13, 0.06 ) ) : toLinear( vec3( 0.42, 0.2, 0.08 ) );
			albedo    = woodColour( p.xy, base );
			shine     = seg == 7 ? 0.9 : 0.7;
		}
	}
	else if( kind == K_FRET )
	{
		albedo = toLinear( vec3( 0.85, 0.82, 0.76 ) );
		shine  = 1.0;
	}
	else if( kind == K_DIAMOND || kind == K_TURRET )
	{
		albedo = toLinear( vec3( 0.86, 0.84, 0.8 ) );
		shine  = 1.0;
	}
	else if( kind == K_BALL )
	{
		albedo = toLinear( vec3( 0.95, 0.93, 0.88 ) );
		shine  = 0.9;
	}
	else
	{
		albedo = Felt * ( 0.85 + fbm2( p.xy * 900.0 ) * 0.08 + fbm2( p.xy * 40.0 ) * 0.06 );
		shine  = 0.0;
		//The wheel's contact shadow on the cloth.
		ao     = 0.35 + 0.65 * smoothstep( 0.46, 0.56, r );
	}

	if( TestFlat == 1 )
		return vec4( albedo, 1.0 );

	vec3 L       = keyLight();
	vec3 off     = p + n * 2e-4;
	float shadow = hitBowl( off, L ).x < 1e8 ? 0.0 : 1.0;
	if( BallAt.w > 0.0 && kind != K_BALL )
	{
		shadow *= sphereShadow( off, L, BallAt.xyz, BALL_R, 6.0 );
		ao     *= 1.0 - 0.8 * sphereOcclusion( p, n, BallAt.xyz, BALL_R );
	}
	vec3 c = shadeSurface( albedo, n, rd, shine, shadow, ao );
	if( glow )
		c += albedo * 0.6 * Lights * ( 0.75 + 0.25 * sin( Time * 6.0 ) );
	return vec4( c, 1.0 );
}

void main()
{
	vec2 p    = canvasPoint( gl_FragCoord.xy );
	vec4 game = shadeRoulette( CamPos, cameraRay( gl_FragCoord.xy + ( Samples == 4 ? sampleOffset( 0 ) : vec2( 0.0 ) ) ) );
	bool edge = fwidth( HitId ) > 0.0;
	if( Samples == 4 || ( Samples == 2 && edge ) )
	{
		vec4 sum = Samples == 4 ? game : vec4( 0.0 );
		for( int i = Samples == 4 ? 1 : 0; i < 4; ++i )
			sum += shadeRoulette( CamPos, cameraRay( gl_FragCoord.xy + sampleOffset( i ) ) );
		game = sum / 4.0;
	}
	if( TestFlat == 2 )
	{
		fragColour = game;
		return;
	}
	fragColour = finish( resultBoard( over( game, backdrop( p ) ) ) );
}
)";

//===========================================================================
// The money wheel (Big Six), face on.
//===========================================================================
const char* const kWheel = R"(
//= mirrored in MoneyWheel.h.
const int SEGMENTS   = 54;
const float RIM      = 0.90;
const float PEG_R    = 0.86;
const float PEG_SIZE = 0.008;
const float PIVOT_Y  = 0.96;
const float CLAP_LEN = 0.115;
const float CLAP_HALF = 0.004;

//Spans, = mirrored in Jackpot.cpp (DrawWheel).
const int SPAN_1 = 0, SPAN_2 = 1, SPAN_5 = 2, SPAN_10 = 3, SPAN_20 = 4, SPAN_JOKER = 5, SPAN_LOGO = 6;

uniform float WheelAngle;    //the painted wheel (and its pegs), rad
uniform float ClapperAngle;  //rad, positive swings the tip toward +x
uniform int   Layout[ 54 ];
uniform int   Highlight;     //the segment under the clapper at rest, or -1
uniform int   Celebrate;

const float SCALE  = 0.46;   //canvas units per metre
const vec2 CENTRE  = vec2( 0.0, -0.035 );

vec3 valueColour( int v )
{
	if( v == 1 )  return toLinear( vec3( 0.96, 0.82, 0.18 ) );
	if( v == 2 )  return toLinear( vec3( 0.16, 0.42, 0.86 ) );
	if( v == 5 )  return toLinear( vec3( 0.62, 0.22, 0.72 ) );
	if( v == 10 ) return toLinear( vec3( 0.16, 0.66, 0.30 ) );
	if( v == 20 ) return toLinear( vec3( 0.95, 0.42, 0.10 ) );
	return toLinear( vec3( 0.06, 0.05, 0.05 ) );
}

int valueSpan( int v )
{
	return v == 1 ? SPAN_1 : v == 2 ? SPAN_2 : v == 5 ? SPAN_5 : v == 10 ? SPAN_10 : v == 20 ? SPAN_20 : v == 40 ? SPAN_JOKER : SPAN_LOGO;
}

//A five-pointed star of outer radius r (Quilez).
float sdStar5( vec2 p, float r, float rf )
{
	const vec2 k1 = vec2( 0.809016994375, -0.587785252292 );
	const vec2 k2 = vec2( -k1.x, k1.y );
	p.x = abs( p.x );
	p -= 2.0 * max( dot( k1, p ), 0.0 ) * k1;
	p -= 2.0 * max( dot( k2, p ), 0.0 ) * k2;
	p.x = abs( p.x );
	p.y -= r;
	vec2 ba = rf * vec2( -k1.y, k1.x ) - vec2( 0.0, 1.0 );
	float h = clamp( dot( p, ba ) / dot( ba, ba ), 0.0, r );
	return length( p - ba * h ) * sign( p.y * ba.x - p.x * ba.y );
}

vec4 drawWheel( vec2 p, float px )
{
	vec4 col = vec4( 0.0 );
	vec2 m   = ( p - CENTRE ) / SCALE;  //metres, the hub at the origin
	float mpx = px / SCALE;
	float r  = length( m );
	vec3 L2  = normalize( vec3( cos( LightAngle ), sin( LightAngle ), 1.4 ) );

	//-- the stand: a post down from the hub and a foot ------------------------
	float post = sdBox( m - vec2( 0.0, -0.75 ), vec2( 0.07, 0.75 ), 0.01 );
	col = layer( col, toLinear( vec3( 0.22, 0.12, 0.06 ) ) * ( TestFlat == 1 ? 1.0 : 0.8 + 0.4 * sstep( 0.07, -0.07, m.x ) ), cover( post, mpx ) );

	//-- the rim: a lacquered ring with a chase of lamps ------------------------------
	float rim = abs( r - 0.955 ) - 0.065;
	if( rim < mpx * 2.0 )
	{
		vec3 lacquer = Accent * ( 0.6 + 0.4 * smoothstep( -0.9, 0.9, m.y ) );
		vec3 n       = normalize( vec3( m / max( r, 1e-4 ) * ( r - 0.955 ) / 0.065 * 0.8, 1.0 ) );
		if( TestFlat == 0 )
			lacquer = lacquer * ( 0.35 + 0.75 * max( dot( n, L2 ), 0.0 ) ) + vec3( 0.6 ) * pow( max( dot( reflect( -L2, n ), vec3( 0.0, 0.0, 1.0 ) ), 0.0 ), 40.0 );
		col = layer( col, lacquer, cover( rim, mpx ) );
		//The lamps: 36 round the rim.
		float a   = atan( m.y, m.x );
		float u   = a / ( 2.0 * PI ) * 36.0;
		float k   = floor( u + 0.5 );
		vec2 c    = 0.955 * vec2( cos( k * 2.0 * PI / 36.0 ), sin( k * 2.0 * PI / 36.0 ) );
		float d   = length( m - c );
		int i     = int( mod( k, 36.0 ) );
		float on  = TestFlat == 1 ? 0.0 : Lights * ( Celebrate > 0 ? float( ( i + int( Time * 8.0 ) ) % 2 == 0 ) : float( ( i + int( Time * 3.0 ) ) % 3 == 0 ) );
		vec3 warm = toLinear( vec3( 1.0, 0.86, 0.5 ) );
		col       = layer( col, mix( warm * 0.2, warm * 3.0, on ), cover( d - 0.022, mpx ) );
		col.rgb  += warm * on * 0.35 * exp( -d * d / 0.0016 ) * col.a;
	}

	//-- the face: 54 segments -------------------------------------------------------
	float face = r - 0.89;
	if( face < mpx * 2.0 )
	{
		float seg = 2.0 * PI / float( SEGMENTS );
		float u   = ( atan( m.y, m.x ) - WheelAngle ) / seg;
		float k   = floor( u );
		float f   = u - k;
		int j     = int( mod( k, float( SEGMENTS ) ) );
		int v     = Layout[ j ];
		vec3 c    = valueColour( v );
		//The id picture (jptest): the segment and its value, nothing else.
		if( TestFlat == 2 )
			return layer( col, vec3( float( j ) / 64.0, float( v ) / 64.0, 0.25 ), cover( face, mpx ) );
		//The segment's frame: x outward along its middle, y round.
		float ac  = WheelAngle + ( k + 0.5 ) * seg;
		vec2 er   = vec2( cos( ac ), sin( ac ) );
		vec2 et   = vec2( -er.y, er.x );
		vec2 q    = vec2( dot( m, er ), dot( m, et ) );
		//Gold dividers between segments, and a gold band inside the pegs.
		float div = ( 0.5 - abs( f - 0.5 ) ) * seg * r - 0.0035;
		float bandO = abs( r - 0.80 ) - 0.006;
		float bandI = abs( r - 0.28 ) - 0.006;
		vec3 gold = toLinear( vec3( 0.95, 0.76, 0.3 ) );
		if( v == 40 || v == 45 )
		{
			//A star, pointing outward (it is symmetric across its axis).
			float star = sdStar5( vec2( q.y, q.x - 0.36 ), 0.035, 0.45 );
			c = mix( c, gold, cover( star, mpx ) );
		}
		//The value, one character under another from just inside the band,
		//each upright when its segment is at the top.
		float ink = stackedDistance( valueSpan( v ), vec2( -q.y, q.x - ( v >= 40 ? 0.775 : 0.775 ) ), v >= 40 ? 0.05 : 0.07, 1.12 );
		//Dark print on the light cards ($1, $20), white on the rest.
		vec3 print = v >= 40 ? gold : ( v == 1 || v == 20 ) ? toLinear( vec3( 0.08, 0.05, 0.03 ) ) : vec3( 0.97 );
		c = mix( c, print, cover( -ink, mpx ) );
		c = mix( c, gold, max( cover( div, mpx ), max( cover( bandO, mpx ), cover( bandI, mpx ) ) ) );
		if( j == Highlight && TestFlat == 0 )
			c *= 1.0 + 0.7 * Lights * ( 0.6 + 0.4 * sin( Time * 7.0 ) );
		if( TestFlat == 0 )
		{
			//A gentle dome of light across the face.
			c *= 0.75 + 0.35 * max( dot( normalize( vec3( m * 0.35, 1.0 ) ), L2 ), 0.0 );
		}
		col = layer( col, c, cover( face, mpx ) );
	}
	if( TestFlat == 2 )
		return col;

	//-- the pegs: brass studs between the segments, at PEG_R --------------------------
	{
		float seg = 2.0 * PI / float( SEGMENTS );
		float u   = ( atan( m.y, m.x ) - WheelAngle ) / seg;
		float k   = floor( u + 0.5 );
		float a   = WheelAngle + k * seg;
		vec2 c    = PEG_R * vec2( cos( a ), sin( a ) );
		vec2 d    = ( m - c ) / ( PEG_SIZE * 1.6 );
		float peg = length( m - c ) - PEG_SIZE * 1.6;
		vec3 n    = vec3( d, sqrt( max( 0.0, 1.0 - dot( d, d ) ) ) );
		vec3 brass = toLinear( vec3( 0.9, 0.75, 0.4 ) );
		vec3 pc   = TestFlat == 1 ? brass : brass * ( 0.3 + 0.8 * max( dot( n, L2 ), 0.0 ) ) + vec3( 0.8 ) * pow( max( dot( n, normalize( L2 + vec3( 0.0, 0.0, 1.0 ) ) ), 0.0 ), 30.0 );
		col = layer( col, vec3( 0.0 ), cover( peg - 0.002, mpx ) * 0.35 );
		col = layer( col, pc, cover( peg, mpx ) );
	}

	//-- the hub ------------------------------------------------------------------------------
	{
		float hub = r - 0.24;
		vec2 d    = m / 0.24;
		vec3 n    = vec3( d * 0.6, sqrt( max( 0.0, 1.0 - dot( d * 0.6, d * 0.6 ) ) ) );
		vec3 hc   = TestFlat == 1 ? Accent : Accent * ( 0.3 + 0.8 * max( dot( n, L2 ), 0.0 ) ) + vec3( 0.5 ) * pow( max( dot( reflect( -L2, n ), vec3( 0.0, 0.0, 1.0 ) ), 0.0 ), 24.0 );
		col       = layer( col, hc, cover( hub, mpx ) );
		float st  = sdStar5( m, 0.17, 0.42 );
		col       = layer( col, toLinear( vec3( 1.0, 0.85, 0.35 ) ) * ( TestFlat == 1 ? 1.0 : 0.8 + 0.3 * max( dot( n, L2 ), 0.0 ) ), cover( st, mpx ) );
		float axle = r - 0.035;
		col        = layer( col, TestFlat == 1 ? vec3( 0.7 ) : chrome( normalize( vec3( m / 0.035 * 0.7, 0.7 ) ), vec3( 0.9 ) ), cover( axle, mpx ) );
	}

	//-- the clapper: a leather flap hanging from its bracket at the top ------------------------
	{
		//Drawn as the pointer it is: 26 mm at the pivot tapering to 8 at the tip.
		//(The physics' contact is its 8 mm edge; the taper is all above the pegs.)
		vec2 pivot = vec2( 0.0, PIVOT_Y );
		vec2 tip   = pivot + CLAP_LEN * vec2( sin( ClapperAngle ), -cos( ClapperAngle ) );
		float along = clamp( dot( m - tip, pivot - tip ) / ( CLAP_LEN * CLAP_LEN ), 0.0, 1.0 );
		float flap = sdSegment( m, pivot, tip, CLAP_HALF + 0.009 * along );
		col = layer( col, vec3( 0.0 ), cover( flap - 0.006, mpx ) * 0.45 );
		vec3 leather = toLinear( vec3( 0.62, 0.38, 0.18 ) ) * ( TestFlat == 1 ? 1.0 : 0.75 + 0.5 * along );
		col = layer( col, leather, cover( flap, mpx ) );
		float bracket = sdBox( m - vec2( 0.0, PIVOT_Y + 0.05 ), vec2( 0.032, 0.04 ), 0.01 );
		col = layer( col, TestFlat == 1 ? vec3( 0.6 ) : chrome( vec3( 0.0, 0.5, 0.85 ), vec3( 0.85 ) ), cover( bracket, mpx ) );
		float bolt = length( m - pivot ) - 0.012;
		col = layer( col, TestFlat == 1 ? vec3( 0.8 ) : chrome( normalize( vec3( ( m - pivot ) / 0.012 * 0.7, 0.7 ) ), vec3( 1.0 ) ), cover( bolt, mpx ) );
	}
	return col;
}

void main()
{
	vec2 p   = canvasPoint( gl_FragCoord.xy );
	vec4 col = drawWheel( p, canvasPixel() );
	if( TestFlat == 2 )
	{
		fragColour = col;
		return;
	}
	fragColour = finish( resultBoard( over( col, backdrop( p ) ) ) );
}
)";

//===========================================================================
// Craps: the end of the table by the back wall, the layout, the dice.
// Drawn in the 3D games' axes (z up). The physics' axes (y up, the back wall
// at z = -0.20) map as (x, y, z) -> (x, -z, y): Jackpot.cpp converts.
//===========================================================================
const char* const kCraps = R"(
//= mirrored in Craps.h.
const float BACK_Y   = 0.20;   //the back wall (physics z = -0.20)
const float NEAR_Y   = -0.34;
const float SIDE_X   = 0.30;
const float WALL_H   = 0.09;
const float PYR_PITCH = 0.020;
const float PYR_H    = 0.006;

//Spans, = mirrored in Jackpot.cpp (DrawCraps).
const int SP_SIX = 0, SP_NINE = 1, SP_COME = 2, SP_FIELD = 3, SP_PASS = 4, SP_DONT = 5, SP_ON = 6, SP_OFF = 7, SP_FIELDNUM = 8;

uniform mat3  DieRot[ 4 ];   //body to world: the two dice, then last roll's two being swept
uniform vec3  DiePos[ 4 ];
uniform int   DieOn[ 4 ];
uniform float DieHalf;       //half the side
uniform int   FaceValue[ 6 ];//the value on the +x -x +y -y +z -z faces, body frame
uniform int   Point;         //0: the puck is OFF
uniform int   Puck;          //0: no puck on the layout
uniform int   Pyramids;
uniform int   Celebrate;

//-- the layout, as distances on the felt (metres; + inside the ink) --------------
float lineBox( vec2 p, vec2 lo, vec2 hi, float w )
{
	vec2 c = 0.5 * ( lo + hi ), h = 0.5 * ( hi - lo );
	return w - abs( sdBox( p - c, h, 0.0 ) );
}

//The point boxes' labels: 4 5 SIX 8 NINE 10.
float pointLabel( int i, vec2 q, float h )
{
	if( i == 2 )
		return textDistance( SP_SIX, q, h );
	if( i == 4 )
		return textDistance( SP_NINE, q, h );
	int v = i == 0 ? 4 : i == 1 ? 5 : i == 3 ? 8 : 10;
	return numberDistance( v, q, h );
}

int pointOfBox( int i )
{
	return i == 0 ? 4 : i == 1 ? 5 : i == 2 ? 6 : i == 3 ? 8 : i == 4 ? 9 : 10;
}

//The felt's colour at p (table coordinates), the layout printed on it.
vec3 feltLayout( vec2 p, float pxm )
{
	vec3 cloth = Felt * ( 0.86 + fbm2( p * 900.0 ) * 0.07 + fbm2( p * 30.0 ) * 0.07 );
	vec3 white = toLinear( vec3( 0.95, 0.94, 0.86 ) );
	vec3 yellow = toLinear( vec3( 0.98, 0.82, 0.25 ) );
	vec3 red   = toLinear( vec3( 0.86, 0.12, 0.1 ) );
	vec3 c     = cloth;
	float w    = 0.0016;
	//The point boxes.
	for( int i = 0; i < 6; ++i )
	{
		vec2 lo = vec2( -0.27 + 0.09 * float( i ), 0.065 );
		vec2 hi = lo + vec2( 0.09, 0.1 );
		c = mix( c, white, cover( -lineBox( p, lo, hi, w ), pxm ) );
		float ink = pointLabel( i, p - vec2( 0.5 * ( lo.x + hi.x ), 0.095 ), 0.032 );
		c = mix( c, white, cover( -ink, pxm ) );
	}
	//COME, FIELD, DON'T PASS BAR, PASS LINE.
	c = mix( c, white, cover( -lineBox( p, vec2( -0.27, -0.005 ), vec2( 0.27, 0.065 ), w ), pxm ) );
	c = mix( c, red, cover( -textDistance( SP_COME, p - vec2( 0.0, 0.03 ), 0.04 ), pxm ) );
	c = mix( c, white, cover( -lineBox( p, vec2( -0.27, -0.1 ), vec2( 0.27, -0.005 ), w ), pxm ) );
	c = mix( c, yellow, cover( -textDistance( SP_FIELD, p - vec2( 0.0, -0.035 ), 0.032 ), pxm ) );
	c = mix( c, white, cover( -textDistance( SP_FIELDNUM, p - vec2( 0.0, -0.077 ), 0.016 ), pxm ) );
	c = mix( c, white, cover( -lineBox( p, vec2( -0.27, -0.155 ), vec2( 0.27, -0.1 ), w ), pxm ) );
	c = mix( c, white, cover( -textDistance( SP_DONT, p - vec2( 0.0, -0.128 ), 0.022 ), pxm ) );
	c = mix( c, white, cover( -lineBox( p, vec2( -0.27, -0.25 ), vec2( 0.27, -0.155 ), w ), pxm ) );
	c = mix( c, white, cover( -textDistance( SP_PASS, p - vec2( 0.0, -0.203 ), 0.04 ), pxm ) );
	return c;
}

//-- the dice: rounded cubes ------------------------------------------------------
float dieDistance( vec3 q )
{
	float r = DieHalf * 0.22;
	vec3 d  = abs( q ) - vec3( DieHalf - r );
	return length( max( d, 0.0 ) ) + min( max( d.x, max( d.y, d.z ) ), 0.0 ) - r;
}

//The nearest die: (t, which), t < 0 none. March inside each one's bounding sphere.
vec2 hitDice( vec3 ro, vec3 rd )
{
	vec2 best = vec2( -1.0 );
	float bound = DieHalf * 1.75;
	for( int i = 0; i < 4; ++i )
	{
		if( DieOn[ i ] == 0 )
			continue;
		vec3 oc = ro - DiePos[ i ];
		float b = dot( oc, rd );
		float h = b * b - ( dot( oc, oc ) - bound * bound );
		if( h < 0.0 )
			continue;
		h        = sqrt( h );
		float t  = max( -b - h, 0.0 );
		float t1 = -b + h;
		mat3 inv = transpose( DieRot[ i ] );
		vec3 lo  = inv * ( ro + rd * t - DiePos[ i ] );
		vec3 ld  = inv * rd;
		for( int k = 0; k < 48 && t < t1; ++k )
		{
			float d = dieDistance( lo );
			if( d < 1e-6 )
			{
				if( best.x < 0.0 || t < best.x )
					best = vec2( t, float( i ) );
				break;
			}
			t  += d;
			lo += ld * d;
		}
	}
	return best;
}

//The pips of a face showing v, at face coordinates q (metres): + inside.
float pipDistance( vec2 q, int v )
{
	float o = DieHalf * 0.56, r = DieHalf * 0.2;
	float best = 1e3;
	if( v == 1 || v == 3 || v == 5 )
		best = min( best, length( q ) );
	if( v >= 2 )
		best = min( best, min( length( q - vec2( -o, o ) ), length( q - vec2( o, -o ) ) ) );
	if( v >= 4 )
		best = min( best, min( length( q - vec2( o, o ) ), length( q - vec2( -o, -o ) ) ) );
	if( v == 6 )
		best = min( best, min( length( q - vec2( -o, 0.0 ) ), length( q - vec2( o, 0.0 ) ) ) );
	return r - best;
}

//A die's colour at body point q with body normal n: translucent red and white pips.
vec3 dieAlbedo( vec3 q, vec3 n, float pxm )
{
	vec3 a = abs( n );
	int f;
	vec2 uv;
	if( a.x >= a.y && a.x >= a.z )
	{
		f  = n.x > 0.0 ? 0 : 1;
		uv = vec2( q.y, q.z );
	}
	else if( a.y >= a.z )
	{
		f  = n.y > 0.0 ? 2 : 3;
		uv = vec2( q.z, q.x );
	}
	else
	{
		f  = n.z > 0.0 ? 4 : 5;
		uv = vec2( q.x, q.y );
	}
	float pip = pipDistance( uv, FaceValue[ f ] );
	vec3 red  = toLinear( vec3( 0.78, 0.04, 0.06 ) );
	return mix( red, vec3( 0.95 ), cover( -pip, pxm ) );
}
)";

const char* const kCrapsMain = R"(
const int C_NONE = 0, C_FELT = 1, C_WALL = 2, C_RAIL = 3, C_DIE = 4, C_PUCK = 5, C_FLOOR = 6;

vec4 shadeCraps( vec3 ro, vec3 rd )
{
	float t  = 1e9;
	int kind = C_NONE;
	vec3 n   = vec3( 0.0, 0.0, 1.0 );
	int die  = -1;

	//The felt.
	if( rd.z < 0.0 )
	{
		float tf = -ro.z / rd.z;
		vec3 p   = ro + rd * tf;
		if( abs( p.x ) <= SIDE_X && p.y <= BACK_Y && p.y >= NEAR_Y )
		{
			t    = tf;
			kind = C_FELT;
		}
	}
	//The back wall, facing the shooter.
	if( rd.y > 0.0 )
	{
		float tw = ( BACK_Y - ro.y ) / rd.y;
		vec3 p   = ro + rd * tw;
		if( tw < t && abs( p.x ) <= SIDE_X && p.z >= 0.0 && p.z <= WALL_H )
		{
			t    = tw;
			kind = C_WALL;
			n    = vec3( 0.0, -1.0, 0.0 );
		}
	}
	//The rails: padded boxes along the sides and the back.
	for( int i = 0; i < 3; ++i )
	{
		vec3 c = i == 0 ? vec3( -SIDE_X - 0.025, -0.07, 0.5 * WALL_H ) : i == 1 ? vec3( SIDE_X + 0.025, -0.07, 0.5 * WALL_H ) : vec3( 0.0, BACK_Y + 0.025, 0.5 * WALL_H );
		vec3 h = i == 2 ? vec3( SIDE_X + 0.05, 0.025, 0.5 * WALL_H ) : vec3( 0.025, 0.29, 0.5 * WALL_H );
		//The padded top: a capsule along the box's top edge.
		vec3 a = c + vec3( i == 2 ? -h.x : 0.0, i == 2 ? 0.0 : -h.y, h.z );
		vec3 e = c + vec3( i == 2 ? h.x : 0.0, i == 2 ? 0.0 : h.y, h.z );
		float pad = hitCapsule( ro, rd, a, e, 0.03 );
		if( pad > 0.0 && pad < t )
		{
			t    = pad;
			kind = C_RAIL;
			n    = capsuleNormal( ro + rd * pad, a, e, 0.03 );
		}
		vec4 b = hitBox( ro - c, rd, h );
		if( b.x > 0.0 && b.x < t )
		{
			t    = b.x;
			kind = C_RAIL;
			n    = b.yzw;
		}
	}
	//The dice.
	vec2 d = hitDice( ro, rd );
	if( d.x > 0.0 && d.x < t )
	{
		t    = d.x;
		kind = C_DIE;
		die  = int( d.y );
	}
	//The puck: ON on the point's box, OFF in the corner of the come.
	vec3 puckAt = Point == 0 ? vec3( -0.235, 0.03, 0.0 ) : vec3( -0.225 + 0.09 * float( Point == 4 ? 0 : Point == 5 ? 1 : Point == 6 ? 2 : Point == 8 ? 3 : Point == 9 ? 4 : 5 ), 0.14, 0.0 );
	if( Puck == 1 )
	{
		vec4 pk = hitCylinder( ro, rd, puckAt, puckAt + vec3( 0.0, 0.0, 0.008 ), 0.022 );
		if( pk.x > 0.0 && pk.x < t )
		{
			t    = pk.x;
			kind = C_PUCK;
			n    = pk.yzw;
		}
	}
	if( kind == C_NONE && BackdropKind == BACK_FELT && rd.z < 0.0 )
	{
		t    = ( -0.05 - ro.z ) / rd.z;
		kind = C_FLOOR;
	}
	HitId = float( kind ) * 16.0 + float( die );
	if( kind == C_NONE )
		return vec4( 0.0 );

	vec3 p      = ro + rd * t;
	float pxm   = t / ( CamFocal * Resolution.y * 0.5 );
	vec3 albedo = vec3( 0.5 );
	float shine = 0.2;
	if( kind == C_FELT )
		albedo = feltLayout( p.xy, pxm );
	else if( kind == C_WALL )
	{
		albedo = toLinear( vec3( 0.12, 0.11, 0.1 ) );
		if( Pyramids == 1 )
		{
			//Square pyramids, PYR_PITCH across: the face under p leans its normal.
			vec2 a  = vec2( p.x, p.z ) / PYR_PITCH;
			vec2 f  = a - floor( a + 0.5 );
			float s = 2.0 * PYR_H / PYR_PITCH;
			n = abs( f.x ) > abs( f.y ) ? normalize( vec3( s * sign( f.x ), -1.0, 0.0 ) ) : normalize( vec3( 0.0, -1.0, s * sign( f.y ) ) );
			shine = 0.25;
		}
	}
	else if( kind == C_RAIL )
	{
		//Padded leather above, varnished wood below.
		bool padded = p.z > WALL_H - 0.012;
		albedo = padded ? toLinear( vec3( 0.07, 0.045, 0.035 ) ) * ( 0.9 + 0.2 * fbm2( p.xy * 300.0 ) )
		                : woodColour( vec2( p.x + p.y, p.z ) + vec2( 3.0 ), toLinear( vec3( 0.36, 0.17, 0.07 ) ) );
		shine  = padded ? 0.45 : 0.7;
	}
	else if( kind == C_DIE )
	{
		mat3 inv = transpose( DieRot[ die ] );
		vec3 q   = inv * ( p - DiePos[ die ] );
		float e  = DieHalf * 1e-3;
		vec3 g   = vec3( dieDistance( q + vec3( e, 0, 0 ) ) - dieDistance( q - vec3( e, 0, 0 ) ),
		                 dieDistance( q + vec3( 0, e, 0 ) ) - dieDistance( q - vec3( 0, e, 0 ) ),
		                 dieDistance( q + vec3( 0, 0, e ) ) - dieDistance( q - vec3( 0, 0, e ) ) );
		vec3 nb  = normalize( g );
		n        = DieRot[ die ] * nb;
		albedo   = dieAlbedo( q, nb, pxm );
		shine    = 0.85;
	}
	else if( kind == C_PUCK )
	{
		bool on   = Point != 0;
		albedo    = on ? vec3( 0.92 ) : vec3( 0.03 );
		vec2 q    = ( p - puckAt ).xy;
		float ink = textDistance( on ? SP_ON : SP_OFF, q, 0.013 );
		albedo    = mix( albedo, on ? vec3( 0.02 ) : vec3( 0.92 ), cover( -ink, pxm ) * step( 0.5, n.z ) );
		shine     = 0.5;
	}
	else
	{
		albedo = toLinear( vec3( 0.05, 0.04, 0.035 ) );
	}
	if( TestFlat == 1 )
		return vec4( albedo, 1.0 );

	//Shadows: the dice (soft, from their bounding spheres) and the walls.
	vec3 L       = keyLight();
	float shadow = 1.0;
	float ao     = 1.0;
	for( int i = 0; i < 4; ++i )
		if( DieOn[ i ] == 1 && !( kind == C_DIE && die == i ) )
		{
			shadow *= sphereShadow( p + n * 1e-4, L, DiePos[ i ], DieHalf * 1.1, 20.0 );
			ao     *= 1.0 - 0.7 * sphereOcclusion( p, n, DiePos[ i ], DieHalf * 1.1 );
		}
	if( kind == C_FELT )
		ao *= 0.55 + 0.45 * smoothstep( 0.0, 0.05, BACK_Y - p.y ) * smoothstep( 0.0, 0.05, SIDE_X - abs( p.x ) );
	vec3 c = shadeSurface( albedo, n, rd, shine, shadow, ao );
	if( kind == C_DIE )
	{
		//Light through the red resin.
		c += albedo * toLinear( vec3( 1.0, 0.2, 0.15 ) ) * 0.35 * ( 1.0 - max( dot( n, -rd ), 0.0 ) );
	}
	return vec4( c, 1.0 );
}

void main()
{
	vec2 p    = canvasPoint( gl_FragCoord.xy );
	vec4 game = shadeCraps( CamPos, cameraRay( gl_FragCoord.xy + ( Samples == 4 ? sampleOffset( 0 ) : vec2( 0.0 ) ) ) );
	bool edge = fwidth( HitId ) > 0.0;
	if( Samples == 4 || ( Samples == 2 && edge ) )
	{
		vec4 sum = Samples == 4 ? game : vec4( 0.0 );
		for( int i = Samples == 4 ? 1 : 0; i < 4; ++i )
			sum += shadeCraps( CamPos, cameraRay( gl_FragCoord.xy + sampleOffset( i ) ) );
		game = sum / 4.0;
	}
	fragColour = finish( resultBoard( over( game, backdrop( p ) ) ) );
}
)";

//===========================================================================
// The lottery drum: a glass sphere of balls, the tube up and the rack along.
//===========================================================================
const char* const kLottery = R"(
//= mirrored in Lottery.h.
const float DRUM    = 0.24;
const float BALL    = 0.021;
const float MOUTH   = 0.034;
const float RACK_Z  = 0.34;      //DRUM + 0.10
const float RACK_X0 = 0.075;
const float RACK_PITCH = 0.047;
const float TUBE_R  = 0.037;
const float RAIL_R  = 0.026;
const float RAIL_END = 0.40;

uniform sampler2D BallData;      //rows: (x y z number), (quaternion w x y z), (screen x y radius px, 0)
uniform int   BallCount;
uniform float Air;               //0..1: the blower

//The UK lotto's colours by tens: white, blue, pink, green, yellow, purple, and orange beyond.
vec3 bandColour( int n )
{
	int b = clamp( n / 10, 0, 6 );
	vec3 c[ 7 ] = vec3[ 7 ]( vec3( 0.95, 0.95, 0.93 ), vec3( 0.2, 0.45, 0.95 ), vec3( 0.98, 0.45, 0.68 ), vec3( 0.25, 0.75, 0.35 ),
	                          vec3( 0.98, 0.84, 0.2 ), vec3( 0.6, 0.35, 0.85 ), vec3( 0.98, 0.55, 0.15 ) );
	return toLinear( c[ b ] );
}

mat3 quatMatrix( vec4 q )
{
	float w = q.x, x = q.y, y = q.z, z = q.w;
	return mat3( 1.0 - 2.0 * ( y * y + z * z ), 2.0 * ( x * y + w * z ), 2.0 * ( x * z - w * y ),
	             2.0 * ( x * y - w * z ), 1.0 - 2.0 * ( x * x + z * z ), 2.0 * ( y * z + w * x ),
	             2.0 * ( x * z + w * y ), 2.0 * ( y * z - w * x ), 1.0 - 2.0 * ( x * x + y * y ) );
}

//The side of a cylinder along z from z0 to z1 (no caps): the nearest t > 0.
float hitTubeZ( vec3 ro, vec3 rd, vec2 c, float r, float z0, float z1, out vec3 n )
{
	vec2 o  = ro.xy - c;
	float a = dot( rd.xy, rd.xy );
	float b = dot( o, rd.xy );
	float h = b * b - a * ( dot( o, o ) - r * r );
	if( h < 0.0 || a < 1e-12 )
		return -1.0;
	h = sqrt( h );
	for( int k = 0; k < 2; ++k )
	{
		float t = ( -b + ( k == 0 ? -h : h ) ) / a;
		float z = ro.z + rd.z * t;
		if( t > 1e-4 && z >= z0 && z <= z1 )
		{
			n = vec3( ( o + rd.xy * t ) / r, 0.0 );
			return t;
		}
	}
	return -1.0;
}

//The same along x, at height zc, from x0 to x1.
float hitTubeX( vec3 ro, vec3 rd, float zc, float r, float x0, float x1, out vec3 n )
{
	vec2 o  = vec2( ro.y, ro.z - zc );
	vec2 d  = rd.yz;
	float a = dot( d, d );
	float b = dot( o, d );
	float h = b * b - a * ( dot( o, o ) - r * r );
	if( h < 0.0 || a < 1e-12 )
		return -1.0;
	h = sqrt( h );
	for( int k = 0; k < 2; ++k )
	{
		float t = ( -b + ( k == 0 ? -h : h ) ) / a;
		float x = ro.x + rd.x * t;
		if( t > 1e-4 && x >= x0 && x <= x1 )
		{
			vec2 q = ( o + d * t ) / r;
			n      = vec3( 0.0, q );
			return t;
		}
	}
	return -1.0;
}

//Glass: what a pane adds over what is behind it (premultiplied), from Fresnel.
vec4 glassOver( vec4 behind, vec3 n, vec3 rd, float strength )
{
	if( dot( n, rd ) > 0.0 )
		n = -n;
	float fre  = 0.04 + 0.96 * pow( 1.0 - abs( dot( n, rd ) ), 5.0 );
	vec3 refl  = studio( reflect( rd, n ).xzy ) * 0.9;
	vec3 L     = keyLight();
	float spec = pow( max( dot( n, normalize( L - rd ) ), 0.0 ), 300.0 ) * 1.4;
	float a    = clamp( ( fre + 0.03 ) * strength, 0.0, 1.0 );
	vec3 tint  = vec3( 0.93, 0.97, 1.0 );
	return vec4( behind.rgb * tint * ( 1.0 - a ) + ( refl * fre + vec3( spec ) ) * strength, max( behind.a, a ) );
}
)";

const char* const kLotteryMain = R"(
const int MAX_NEAR = 10;

vec4 shadeLottery( vec3 ro, vec3 rd, int nearCount, int nearList[ MAX_NEAR ] )
{
	float t  = 1e9;
	vec3 n   = vec3( 0.0, 0.0, 1.0 );
	int kind = 0;   //0 none, 1 ball, 2 stand, 3 band, 4 floor
	int ball = -1;

	for( int k = 0; k < nearCount; ++k )
	{
		int i   = nearList[ k ];
		vec4 b  = texelFetch( BallData, ivec2( i, 0 ), 0 );
		float h = hitSphere( ro, rd, b.xyz, BALL );
		if( h > 0.0 && h < t )
		{
			t    = h;
			kind = 1;
			ball = i;
			n    = ( ro + rd * h - b.xyz ) / BALL;
		}
	}
	//The stand: the blower's housing and the plinth.
	vec4 s1 = hitCylinder( ro, rd, vec3( 0.0, 0.0, -0.33 ), vec3( 0.0, 0.0, -0.225 ), 0.11 );
	if( s1.x > 0.0 && s1.x < t )
	{
		t    = s1.x;
		kind = 2;
		n    = s1.yzw;
	}
	vec4 s2 = hitCylinder( ro, rd, vec3( 0.0, 0.0, -0.38 ), vec3( 0.0, 0.0, -0.33 ), 0.24 );
	if( s2.x > 0.0 && s2.x < t )
	{
		t    = s2.x;
		kind = 2;
		n    = s2.yzw;
	}
	//The chrome band round the drum's equator, and the collar at the mouth.
	vec3 bn;
	float bt = hitTubeZ( ro, rd, vec2( 0.0 ), DRUM + 0.006, -0.009, 0.009, bn );
	if( bt > 0.0 && bt < t )
	{
		t    = bt;
		kind = 3;
		n    = bn;
	}
	vec4 col = hitCylinder( ro, rd, vec3( 0.0, 0.0, DRUM - 0.012 ), vec3( 0.0, 0.0, DRUM + 0.012 ), TUBE_R + 0.008 );
	if( col.x > 0.0 && col.x < t && abs( col.w ) < 0.5 )
	{
		t    = col.x;
		kind = 3;
		n    = col.yzw;
	}
	if( kind == 0 && BackdropKind == BACK_FELT && rd.z < 0.0 )
	{
		t    = ( -0.38 - ro.z ) / rd.z;
		kind = 4;
	}

	HitId = float( kind ) * 256.0 + float( ball );
	vec4 c = vec4( 0.0 );
	if( kind != 0 )
	{
		vec3 p      = ro + rd * t;
		vec3 albedo = vec3( 0.5 );
		float shine = 0.5;
		if( kind == 1 )
		{
			vec4 b    = texelFetch( BallData, ivec2( ball, 0 ), 0 );
			vec4 q    = texelFetch( BallData, ivec2( ball, 1 ), 0 );
			int num   = int( b.w + 0.5 );
			mat3 R    = quatMatrix( q );
			vec3 nb   = transpose( R ) * n;
			albedo    = bandColour( num );
			//A white patch on each pole of the body's z, the number on it.
			float pole = abs( nb.z );
			float pxm  = t / ( CamFocal * Resolution.y * 0.5 );
			float edge = ( pole - 0.80 ) * BALL;
			albedo     = mix( albedo, vec3( 0.93 ), cover( -edge, pxm ) );
			vec2 face  = nb.xy * BALL * sign( nb.z );
			face.x    *= sign( nb.z );
			float ink  = numberDistance( num, face, BALL * 0.46 );
			albedo     = mix( albedo, vec3( 0.0 ), cover( -ink - 0.15 * pxm, pxm ) * step( 0.8, pole ) );
			shine      = 0.8;
		}
		else if( kind == 2 || kind == 3 )
		{
			albedo = kind == 3 ? toLinear( vec3( 0.85, 0.83, 0.8 ) ) : toLinear( vec3( 0.16, 0.16, 0.18 ) );
			shine  = kind == 3 ? 1.0 : 0.85;
		}
		else
		{
			albedo = Felt * ( 0.85 + fbm2( p.xy * 600.0 ) * 0.1 );
			shine  = 0.0;
		}
		if( TestFlat == 1 )
			c = vec4( albedo, 1.0 );
		else
		{
			float ao = 1.0;
			if( kind == 4 )
				ao = 0.4 + 0.6 * smoothstep( 0.2, 0.4, length( p.xy ) );
			c = vec4( shadeSurface( albedo, n, rd, shine, 1.0, ao ), 1.0 );
		}
	}
	if( TestFlat == 1 )
		return c;

	//The glass, back to front: the far side of the drum, then its near side,
	//and the tube and the rail, wherever they are in front of what was hit.
	vec3 oc = ro;
	float b = dot( oc, rd );
	float h = b * b - ( dot( oc, oc ) - DRUM * DRUM );
	if( h > 0.0 )
	{
		h        = sqrt( h );
		float t0 = -b - h, t1 = -b + h;
		if( t1 > 0.0 && t1 < t )
			c = glassOver( c, normalize( ro + rd * t1 ), rd, 0.5 );
		if( t0 > 0.0 && t0 < t )
			c = glassOver( c, normalize( ro + rd * t0 ), rd, 1.0 );
	}
	vec3 tn;
	float tt = hitTubeZ( ro, rd, vec2( 0.0 ), TUBE_R, DRUM - 0.004, RACK_Z + RAIL_R, tn );
	if( tt > 0.0 && tt < t )
		c = glassOver( c, tn, rd, 0.8 );
	tt = hitTubeX( ro, rd, RACK_Z, RAIL_R, -0.03, RAIL_END, tn );
	if( tt > 0.0 && tt < t )
		c = glassOver( c, tn, rd, 0.8 );
	return c;
}

void main()
{
	vec2 p = canvasPoint( gl_FragCoord.xy );
	//The balls that can be under this pixel, from their projected discs.
	int nearList[ MAX_NEAR ];
	int nearCount = 0;
	for( int i = 0; i < BallCount && nearCount < MAX_NEAR; ++i )
	{
		vec4 sc = texelFetch( BallData, ivec2( i, 2 ), 0 );
		if( sc.z > 0.0 && length( gl_FragCoord.xy - sc.xy ) < sc.z )
			nearList[ nearCount++ ] = i;
	}
	vec4 game = shadeLottery( CamPos, cameraRay( gl_FragCoord.xy + ( Samples == 4 ? sampleOffset( 0 ) : vec2( 0.0 ) ) ), nearCount, nearList );
	bool edge = fwidth( HitId ) > 0.0;
	if( Samples == 4 || ( Samples == 2 && edge ) )
	{
		vec4 sum = Samples == 4 ? game : vec4( 0.0 );
		for( int i = Samples == 4 ? 1 : 0; i < 4; ++i )
			sum += shadeLottery( CamPos, cameraRay( gl_FragCoord.xy + sampleOffset( i ) ), nearCount, nearList );
		game = sum / 4.0;
	}
	fragColour = finish( resultBoard( over( game, backdrop( p ) ) ) );
}
)";

//===========================================================================
// The shower: one quad per piece, the exact disc found in the fragment.
// The world is in frame heights (y up, the frame from -0.5 to 0.5, z toward
// the viewer), seen straight on, orthographically.
//===========================================================================
const char* const kShowerVertex = R"(
layout( location = 0 ) in vec2 corner;
layout( location = 1 ) in vec4 iPos;   //x y z radius
layout( location = 2 ) in vec4 iQuat;  //w x y z
layout( location = 3 ) in vec4 iMisc;  //half thickness, kind, colour, fade
uniform vec2 Resolution;
uniform mat3 View;          //world to view: a little from above (Jackpot.cpp)
uniform float ViewLift;     //view y added after it
out vec2 uv;
out vec2 world;
flat out vec4 pos;
flat out vec4 quat;
flat out vec4 misc;
void main()
{
	float bound = sqrt( iPos.w * iPos.w + iMisc.x * iMisc.x ) * 1.02;
	vec3 centre = View * iPos.xyz + vec3( 0.0, ViewLift, 0.0 );
	world       = centre.xy + corner * bound;
	pos         = iPos;
	quat        = iQuat;
	misc        = iMisc;
	float aspect = Resolution.x / Resolution.y;
	gl_Position = vec4( world.x / ( 0.5 * aspect ), world.y / 0.5, 0.0, 1.0 );
	uv          = gl_Position.xy * 0.5 + 0.5;
}
)";

const char* const kShowerFragment = R"(
in vec2 world;
flat in vec4 pos;
flat in vec4 quat;
flat in vec4 misc;
uniform mat3 View;
uniform float ViewLift;

mat3 pieceMatrix( vec4 q )
{
	float w = q.x, x = q.y, y = q.z, z = q.w;
	return mat3( 1.0 - 2.0 * ( y * y + z * z ), 2.0 * ( x * y + w * z ), 2.0 * ( x * z - w * y ),
	             2.0 * ( x * y - w * z ), 1.0 - 2.0 * ( x * x + z * z ), 2.0 * ( y * z + w * x ),
	             2.0 * ( x * z + w * y ), 2.0 * ( y * z - w * x ), 1.0 - 2.0 * ( x * x + y * y ) );
}

//The chips' denominations: white, red, green, black, purple, orange.
vec3 chipColour( int c )
{
	vec3 k[ 6 ] = vec3[ 6 ]( vec3( 0.92, 0.91, 0.88 ), vec3( 0.75, 0.06, 0.07 ), vec3( 0.1, 0.55, 0.22 ),
	                          vec3( 0.06, 0.06, 0.07 ), vec3( 0.45, 0.15, 0.62 ), vec3( 0.96, 0.5, 0.1 ) );
	return toLinear( k[ clamp( c, 0, 5 ) ] );
}

void main()
{
	float R    = pos.w, H = misc.x;
	mat3 M     = pieceMatrix( quat );
	mat3 inv   = transpose( M );
	//The ray, straight into the view, taken back to the world, then the body.
	mat3 back  = transpose( View );
	vec3 rdw   = back * vec3( 0.0, 0.0, -1.0 );
	vec3 ro    = inv * ( back * ( vec3( world, 10.0 ) - vec3( 0.0, ViewLift, 0.0 ) ) - pos.xyz );
	vec3 rd    = inv * rdw;
	//The disc: a cylinder along the body's z, radius R, from -H to H.
	float t    = -1.0;
	vec3 nb    = vec3( 0.0 );
	{
		float a = dot( rd.xy, rd.xy ), b = dot( ro.xy, rd.xy ), c = dot( ro.xy, ro.xy ) - R * R;
		float h = b * b - a * c;
		if( h >= 0.0 && a > 1e-12 )
		{
			float ts = ( -b - sqrt( h ) ) / a;
			float z  = ro.z + rd.z * ts;
			if( abs( z ) <= H )
			{
				t  = ts;
				nb = vec3( ( ro.xy + rd.xy * ts ) / R, 0.0 );
			}
		}
		for( int s = -1; s <= 1; s += 2 )
		{
			if( abs( rd.z ) < 1e-9 )
				break;
			float tc = ( float( s ) * H - ro.z ) / rd.z;
			vec2 q   = ro.xy + rd.xy * tc;
			if( dot( q, q ) <= R * R && ( t < 0.0 || tc < t ) )
			{
				t  = tc;
				nb = vec3( 0.0, 0.0, float( s ) );
			}
		}
	}
	if( t < 0.0 )
		discard;
	vec3 lp     = ro + rd * t;
	vec3 n      = M * nb;
	bool coin   = misc.y > 0.5;
	float pxw   = 1.0 / Resolution.y;
	vec3 albedo;
	float shine;
	float rr    = length( lp.xy ) / R;
	float ang   = atan( lp.y, lp.x );
	if( coin )
	{
		//Gold, a raised milled rim and a star struck in the middle.
		albedo = toLinear( vec3( 1.0, 0.78, 0.3 ) );
		shine  = 1.0;
		if( abs( nb.z ) > 0.5 )
		{
			float rim = smoothstep( 0.78, 0.84, rr );
			n         = normalize( M * vec3( lp.xy / R * 0.5 * rim * ( 1.0 - rim ) * 4.0, nb.z ) );
			vec2 q    = lp.xy / R;
			vec2 k1   = vec2( 0.809016994375, -0.587785252292 );
			vec2 k2   = vec2( -k1.x, k1.y );
			q.x = abs( q.x );
			q -= 2.0 * max( dot( k1, q ), 0.0 ) * k1;
			q -= 2.0 * max( dot( k2, q ), 0.0 ) * k2;
			q.x = abs( q.x );
			q.y -= 0.45;
			vec2 ba    = 0.42 * vec2( -k1.y, k1.x ) - vec2( 0.0, 1.0 );
			float hh   = clamp( dot( q, ba ) / dot( ba, ba ), 0.0, 0.45 );
			float star = length( q - ba * hh ) * sign( q.y * ba.x - q.x * ba.y );
			albedo    *= 1.0 - 0.25 * sstep( 0.02, -0.02, star );
		}
		else
			albedo *= 0.75 + 0.25 * step( 0.5, fract( ang * 40.0 / PI ) );
	}
	else
	{
		//A casino chip: the colour, six white inserts round the edge, an inlay.
		vec3 base   = chipColour( int( misc.z + 0.5 ) );
		vec3 insert = int( misc.z + 0.5 ) == 0 ? toLinear( vec3( 0.12, 0.3, 0.75 ) ) : vec3( 0.93 );
		float seg   = fract( ang * 6.0 / ( 2.0 * PI ) + 0.25 );
		bool spot   = seg < 0.32;
		albedo      = base;
		shine       = 0.35;
		if( abs( nb.z ) > 0.5 )
		{
			if( rr > 0.74 && spot )
				albedo = insert;
			float inlay = abs( rr - 0.62 ) - 0.012;
			albedo      = mix( albedo, insert, cover( inlay * R, pxw ) * 0.85 );
			if( rr < 0.55 )
				albedo = mix( base, vec3( 0.95 ), 0.65 );
		}
		else if( spot )
			albedo = insert;
	}
	vec3 c   = TestFlat == 1 ? albedo : shadeSurface( albedo, normalize( n ), rdw, shine, 1.0, 1.0 );
	float a  = clamp( misc.w, 0.0, 1.0 );
	fragColour = vec4( c * a, a );
	//Depth from the hit: nearer (larger z) is smaller.
	vec3 hit     = View * ( M * lp + pos.xyz );
	gl_FragDepth = clamp( 0.5 - hit.z / 4.0, 0.0, 1.0 );
}
)";

const char* const kComposite = R"(
in vec2 uv;
out vec4 fragColour;
uniform sampler2D Pieces;
uniform float Strength;   //Mix, on the effect
void main()
{
	vec4 s        = texture( Pieces, uv );
	vec3 straight = s.a > 0.0 ? s.rgb / s.a : vec3( 0.0 );
	vec3 shown    = pow( max( straight, 0.0 ), vec3( 1.0 / 2.2 ) );
	fragColour    = vec4( shown, 1.0 ) * s.a * Strength;
}
)";

} // namespace jackpot::shaders
