/**
    jptest -- render Jackpot offline, and measure what its games are doing.

    It drives the REAL plugin class, through the same ProcessOpenGL a host
    calls, on a synthetic 60 fps clock, in a headless CGL context, with the
    planners run on the render thread (`SetSynchronousForTest`) so a play starts
    on the frame it is asked for and every run is the same run. The physics and
    outcome checks read the plan the plugin is drawing; the picture checks read
    the pixels the shipped shaders drew.

        jptest --out /tmp/jackpot.png              the source, after a play
        jptest --over --out /tmp/o.png             the Over effect on the harness's card
        jptest --list                              every parameter and its default
        jptest --film N                            N frames, raw RGBA on stdout
        jptest --pipe                              raw frames in (Over), raw frames out
        jptest --offline                           the checks that need no GL context (CI)

    `--script` is the fleet's cue format: `frame  Parameter Name  value` lines,
    held before the first key and after the last, linearly interpolated
    between. A button press is three keys (0, 1, 0).

    The claims, one flag each -- see README "Building and testing".
*/

#include "Controls.h"
#include "Craps.h"
#include "Jackpot.h"
#include "Lottery.h"
#include "MoneyWheel.h"
#include "Physics.h"
#include "Roulette.h"
#include "Shaders.h"
#include "Shower.h"
#include "Slots.h"
#include "SymbolArt.h"
#include "Typeface.h"

#include <OpenGL/OpenGL.h>
#include <OpenGL/gl3.h>
#include <zlib.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <unistd.h>
#include <vector>

using namespace jackpot;
using dice::AxisAngle;
using dice::FromMatrix;
using dice::ToMatrix;
using dice::Length;
using dice::Dot;
using dice::Normalise;
using dice::Transpose;

namespace
{
using Floats = std::vector< float >;
using Bytes  = std::vector< unsigned char >;

//---------------------------------------------------------------------------
// Reporting.
//---------------------------------------------------------------------------
int g_failures = 0;

std::string fmt( const char* format, ... )
{
	char buffer[ 2048 ];
	va_list args;
	va_start( args, format );
	std::vsnprintf( buffer, sizeof( buffer ), format, args );
	va_end( args );
	return buffer;
}

void Check( bool condition, const std::string& message )
{
	std::printf( "  %s  %s\n", condition ? "ok  " : "FAIL", message.c_str() );
	if( !condition )
		++g_failures;
}

int Verdict()
{
	std::printf( "\n  %s\n", g_failures == 0 ? "PASS" : "FAIL" );
	return g_failures == 0 ? 0 : 1;
}

//---------------------------------------------------------------------------
// PNG and context: boreal's harness, unchanged.
//---------------------------------------------------------------------------
void putU32( Bytes& out, uint32_t value )
{
	out.push_back( static_cast< unsigned char >( value >> 24 ) );
	out.push_back( static_cast< unsigned char >( value >> 16 ) );
	out.push_back( static_cast< unsigned char >( value >> 8 ) );
	out.push_back( static_cast< unsigned char >( value ) );
}

void putChunk( Bytes& out, const char* type, const Bytes& data )
{
	putU32( out, static_cast< uint32_t >( data.size() ) );
	const size_t start = out.size();
	out.insert( out.end(), type, type + 4 );
	out.insert( out.end(), data.begin(), data.end() );
	uLong crc = crc32( 0L, Z_NULL, 0 );
	crc       = crc32( crc, out.data() + start, static_cast< uInt >( 4 + data.size() ) );
	putU32( out, static_cast< uint32_t >( crc ) );
}

/// `rgba` is floats, row 0 at the BOTTOM (GL's order); the file is written top
/// row first, which is the only place anything here flips.
bool writePng( const std::string& path, int width, int height, const Floats& rgba )
{
	Bytes raw;
	raw.reserve( static_cast< size_t >( height ) * ( 1 + static_cast< size_t >( width ) * 4 ) );
	for( int y = height - 1; y >= 0; --y )
	{
		raw.push_back( 0 );
		for( int x = 0; x < width; ++x )
			for( int c = 0; c < 4; ++c )
			{
				const float v = rgba[ ( static_cast< size_t >( y ) * width + x ) * 4 + c ];
				raw.push_back( static_cast< unsigned char >( std::lround( std::clamp( v, 0.0f, 1.0f ) * 255.0f ) ) );
			}
	}

	uLongf compressedSize = compressBound( static_cast< uLong >( raw.size() ) );
	Bytes compressed( compressedSize );
	if( compress2( compressed.data(), &compressedSize, raw.data(), static_cast< uLong >( raw.size() ), 6 ) != Z_OK )
		return false;
	compressed.resize( compressedSize );

	Bytes png = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' };
	Bytes ihdr;
	putU32( ihdr, static_cast< uint32_t >( width ) );
	putU32( ihdr, static_cast< uint32_t >( height ) );
	ihdr.insert( ihdr.end(), { 8, 6, 0, 0, 0 } );
	putChunk( png, "IHDR", ihdr );
	putChunk( png, "IDAT", compressed );
	putChunk( png, "IEND", {} );

	FILE* file = fopen( path.c_str(), "wb" );
	if( file == nullptr )
		return false;
	const size_t written = fwrite( png.data(), 1, png.size(), file );
	fclose( file );
	return written == png.size();
}

CGLContextObj createContext()
{
	//Accelerated first; fall back so the harness still runs somewhere without
	//a GPU, where it will at least prove the shaders compile.
	const CGLPixelFormatAttribute accelerated[] = {
		kCGLPFAOpenGLProfile, static_cast< CGLPixelFormatAttribute >( kCGLOGLPVersion_GL4_Core ),
		kCGLPFAAccelerated,
		kCGLPFAColorSize, static_cast< CGLPixelFormatAttribute >( 24 ),
		kCGLPFAAlphaSize, static_cast< CGLPixelFormatAttribute >( 8 ),
		static_cast< CGLPixelFormatAttribute >( 0 )
	};
	const CGLPixelFormatAttribute software[] = {
		kCGLPFAOpenGLProfile, static_cast< CGLPixelFormatAttribute >( kCGLOGLPVersion_GL4_Core ),
		kCGLPFAColorSize, static_cast< CGLPixelFormatAttribute >( 24 ),
		kCGLPFAAlphaSize, static_cast< CGLPixelFormatAttribute >( 8 ),
		static_cast< CGLPixelFormatAttribute >( 0 )
	};

	CGLPixelFormatObj format = nullptr;
	GLint formatCount        = 0;
	if( CGLChoosePixelFormat( accelerated, &format, &formatCount ) != kCGLNoError || format == nullptr )
	{
		if( CGLChoosePixelFormat( software, &format, &formatCount ) != kCGLNoError || format == nullptr )
			return nullptr;
	}

	CGLContextObj context = nullptr;
	const CGLError error  = CGLCreateContext( format, nullptr, &context );
	CGLDestroyPixelFormat( format );
	if( error != kCGLNoError )
		return nullptr;

	CGLSetCurrentContext( context );
	return context;
}

const char* kindName( unsigned int type )
{
	switch( type )
	{
	case FF_TYPE_BOOLEAN: return "bool";
	case FF_TYPE_EVENT: return "event";
	case FF_TYPE_RED: return "red";
	case FF_TYPE_GREEN: return "green";
	case FF_TYPE_BLUE: return "blue";
	case FF_TYPE_OPTION: return "option";
	case FF_TYPE_BUFFER: return "buffer";
	case FF_TYPE_STANDARD: return "standard";
	case FF_TYPE_TEXT: return "text";
	case FF_TYPE_FILE: return "file";
	case FF_TYPE_INTEGER: return "integer";
	default: return "other";
	}
}

using Track = std::vector< std::pair< int, float > >;

/// The whole of `text` as a number. strtof alone reads "Fixed" as 0 and says
/// nothing, which renders a picture that looks deliberate and is wrong.
bool parseNumber( const std::string& text, float& out )
{
	char* end         = nullptr;
	const float value = std::strtof( text.c_str(), &end );
	if( text.empty() || end != text.c_str() + text.size() )
		return false;
	out = value;
	return true;
}

std::map< std::string, Track > loadScript( const std::string& path, std::string& error )
{
	std::map< std::string, Track > tracks;
	std::ifstream file( path );
	if( !file )
	{
		error = "cannot open " + path;
		return tracks;
	}

	std::string line;
	int lineNumber = 0;
	while( std::getline( file, line ) )
	{
		++lineNumber;
		const size_t hash = line.find( '#' );
		if( hash != std::string::npos )
			line.erase( hash );
		std::istringstream in( line );

		int frame = 0;
		if( !( in >> frame ) )
			continue;

		//The name is everything up to the last token: parameters have spaces
		//in them ("Spin Time") and the value never does.
		std::vector< std::string > words;
		std::string word;
		while( in >> word )
			words.push_back( word );
		if( words.size() < 2 )
		{
			error = path + ":" + std::to_string( lineNumber ) + ": expected `frame Parameter Name value`";
			return {};
		}

		float value = 0.0f;
		if( !parseNumber( words.back(), value ) )
		{
			error = path + ":" + std::to_string( lineNumber ) + ": '" + words.back() + "' is not a number (cues take an option's index)";
			return {};
		}
		words.pop_back();
		std::string name = words.front();
		for( size_t i = 1; i < words.size(); ++i )
			name += " " + words[ i ];

		tracks[ name ].emplace_back( frame, value );
	}

	for( auto& entry : tracks )
		std::sort( entry.second.begin(), entry.second.end() );
	return tracks;
}

float valueAt( const Track& track, int frame )
{
	if( track.empty() )
		return 0.0f;
	if( frame <= track.front().first )
		return track.front().second;
	if( frame >= track.back().first )
		return track.back().second;
	for( size_t i = 0; i + 1 < track.size(); ++i )
	{
		const auto& a = track[ i ];
		const auto& b = track[ i + 1 ];
		if( frame >= a.first && frame <= b.first )
		{
			if( b.first == a.first )
				return b.second;
			const float t = static_cast< float >( frame - a.first ) / static_cast< float >( b.first - a.first );
			return a.second + ( b.second - a.second ) * t;
		}
	}
	return track.back().second;
}

//---------------------------------------------------------------------------
// The card, for the Over effect: a dusk gradient with a few bright stripes,
// so a reel symbol made of the clip has something to show.
//---------------------------------------------------------------------------
Floats buildCard( int width, int height )
{
	Floats card( static_cast< size_t >( width ) * height * 4 );
	for( int y = 0; y < height; ++y )
		for( int x = 0; x < width; ++x )
		{
			const double u = ( x + 0.5 ) / width, v = ( y + 0.5 ) / height;
			float* o       = &card[ ( static_cast< size_t >( y ) * width + x ) * 4 ];
			const bool stripe = static_cast< int >( std::floor( u * 12.0 + v * 3.0 ) ) % 3 == 0;
			o[ 0 ] = static_cast< float >( 0.15 + 0.5 * v + ( stripe ? 0.25 : 0.0 ) );
			o[ 1 ] = static_cast< float >( 0.10 + 0.25 * u );
			o[ 2 ] = static_cast< float >( 0.35 + 0.4 * ( 1.0 - v ) );
			o[ 3 ] = 1.0f;
		}
	return card;
}

GLuint makeTexture( int width, int height, const float* pixels )
{
	GLuint texture = 0;
	glGenTextures( 1, &texture );
	glBindTexture( GL_TEXTURE_2D, texture );
	glTexImage2D( GL_TEXTURE_2D, 0, GL_RGBA32F, width, height, 0, GL_RGBA, GL_FLOAT, pixels );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );
	glBindTexture( GL_TEXTURE_2D, 0 );
	return texture;
}

//---------------------------------------------------------------------------
// A rig: the real plugin, a float output framebuffer, a synthetic 60 fps clock.
//---------------------------------------------------------------------------
struct Rig
{
	JackpotPlugin plugin;
	int width = 0, height = 0;
	GLuint sourceTexture = 0, outputTexture = 0, outputFBO = 0;
	int frame            = 0;
	double fps           = 60.0;
	double clockOffset   = 0.0;

	ProcessOpenGLStruct process    = {};
	FFGLTextureStruct inputStruct  = {};
	FFGLTextureStruct* inputs[ 1 ] = { nullptr };

	explicit Rig( bool effect = false ) : plugin( effect )
	{
		plugin.SetSynchronousForTest( true );
	}

	~Rig()
	{
		plugin.DeInitGL();
		if( outputFBO )
			glDeleteFramebuffers( 1, &outputFBO );
		if( outputTexture )
			glDeleteTextures( 1, &outputTexture );
		if( sourceTexture )
			glDeleteTextures( 1, &sourceTexture );
	}

	bool Init( int w, int h, const Floats* picture = nullptr )
	{
		width  = w;
		height = h;
		FFGLViewportStruct viewport = {};
		viewport.width              = static_cast< FFUInt32 >( width );
		viewport.height             = static_cast< FFUInt32 >( height );
		if( plugin.InitGL( &viewport ) != FF_SUCCESS )
		{
			std::fprintf( stderr, "InitGL failed -- see ~/Library/Logs/jackpot for why\n" );
			return false;
		}
		plugin.SetClockScaleForTest( 1.0 );

		outputTexture = makeTexture( width, height, nullptr );
		glGenFramebuffers( 1, &outputFBO );
		glBindFramebuffer( GL_FRAMEBUFFER, outputFBO );
		glFramebufferTexture2D( GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, outputTexture, 0 );
		if( glCheckFramebufferStatus( GL_FRAMEBUFFER ) != GL_FRAMEBUFFER_COMPLETE )
			return false;

		process.HostFBO = outputFBO;
		if( plugin.IsEffect() )
		{
			const Floats card = picture ? *picture : buildCard( width, height );
			sourceTexture     = makeTexture( width, height, card.data() );
			inputStruct.Width = inputStruct.HardwareWidth = static_cast< FFUInt32 >( width );
			inputStruct.Height = inputStruct.HardwareHeight = static_cast< FFUInt32 >( height );
			inputStruct.Handle                              = sourceTexture;
			inputs[ 0 ]                                     = &inputStruct;
			process.numInputTextures                        = 1;
			process.inputTextures                           = inputs;
		}
		return true;
	}

	/// A new frame size: the output buffer reallocated, the plugin told by the
	/// viewport it is drawn into (a source) or the input's size (an effect).
	void Resize( int w, int h )
	{
		width  = w;
		height = h;
		glDeleteFramebuffers( 1, &outputFBO );
		glDeleteTextures( 1, &outputTexture );
		outputTexture = makeTexture( width, height, nullptr );
		glGenFramebuffers( 1, &outputFBO );
		glBindFramebuffer( GL_FRAMEBUFFER, outputFBO );
		glFramebufferTexture2D( GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, outputTexture, 0 );
		process.HostFBO = outputFBO;
		if( plugin.IsEffect() )
		{
			glDeleteTextures( 1, &sourceTexture );
			const Floats card = buildCard( width, height );
			sourceTexture     = makeTexture( width, height, card.data() );
			inputStruct.Width = inputStruct.HardwareWidth = static_cast< FFUInt32 >( width );
			inputStruct.Height = inputStruct.HardwareHeight = static_cast< FFUInt32 >( height );
			inputStruct.Handle = sourceTexture;
		}
	}

	void Upload( const Floats& picture )
	{
		glBindTexture( GL_TEXTURE_2D, sourceTexture );
		glTexSubImage2D( GL_TEXTURE_2D, 0, 0, 0, width, height, GL_RGBA, GL_FLOAT, picture.data() );
		glBindTexture( GL_TEXTURE_2D, 0 );
	}

	void Set( unsigned int id, float value )
	{
		plugin.SetFloatParameter( id, value );
	}

	void Press( unsigned int id )
	{
		plugin.SetFloatParameter( id, 1.0f );
		plugin.SetFloatParameter( id, 0.0f );
	}

	bool Render( int frames = 1 )
	{
		for( int i = 0; i < frames; ++i )
		{
			const double seconds = clockOffset + static_cast< double >( frame ) / fps;
			plugin.SetTime( seconds );
			++frame;
			glBindFramebuffer( GL_FRAMEBUFFER, outputFBO );
			glViewport( 0, 0, width, height );
			glClearColor( 0.0f, 0.0f, 0.0f, 0.0f );
			glClear( GL_COLOR_BUFFER_BIT );
			if( plugin.ProcessOpenGL( &process ) != FF_SUCCESS )
			{
				std::fprintf( stderr, "ProcessOpenGL failed\n" );
				return false;
			}
		}
		return true;
	}

	/// Press Play and play the game through to rest, plus `after` frames.
	bool PlayToRest( int after = 2 )
	{
		Press( PT_PLAY );
		if( !Render( 1 ) )
			return false;
		const int frames = static_cast< int >( std::ceil( plugin.CurrentPlay().Clock().duration * fps ) ) + after;
		return Render( frames );
	}

	Floats Output() const
	{
		Floats pixels( static_cast< size_t >( width ) * height * 4 );
		glBindFramebuffer( GL_FRAMEBUFFER, outputFBO );
		glPixelStorei( GL_PACK_ALIGNMENT, 1 );
		glReadPixels( 0, 0, width, height, GL_RGBA, GL_FLOAT, pixels.data() );
		return pixels;
	}
};

//---------------------------------------------------------------------------
// Parameters by display name.
//---------------------------------------------------------------------------
struct NamedParameter
{
	std::string name;
	unsigned int index;
	float value;
	std::string kind;
};

std::vector< NamedParameter > listParameters( JackpotPlugin& plugin )
{
	std::vector< NamedParameter > list;
	for( unsigned int i = 0; i < plugin.ParamCount(); ++i )
	{
		const char* const name = plugin.GetParamName( i );
		list.push_back( NamedParameter { name ? name : "?", i, plugin.GetFloatParameter( i ),
		                                 kindName( plugin.GetParamType( i ) ) } );
	}
	return list;
}

bool applySetting( JackpotPlugin& plugin, const std::string& assignment, std::string& error )
{
	const size_t equals = assignment.find( '=' );
	if( equals == std::string::npos )
	{
		error = "expected Name=Value";
		return false;
	}
	const std::string name  = assignment.substr( 0, equals );
	const std::string value = assignment.substr( equals + 1 );
	for( const NamedParameter& parameter : listParameters( plugin ) )
		if( parameter.name == name )
		{
			if( parameter.kind == "text" || parameter.kind == "file" )
			{
				plugin.SetTextParameter( parameter.index, value.c_str() );
				return true;
			}
			float number = 0.0f;
			if( !parseNumber( value, number ) && parameter.kind == "option" )
			{
				//An option by its name. Every option here takes its index as its
				//value (SetParamElementInfo in Jackpot.cpp).
				for( unsigned int e = 0; e < plugin.GetNumParamElements( parameter.index ); ++e )
				{
					const char* const element = plugin.GetParamElementName( parameter.index, e );
					if( element != nullptr && value == element )
					{
						plugin.SetFloatParameter( parameter.index, static_cast< float >( e ) );
						return true;
					}
				}
				error = "'" + value + "' is not one of " + name + "'s options";
				return false;
			}
			if( !parseNumber( value, number ) )
			{
				error = "'" + value + "' is not a number";
				return false;
			}
			plugin.SetFloatParameter( parameter.index, number );
			return true;
		}
	error = "no parameter called '" + name + "'";
	return false;
}

/// --pipe and --film. Raw RGBA, top row first, on the synthetic 60 fps clock.
int runPipe( bool effect, int width, int height, const std::string& scriptPath, int filmFrames,
             const std::vector< std::string >& settings, const std::vector< int >& plays )
{
	Rig rig( effect );
	if( !rig.Init( width, height ) )
		return 1;
	for( const std::string& setting : settings )
	{
		std::string error;
		if( !applySetting( rig.plugin, setting, error ) )
		{
			std::fprintf( stderr, "--set %s: %s\n", setting.c_str(), error.c_str() );
			return 2;
		}
	}

	//A misspelt cue that silently did nothing would film a take that looks
	//deliberate and is wrong: refuse any name that is not a parameter.
	std::map< unsigned int, Track > automation;
	if( !scriptPath.empty() )
	{
		std::string error;
		const std::map< std::string, Track > tracks = loadScript( scriptPath, error );
		if( !error.empty() )
		{
			std::fprintf( stderr, "%s\n", error.c_str() );
			return 2;
		}
		const std::vector< NamedParameter > known = listParameters( rig.plugin );
		for( const auto& entry : tracks )
		{
			bool found = false;
			for( const NamedParameter& parameter : known )
				if( parameter.name == entry.first )
				{
					automation[ parameter.index ] = entry.second;
					found                         = true;
				}
			if( !found )
			{
				std::fprintf( stderr, "script names '%s', which is not a parameter (try --list)\n", entry.first.c_str() );
				return 2;
			}
		}
	}

	std::vector< unsigned char > in( static_cast< size_t >( width ) * height * 4 );
	Floats picture( in.size() );
	for( int index = 0; filmFrames < 0 || index < filmFrames; ++index )
	{
		if( filmFrames < 0 )
		{
			size_t filled = 0;
			while( filled < in.size() )
			{
				const ssize_t got = read( STDIN_FILENO, in.data() + filled, in.size() - filled );
				if( got <= 0 )
					break;
				filled += static_cast< size_t >( got );
			}
			if( filled < in.size() )
				break;
			for( int y = 0; y < height; ++y )
				for( int x = 0; x < width * 4; ++x )
					picture[ static_cast< size_t >( height - 1 - y ) * width * 4 + x ] =
						in[ static_cast< size_t >( y ) * width * 4 + x ] / 255.0f;
			if( effect )
				rig.Upload( picture );
		}

		for( const auto& track : automation )
			rig.plugin.SetFloatParameter( track.first, valueAt( track.second, index ) );
		if( std::find( plays.begin(), plays.end(), index ) != plays.end() )
			rig.Press( PT_PLAY );
		if( !rig.Render( 1 ) )
			return 1;

		const Floats out = rig.Output();
		std::vector< unsigned char > bytes( in.size() );
		for( int y = 0; y < height; ++y )
			for( int x = 0; x < width * 4; ++x )
				bytes[ static_cast< size_t >( y ) * width * 4 + x ] = static_cast< unsigned char >( std::lround(
					std::clamp( out[ static_cast< size_t >( height - 1 - y ) * width * 4 + x ], 0.0f, 1.0f ) * 255.0f ) );
		size_t written = 0;
		while( written < bytes.size() )
		{
			const ssize_t put = write( STDOUT_FILENO, bytes.data() + written, bytes.size() - written );
			//The reader has gone. SIGPIPE is ignored in main(), so this is EPIPE
			//and not a silent 141: say so and stop.
			if( put <= 0 )
			{
				std::fprintf( stderr, "stdout closed at frame %d\n", index );
				return 1;
			}
			written += static_cast< size_t >( put );
		}
	}
	return 0;
}

//===========================================================================
// The checks.
//
// Each takes a Perturb. With every field at its default the check scores the
// plugin; `--negative` sets one field at a time to a deliberately wrong model
// and requires the check to FAIL.
//===========================================================================
struct Perturb
{
	bool uniformVirtual      = false;///< --nearmiss: a virtual reel with no weighting
	bool reelOffset          = false;///< --slots-readback: the reels drawn one stop on
	bool noBlur              = false;///< --blur: score the picture against the shutter-closed reference
	bool noShift             = false;///< --roulette, --roulette-readback, --wheel: never turn the paint
	bool fretsSeeShift       = false;///< --roulette: the frets moved half a pocket (the physics sees it)
	bool pegsSeeShift        = false;///< --wheel: the pegs turned half a segment
	bool biasedDraw          = false;///< chi-squares: Random draws Below( n - 1 )
	bool noWarp              = false;///< --duration: play at 1x
	bool superBounce         = false;///< --shower: restitution 1.3
	bool noDrag              = false;///< --shower: the piece falls without its drag
	bool noGyro              = false;///< --shower: the tumble without its gyroscopic term
	bool ballInertia         = false;///< --shower: a disc with a ball's moments of inertia
	bool identitySymmetry    = false;///< --craps: S = I
	bool identityPermutation = false;///< --lottery: the balls painted as numbered
	bool defaultsShifted     = false;///< --defaults: expect Roulette
	bool determinismSeeds    = false;///< --determinism: expect seeds 3 and 4 to agree
};

using CheckFn = int ( * )( const Perturb& );
struct CheckEntry
{
	const char* flag;
	CheckFn run;
};

//---------------------------------------------------------------------------
// Shared helpers.
//---------------------------------------------------------------------------
constexpr double kFrame = 1.0 / 60.0;

/// The chi-square critical value for `dof` degrees of freedom at p = 0.001
/// (Wilson-Hilferty; within 1% of the tables from 5 degrees up).
double chiSquareCritical( int dof )
{
	const double z = 3.0902;
	const double k = static_cast< double >( dof );
	const double a = 2.0 / ( 9.0 * k );
	return k * std::pow( 1.0 - a + z * std::sqrt( a ), 3.0 );
}

double chiSquare( const std::vector< long >& counts, const std::vector< double >& expected )
{
	double chi = 0.0;
	for( size_t i = 0; i < counts.size(); ++i )
		chi += ( counts[ i ] - expected[ i ] ) * ( counts[ i ] - expected[ i ] ) / std::max( expected[ i ], 1e-12 );
	return chi;
}

int wrapStop( double s )
{
	const long k = std::lround( s );
	return static_cast< int >( ( ( k % slots::kStops ) + slots::kStops ) % slots::kStops );
}

double wrapAngle( double a )
{
	a = std::fmod( a, 2.0 * kPi );
	return a < 0.0 ? a + 2.0 * kPi : a;
}

const char* resultName( Result r )
{
	static const char* const names[] = { "Random", "Win", "Jackpot", "Near Miss", "Lose", "Fixed" };
	return names[ static_cast< int >( r ) ];
}

/// A number written in the shipped shader text after `marker` -- for the one
/// or two constants a check needs that exist only in the GLSL.
double shaderConstant( const char* program, const std::string& marker )
{
	for( const shaders::Program& p : shaders::Programs() )
		if( std::string( p.name ) == program )
		{
			const size_t at = p.fragment.find( marker );
			if( at != std::string::npos )
				return std::strtod( p.fragment.c_str() + at + marker.size(), nullptr );
		}
	return std::nan( "" );
}

//===========================================================================
// --slots: the stops drawn are on the payline at rest; Result is honoured;
// the pay table agrees with README's; the reels stop in order.
//===========================================================================
int runSlots( const Perturb& )
{
	std::printf( "\n=== slots: every Result, 3 and 5 reels, two symbol sets, many seeds\n" );
	const Result kinds[] = { Result::Random, Result::Win, Result::Jackpot, Result::NearMiss, Result::Lose, Result::Fixed };
	for( SymbolSet set : { SymbolSet::Fruit, SymbolSet::SevensAndBars } )
		for( int reels : { 3, 5 } )
		{
			int plays = 0, offLine = 0, notIntegral = 0, wrongResult = 0, order = 0, stillBefore = 0;
			for( Result kind : kinds )
				for( int fixed = 0; fixed <= ( kind == Result::Fixed ? 11 : 0 ); ++fixed )
					for( uint32_t seed = 1; seed <= 24; ++seed )
					{
						slots::Request r;
						r.set      = set;
						r.reels    = reels;
						r.result   = kind;
						r.fixed    = fixed;
						r.seed     = seed;
						r.play     = seed * 7u;
						r.duration = 2.0 + ( seed % 5 );
						r.damping  = 0.2 + 0.03 * ( seed % 20 );
						for( int i = 0; i < slots::kMaxReels; ++i )
							r.from[ static_cast< size_t >( i ) ] = static_cast< double >( ( seed * 5u + static_cast< uint32_t >( i ) * 3u ) % slots::kStops );
						const slots::Plan p = slots::MakePlan( r );
						++plays;
						const double D = p.playback.duration;
						int stops[ slots::kMaxReels ] = {};
						for( int i = 0; i < reels; ++i )
						{
							const double at = p.Position( i, D );
							notIntegral += std::fabs( at - std::round( at ) ) > 1e-9 ? 1 : 0;
							stops[ i ] = wrapStop( at );
							offLine += stops[ i ] != p.stops[ static_cast< size_t >( i ) ] ? 1 : 0;
							const slots::ReelMotion& m = p.reels[ static_cast< size_t >( i ) ];
							if( i + 1 < reels && !( m.restAt < p.reels[ static_cast< size_t >( i + 1 ) ].restAt ) )
								++order;
							if( i == reels - 1 && std::fabs( m.restAt - D ) > 1e-12 )
								++order;
							stillBefore += std::fabs( p.Position( i, m.restAt - kFrame ) - m.target ) > 0.0 ? 0 : 1;
						}
						const int pays = slots::Pays( set, reels, stops );
						const bool jackpot = slots::IsJackpot( set, reels, stops );
						const bool near    = slots::IsNearMiss( set, reels, stops );
						bool ok            = pays == p.outcome.payout;
						switch( kind )
						{
						case Result::Win: ok = ok && pays > 0 && !jackpot; break;
						case Result::Jackpot: ok = ok && jackpot; break;
						case Result::NearMiss: ok = ok && near; break;
						case Result::Lose: ok = ok && pays == 0 && !near; break;
						case Result::Fixed:
						{
							int want = std::min( fixed, static_cast< int >( slots::Diamond ) );
							for( int i = 0; i < reels; ++i )
							{
								const auto& strip = slots::Reels( set )[ static_cast< size_t >( i ) ].symbol;
								const bool has    = std::find( strip.begin(), strip.end(), want ) != strip.end();
								ok = ok && ( !has || strip[ static_cast< size_t >( stops[ i ] ) ] == want );
							}
							break;
						}
						default: break;
						}
						wrongResult += ok ? 0 : 1;
					}
			const char* name = set == SymbolSet::Fruit ? "Fruit" : "Sevens and Bars";
			Check( offLine == 0 && notIntegral == 0, fmt( "%s, %d reels: %d plays, every reel at rest exactly on its drawn stop (off %d, between stops %d)", name, reels, plays, offLine, notIntegral ) );
			Check( wrongResult == 0, fmt( "%s, %d reels: Win, Jackpot, Near Miss, Lose and Fixed as asked, payout as the table (%d wrong)", name, reels, wrongResult ) );
			Check( order == 0 && stillBefore == 0, fmt( "%s, %d reels: reels stop left to right, the last at Spin Time, each still moving a frame before (%d, %d wrong)", name, reels, order, stillBefore ) );
		}

	//The pay table, by hand from README "Results" (Fruit strips: reel 1 has a
	//cherry at stop 2 and a bell at 8; reel 2 a cherry at 6, a bell at 4; reel
	//3 a bell at 6. Sevens and Bars: stop 2 is BAR, 2BAR, BAR on reels 1-3).
	struct Case
	{
		SymbolSet set;
		int reels;
		std::array< int, 5 > stops;
		int pays;
		const char* what;
	};
	const Case cases[] = {
		{ SymbolSet::Fruit, 3, { 0, 0, 0, 0, 0 }, 200, "three sevens" },
		{ SymbolSet::Fruit, 5, { 0, 0, 0, 0, 0 }, 4000, "five sevens (x20)" },
		{ SymbolSet::Fruit, 5, { 0, 0, 0, 0, 1 }, 800, "four sevens (x4)" },
		{ SymbolSet::Fruit, 3, { 2, 1, 1, 0, 0 }, 2, "a cherry on reel 1" },
		{ SymbolSet::Fruit, 3, { 2, 6, 1, 0, 0 }, 5, "cherries on reels 1 and 2" },
		{ SymbolSet::Fruit, 3, { 8, 4, 6, 0, 0 }, 18, "three bells" },
		{ SymbolSet::Fruit, 3, { 1, 1, 1, 0, 0 }, 0, "three blanks" },
		{ SymbolSet::SevensAndBars, 3, { 2, 2, 2, 0, 0 }, 10, "BAR 2BAR BAR: any bars" },
	};
	int wrong = 0;
	for( const Case& c : cases )
	{
		const int got = slots::Pays( c.set, c.reels, c.stops.data() );
		if( got != c.pays )
		{
			std::printf( "        %s pays %d, README says %d\n", c.what, got, c.pays );
			++wrong;
		}
	}
	Check( wrong == 0, fmt( "the pay table: %zu hand-worked lines from README agree (%d wrong)", sizeof( cases ) / sizeof( cases[ 0 ] ), wrong ) );
	return Verdict();
}

//===========================================================================
// --nearmiss: the virtual reel's near-miss rate against the table's exact
// value, and against the physical strip's.
//===========================================================================
int runNearMiss( const Perturb& perturb )
{
	std::printf( "\n=== near miss: the virtual reel (Telnaes) against the physical strip, reel 3, 400000 pulls each\n" );
	const long M = 400000;
	const slots::Reel& reel = slots::Reels( SymbolSet::Fruit )[ 2 ];
	const int seven = reel.seven;
	const int above = ( seven + 1 ) % slots::kStops, below = ( seven + slots::kStops - 1 ) % slots::kStops;
	double ratio[ 2 ] = {};
	for( int strip = 0; strip < 2; ++strip )
	{
		slots::Request r;
		r.set            = SymbolSet::Fruit;
		r.reels          = 3;
		r.result         = Result::Random;
		r.strip          = strip == 0 ? Strip::Virtual : Strip::Physical;
		r.seed           = 11;
		r.uniformVirtual = perturb.uniformVirtual;
		long sevens = 0, beside = 0;
		for( long k = 0; k < M; ++k )
		{
			r.play = static_cast< uint32_t >( k );
			const int s = slots::DrawStops( r )[ 2 ];
			sevens += s == seven ? 1 : 0;
			beside += ( s == above || s == below ) ? 1 : 0;
		}
		const double p7 = strip == 0 ? reel.weight[ static_cast< size_t >( seven ) ] / 64.0 : 1.0 / slots::kStops;
		const double pb = strip == 0 ? ( reel.weight[ static_cast< size_t >( above ) ] + reel.weight[ static_cast< size_t >( below ) ] ) / 64.0 : 2.0 / slots::kStops;
		const double m7 = static_cast< double >( sevens ) / M, mb = static_cast< double >( beside ) / M;
		const double s7 = std::sqrt( p7 * ( 1 - p7 ) / M ), sb = std::sqrt( pb * ( 1 - pb ) / M );
		const char* name = strip == 0 ? "Virtual " : "Physical";
		Check( std::fabs( m7 - p7 ) < 4.0 * s7 && std::fabs( mb - pb ) < 4.0 * sb,
		       fmt( "%s: the seven on the line %.5f (table %.5f), a blank beside it %.5f (table %.5f), each within 4 sigma", name, m7, p7, mb, pb ) );
		ratio[ strip ] = mb / std::max( m7, 1e-12 );
	}
	//What the near miss IS: the seven sailing past just off the line, against
	//landing on it. A fair strip shows it twice as often; Telnaes's twelve times.
	Check( ratio[ 0 ] > 3.0 * ratio[ 1 ], fmt( "near misses per jackpot seven, per reel: Virtual %.1f, Physical %.1f (the virtual reel's must be over 3x)", ratio[ 0 ], ratio[ 1 ] ) );
	{
		const auto& reels = slots::Reels( SymbolSet::Fruit );
		auto w = [ & ]( int reelIndex, int stop ) { return reels[ static_cast< size_t >( reelIndex ) ].weight[ static_cast< size_t >( ( stop + slots::kStops ) % slots::kStops ) ] / 64.0; };
		const double virtualNear = w( 0, 0 ) * w( 1, 0 ) * ( w( 2, 1 ) + w( 2, -1 ) ), virtualJack = w( 0, 0 ) * w( 1, 0 ) * w( 2, 0 );
		std::printf( "        three reels, from the table: near miss %.3g a pull and jackpot %.3g (Virtual); %.3g and %.3g (Physical)\n", virtualNear,
		             virtualJack, 2.0 / std::pow( 22.0, 3.0 ), 1.0 / std::pow( 22.0, 3.0 ) );
	}
	return Verdict();
}

//===========================================================================
// The slot machine read out of the picture. The reel geometry below is the
// ONE transcription of kSlots's window (Shaders.cpp): where reel i's window
// is, and how a row of it maps to a stop on the drum.
//===========================================================================
struct ReelWindow
{
	int reels   = 3;
	double zoom = 1.0;
	int width = 0, height = 0;

	double W() const { return reels == 5 ? 0.44 : 0.30; }
	double Wi() const { return W() - 0.045; }
	double ReelWidth() const { return ( 2.0 * Wi() - ( reels + 1 ) * 0.008 ) / reels; }
	/// The pixel at reel-local (x across -1..1, y up -1..1 of the window).
	void Pixel( int reel, double lx, double ly, int& px, int& py ) const
	{
		const double rw = ReelWidth();
		const double xc = -Wi() + 0.008 + 0.5 * rw + reel * ( rw + 0.008 );
		const double qx = xc + lx * 0.5 * rw, qy = 0.06 + ly * 0.145;
		const double cx = qx - 0.035, cy = qy;//q = p - ( -0.035, 0 )
		px = static_cast< int >( std::floor( cx * height * zoom + 0.5 * width ) );
		py = static_cast< int >( std::floor( cy * height * zoom + 0.5 * height ) );
	}
	/// The drum's coordinate at window row ly, in stops from the payline, and
	/// the tile's horizontal coordinate at lx.
	void Drum( double lx, double ly, double& du, double& tx ) const
	{
		const double pitch = 2.0 * kPi / slots::kStops, phiMax = 1.5 * pitch;
		const double phi   = std::asin( std::clamp( ly * std::sin( phiMax ), -1.0, 1.0 ) );
		const double arc   = 0.145 / std::sin( phiMax ) * pitch;
		du                 = phi / pitch;
		tx                 = 0.5 + lx * 0.5 * ReelWidth() / ( arc * 0.9 );
	}
};

/// The display colour the flat shader draws for stop position s (drum
/// coordinate u = s + du), integrated over [a, b] with `taps` samples, the
/// atlas read nearest.
void reelColour( const SymbolAtlas& atlas, const slots::Reel& reel, double a, double b, int taps, double du, double tx, double out[ 3 ] )
{
	const double paper[ 3 ] = { std::pow( 0.96, 2.2 ), std::pow( 0.94, 2.2 ), std::pow( 0.88, 2.2 ) };
	double sum[ 3 ]         = {};
	for( int j = 0; j < taps; ++j )
	{
		const double s = a + ( b - a ) * ( j + 0.5 ) / taps;
		const double u = s + du;
		const double k = std::floor( u + 0.5 );
		const double f = u - k;
		const int stop = ( ( static_cast< int >( k ) % slots::kStops ) + slots::kStops ) % slots::kStops;
		const int sym  = reel.symbol[ static_cast< size_t >( stop ) ];
		const double ty = 0.5 + f / 0.9;
		double ink[ 4 ] = {};
		if( sym > 0 && tx >= 0.0 && tx <= 1.0 && ty >= 0.0 && ty <= 1.0 )
			atlas.At( sym, std::clamp( tx, 0.004, 0.996 ), std::clamp( ty, 0.004, 0.996 ), ink );
		for( int c = 0; c < 3; ++c )
		{
			const double straight = ink[ 3 ] > 0.0 ? ink[ c ] / ink[ 3 ] : 0.0;
			sum[ c ] += std::pow( straight, 2.2 ) * ink[ 3 ] + paper[ c ] * ( 1.0 - ink[ 3 ] );
		}
	}
	for( int c = 0; c < 3; ++c )
		out[ c ] = std::pow( sum[ c ] / taps, 1.0 / 2.2 );
}

void setGame( Rig& rig, Game game )
{
	rig.Set( PT_GAME, static_cast< float >( game ) );
}

//===========================================================================
// --blur: the strip integrated over the shutter's travel.
//===========================================================================
int runBlur( const Perturb& perturb )
{
	std::printf( "\n=== blur: the reels are the strip integrated over the shutter, sharp at rest\n" );
	for( const auto& size : { std::pair< int, int > { 640, 360 }, std::pair< int, int > { 320, 180 } } )
	{
		Rig rig;
		if( !rig.Init( size.first, size.second ) )
			return 1;
		rig.plugin.SetFlatForTest( 1 );
		rig.Set( PT_SPIN_TIME, ParamFromSpinTime( 6.0 ) );
		rig.Set( PT_BLUR, 1.0f );
		rig.Set( PT_LIGHTS, 0.0f );
		rig.Press( PT_PLAY );
		rig.Render( 1 );
		rig.Render( 89 );//1.5 s: every reel at its run
		ReelWindow win;
		win.reels  = 3;
		win.zoom   = ZoomFromParam( rig.plugin.GetFloatParameter( PT_ZOOM ) );
		win.width  = rig.width;
		win.height = rig.height;
		const slots::Plan& plan = rig.plugin.CurrentPlay().slots;
		const SymbolAtlas& atlas = rig.plugin.CurrentSymbols();
		auto score = [ & ]( const Floats& img, double t, double shutter, double& errSharp, double& errBlur, double& travel ) {
			errSharp = errBlur = 0.0;
			int n = 0;
			travel = 0.0;
			for( int i = 0; i < 3; ++i )
			{
				const double b = plan.Position( i, t ), a = plan.Position( i, t - shutter );
				travel        = std::max( travel, std::fabs( b - a ) );
				const slots::Reel& reel = slots::Reels( SymbolSet::Fruit )[ static_cast< size_t >( i ) ];
				for( double ly = -0.8; ly <= 0.8; ly += 0.02 )
					for( double lx : { -0.3, 0.0, 0.3 } )
					{
						int px = 0, py = 0;
						win.Pixel( i, lx, ly, px, py );
						if( px < 0 || py < 0 || px >= rig.width || py >= rig.height )
							continue;
						double du = 0.0, tx = 0.0;
						win.Drum( lx, ly, du, tx );
						double sharp[ 3 ], blur[ 3 ];
						reelColour( atlas, reel, b, b, 1, du, tx, sharp );
						reelColour( atlas, reel, a, b, 96, du, tx, blur );
						const float* o = &img[ ( static_cast< size_t >( py ) * rig.width + px ) * 4 ];
						for( int c = 0; c < 3; ++c )
						{
							errSharp += std::fabs( o[ c ] - sharp[ c ] );
							errBlur += std::fabs( o[ c ] - blur[ c ] );
						}
						n += 3;
					}
			}
			errSharp /= std::max( n, 1 );
			errBlur /= std::max( n, 1 );
		};
		double sharp = 0.0, blur = 0.0, travel = 0.0;
		const double t = rig.plugin.PlaybackSeconds();
		score( rig.Output(), t, BlurFromParam( 1.0f ) * kFrame, sharp, blur, travel );
		if( perturb.noBlur )
			std::swap( sharp, blur );
		Check( blur < 0.5 * sharp && blur < 0.08,
		       fmt( "%dx%d, at speed (%.2f stops a frame): the picture is %.3f from the strip integrated over the shutter, %.3f from the instant", rig.width,
		            rig.height, travel, blur, sharp ) );
		//At rest: no travel, nothing to integrate -- sharp.
		rig.Render( static_cast< int >( std::ceil( ( plan.playback.duration - t ) * 60.0 ) ) + 3 );
		score( rig.Output(), rig.plugin.PlaybackSeconds(), BlurFromParam( 1.0f ) * kFrame, sharp, blur, travel );
		Check( sharp < 0.08 && travel == 0.0, fmt( "%dx%d, at rest: %.3f from the sharp strip", rig.width, rig.height, sharp ) );
	}
	return Verdict();
}

//===========================================================================
// --slots-readback: the payline read out of the rendered frame.
//===========================================================================
int runSlotsReadback( const Perturb& perturb )
{
	std::printf( "\n=== slots readback: the symbols on the payline, read from the picture against the atlas\n" );
	for( const auto& size : { std::pair< int, int > { 640, 360 }, std::pair< int, int > { 320, 180 } } )
		for( int reels : { 3, 5 } )
		{
			Rig rig;
			if( !rig.Init( size.first, size.second ) )
				return 1;
			rig.plugin.SetFlatForTest( 1 );
			rig.plugin.Flags().reelOffset = perturb.reelOffset ? 1.0 : 0.0;
			rig.Set( PT_REELS, reels == 5 ? 1.0f : 0.0f );
			rig.Set( PT_SPIN_TIME, ParamFromSpinTime( 2.0 ) );
			rig.Set( PT_LIGHTS, 0.0f );
			rig.Set( PT_DISPLAY, 0.0f );
			ReelWindow win;
			win.reels  = reels;
			win.zoom   = ZoomFromParam( rig.plugin.GetFloatParameter( PT_ZOOM ) );
			win.width  = rig.width;
			win.height = rig.height;
			int read = 0, wrong = 0;
			double worstMargin = 1e9;
			std::string worstPair;
			const Result kinds[] = { Result::Random, Result::Jackpot, Result::NearMiss, Result::Win, Result::Lose, Result::Fixed };
			for( int k = 0; k < 12; ++k )
			{
				rig.Set( PT_RESULT, static_cast< float >( kinds[ k % 6 ] ) );
				rig.Set( PT_FIXED_NUMBER, static_cast< float >( 1 + k ) );
				rig.Set( PT_SEED, static_cast< float >( 3 + k ) );
				if( !rig.PlayToRest() )
					return 1;
				const Floats img = rig.Output();
				const slots::Plan& plan = rig.plugin.CurrentPlay().slots;
				const SymbolAtlas& atlas = rig.plugin.CurrentSymbols();
				for( int i = 0; i < reels; ++i )
				{
					//Rows across the symbol on the line (a third of a stop either
					//side of the payline, where the drum puts it) and columns across
					//the middle of the reel: each sample's colour, and every
					//candidate symbol's reference there (a strip of that symbol
					//alone, read at stop 0).
					const int candidates = slots::kSymbolCount - 1;
					std::vector< std::array< double, 3 > > seen;
					std::vector< std::vector< std::array< double, 3 > > > refs( static_cast< size_t >( candidates ) );
					const double pitch = 2.0 * kPi / slots::kStops;
					for( double f : { -0.32, -0.16, 0.0, 0.16, 0.32 } )
						for( double lx = -0.6; lx <= 0.6; lx += 0.1 )
						{
							const double ly = std::sin( f * pitch ) / std::sin( 1.5 * pitch );
							int px = 0, py = 0;
							win.Pixel( i, lx, ly, px, py );
							const float* o = &img[ ( static_cast< size_t >( py ) * rig.width + px ) * 4 ];
							seen.push_back( { o[ 0 ], o[ 1 ], o[ 2 ] } );
							double du = 0.0, tx = 0.0;
							win.Drum( lx, ly, du, tx );
							for( int sym = 0; sym < candidates; ++sym )
							{
								slots::Reel only;
								only.symbol.fill( sym );
								std::array< double, 3 > want {};
								reelColour( atlas, only, 0.0, 0.0, 1, du, tx, want.data() );
								refs[ static_cast< size_t >( sym ) ].push_back( want );
							}
						}
					//Pairwise, as polyhedral reads a number: two candidates are judged
					//only where their references disagree by more than 0.1 (the
					//paper both leave blank says nothing), and the symbol read is the
					//one that beats every other. The reference reads the atlas's top
					//level and the shader its mip chain, so each is a little off
					//everywhere; where two symbols differ, the right one is far less.
					auto duel = [ & ]( int a, int b, double& ea, double& eb ) {
						ea = eb = 0.0;
						for( size_t k = 0; k < seen.size(); ++k )
						{
							const auto& ra = refs[ static_cast< size_t >( a ) ][ k ];
							const auto& rb = refs[ static_cast< size_t >( b ) ][ k ];
							double apart = 0.0;
							for( int c = 0; c < 3; ++c )
								apart = std::max( apart, std::fabs( ra[ c ] - rb[ c ] ) );
							if( apart <= 0.1 )
								continue;
							for( int c = 0; c < 3; ++c )
							{
								ea += std::fabs( seen[ k ][ c ] - ra[ c ] );
								eb += std::fabs( seen[ k ][ c ] - rb[ c ] );
							}
						}
					};
					int best = -1;
					for( int a = 0; a < candidates && best < 0; ++a )
					{
						bool beatsAll = true;
						for( int b = 0; b < candidates && beatsAll; ++b )
						{
							if( a == b )
								continue;
							double ea = 0.0, eb = 0.0;
							duel( a, b, ea, eb );
							beatsAll = ea < eb;
						}
						if( beatsAll )
							best = a;
					}
					if( best >= 0 )
						for( int b = 0; b < candidates; ++b )
						{
							if( b == best )
								continue;
							double ea = 0.0, eb = 0.0;
							duel( best, b, ea, eb );
							const double margin = eb / std::max( ea, 1e-9 );
							if( margin < worstMargin )
							{
								worstMargin = margin;
								worstPair   = fmt( "%s over %s by %.1fx", slots::SymbolName( best ), slots::SymbolName( b ), margin );
							}
						}
					const int expected = slots::Reels( SymbolSet::Fruit )[ static_cast< size_t >( i ) ].symbol[ static_cast< size_t >( plan.stops[ static_cast< size_t >( i ) ] ) ];
					++read;
					if( best != expected )
					{
						++wrong;
						if( wrong <= 3 && !perturb.reelOffset )
							std::printf( "        reel %d read %s, the plan drew %s\n", i + 1, slots::SymbolName( best ), slots::SymbolName( expected ) );
					}
				}
			}
			Check( wrong == 0, fmt( "%dx%d, %d reels: %d payline symbols read, %d wrong (closest call: %s)", rig.width, rig.height, reels, read, wrong, worstPair.c_str() ) );
		}
	return Verdict();
}

//===========================================================================
// --roulette: the pocket, the C37 claim, the ball off the track, fairness.
//===========================================================================
/// The number a viewer reads under the ball: the ball's angle against the
/// painted ring's, both as drawn.
int numberUnderBall( const roulette::Plan& p, double t )
{
	V3 ball;
	if( !p.Ball( t, ball ) )
		return -1;
	const std::vector< int >& numbers = roulette::Numbers( p.request.wheel );
	const int N         = static_cast< int >( numbers.size() );
	const double pocket = 2.0 * kPi / N;
	const double u      = ( std::atan2( ball.y, ball.x ) - p.PaintAngle( t ) ) / pocket;
	const long j        = std::lround( u );
	return numbers[ static_cast< size_t >( ( ( j % N ) + N ) % N ) ];
}

int runRoulette( const Perturb& perturb )
{
	std::printf( "\n=== roulette: every pocket of both wheels, the C37 claim, the ball off the track, Random\n" );
	const double lipDrawn = shaderConstant( "roulette", "LIP_TOP    = " );
	double highest       = -1.0;
	for( RouletteWheel wheel : { RouletteWheel::European, RouletteWheel::American } )
	{
		const std::vector< int >& numbers = roulette::Numbers( wheel );
		int wrong = 0, unsettled = 0, plans = 0;
		for( int n : numbers )
		{
			roulette::Request r;
			r.wheel      = wheel;
			r.result     = Result::Fixed;
			r.bet        = n;
			r.seed       = static_cast< uint32_t >( 17 + n );
			r.play       = 1;
			r.duration   = 5.0;
			r.rotorSpeed = RotorSpeedFromParam( 0.5f );
			r.noShift    = perturb.noShift;
			const roulette::Plan p = roulette::MakePlan( r );
			++plans;
			unsettled += p.sim.settled ? 0 : 1;
			const double after = p.playback.duration + 0.5;
			const int shown    = numberUnderBall( p, after );
			wrong += ( shown != n || p.outcome.value != n ) ? 1 : 0;
			for( const V3& k : p.sim.keys )
				highest = std::max( highest, k.z + roulette::kBallRadius );
		}
		Check( wrong == 0 && unsettled == 0, fmt( "%s: every number, Fixed: the ball at rest in the pocket painted with it, read from the ball and the ring as drawn (%d of %d wrong, %d unsettled)",
		                                       wheel == RouletteWheel::European ? "European" : "American", wrong, plans, unsettled ) );
	}
	Check( highest < lipDrawn, fmt( "the ball never rises above the drawn lip: top of the ball at most %.4f m, the lip drawn to %.3f m", highest, lipDrawn ) );

	//C37: the frets are placed from the rotor's fractional pocket alone, so
	//whole pockets of turn cannot change one bit of the ball's path.
	{
		roulette::Launch L;
		L.ballSpeed  = 2.2;
		L.ballAngle  = 1.1;
		L.fretPhase  = 0.31;
		L.rotorSpeed = 2.5;
		L.pockets    = 37;
		const roulette::Simulation base = roulette::Simulate( L );
		int identical = 0, pocketsRight = 0;
		const int turns[] = { 1, 5, 18, 36 };
		for( int k : turns )
		{
			roulette::Launch m = L;
			m.fretTurns        = k;
			if( perturb.fretsSeeShift )
				m.fretNudge = 0.5;
			const roulette::Simulation s = roulette::Simulate( m );
			const bool same = s.keys.size() == base.keys.size() && std::memcmp( s.keys.data(), base.keys.data(), s.keys.size() * sizeof( V3 ) ) == 0;
			identical += same ? 1 : 0;
			pocketsRight += s.pocket == ( ( base.pocket - k ) % 37 + 37 ) % 37 ? 1 : 0;
		}
		Check( identical == 4 && pocketsRight == 4 && base.settled && base.keys.size() > 100,
		       fmt( "C37: the rotor turned 1, 5, 18 and 36 whole pockets, the ball's path bit-identical (%d of 4), its pocket in the rotor's frame moved by k (%d of 4), %zu keys",
		            identical, pocketsRight, base.keys.size() ) );
	}

	//The ball leaves the lip when v^2 = g r tan(alpha), and not before.
	{
		int within = 0, n = 0;
		double worst = 0.0;
		for( double speed : { 1.4, 2.0, 2.8, 3.6, 4.4 } )
			for( double angle : { 0.2, 2.9 } )
			{
				roulette::Launch L;
				L.ballSpeed = speed;
				L.ballAngle = angle;
				L.fretPhase = 0.6;
				const roulette::Simulation s = roulette::Simulate( L );
				const double want = roulette::kGravity * s.departRadius * roulette::kTrackSlope;
				const double err  = std::fabs( s.departSpeed * s.departSpeed / want - 1.0 );
				worst             = std::max( worst, err );
				within += err < 0.02 && s.departTime > 0.0 ? 1 : 0;
				++n;
			}
		Check( within == n, fmt( "%d releases: the ball leaves the lip within 2%% of v^2 = g r tan 14 (worst %.2f%%)", n, 100.0 * worst ) );
	}

	//Random: uniform over the pockets (the draw), and the plan shows the draw.
	for( RouletteWheel wheel : { RouletteWheel::European, RouletteWheel::American } )
	{
		const int N = static_cast< int >( roulette::Numbers( wheel ).size() );
		const long M = static_cast< long >( N ) * 300;
		std::vector< long > counts( static_cast< size_t >( N ), 0 );
		roulette::Request r;
		r.wheel      = wheel;
		r.result     = Result::Random;
		r.biasedDraw = perturb.biasedDraw;
		for( long k = 0; k < M; ++k )
		{
			r.seed = static_cast< uint32_t >( k / 1000 );
			r.play = static_cast< uint32_t >( k % 1000 );
			++counts[ static_cast< size_t >( roulette::Wanted( r ) ) ];
		}
		const double chi = chiSquare( counts, std::vector< double >( static_cast< size_t >( N ), static_cast< double >( M ) / N ) );
		Check( chi < chiSquareCritical( N - 1 ), fmt( "%s Random: chi-square %.1f over %ld spins, %d pockets (critical %.1f at p = 0.001)", wheel == RouletteWheel::European ? "European" : "American", chi, M, N,
		                                              chiSquareCritical( N - 1 ) ) );
	}
	{
		int agree = 0;
		for( uint32_t seed = 1; seed <= 12; ++seed )
		{
			roulette::Request r;
			r.result   = Result::Random;
			r.seed     = seed;
			r.play     = 3;
			r.duration = 4.0;
			const roulette::Plan p = roulette::MakePlan( r );
			agree += p.outcome.value == roulette::Numbers( r.wheel )[ static_cast< size_t >( roulette::Wanted( r ) ) ] && numberUnderBall( p, p.playback.duration + 0.3 ) == p.outcome.value ? 1 : 0;
		}
		Check( agree == 12, fmt( "12 Random spins end to end: the ball rests in the number drawn (%d of 12)", agree ) );
	}
	return Verdict();
}

//===========================================================================
// --wheel: the money wheel.
//===========================================================================
int segmentUnderClapper( const wheel::Plan& p, double t )
{
	const double seg = 2.0 * kPi / wheel::kSegments;
	const long j     = static_cast< long >( std::floor( wrapAngle( 0.5 * kPi - p.PaintAngle( t ) ) / seg ) );
	return static_cast< int >( ( ( j % wheel::kSegments ) + wheel::kSegments ) % wheel::kSegments );
}

int runWheel( const Perturb& perturb )
{
	std::printf( "\n=== money wheel: every value, the C54 claim, fairness, the clapper takes the energy\n" );
	const std::vector< int >& layout = wheel::Layout();
	{
		int wrong = 0, plans = 0, unsettled = 0;
		std::set< int > segments;
		for( int bet : { 1, 2, 5, 10, 20, 40, 45 } )
			for( uint32_t seed = 1; seed <= 5; ++seed )
			{
				wheel::Request r;
				r.result    = Result::Fixed;
				r.bet       = bet;
				r.seed      = seed;
				r.play      = 2;
				r.duration  = 6.0;
				r.stiffness = ClapperFromParam( 0.23f );
				r.paintNow  = 0.4 * seed;
				r.noShift   = perturb.noShift;
				const wheel::Plan p = wheel::MakePlan( r );
				++plans;
				unsettled += p.sim.settled ? 0 : 1;
				const int j = segmentUnderClapper( p, p.playback.duration + 0.5 );
				segments.insert( j );
				wrong += ( layout[ static_cast< size_t >( j ) ] != bet || p.outcome.value != bet ) ? 1 : 0;
			}
		Check( wrong == 0 && unsettled == 0, fmt( "Fixed, every value: the segment under the clapper as drawn shows it (%d of %d wrong, %d unsettled, %zu different segments)", wrong, plans, unsettled, segments.size() ) );
	}
	{
		wheel::Release r;
		r.speed     = 6.0;
		r.phase     = 0.287;
		r.stiffness = 4.0;
		const wheel::Simulation base = wheel::Simulate( r );
		int identical = 0, right = 0;
		for( long k : { 1L, 7L, 27L, 53L } )
		{
			wheel::Release m = r;
			m.turns          = k;
			if( perturb.pegsSeeShift )
				m.pegTurn = 0.5;
			const wheel::Simulation s = wheel::Simulate( m );
			const bool same = s.phase.size() == base.phase.size() && std::memcmp( s.phase.data(), base.phase.data(), s.phase.size() * sizeof( double ) ) == 0
			                  && std::memcmp( s.clapper.data(), base.clapper.data(), s.clapper.size() * sizeof( double ) ) == 0;
			identical += same ? 1 : 0;
			right += s.segment == static_cast< int >( ( ( base.segment - k ) % 54 + 54 ) % 54 ) ? 1 : 0;
		}
		Check( identical == 4 && right == 4 && base.settled && base.phase.size() > 100,
		       fmt( "C54: the wheel turned 1, 7, 27 and 53 whole segments, the pegs' phase and the clapper bit-identical (%d of 4), the segment under it moved by k (%d of 4)", identical, right ) );
		int took = 0;
		std::vector< double > loss = base.pegLoss;
		for( double l : loss )
			took += l > 0.0 ? 1 : 0;
		std::sort( loss.begin(), loss.end() );
		const double median = loss.empty() ? 0.0 : loss[ loss.size() / 2 ];
		Check( took == static_cast< int >( loss.size() ) && !loss.empty() && median > 5.0 * wheel::BearingWorkPerSegment(),
		       fmt( "every one of %zu peg passages took energy; the median %.2f J against the bearing's %.3f J a segment: the clapper is what slows it", loss.size(), median,
		            wheel::BearingWorkPerSegment() ) );
	}
	{
		//Random: each segment 1/54 (which is how a one-segment bias shows),
		//so each value its count / 54.
		const long M = 54L * 400;
		std::vector< long > bySegment( 54, 0 );
		std::map< int, long > byValue;
		std::map< int, int > count;
		for( int v : layout )
			++count[ v ];
		wheel::Request r;
		r.result     = Result::Random;
		r.biasedDraw = perturb.biasedDraw;
		for( long k = 0; k < M; ++k )
		{
			r.seed = static_cast< uint32_t >( k / 1000 );
			r.play = static_cast< uint32_t >( k % 1000 );
			const int j = wheel::Wanted( r );
			++bySegment[ static_cast< size_t >( j ) ];
			++byValue[ layout[ static_cast< size_t >( j ) ] ];
		}
		const double chiSeg = chiSquare( bySegment, std::vector< double >( 54, static_cast< double >( M ) / 54.0 ) );
		std::vector< long > values;
		std::vector< double > expected;
		std::string shares;
		for( const auto& c : count )
		{
			values.push_back( byValue[ c.first ] );
			expected.push_back( static_cast< double >( M ) * c.second / 54.0 );
			shares += fmt( " %s %.3f/%.3f", wheel::Name( c.first ).c_str(), static_cast< double >( byValue[ c.first ] ) / M, c.second / 54.0 );
		}
		const double chiVal = chiSquare( values, expected );
		Check( chiSeg < chiSquareCritical( 53 ) && chiVal < chiSquareCritical( static_cast< int >( values.size() ) - 1 ),
		       fmt( "Random over %ld spins: chi-square %.1f over the segments (critical %.1f), %.1f over the values (critical %.1f)", M, chiSeg, chiSquareCritical( 53 ), chiVal,
		            chiSquareCritical( static_cast< int >( values.size() ) - 1 ) ) );
		std::printf( "        measured/expected:%s\n", shares.c_str() );
	}
	return Verdict();
}

//===========================================================================
// --craps: the dice show the total asked for; the pass line's state machine.
//===========================================================================
int topValue( const craps::Plan& p, int i, double t )
{
	V3 x;
	M3 r;
	if( !p.DieAt( i, t, x, r ) )
		return -1;
	const dice::geo::Solid& cube = dice::geo::GetSolid( dice::geo::Shape::Cube );
	const dice::geo::Labelling& labels = dice::geo::GetLabelling( dice::geo::DieType::D6, 0 );
	return labels.digit[ static_cast< size_t >( dice::geo::TopItem( cube, false, r ) ) ];
}

int runCraps( const Perturb& perturb )
{
	std::printf( "\n=== craps: every total, from the dice as drawn; the throw; the pass line\n" );
	const double side = dice::geo::GetSolid( dice::geo::Shape::Cube ).inradius;
	int wrong = 0, plans = 0, fellBack = 0, noWall = 0, notFlat = 0, touching = 0;
	long steps = 0;
	for( int total = 2; total <= 12; ++total )
		for( uint32_t seed = 1; seed <= 4; ++seed )
		{
			craps::Request r;
			r.result           = Result::Fixed;
			r.fixed            = total;
			r.seed             = seed;
			r.play             = static_cast< uint32_t >( total );
			r.duration         = 3.0;
			r.identitySymmetry = perturb.identitySymmetry;
			const craps::Plan p = craps::MakePlan( r );
			++plans;
			if( p.idle )
			{
				++fellBack;
				continue;
			}
			steps += p.stats.steps;
			const double end = p.playback.duration + 0.5;
			const int a = topValue( p, 0, end ), b = topValue( p, 1, end );
			wrong += ( a + b != total || p.outcome.value != total ) ? 1 : 0;
			noWall += p.hitWall ? 0 : 1;
			V3 x0, x1;
			M3 r0, r1;
			p.DieAt( 0, end, x0, r0 );
			p.DieAt( 1, end, x1, r1 );
			notFlat += ( std::fabs( x0.y - side ) > 1e-6 || std::fabs( x1.y - side ) > 1e-6 ) ? 1 : 0;
			touching += Length( x0 - x1 ) < 2.0 * side - 1e-6 ? 1 : 0;
		}
	Check( wrong == 0 && fellBack == 0 && steps > 0, fmt( "every total 2..12, Fixed: the faces on top of the dice as drawn add up to it (%d of %d wrong, %d fell back to rest, %ld physics steps)", wrong, plans, fellBack, steps ) );
	Check( noWall == 0 && notFlat == 0 && touching == 0, fmt( "every throw reached the back wall (%d short), every die at rest flat on the felt (%d not), none inside the other (%d)", noWall, notFlat, touching ) );

	//The pass line: a scripted shooter.
	struct Roll
	{
		int point, total, decision, after;
	};
	const Roll script[] = { { 0, 7, 1, 0 }, { 0, 11, 1, 0 }, { 0, 2, -1, 0 }, { 0, 3, -1, 0 }, { 0, 12, -1, 0 }, { 0, 6, 0, 6 },
		                    { 6, 8, 0, 6 }, { 6, 11, 0, 6 }, { 6, 6, 1, 0 }, { 0, 4, 0, 4 }, { 4, 7, -1, 0 }, { 0, 10, 0, 10 }, { 10, 10, 1, 0 } };
	int bad = 0;
	for( const Roll& roll : script )
		bad += ( craps::Decide( roll.point, roll.total ) != roll.decision || craps::PointAfter( roll.point, roll.total ) != roll.after ) ? 1 : 0;
	Check( bad == 0, fmt( "the pass line through a scripted shooter: naturals, craps, a point made, a seven-out (%d of %zu rolls wrong)", bad, sizeof( script ) / sizeof( script[ 0 ] ) ) );
	int asked = 0, kept = 0;
	for( int point : { 0, 4, 5, 6, 8, 9, 10 } )
		for( Result kind : { Result::Win, Result::Lose } )
			for( uint32_t seed = 1; seed <= 6; ++seed )
			{
				craps::Request r;
				r.result = kind;
				r.point  = point;
				r.seed   = seed;
				const std::array< int, 2 > d = craps::DrawDice( r );
				const int decision = craps::Decide( point, d[ 0 ] + d[ 1 ] );
				kept += ( kind == Result::Win ? decision > 0 : decision < 0 ) ? 1 : 0;
				++asked;
			}
	Check( kept == asked, fmt( "Win and Lose against every point: the dice drawn decide that way (%d of %d)", kept, asked ) );
	return Verdict();
}

//===========================================================================
// --lottery: the numbers drawn are the numbers asked for; the drum.
//===========================================================================
int runLottery( const Perturb& perturb )
{
	std::printf( "\n=== lottery: the balls on the rack are the numbers drawn, and the drum keeps its balls\n" );
	int wrong = 0, plans = 0, notBijection = 0, notRising = 0, fixedFirst = 0;
	double overlap = 0.0, escape = 0.0;
	for( int N : { 10, 25, 49, 75 } )
		for( int k = 0; k < 4; ++k )
		{
			lottery::Request r;
			r.balls    = N;
			r.draw     = std::min( 6, N );
			r.result   = k < 3 ? Result::Fixed : Result::Random;
			r.bet      = k == 0 ? 1 : k == 1 ? ( N + 1 ) / 2 : N;
			r.seed     = static_cast< uint32_t >( 5 + k );
			r.play     = 1;
			r.duration = 12.0;
			r.air      = AirFromParam( 0.43f );
			r.identityPermutation = perturb.identityPermutation;
			const lottery::Plan p = lottery::MakePlan( r );
			++plans;
			fixedFirst += ( r.result == Result::Fixed && ( p.numbers.empty() || p.numbers[ 0 ] != r.bet ) ) ? 1 : 0;
			//The rack at the end, left to right.
			std::vector< lottery::BallState > racked;
			for( const lottery::BallState& b : p.Balls( p.playback.duration + 0.5 ) )
				if( b.x.z > lottery::kDrum + 0.05 )
					racked.push_back( b );
			std::sort( racked.begin(), racked.end(), []( const lottery::BallState& a, const lottery::BallState& b ) { return a.x.x < b.x.x; } );
			bool same = racked.size() == p.numbers.size();
			for( size_t i = 0; same && i < racked.size(); ++i )
				same = racked[ i ].number == p.numbers[ i ];
			wrong += same ? 0 : 1;
			std::vector< int > paint = p.paint;
			std::sort( paint.begin(), paint.end() );
			for( int i = 0; i < N; ++i )
				notBijection += paint[ static_cast< size_t >( i ) ] != i + 1 ? 1 : 0;
			overlap = std::max( overlap, p.sim.maxOverlap );
			escape  = std::max( escape, p.sim.maxEscape );
			//Each drawn ball rises up the tube: its height never falls on the way.
			for( size_t c = 0; c < p.sim.captured.size(); ++c )
			{
				double last = -1.0;
				for( double s = 0.0; s <= lottery::Plan::kRise; s += 0.01 )
				{
					const double t = p.playback.duration * 0.0;
					(void)t;
					const double sim = p.drain + p.sim.caughtAt[ c ] + s;
					//Playback seconds for that simulated time: invert the rate curve by bisection.
					double lo = 0.0, hi = p.playback.duration;
					for( int it = 0; it < 50; ++it )
					{
						const double mid = 0.5 * ( lo + hi );
						( p.playback.SimTime( mid ) < sim ? lo : hi ) = mid;
					}
					for( const lottery::BallState& b : p.Balls( hi ) )
						if( b.number == p.paint[ static_cast< size_t >( p.sim.captured[ c ] ) ] )
						{
							if( b.x.z < last - 1e-9 )
								++notRising;
							last = b.x.z;
						}
				}
			}
		}
	Check( wrong == 0 && fixedFirst == 0, fmt( "%d draws, 10 to 75 balls: the balls on the rack, left to right, are the numbers drawn (%d wrong); Fixed's number first (%d not)", plans, wrong, fixedFirst ) );
	Check( notBijection == 0, fmt( "the paint is a permutation of 1..N in every draw (%d numbers missing or doubled)", notBijection ) );
	Check( notRising == 0, fmt( "every drawn ball rises up the tube (%d samples fell)", notRising ) );
	//Slop: one step's closing at 4.5 m/s, 1/1500 s, is 3 mm; the projection
	//takes it out the same step, so a deeper overlap is a solver fault.
	Check( overlap <= 0.003 && escape <= 1e-9, fmt( "balls never overlap more than 3 mm (worst %.2f mm) nor leave the glass (worst %.3g m)", overlap * 1000.0, escape ) );
	{
		const int N = 49;
		const long M = 49L * 300;
		std::vector< long > counts( N, 0 );
		lottery::Request r;
		r.balls      = N;
		r.draw       = 1;
		r.result     = Result::Random;
		r.biasedDraw = perturb.biasedDraw;
		for( long k = 0; k < M; ++k )
		{
			r.seed = static_cast< uint32_t >( k / 1000 );
			r.play = static_cast< uint32_t >( k % 1000 );
			++counts[ static_cast< size_t >( lottery::DrawNumbers( r )[ 0 ] - 1 ) ];
		}
		const double chi = chiSquare( counts, std::vector< double >( N, static_cast< double >( M ) / N ) );
		Check( chi < chiSquareCritical( N - 1 ), fmt( "Random, 49 balls: chi-square %.1f over %ld first balls (critical %.1f)", chi, M, chiSquareCritical( N - 1 ) ) );
	}
	return Verdict();
}

//===========================================================================
// --shower: flight, contacts, the pile, Euler's disk.
//===========================================================================
int runShower( const Perturb& perturb )
{
	std::printf( "\n=== shower: free flight against the closed form, energy at contact, flat in the pile, Euler's disk\n" );
	//Falling face down from rest under quadratic drag: v = -V tanh( g t / V ),
	//y = y0 - V^2 / g ln cosh( g t / V ), V = sqrt( g / k ).
	{
		Shower sh;
		sh.settings.pile   = false;
		sh.settings.size   = 0.06;
		sh.settings.noDrag = perturb.noDrag;
		Piece p;
		p.kind   = PieceKind::Coin;
		p.radius = 0.5 * sh.settings.size * 0.615;
		p.half   = p.radius * 0.075;
		p.q      = FromMatrix( AxisAngle( { 1, 0, 0 }, -0.5 * kPi ) );//the disc's axis vertical: face on to the fall
		p.x      = { 0.0, 50.0, 0.0 };
		const double mpu = sh.MetresPerUnit( p );
		const double g   = 9.81 / mpu;
		const double k   = Shower::kCoinFace * mpu;
		const double V   = std::sqrt( g / k );
		const double T   = 1.5;
		const int steps  = static_cast< int >( std::lround( T / Shower::kDt ) );
		for( int i = 0; i < steps; ++i )
			sh.StepPiece( p );
		const double y = 50.0 - V * V / g * std::log( std::cosh( g * T / V ) );
		const double v = -V * std::tanh( g * T / V );
		//The integrator is first order: its error by T is about half a step's
		//fall at the end speed for every step taken, g T dt / 2, doubled.
		const double tolerance = g * T * Shower::kDt;
		Check( std::fabs( p.x.y - y ) < tolerance && std::fabs( p.v.y - v ) < 2.0 * g * Shower::kDt * 10.0,
		       fmt( "a coin falling face on for %.1f s: y %.4f against %.4f (tolerance %.4f, g T dt), v %.3f against %.3f frame heights/s", T, p.x.y, y, tolerance, p.v.y, v ) );
	}
	//The tumble: in the air nothing torques a piece, so its angular momentum
	//(world) is kept while its spin axis wobbles round it.
	{
		Shower sh;
		sh.settings.pile   = false;
		sh.settings.noDrag = true;
		sh.settings.noGyro = perturb.noGyro;
		Piece p;
		p.kind   = PieceKind::Chip;
		p.radius = 0.03;
		p.half   = p.radius * 0.085;
		p.q      = FromMatrix( AxisAngle( Normalise( V3 { 0.3, 1.0, 0.2 } ), 0.7 ) );
		p.x      = { 0.0, 1e3, 0.0 };
		p.w      = ToMatrix( p.q ) * V3 { 4.0, -2.5, 30.0 };//mostly about its axis, with a wobble
		auto momentum = [ & ]( const Piece& q ) {
			const M3 r     = ToMatrix( q.q );
			const V3 wb    = Transpose( r ) * q.w;
			const double h = 2.0 * q.half;
			const double axial = 0.5 * q.radius * q.radius, across = 0.25 * q.radius * q.radius + h * h / 12.0;
			return r * V3 { across * wb.x, across * wb.y, axial * wb.z };
		};
		const V3 L0  = momentum( p );
		double drift = 0.0, axisMoved = 0.0;
		const V3 axis0 = ToMatrix( p.q ) * V3 { 0, 0, 1 };
		for( int i = 0; i < 1200; ++i )
		{
			sh.StepPiece( p );
			drift     = std::max( drift, Length( momentum( p ) - L0 ) / Length( L0 ) );
			axisMoved = std::max( axisMoved, std::acos( std::clamp( Dot( ToMatrix( p.q ) * V3 { 0, 0, 1 }, axis0 ), -1.0, 1.0 ) ) );
		}
		//The integrator is first order: a step's error in L is about
		//|w| dt of its turn, compounded over 1200 steps of a wobble that comes
		//round on itself -- 2% is the room that allows, and it is a choice.
		Check( drift < 0.02 && axisMoved > 0.1, fmt( "a chip tumbling for 1 s: its angular momentum kept to %.2f%% while its axis wobbled through %.0f degrees", 100.0 * drift, axisMoved * 180.0 / kPi ) );
	}
	//A real shower onto the pile.
	{
		Shower sh;
		sh.settings.pattern = Pattern::Rain;
		sh.settings.pieces  = Pieces::Mixed;
		sh.settings.rate    = 120.0;
		sh.settings.size    = 0.06;
		sh.settings.pile    = true;
		sh.settings.restitution = perturb.superBounce ? 1.3 : -1.0;
		sh.seed = 9;
		sh.Clear();
		sh.Start( 3.0 );
		for( int f = 0; f < 60 * 9; ++f )
			sh.Advance( kFrame, false );
		int resting = 0, flat = 0;
		for( const Piece& p : sh.Pieces() )
			if( p.resting )
			{
				++resting;
				const V3 axis = ToMatrix( p.q ) * V3 { 0, 0, 1 };
				flat += std::fabs( axis.y ) >= std::cos( kPi / 180.0 ) ? 1 : 0;
			}
		Check( sh.energyRises == 0 && sh.contactSteps > 0 && sh.spawned > 0,
		       fmt( "%ld pieces, %ld contact steps: no contact ever left a piece with more energy than it found (%ld did, worst +%.3g)", sh.spawned, sh.contactSteps, sh.energyRises, sh.worstRise ) );
		Check( resting > 0 && flat == resting && sh.worstSnap <= 16.3,
		       fmt( "%d pieces at rest in the pile, every one flat within 1 degree (%d); none was turned more than %.1f degrees to lie down (the settle's 16-degree gate)", resting, flat, sh.worstSnap ) );
	}
	//Euler's disk: a coin rolling on its edge, leaning over. A disc rolling
	//at lean alpha turns its contact round at Omega^2 = 4 g / ( r sin alpha ):
	//faster and faster as it flattens, which is the whirr.
	{
		Shower sh;
		sh.settings.pile   = true;
		sh.settings.size   = 0.1;
		sh.settings.ballInertia = perturb.ballInertia;
		sh.Clear();
		Piece p;
		p.kind   = PieceKind::Coin;
		p.radius = 0.05;
		p.half   = p.radius * 0.075;
		const double alpha = 0.35;
		p.q = FromMatrix( AxisAngle( { 0, 0, 1 }, -alpha ) * AxisAngle( { 1, 0, 0 }, -0.5 * kPi ) );
		p.x = { 0.0, Shower::kFloor + p.radius * std::sin( alpha ) + p.half * std::cos( alpha ) + 1e-4, 0.0 };
		const double g = 9.81 / sh.MetresPerUnit( p );
		p.w            = { 0.0, std::sqrt( 4.0 * g / ( p.radius * std::sin( alpha ) ) ), 0.0 };
		struct Window
		{
			double lean, rate;
		};
		std::vector< Window > windows;
		double lastAz = 0.0, turned = 0.0, lean = 0.0;
		int n = 0;
		for( int step = 0; step < 1200 * 6 && !p.resting; ++step )
		{
			sh.StepPiece( p );
			const V3 axis = ToMatrix( p.q ) * V3 { 0, 0, 1 };
			const V3 down = V3 { 0, -1, 0 } - axis * Dot( V3 { 0, -1, 0 }, axis );
			const double az = std::atan2( down.z, down.x );
			if( step > 0 )
			{
				double d = az - lastAz;
				d -= 2.0 * kPi * std::floor( d / ( 2.0 * kPi ) + 0.5 );
				turned += d;
				lean += std::acos( std::clamp( std::fabs( axis.y ), 0.0, 1.0 ) );
				++n;
			}
			lastAz = az;
			if( n == 120 )
			{
				windows.push_back( { lean / n, std::fabs( turned ) / ( n * Shower::kDt ) } );
				turned = lean = 0.0;
				n = 0;
			}
		}
		//Every window after the launch's first quarter second, leaning 1 to 10
		//degrees, against the law. 15% is a choice: the law is for a disc of no
		//thickness (this one is 0.15 r thick) at a steady lean (the window
		//averages a lean that falls through it).
		int within = 0, judged = 0;
		double slowest = 1e9, fastest = 0.0, worst = 0.0;
		std::string trace;
		for( size_t i = 3; i < windows.size(); ++i )
		{
			const Window& w = windows[ i ];
			if( w.lean < kPi / 180.0 || w.lean > 10.0 * kPi / 180.0 )
				continue;
			const double law = std::sqrt( 4.0 * g / ( p.radius * std::sin( w.lean ) ) );
			const double err = std::fabs( w.rate / law - 1.0 );
			worst            = std::max( worst, err );
			within += err < 0.15 ? 1 : 0;
			++judged;
			slowest = std::min( slowest, w.rate );
			fastest = std::max( fastest, w.rate );
			trace += fmt( " %.1f deg %.0f/%.0f", w.lean * 180.0 / kPi, w.rate, law );
		}
		Check( judged >= 5 && within == judged && fastest > 1.8 * slowest,
		       fmt( "Euler's disk: %d windows from 10 to 1 degree of lean, the contact's rate within 15%% of Omega^2 = 4 g / ( r sin alpha ) in %d (worst %.0f%%), rising %.1fx as it flattened:%s",
		            judged, within, 100.0 * worst, fastest / std::max( slowest, 1e-9 ), trace.c_str() ) );
	}
	return Verdict();
}

//===========================================================================
// --duration: every game ends at Spin Time.
//===========================================================================
struct GameState
{
	std::vector< double > values;
	bool operator==( const GameState& o ) const { return values == o.values; }
	double Distance( const GameState& o ) const
	{
		double d = 0.0;
		for( size_t i = 0; i < values.size() && i < o.values.size(); ++i )
			d = std::max( d, std::fabs( values[ i ] - o.values[ i ] ) );
		return values.size() == o.values.size() ? d : 1e9;
	}
};

/// What a viewer sees moving, game by game: the reels; the ball against the
/// ring; the painted wheel; the dice; the balls.
GameState stateAt( Game game, const Play& play, double t )
{
	GameState s;
	switch( game )
	{
	case Game::Slots:
		for( int i = 0; i < play.slots.request.reels; ++i )
			s.values.push_back( play.slots.Position( i, t ) );
		break;
	case Game::Roulette:
	{
		V3 b;
		if( play.roulette.Ball( t, b ) )
		{
			//By its sine and cosine: an angle near pi must not read as a jump.
			const double inRing = std::atan2( b.y, b.x ) - play.roulette.PaintAngle( t );
			s.values = { std::cos( inRing ), std::sin( inRing ), std::hypot( b.x, b.y ), b.z };
		}
		break;
	}
	case Game::MoneyWheel: s.values = { play.wheel.PaintAngle( t ), play.wheel.Clapper( t ) }; break;
	case Game::Craps:
		for( int i = 0; i < 2; ++i )
		{
			V3 x;
			M3 r;
			if( play.craps.DieAt( i, t, x, r ) )
				s.values.insert( s.values.end(), { x.x, x.y, x.z, r.m[ 0 ][ 0 ], r.m[ 1 ][ 1 ], r.m[ 2 ][ 2 ], r.m[ 0 ][ 1 ] } );
		}
		break;
	case Game::Lottery:
		for( const lottery::BallState& b : play.lottery.Balls( t ) )
			s.values.insert( s.values.end(), { b.x.x, b.x.y, b.x.z } );
		break;
	default: break;
	}
	return s;
}

Play makePlay( Game game, double duration, uint32_t seed, bool noWarp )
{
	Play p;
	p.game = game;
	switch( game )
	{
	case Game::Slots:
	{
		slots::Request r;
		r.seed = seed, r.duration = duration;
		p.slots = slots::MakePlan( r );
		break;
	}
	case Game::Roulette:
	{
		roulette::Request r;
		r.seed = seed, r.duration = duration, r.noWarp = noWarp;
		p.roulette = roulette::MakePlan( r );
		break;
	}
	case Game::MoneyWheel:
	{
		wheel::Request r;
		r.seed = seed, r.duration = duration, r.noWarp = noWarp;
		r.stiffness = ClapperFromParam( 0.23f );
		p.wheel     = wheel::MakePlan( r );
		break;
	}
	case Game::Craps:
	{
		craps::Request r;
		r.seed = seed, r.duration = duration, r.noWarp = noWarp;
		p.craps = craps::MakePlan( r );
		break;
	}
	case Game::Lottery:
	{
		lottery::Request r;
		r.seed = seed, r.duration = duration, r.noWarp = noWarp;
		r.air = AirFromParam( 0.43f );
		p.lottery = lottery::MakePlan( r );
		break;
	}
	default: break;
	}
	return p;
}

int runDuration( const Perturb& perturb )
{
	std::printf( "\n=== duration: every game at rest at exactly Spin Time, still moving a frame before\n" );
	const Game games[] = { Game::Slots, Game::Roulette, Game::MoneyWheel, Game::Craps, Game::Lottery };
	const char* names[] = { "slots", "roulette", "money wheel", "craps", "lottery" };
	for( int g = 0; g < 5; ++g )
	{
		int wrongEnd = 0, notMoving = 0, notStill = 0, plays = 0;
		double warpLo = 1e9, warpHi = 0.0, worstEase = 1.0;
		std::string warps;
		for( double D : { 3.0, 6.0, 12.0 } )
			for( uint32_t seed = 1; seed <= 3; ++seed )
			{
				const Play p = makePlay( games[ g ], D, seed, perturb.noWarp && games[ g ] != Game::Slots );
				++plays;
				const Playback& clock = p.Clock();
				wrongEnd += std::fabs( clock.duration - D ) > 1e-9 ? 1 : 0;
				const GameState atEnd = stateAt( games[ g ], p, D ), before = stateAt( games[ g ], p, D - kFrame ), later = stateAt( games[ g ], p, D + 0.75 );
				notMoving += atEnd.Distance( before ) > 1e-9 ? 0 : 1;
				notStill += atEnd.Distance( later ) < 1e-9 ? 0 : 1;
				warps += fmt( " %.0fs:%.2f", D, clock.warp );
				warpLo    = std::min( warpLo, clock.warp );
				warpHi    = std::max( warpHi, clock.warp );
				if( clock.rateStart > 0.0 )
					worstEase = std::min( worstEase, clock.rateEnd / clock.rateStart );
			}
		Check( wrongEnd == 0 && notMoving == 0 && notStill == 0 && worstEase >= 0.65 - 1e-12,
		       fmt( "%s: %d plays of 3, 6 and 12 s: ends at Spin Time (%d not), moving a frame before (%d not), still after (%d not); warp %.2f..%.2f, the ease never below %.2f", names[ g ], plays,
		            wrongEnd, notMoving, notStill, warpLo, warpHi, worstEase ) );
		std::printf( "        warps (simulated seconds per second played):%s\n", warps.c_str() );
	}
	//Land On Beat and Bar: the wait ends on the boundary, at least Spin Time away.
	{
		int off = 0, n = 0;
		for( double bpm : { 90.0, 120.0, 128.0, 174.0 } )
			for( double phase : { 0.0, 0.13, 0.5, 0.99 } )
				for( bool bar : { false, true } )
					for( double lead : { 1.0, 4.0 } )
					{
						Transport tr;
						tr.SetBeatInfo( bpm, phase );
						tr.Advance( 0.0 );
						const double wait   = tr.SecondsToNext( bar, lead );
						const double period = bar ? tr.BarSeconds() : tr.BeatSeconds();
						const double pos    = ( bar ? phase : std::fmod( phase * 4.0, 1.0 ) ) + wait / period;
						off += ( std::fabs( pos - std::round( pos ) ) > 1e-9 || wait < lead || wait >= lead + period ) ? 1 : 0;
						++n;
					}
		Check( off == 0, fmt( "Land On Beat and Bar: %d waits, every one ends on a boundary, the first at least Spin Time away (%d not)", n, off ) );
	}
	return Verdict();
}

//===========================================================================
// --defaults, --names, --determinism
//===========================================================================
int runDefaults( const Perturb& perturb )
{
	std::printf( "\n=== defaults: a three-reel fruit machine in a dark room, showering on a win\n" );
	for( bool effect : { false, true } )
	{
		JackpotPlugin plugin( effect );
		const Game want = perturb.defaultsShifted ? Game::Roulette : Game::Slots;
		const bool ok = plugin.CurrentGame() == want && std::fabs( SpinTimeFromParam( plugin.GetFloatParameter( PT_SPIN_TIME ) ) - 4.0 ) < 1e-4
		                && OptionIndex( plugin.GetFloatParameter( PT_RESULT ), 6 ) == static_cast< int >( Result::Random )
		                && OptionIndex( plugin.GetFloatParameter( PT_REELS ), 2 ) == 0 && OptionIndex( plugin.GetFloatParameter( PT_SHOWER ), 4 ) == static_cast< int >( ShowerWhen::OnWin )
		                && OptionIndex( plugin.GetFloatParameter( PT_BACKDROP ), 3 ) == static_cast< int >( Backdrop::Dark )
		                && std::fabs( ZoomFromParam( plugin.GetFloatParameter( PT_ZOOM ) ) - 1.0 ) < 0.01 && ( !effect || plugin.GetFloatParameter( PT_MIX ) == 1.0f );
		Check( ok, fmt( "%s: Slots, 4 s, Random, 3 reels, shower On Win, Dark, Zoom 1%s", effect ? "Over" : "source", effect ? ", Mix 1" : "" ) );
	}
	return Verdict();
}

int runNames( const Perturb& )
{
	std::printf( "\n=== names: what a host and an OSC address see\n" );
	for( bool effect : { false, true } )
	{
		JackpotPlugin plugin( effect );
		std::set< std::string > seen;
		int clashes = 0, slashes = 0, emptyElements = 0;
		for( unsigned int i = 0; i < plugin.ParamCount(); ++i )
		{
			std::string name = plugin.GetParamName( i );
			slashes += name.find( '/' ) != std::string::npos ? 1 : 0;
			std::string key;
			for( char c : name )
				if( c != ' ' )
					key += static_cast< char >( std::tolower( static_cast< unsigned char >( c ) ) );
			clashes += seen.insert( key ).second ? 0 : 1;
			if( plugin.GetParamType( i ) == FF_TYPE_OPTION )
				for( unsigned int e = 0; e < plugin.GetNumParamElements( i ); ++e )
				{
					const char* element = plugin.GetParamElementName( i, e );
					emptyElements += ( element == nullptr || element[ 0 ] == '\0' ) ? 1 : 0;
				}
		}
		const int symbols = static_cast< int >( plugin.GetNumParamElements( PT_SYMBOLS ) );
		Check( clashes == 0 && slashes == 0 && emptyElements == 0 && symbols == ( effect ? 3 : 2 ),
		       fmt( "%s: %u parameters, unique as Arena addresses them (%d clash), no '/' (%d), no empty option (%d), Symbols has %d (Clip on the effect only)", effect ? "Over" : "source",
		            plugin.ParamCount(), clashes, slashes, emptyElements, symbols ) );
	}
	return Verdict();
}

int runDeterminism( const Perturb& perturb )
{
	std::printf( "\n=== determinism: the same request is the same play, to the bit\n" );
	const Game games[] = { Game::Slots, Game::Roulette, Game::MoneyWheel, Game::Craps, Game::Lottery };
	const char* names[] = { "slots", "roulette", "money wheel", "craps", "lottery" };
	for( int g = 0; g < 5; ++g )
	{
		const Play a = makePlay( games[ g ], 5.0, 3, false ), b = makePlay( games[ g ], 5.0, 3, false );
		const Play c = makePlay( games[ g ], 5.0, perturb.determinismSeeds ? 3 : 4, false );
		bool same = a.Result().text == b.Result().text && a.Clock().natural == b.Clock().natural;
		bool differ = false;
		for( double t = 0.0; t <= 5.0; t += 0.25 )
		{
			same   = same && stateAt( games[ g ], a, t ) == stateAt( games[ g ], b, t );
			differ = differ || !( stateAt( games[ g ], a, t ) == stateAt( games[ g ], c, t ) );
		}
		Check( same && differ, fmt( "%s: seed 3 twice is one play (%s); seed 4 is another (%s)", names[ g ], same ? "yes" : "NO", differ ? "yes" : "NO" ) );
	}
	return Verdict();
}

//===========================================================================
// The picture checks (GL).
//===========================================================================
/// The id picture's G channel at a world point: the number painted there.
int idAt( const Floats& img, const JackpotPlugin::Camera& cam, V3 p )
{
	const V3 at = cam.Project( p );
	const int x = static_cast< int >( std::floor( at.x ) ), y = static_cast< int >( std::floor( at.y ) );
	if( at.z <= 0.0 || x < 0 || y < 0 || x >= cam.width || y >= cam.height )
		return -1;
	return static_cast< int >( std::lround( img[ ( static_cast< size_t >( y ) * cam.width + x ) * 4 + 1 ] * 64.0f ) );
}

int runRouletteReadback( const Perturb& perturb )
{
	std::printf( "\n=== roulette readback: the number painted under the ball and beside it, read from the picture\n" );
	for( const auto& size : { std::pair< int, int > { 640, 360 }, std::pair< int, int > { 320, 180 } } )
		for( RouletteWheel wheel : { RouletteWheel::European, RouletteWheel::American } )
		{
			Rig rig;
			if( !rig.Init( size.first, size.second ) )
				return 1;
			rig.plugin.Flags().noShift = perturb.noShift;
			setGame( rig, Game::Roulette );
			rig.Set( PT_WHEEL, static_cast< float >( wheel ) );
			rig.Set( PT_RESULT, static_cast< float >( Result::Fixed ) );
			rig.Set( PT_SPIN_TIME, ParamFromSpinTime( 4.0 ) );
			int read = 0, wrong = 0;
			const int asks[] = { 0, 17, 32, 5, wheel == RouletteWheel::American ? 37 : 26 };
			for( int n : asks )
			{
				rig.Set( PT_FIXED_NUMBER, static_cast< float >( n ) );
				rig.plugin.SetFlatForTest( 0 );
				if( !rig.PlayToRest( 30 ) )
					return 1;
				rig.plugin.SetFlatForTest( 2 );
				rig.Render( 1 );
				const Floats img = rig.Output();
				const roulette::Plan& p = rig.plugin.CurrentPlay().roulette;
				V3 ball;
				p.Ball( rig.plugin.PlaybackSeconds(), ball );
				const double a = std::atan2( ball.y, ball.x );
				const JackpotPlugin::Camera& cam = rig.plugin.LastCamera();
				//The pocket's floor under the ball, and the number band beside it.
				const int floorReads = idAt( img, cam, { ball.x, ball.y, -roulette::kPocketDepth } );
				const double rBand   = 0.5 * ( roulette::kPocketOut + roulette::kRotorOut );
				const int bandReads  = idAt( img, cam, { rBand * std::cos( a ), rBand * std::sin( a ), roulette::kApronSlope * ( rBand - roulette::kPocketOut ) } );
				read += 2;
				wrong += ( floorReads != n ) + ( bandReads != n );
			}
			Check( wrong == 0, fmt( "%dx%d %s: %d numbers read, %d wrong", rig.width, rig.height, wheel == RouletteWheel::European ? "European" : "American", read, wrong ) );
		}
	return Verdict();
}

/// The id picture of the money wheel: the segment under the clapper.
int runWheelReadback( const Perturb& perturb )
{
	std::printf( "\n=== money wheel readback: the segment under the clapper, read from the picture\n" );
	for( const auto& size : { std::pair< int, int > { 640, 360 }, std::pair< int, int > { 320, 180 } } )
	{
		Rig rig;
		if( !rig.Init( size.first, size.second ) )
			return 1;
		rig.plugin.Flags().noShift = perturb.noShift;
		setGame( rig, Game::MoneyWheel );
		rig.Set( PT_RESULT, static_cast< float >( Result::Fixed ) );
		rig.Set( PT_SPIN_TIME, ParamFromSpinTime( 5.0 ) );
		int read = 0, wrong = 0;
		for( int bet : { 1, 2, 5, 10, 20, 40, 45 } )
		{
			rig.Set( PT_FIXED_NUMBER, static_cast< float >( bet ) );
			rig.plugin.SetFlatForTest( 0 );
			if( !rig.PlayToRest( 30 ) )
				return 1;
			rig.plugin.SetFlatForTest( 2 );
			rig.Render( 1 );
			const Floats img = rig.Output();
			//Just inside the pegs, straight below the clapper: kWheel's canvas
			//(the hub at (0, -0.035), 0.46 canvas units a metre).
			const double zoom = ZoomFromParam( rig.plugin.GetFloatParameter( PT_ZOOM ) );
			const double cy   = -0.035 + 0.46 * 0.78;
			const int x = rig.width / 2, y = static_cast< int >( std::floor( cy * rig.height * zoom + 0.5 * rig.height ) );
			const int value = static_cast< int >( std::lround( img[ ( static_cast< size_t >( y ) * rig.width + x ) * 4 + 1 ] * 64.0f ) );
			++read;
			wrong += value != bet ? 1 : 0;
		}
		Check( wrong == 0, fmt( "%dx%d: %d results read under the clapper, %d wrong", rig.width, rig.height, read, wrong ) );
	}
	return Verdict();
}

//===========================================================================
// --over-check: the effect leaves the clip alone where there is no game.
//===========================================================================
int runOver( const Perturb& )
{
	std::printf( "\n=== over: the clip (premultiplied) untouched where there is no game, Mix 0 the clip everywhere, its alpha kept\n" );
	for( const auto& size : { std::pair< int, int > { 640, 360 }, std::pair< int, int > { 320, 180 } } )
	{
		//A card with alpha: the left third transparent, the middle half-clear.
		Floats card = buildCard( size.first, size.second );
		for( int y = 0; y < size.second; ++y )
			for( int x = 0; x < size.first; ++x )
			{
				float* o = &card[ ( static_cast< size_t >( y ) * size.first + x ) * 4 ];
				const float a = x < size.first / 3 ? 0.0f : x < 2 * size.first / 3 ? 0.5f : 1.0f;
				for( int c = 0; c < 3; ++c )
					o[ c ] *= a;
				o[ 3 ] = a;
			}
		for( Game game : { Game::Slots, Game::MoneyWheel } )
		{
			Rig rig( true );
			if( !rig.Init( size.first, size.second, &card ) )
				return 1;
			setGame( rig, game );
			rig.Set( PT_BACKDROP, static_cast< float >( Backdrop::None ) );
			rig.Set( PT_SHOWER, static_cast< float >( ShowerWhen::Off ) );
			rig.Render( 3 );
			const Floats out = rig.Output();
			//Far outside the game: the corner columns of the frame. The round
			//trip through linear (pow 2.2, then 1/2.2) is the only change, a
			//few float ULPs.
			double worst = 0.0;
			for( int y = 0; y < size.second; ++y )
				for( int x : { 0, 1, size.first - 2, size.first - 1 } )
					for( int c = 0; c < 4; ++c )
					{
						const size_t i = ( static_cast< size_t >( y ) * size.first + x ) * 4 + c;
						worst          = std::max( worst, static_cast< double >( std::fabs( out[ i ] - card[ i ] ) ) );
					}
			rig.Set( PT_MIX, 0.0f );
			rig.Render( 1 );
			const Floats mixed = rig.Output();
			double worstMix = 0.0;
			for( size_t i = 0; i < mixed.size(); ++i )
				worstMix = std::max( worstMix, static_cast< double >( std::fabs( mixed[ i ] - card[ i ] ) ) );
			Check( worst < 1e-5 && worstMix < 1e-5, fmt( "%dx%d %s: the frame's edges are the clip, alpha and all, to %.1e; Mix 0 is the clip everywhere to %.1e", size.first, size.second,
			                                          game == Game::Slots ? "slots" : "money wheel", worst, worstMix ) );
		}
	}
	return Verdict();
}

//===========================================================================
// --resize: a shower that is falling when the frame changes size keeps falling.
//===========================================================================
int runResize( const Perturb& )
{
	std::printf( "\n=== resize: mid-shower, 640x360 to 320x180\n" );
	Rig rig;
	if( !rig.Init( 640, 360 ) )
		return 1;
	setGame( rig, Game::ShowerOnly );
	rig.Set( PT_SHOWER, static_cast< float >( ShowerWhen::Always ) );
	rig.Render( 90 );
	const size_t before = rig.plugin.CurrentShower().Pieces().size();
	const long spawned  = rig.plugin.CurrentShower().spawned;
	rig.Resize( 320, 180 );
	rig.Render( 1 );
	const size_t after = rig.plugin.CurrentShower().Pieces().size();
	const Floats img   = rig.Output();
	double ink = 0.0;
	for( size_t i = 3; i < img.size(); i += 4 )
		ink += img[ i ];
	Check( before > 50 && after + 30 >= before && rig.plugin.CurrentShower().spawned >= spawned && glGetError() == GL_NO_ERROR,
	       fmt( "%zu pieces before, %zu after: the shower carried on at the new size, no GL error", before, after ) );
	Check( ink > 0.0, fmt( "the new size draws (alpha summed %.0f)", ink ) );
	return Verdict();
}

//===========================================================================
// --fonts: a font by name, a missing font, a font file.
//===========================================================================
int runFonts( const Perturb& )
{
	std::printf( "\n=== fonts: by name, missing, by file\n" );
	Rig rig;
	if( !rig.Init( 320, 180 ) )
		return 1;
	const std::vector< FontFile >& fonts = InstalledFonts();
	std::string family, path;
	for( const FontFile& f : fonts )
		if( f.family == "Georgia" || f.family == "Helvetica" || ( family.empty() && f.collectionIndex == 0 ) )
		{
			family = f.family;
			path   = f.path;
			if( family == "Georgia" )
				break;
		}
	if( family.empty() )
	{
		Check( false, "no installed font to test with" );
		return Verdict();
	}
	rig.plugin.SetTextParameter( PT_FONT_NAME, family.c_str() );
	rig.Render( 1 );
	Check( rig.plugin.FontFamily() == family, fmt( "Font Name '%s' loads it (%s)", family.c_str(), rig.plugin.FontFamily().c_str() ) );
	rig.plugin.SetTextParameter( PT_FONT_NAME, "No Such Typeface" );
	rig.Render( 1 );
	Check( rig.plugin.FontFamily().empty(), "a Font Name not installed here falls back to the built-in face" );
	rig.plugin.SetTextParameter( PT_FONT_FILE, path.c_str() );
	rig.Render( 1 );
	Check( !rig.plugin.FontFamily().empty(), fmt( "Font File %s loads (%s)", path.c_str(), rig.plugin.FontFamily().c_str() ) );
	return Verdict();
}

//===========================================================================
// --state: what carries from play to play.
//===========================================================================
int runState( const Perturb& )
{
	std::printf( "\n=== state: credits, the craps point, the wheels turning on between plays\n" );
	Rig rig;
	if( !rig.Init( 320, 180 ) )
		return 1;
	rig.Set( PT_SPIN_TIME, ParamFromSpinTime( 2.0 ) );
	rig.Set( PT_RESULT, static_cast< float >( Result::Jackpot ) );
	const int credits = rig.plugin.Credits();
	rig.PlayToRest();
	Check( rig.plugin.Credits() == credits - 1 + 200, fmt( "a jackpot on three reels: credits %d -> %d (a credit to play, 200 paid)", credits, rig.plugin.Credits() ) );

	//A pull while the reels run: they carry on from where they are. (Started
	//from the last rest instead, they jumped back a lap or more.)
	{
		rig.Set( PT_RESULT, static_cast< float >( Result::Random ) );
		rig.Press( PT_PLAY );
		rig.Render( 60 );
		double before[ 3 ];
		for( int i = 0; i < 3; ++i )
			before[ i ] = rig.plugin.CurrentPlay().slots.Position( i, rig.plugin.PlaybackSeconds() );
		rig.Press( PT_PLAY );
		rig.Render( 1 );
		double worst = 0.0;
		for( int i = 0; i < 3; ++i )
			worst = std::max( worst, std::fabs( rig.plugin.CurrentPlay().slots.Position( i, rig.plugin.PlaybackSeconds() ) - before[ i ] ) );
		//A frame at the fastest a reel runs (about 60 stops a second) is one stop.
		Check( worst < 1.0, fmt( "slots: Play again mid-spin, no reel moved more than %.2f stops in the frame (a frame's run is under 1)", worst ) );
	}

	setGame( rig, Game::Craps );
	rig.Set( PT_RESULT, static_cast< float >( Result::Fixed ) );
	rig.Set( PT_FIXED_NUMBER, 6.0f );
	rig.Render( 2 );
	rig.PlayToRest();
	const int point = rig.plugin.CrapsPoint();
	rig.PlayToRest();
	const int after = rig.plugin.CrapsPoint();
	Check( point == 6 && after == 0 && rig.plugin.CurrentPlay().Result().win, fmt( "craps: a 6 on the come-out is the point (%d); a 6 again makes it and the puck goes OFF (%d)", point, after ) );

	//The roulette ring turns on from where it was when Play is pressed: no jump.
	setGame( rig, Game::Roulette );
	rig.Set( PT_RESULT, static_cast< float >( Result::Random ) );
	rig.Render( 2 );
	rig.PlayToRest( 20 );
	const double t0 = rig.plugin.PlaybackSeconds();
	const double a0 = rig.plugin.CurrentPlay().roulette.PaintAngle( t0 );
	rig.Press( PT_PLAY );
	rig.Render( 1 );
	const double a1 = rig.plugin.CurrentPlay().roulette.PaintAngle( rig.plugin.PlaybackSeconds() );
	const double step = std::fabs( std::remainder( a1 - a0, 2.0 * kPi ) );
	Check( step < 0.2, fmt( "roulette: across a new Play the ring moved %.3f rad in a frame (it is turned by the croupier's hand, not jumped)", step ) );
	return Verdict();
}

//===========================================================================
// --bench
//===========================================================================
int runBench()
{
	struct Size
	{
		int w, h;
		const char* name;
	};
	const Size sizes[] = { { 1280, 720, "720p" }, { 1920, 1080, "1080p" }, { 3840, 2160, "4K" } };
	const Game games[] = { Game::Slots, Game::Roulette, Game::MoneyWheel, Game::Craps, Game::Lottery, Game::ShowerOnly };
	const char* names[] = { "slots", "roulette", "money wheel", "craps", "lottery (49 balls)", "shower (Always)" };
	std::printf( "\n=== bench: ms/frame mid-play (Dark backdrop, a shower falling over the shower row), median of 40 frames\n" );
	for( int g = 0; g < 6; ++g )
		for( const Size& size : sizes )
		{
			Rig rig;
			if( !rig.Init( size.w, size.h ) )
				return 1;
			setGame( rig, games[ g ] );
			rig.Set( PT_SPIN_TIME, ParamFromSpinTime( 12.0 ) );
			if( games[ g ] == Game::ShowerOnly )
				rig.Set( PT_SHOWER, static_cast< float >( ShowerWhen::Always ) );
			rig.Render( 2 );
			rig.Press( PT_PLAY );
			rig.Render( games[ g ] == Game::Lottery ? 300 : 120 );
			glFinish();
			std::vector< double > times;
			for( int f = 0; f < 40; ++f )
			{
				const auto start = std::chrono::steady_clock::now();
				rig.Render( 1 );
				glFinish();
				times.push_back( std::chrono::duration< double, std::milli >( std::chrono::steady_clock::now() - start ).count() );
			}
			std::sort( times.begin(), times.end() );
			std::printf( "  %-20s %-6s median %6.2f ms/frame, worst %6.2f  (%4.1f%% of 60 fps)\n", names[ g ], size.name, times[ 20 ], times.back(), 100.0 * times[ 20 ] / ( 1000.0 / 60.0 ) );
		}
	std::printf( "\n=== bench: planning a play (CPU, on the worker), median of 5\n" );
	for( int g = 1; g < 5; ++g )
	{
		std::vector< double > ms;
		for( uint32_t seed = 1; seed <= 5; ++seed )
		{
			const auto start = std::chrono::steady_clock::now();
			makePlay( games[ g ], 8.0, seed, false );
			ms.push_back( std::chrono::duration< double, std::milli >( std::chrono::steady_clock::now() - start ).count() );
		}
		std::sort( ms.begin(), ms.end() );
		std::printf( "  %-20s median %7.1f ms, worst %7.1f ms\n", names[ g ], ms[ 2 ], ms.back() );
	}
	return 0;
}

//===========================================================================
// The registry, --offline and --negative.
//===========================================================================
const std::vector< CheckEntry >& checks()
{
	static const std::vector< CheckEntry > list = {
		{ "slots", runSlots },       { "nearmiss", runNearMiss },       { "blur", runBlur },
		{ "slots-readback", runSlotsReadback }, { "roulette", runRoulette }, { "roulette-readback", runRouletteReadback },
		{ "wheel", runWheel },       { "wheel-readback", runWheelReadback }, { "craps", runCraps },
		{ "lottery", runLottery },   { "shower", runShower },           { "duration", runDuration },
		{ "defaults", runDefaults }, { "names", runNames },             { "determinism", runDeterminism },
		{ "over-check", runOver },   { "resize", runResize },           { "fonts", runFonts },
		{ "state", runState },
	};
	return list;
}

/// The checks that need no GL context. The one place that knows which they
/// are -- `--offline` (what CI runs) is everything else's complement.
bool isOffline( const std::string& flag )
{
	static const char* const offline[] = { "slots", "nearmiss", "roulette", "wheel", "craps", "lottery", "shower", "duration", "defaults", "names", "determinism" };
	for( const char* name : offline )
		if( flag == name )
			return true;
	return false;
}

int runNegative( bool offlineOnly = false )
{
	struct Case
	{
		const char* name;
		CheckFn check;
		Perturb perturb;
		const char* what;
	};
	std::vector< Case > cases;
	auto add = [ & ]( const char* name, CheckFn fn, const char* what, std::function< void( Perturb& ) > set ) {
		Perturb p;
		set( p );
		cases.push_back( { name, fn, p, what } );
	};
	add( "roulette", runRoulette, "no pocket shift: the physics' own pocket shows", []( Perturb& p ) { p.noShift = true; } );
	add( "roulette", runRoulette, "the shift reaches the frets (half a pocket): the trajectory is not the same", []( Perturb& p ) { p.fretsSeeShift = true; } );
	add( "roulette", runRoulette, "Random drawn Below( N - 1 ): the last pocket never comes up", []( Perturb& p ) { p.biasedDraw = true; } );
	add( "roulette-readback", runRouletteReadback, "no pocket shift, read from the picture", []( Perturb& p ) { p.noShift = true; } );
	add( "wheel", runWheel, "no segment shift", []( Perturb& p ) { p.noShift = true; } );
	add( "wheel", runWheel, "the pegs turned half a segment: the trajectory is not the same", []( Perturb& p ) { p.pegsSeeShift = true; } );
	add( "wheel", runWheel, "Random drawn Below( 53 )", []( Perturb& p ) { p.biasedDraw = true; } );
	add( "wheel-readback", runWheelReadback, "no segment shift, read from the picture", []( Perturb& p ) { p.noShift = true; } );
	add( "nearmiss", runNearMiss, "a uniform virtual reel: the near miss is the physical strip's", []( Perturb& p ) { p.uniformVirtual = true; } );
	add( "slots-readback", runSlotsReadback, "the reels drawn one stop on", []( Perturb& p ) { p.reelOffset = true; } );
	add( "blur", runBlur, "scored against the instant, not the shutter", []( Perturb& p ) { p.noBlur = true; } );
	add( "craps", runCraps, "S = identity: the physics' own faces show", []( Perturb& p ) { p.identitySymmetry = true; } );
	add( "lottery", runLottery, "the balls painted as numbered: the physics' own catch shows", []( Perturb& p ) { p.identityPermutation = true; } );
	add( "lottery", runLottery, "Random drawn Below( n - 1 )", []( Perturb& p ) { p.biasedDraw = true; } );
	add( "duration", runDuration, "played at 1x: each game stops when it stops", []( Perturb& p ) { p.noWarp = true; } );
	add( "shower", runShower, "restitution 1.3: a contact adds energy", []( Perturb& p ) { p.superBounce = true; } );
	add( "shower", runShower, "no drag: the fall is ballistic, not the closed form", []( Perturb& p ) { p.noDrag = true; } );
	add( "shower", runShower, "no gyroscopic term: a tumbling chip's angular momentum is not kept", []( Perturb& p ) { p.noGyro = true; } );
	add( "shower", runShower, "a ball's moments of inertia: the coin does not roll like Euler's disk", []( Perturb& p ) { p.ballInertia = true; } );
	add( "defaults", runDefaults, "expect the default game to be roulette", []( Perturb& p ) { p.defaultsShifted = true; } );
	add( "determinism", runDeterminism, "expect seeds 3 and 4 to agree", []( Perturb& p ) { p.determinismSeeds = true; } );

	if( offlineOnly )
		cases.erase( std::remove_if( cases.begin(), cases.end(), []( const Case& c ) { return !isOffline( c.name ); } ), cases.end() );

	int unfalsifiable = 0;
	for( const Case& c : cases )
	{
		std::printf( "\n=== negative control: %s -- %s\n", c.name, c.what );
		const int before = g_failures;
		g_failures       = 0;
		c.check( c.perturb );
		const int observed = g_failures;
		g_failures         = before;
		if( observed > 0 )
			std::printf( "  ok    %s failed %d check%s, as it must\n", c.name, observed, observed == 1 ? "" : "s" );
		else
		{
			std::printf( "  FAIL  %s PASSED against a wrong model -- it cannot fail, so it is not a check\n", c.name );
			++unfalsifiable;
		}
	}
	std::printf( "\nnegative controls: %zu wrong models, %d of them undetected\n", cases.size(), unfalsifiable );
	std::printf( "\n  %s\n", unfalsifiable == 0 ? "PASS" : "FAIL" );
	return unfalsifiable == 0 ? 0 : 1;
}

std::string describePlay( const JackpotPlugin& plugin )
{
	const Play& play = plugin.CurrentPlay();
	const Outcome& o = play.Result();
	return fmt( "%s%s%s, natural %.2f s, warp %.2f", o.text.c_str(), o.jackpot ? " (JACKPOT)" : o.win ? " (win)" : "",
	            o.nearMiss ? " (near miss)" : "", play.Clock().natural, play.Clock().warp );
}
} // namespace

//---------------------------------------------------------------------------
int main( int argc, char** argv )
{
	std::string outPath = "/tmp/jackpot.png";
	std::vector< std::string > settings;
	int width = 1280, height = 720, frames = -1;
	std::vector< int > plays;
	bool effect = false;
	std::string mode, scriptPath;
	int filmFrames = -1;

	for( int i = 1; i < argc; ++i )
	{
		const std::string argument = argv[ i ];
		const bool hasNext         = i + 1 < argc;
		if( argument == "--help" || argument == "-h" )
		{
			std::printf( "jptest -- render Jackpot offline and measure its games\n\n"
			             "  --out PATH        render and write a PNG (default /tmp/jackpot.png)\n"
			             "  --over            the Over effect, on the harness's card\n"
			             "  --size WxH        render size (default 1280x720)\n"
			             "  --frames N        frames of 60 fps before reading back (default: one play to rest)\n"
			             "  --play N          press Play on frame N. Repeatable. (default: frame 0)\n"
			             "  --set \"Name=V\"    set a parameter by its display name; an option by\n"
			             "                    its index or its name. Repeatable.\n"
			             "  --list            every parameter and its default\n"
			             "  --pipe            raw RGBA frames in (Over) and out\n"
			             "  --film N          N frames, raw RGBA on stdout\n"
			             "  --script PATH     cues for --pipe/--film: 'frame Name value'\n\n"
			             "  --shaders --out DIR   every program as compiled, written out and through the driver\n\n"
			             "  checks: --slots --nearmiss --blur --slots-readback --roulette --roulette-readback\n"
			             "          --wheel --wheel-readback --craps --lottery --shower --duration --defaults\n"
			             "          --names --determinism --over-check --resize --fonts --state --negative --bench\n"
			             "  --offline         the checks and negative controls that need no GL context\n" );
			return 0;
		}
		else if( argument == "--out" && hasNext )
			outPath = argv[ ++i ];
		else if( argument == "--set" && hasNext )
			settings.push_back( argv[ ++i ] );
		else if( argument == "--frames" && hasNext )
			frames = std::atoi( argv[ ++i ] );
		else if( argument == "--play" && hasNext )
			plays.push_back( std::atoi( argv[ ++i ] ) );
		else if( argument == "--over" )
			effect = true;
		else if( argument == "--pipe" )
			mode = "pipe";
		else if( argument == "--film" && hasNext )
		{
			mode       = "pipe";
			filmFrames = std::max( 1, std::atoi( argv[ ++i ] ) );
		}
		else if( argument == "--script" && hasNext )
			scriptPath = argv[ ++i ];
		else if( argument == "--list" )
			mode = "list";
		else if( argument == "--size" && hasNext )
		{
			const std::string value = argv[ ++i ];
			const size_t cross      = value.find( 'x' );
			if( cross != std::string::npos )
			{
				width  = std::atoi( value.substr( 0, cross ).c_str() );
				height = std::atoi( value.substr( cross + 1 ).c_str() );
			}
		}
		else if( argument.rfind( "--", 0 ) == 0 )
			mode = argument.substr( 2 );
		else
		{
			std::fprintf( stderr, "unknown argument '%s' (try --help)\n", argument.c_str() );
			return 2;
		}
	}

	if( mode == "shaders" )
	{
		//Every program exactly as the plugin compiles it, for tools/glslc.sh.
		const std::string dir = outPath;
		for( const shaders::Program& program : shaders::Programs() )
		{
			std::ofstream( dir + "/" + program.name + ".vert" ) << program.vertex;
			std::ofstream( dir + "/" + program.name + ".frag" ) << program.fragment;
		}
		std::printf( "wrote %zu programs to %s\n", shaders::Programs().size(), dir.c_str() );
		//And each through this machine's driver, with its log on a failure.
		CGLContextObj context = createContext();
		if( context == nullptr )
			return 1;
		int failed = 0;
		for( const shaders::Program& program : shaders::Programs() )
		{
			auto compile = [ & ]( GLenum type, const std::string& text, const char* what ) {
				const GLuint shader = glCreateShader( type );
				const char* source  = text.c_str();
				glShaderSource( shader, 1, &source, nullptr );
				glCompileShader( shader );
				GLint ok = 0;
				glGetShaderiv( shader, GL_COMPILE_STATUS, &ok );
				if( !ok )
				{
					char log[ 4096 ] = {};
					glGetShaderInfoLog( shader, sizeof( log ), nullptr, log );
					std::printf( "  FAIL  %s %s:\n%s\n", program.name, what, log );
					++failed;
				}
				return shader;
			};
			const GLuint vs = compile( GL_VERTEX_SHADER, program.vertex, "vertex" );
			const GLuint fs = compile( GL_FRAGMENT_SHADER, program.fragment, "fragment" );
			const GLuint linked = glCreateProgram();
			glAttachShader( linked, vs );
			glAttachShader( linked, fs );
			glLinkProgram( linked );
			GLint ok = 0;
			glGetProgramiv( linked, GL_LINK_STATUS, &ok );
			if( !ok )
			{
				char log[ 4096 ] = {};
				glGetProgramInfoLog( linked, sizeof( log ), nullptr, log );
				std::printf( "  FAIL  %s link:\n%s\n", program.name, log );
				++failed;
			}
			else
				std::printf( "  ok    %s\n", program.name );
			glDeleteProgram( linked );
			glDeleteShader( vs );
			glDeleteShader( fs );
		}
		CGLSetCurrentContext( nullptr );
		CGLDestroyContext( context );
		return failed == 0 ? 0 : 1;
	}

	if( mode == "list" )
	{
		JackpotPlugin plugin( effect );
		std::printf( "%-3s %-18s %-9s %s\n", "id", "name", "kind", "default" );
		for( const NamedParameter& parameter : listParameters( plugin ) )
			std::printf( "%-3u %-18s %-9s %.4f\n", parameter.index, parameter.name.c_str(), parameter.kind.c_str(),
			             parameter.value );
		return 0;
	}

	//A reader that hangs up must end --pipe/--film with exit 1 and a message,
	//not SIGPIPE's silent 141: ignored here, the write fails with EPIPE.
	std::signal( SIGPIPE, SIG_IGN );

	if( mode == "offline" )
	{
		//No context at all: this is what a runner with no accelerated GL can
		//run. The skip is loud, so a green run is not read as one that
		//checked the shaders against a driver.
		int failed = 0;
		for( const CheckEntry& check : checks() )
			if( isOffline( check.flag ) )
			{
				g_failures = 0;
				failed |= check.run( Perturb {} );
			}
		failed |= runNegative( true );
		std::printf( "\n  offline: the checks that need no GL context. The shader and pixel checks were NOT run --\n"
		             "  tools/verify.sh runs them against a real driver, at 320x180 and above.\n"
		             "\n  %s\n", failed == 0 ? "PASS" : "FAIL" );
		return failed == 0 ? 0 : 1;
	}

	CGLContextObj context = createContext();
	if( context == nullptr )
	{
		std::fprintf( stderr, "could not create an OpenGL 4.1 core context\n" );
		return 1;
	}

	int result = 0;
	bool ran   = false;
	for( const CheckEntry& check : checks() )
		if( mode == check.flag )
		{
			result = check.run( Perturb {} );
			ran    = true;
		}

	if( ran )
		;
	else if( mode == "pipe" )
		result = runPipe( effect, width, height, scriptPath, filmFrames, settings, plays );
	else if( mode == "negative" )
		result = runNegative();
	else if( mode == "bench" )
		result = runBench();
	else if( !mode.empty() )
	{
		std::fprintf( stderr, "unknown mode --%s (try --help)\n", mode.c_str() );
		result = 2;
	}
	else
	{
		Rig rig( effect );
		if( !rig.Init( width, height ) )
			result = 1;
		else
		{
			for( const std::string& setting : settings )
			{
				std::string error;
				if( !applySetting( rig.plugin, setting, error ) )
				{
					std::fprintf( stderr, "--set %s: %s\n", setting.c_str(), error.c_str() );
					return 2;
				}
			}
			if( plays.empty() )
				plays.push_back( 0 );
			int total = frames;
			if( total < 0 )
				total = plays.back() + 1;
			for( int f = 0; f < std::max( total, 1 ) && result == 0; ++f )
			{
				if( std::find( plays.begin(), plays.end(), f ) != plays.end() )
					rig.Press( PT_PLAY );
				if( !rig.Render( 1 ) )
					result = 1;
			}
			//One play to rest: the frame it started on, then its length.
			if( frames < 0 && result == 0 )
				result = rig.Render( static_cast< int >( std::ceil( rig.plugin.CurrentPlay().Clock().duration * 60.0 ) ) + 2 ) ? 0 : 1;
			if( result == 0 )
			{
				if( writePng( outPath, width, height, rig.Output() ) )
					std::printf( "wrote %s -- %dx%d, %d frames; %s\n", outPath.c_str(), width, height, rig.frame,
					             describePlay( rig.plugin ).c_str() );
				else
					result = 1;
			}
		}
	}

	CGLSetCurrentContext( nullptr );
	CGLDestroyContext( context );
	return result;
}
