#include "Jackpot.h"

#include "Diag.h"
#include "GLState.h"
#include "Shaders.h"


#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <string>

using namespace ffglex;

namespace jackpot
{
namespace
{
std::string glStringOrUnknown( GLenum name )
{
	const GLubyte* value = glGetString( name );
	return value ? reinterpret_cast< const char* >( value ) : "unknown";
}

double wallSeconds()
{
	using namespace std::chrono;
	static const steady_clock::time_point start = steady_clock::now();
	return duration_cast< duration< double > >( steady_clock::now() - start ).count();
}

constexpr int kClockVotes       = 4;
constexpr double kMaxFrameDelta = 0.25;///< host seconds; a bigger step is a jump
constexpr double kBannerFor     = 3.5; ///< seconds a win's banner stays up
constexpr double kCountUp       = 1.6; ///< seconds the win meter takes to count
constexpr float kSpace          = 0.5f;///< a space's advance in H, = SPACE in Shaders.cpp (0.35 ran "31 6" together)
constexpr int kBallSlots        = 192; ///< columns of the lottery's ball texture: this draw's, and the last's draining

const char* const kGameNames[]     = { "Slots", "Roulette", "Money Wheel", "Craps", "Lottery", "Shower Only" };
const char* const kLandNames[]     = { "Time", "Beat", "Bar" };
const char* const kResultNames[]   = { "Random", "Win", "Jackpot", "Near Miss", "Lose", "Fixed" };
const char* const kReelNames[]     = { "3", "5" };
const char* const kSymbolNames[]   = { "Fruit", "Sevens and Bars", "Clip" };
const char* const kStripNames[]    = { "Virtual", "Physical" };
const char* const kWheelNames[]    = { "European", "American" };
const char* const kShowerNames[]   = { "Off", "On Win", "On Jackpot", "Always" };
const char* const kPatternNames[]  = { "Rain", "Fountain", "Burst", "Pour" };
const char* const kPiecesNames[]   = { "Chips", "Coins", "Mixed" };
const char* const kBackdropNames[] = { "None", "Felt", "Dark" };

/// Colours arrive as display values; the shader lights in linear.
void linearColour( const float* p, float out[ 3 ] )
{
	for( int i = 0; i < 3; ++i )
		out[ i ] = std::pow( std::clamp( p[ i ], 0.0f, 1.0f ), 2.2f );
}

/// The slot spans, mirrored by the SPAN_ constants in Shaders.cpp.
enum SlotSpan
{
	SPAN_MARQUEE = 0,
	SPAN_CREDITS,
	SPAN_WIN,
	SPAN_BANNER,
	SPAN_SPIN,
};

double Smooth( double x )
{
	x = std::clamp( x, 0.0, 1.0 );
	return x * x * ( 3.0 - 2.0 * x );
}
} // namespace

static_assert( PT_SOURCE_COUNT - PT_ABOUT_TEXT == stoatworks::about::kParamCount,
               "the About run no longer matches StoatworksAbout.h -- add or remove a PT_ABOUT_BUTTON_n to match" );

//---------------------------------------------------------------------------
const Playback& Play::Clock() const
{
	switch( game )
	{
	case Game::Roulette: return roulette.playback;
	case Game::MoneyWheel: return wheel.playback;
	case Game::Craps: return craps.playback;
	case Game::Lottery: return lottery.playback;
	default: return slots.playback;
	}
}

const Outcome& Play::Result() const
{
	switch( game )
	{
	case Game::Roulette: return roulette.outcome;
	case Game::MoneyWheel: return wheel.outcome;
	case Game::Craps: return craps.outcome;
	case Game::Lottery: return lottery.outcome;
	default: return slots.outcome;
	}
}

bool Play::Idle() const
{
	switch( game )
	{
	case Game::Roulette: return roulette.idle;
	case Game::MoneyWheel: return wheel.idle;
	case Game::Craps: return craps.idle;
	case Game::Lottery: return lottery.idle;
	case Game::ShowerOnly: return true;
	default: return slots.idle;
	}
}

//---------------------------------------------------------------------------
JackpotPlugin::JackpotPlugin( bool effect ) : isEffect( effect )
{
	SetMinInputs( isEffect ? 1 : 0 );
	SetMaxInputs( isEffect ? 1 : 0 );
	SetTimeSupported( true );

	//-------------------------------------------------------------------
	// Defaults: a three-reel fruit machine in a dark room, showering on a win.
	//-------------------------------------------------------------------
	params[ PT_GAME ]         = static_cast< float >( Game::Slots );
	params[ PT_SPIN_TIME ]    = ParamFromSpinTime( 4.0 );
	params[ PT_LAND_ON ]      = static_cast< float >( LandOn::Time );
	params[ PT_RESULT ]       = static_cast< float >( Result::Random );
	params[ PT_FIXED_NUMBER ] = 7.0f;
	params[ PT_SEED ]         = 0.0f;
	params[ PT_AUTO_PLAY ]    = 0.0f;
	params[ PT_INTERVAL ]     = ParamFromInterval( 10.0 );
	params[ PT_REELS ]        = 0.0f;
	params[ PT_SYMBOLS ]      = static_cast< float >( SymbolSet::Fruit );
	params[ PT_STRIP ]        = static_cast< float >( Strip::Virtual );
	params[ PT_BLUR ]         = 0.5f;
	params[ PT_REEL_BOUNCE ]  = 0.45f;
	params[ PT_WHEEL ]        = static_cast< float >( RouletteWheel::European );
	params[ PT_ROTOR_SPEED ]  = 0.5f;
	params[ PT_DEFLECTORS ]   = 1.0f;
	params[ PT_CLAPPER ]      = 0.23f;
	params[ PT_PYRAMIDS ]     = 1.0f;
	params[ PT_PUCK ]         = 1.0f;
	params[ PT_BALLS ]        = 49.0f;
	params[ PT_DRAW ]         = 6.0f;
	params[ PT_AIR ]          = 0.43f;
	params[ PT_SHOWER ]       = static_cast< float >( ShowerWhen::OnWin );
	params[ PT_PATTERN ]      = static_cast< float >( Pattern::Rain );
	params[ PT_PIECES ]       = static_cast< float >( Pieces::Mixed );
	params[ PT_AMOUNT ]       = 0.45f;
	params[ PT_PIECE_SIZE ]   = ParamFromPieceSize( 0.06 );
	params[ PT_PILE ]         = 1.0f;
	params[ PT_BACKDROP ]     = static_cast< float >( Backdrop::Dark );
	params[ PT_FELT_R ]       = 0.04f;
	params[ PT_FELT_G ]       = 0.36f;
	params[ PT_FELT_B ]       = 0.18f;
	params[ PT_ACCENT_R ]     = 0.66f;
	params[ PT_ACCENT_G ]     = 0.05f;
	params[ PT_ACCENT_B ]     = 0.07f;
	params[ PT_LIGHTS ]       = 0.8f;
	params[ PT_DISPLAY ]      = 1.0f;
	params[ PT_FONT ]         = 0.0f;
	params[ PT_LIGHT_ANGLE ]  = 0.375f;//-45 degrees
	params[ PT_TILT ]         = 0.5f;  //55 degrees
	params[ PT_ZOOM ]         = 0.43f; //about 1
	params[ PT_MIX ]          = 1.0f;

	auto standard = [ this ]( unsigned int id, const char* name ) { SetParamInfo( id, name, FF_TYPE_STANDARD, params[ id ] ); };
	auto option   = [ this ]( unsigned int id, const char* name, const char* const* names, int count ) {
		SetOptionParamInfo( id, name, static_cast< unsigned int >( count ), params[ id ] );
		for( int i = 0; i < count; ++i )
			SetParamElementInfo( id, static_cast< unsigned int >( i ), names[ i ], static_cast< float >( i ) );
	};
	auto integer = [ this ]( unsigned int id, const char* name, float lo, float hi ) {
		//Only FF_TYPE_STANDARD has its default clamped into 0..1, so an integer
		//is declared with its real default and range.
		SetParamInfo( id, name, FF_TYPE_INTEGER, params[ id ] );
		SetParamRange( id, lo, hi );
	};
	auto boolean = [ this ]( unsigned int id, const char* name ) { SetParamInfo( id, name, FF_TYPE_BOOLEAN, params[ id ] > 0.5f ); };
	auto colour  = [ this ]( unsigned int id, const char* red, const char* green, const char* blue ) {
		SetParamInfo( id, red, FF_TYPE_RED, params[ id ] );
		SetParamInfo( id + 1, green, FF_TYPE_GREEN, params[ id + 1 ] );
		SetParamInfo( id + 2, blue, FF_TYPE_BLUE, params[ id + 2 ] );
	};

	option( PT_GAME, "Game", kGameNames, static_cast< int >( Game::Count ) );
	SetParamInfo( PT_PLAY, "Play", FF_TYPE_EVENT, false );
	standard( PT_SPIN_TIME, "Spin Time" );
	option( PT_LAND_ON, "Land On", kLandNames, static_cast< int >( LandOn::Count ) );
	option( PT_RESULT, "Result", kResultNames, static_cast< int >( Result::Count ) );
	integer( PT_FIXED_NUMBER, "Fixed Number", 0.0f, static_cast< float >( kMaxFixed ) );
	integer( PT_SEED, "Seed", 0.0f, 9999.0f );
	boolean( PT_AUTO_PLAY, "Auto Play" );
	standard( PT_INTERVAL, "Interval" );

	option( PT_REELS, "Reels", kReelNames, 2 );
	option( PT_SYMBOLS, "Symbols", kSymbolNames, isEffect ? 3 : 2 );
	option( PT_STRIP, "Strip", kStripNames, static_cast< int >( Strip::Count ) );
	standard( PT_BLUR, "Blur" );
	standard( PT_REEL_BOUNCE, "Reel Bounce" );

	option( PT_WHEEL, "Wheel", kWheelNames, static_cast< int >( RouletteWheel::Count ) );
	standard( PT_ROTOR_SPEED, "Rotor Speed" );
	boolean( PT_DEFLECTORS, "Deflectors" );

	standard( PT_CLAPPER, "Clapper" );

	boolean( PT_PYRAMIDS, "Pyramids" );
	boolean( PT_PUCK, "Puck" );

	integer( PT_BALLS, "Balls", static_cast< float >( kMinBalls ), static_cast< float >( kMaxBalls ) );
	integer( PT_DRAW, "Draw", 1.0f, static_cast< float >( kMaxDraw ) );
	standard( PT_AIR, "Air" );

	option( PT_SHOWER, "Shower", kShowerNames, static_cast< int >( ShowerWhen::Count ) );
	SetParamInfo( PT_SHOWER_NOW, "Shower Now", FF_TYPE_EVENT, false );
	option( PT_PATTERN, "Pattern", kPatternNames, static_cast< int >( Pattern::Count ) );
	option( PT_PIECES, "Pieces", kPiecesNames, static_cast< int >( Pieces::Count ) );
	standard( PT_AMOUNT, "Amount" );
	standard( PT_PIECE_SIZE, "Piece Size" );
	boolean( PT_PILE, "Pile" );

	option( PT_BACKDROP, "Backdrop", kBackdropNames, static_cast< int >( Backdrop::Count ) );
	colour( PT_FELT_R, "Felt Colour", "Felt_Green", "Felt_Blue" );
	colour( PT_ACCENT_R, "Accent Colour", "Accent_Green", "Accent_Blue" );
	standard( PT_LIGHTS, "Lights" );
	boolean( PT_DISPLAY, "Display" );

	//The font list is scanned here, once per process: SetParamElementInfo is
	//how a host learns a dropdown's contents (downpour's note).
	const std::vector< FontFile >& fonts = InstalledFonts();
	SetOptionParamInfo( PT_FONT, "Font", static_cast< unsigned int >( fonts.size() + 1 ), 0.0f );
	SetParamElementInfo( PT_FONT, 0, "Built-in", 0.0f );
	for( size_t i = 0; i < fonts.size(); ++i )
		SetParamElementInfo( PT_FONT, static_cast< unsigned int >( i + 1 ), fonts[ i ].family.c_str(), static_cast< float >( i + 1 ) );
	SetFileParamInfo( PT_FONT_FILE, "Font File", { "ttf", "otf", "ttc", "otc" }, "" );
	SetParamInfo( PT_FONT_NAME, "Font Name", FF_TYPE_TEXT, "" );
	standard( PT_LIGHT_ANGLE, "Light Angle" );
	standard( PT_TILT, "Tilt" );
	standard( PT_ZOOM, "Zoom" );

	for( unsigned int id = PT_GAME; id <= PT_INTERVAL; ++id )
		SetParamGroup( id, "Game" );
	for( unsigned int id = PT_REELS; id <= PT_REEL_BOUNCE; ++id )
		SetParamGroup( id, "Slots" );
	for( unsigned int id = PT_WHEEL; id <= PT_DEFLECTORS; ++id )
		SetParamGroup( id, "Roulette" );
	SetParamGroup( PT_CLAPPER, "Money Wheel" );
	for( unsigned int id = PT_PYRAMIDS; id <= PT_PUCK; ++id )
		SetParamGroup( id, "Craps" );
	for( unsigned int id = PT_BALLS; id <= PT_AIR; ++id )
		SetParamGroup( id, "Lottery" );
	for( unsigned int id = PT_SHOWER; id <= PT_PILE; ++id )
		SetParamGroup( id, "Shower" );
	for( unsigned int id = PT_BACKDROP; id <= PT_ZOOM; ++id )
		SetParamGroup( id, "Look" );

	SetParamInfo( PT_ABOUT_TEXT, "About", FF_TYPE_TEXT, stoatworks::about::defaultText() );
	{
		FFUInt32 aboutId = PT_ABOUT_TEXT + 1;
		for( const auto& b : stoatworks::about::buttons() )
			SetParamInfo( aboutId++, b.label, FF_TYPE_EVENT, false );
	}
	for( unsigned int id = PT_ABOUT_TEXT; id < PT_SOURCE_COUNT; ++id )
		SetParamGroup( id, "About" );

	if( isEffect )
	{
		standard( PT_MIX, "Mix" );
		SetParamGroup( PT_MIX, "Over" );
	}

	reelStops = { 0.0, 6.0, 12.0, 16.0, 4.0 };
	symbols   = PaintSymbols();
}

JackpotPlugin::~JackpotPlugin()
{
	if( pendingCancel )
		pendingCancel->store( true );
	if( pending.valid() )
		pending.wait();
	ReapAbandoned( true );
}

//---------------------------------------------------------------------------
FFResult JackpotPlugin::InitGL( const FFGLViewportStruct* vp )
{
	diag::init();
	diag::info( std::string( "GL vendor=" ) + glStringOrUnknown( GL_VENDOR ) + " renderer=" + glStringOrUnknown( GL_RENDERER )
	            + " version=" + glStringOrUnknown( GL_VERSION ) );

	//Every program from the one list (Shaders.cpp), so what the harness checks
	//through glslc is what is compiled here.
	bool ok = true;
	const std::vector< shaders::Program > list = shaders::Programs();
	for( size_t i = 0; i < list.size(); ++i )
	{
		ffglex::FFGLShader& shader = i < static_cast< size_t >( Game::Count ) ? programs[ i ] : i == list.size() - 2 ? showerProgram : compositeProgram;
		if( !shader.Compile( list[ i ].vertex.c_str(), list[ i ].fragment.c_str() ) )
		{
			diag::error( std::string( "the " ) + list[ i ].name + " shader failed to compile" );
			ok = false;
		}
	}
	if( !ok || !quad.Initialise() )
	{
		FFGLLog::LogToHost( "Jackpot: a shader failed to compile" );
		DeInitGL();
		return FF_FAIL;
	}

	GLuint textures[ 4 ];
	glGenTextures( 4, textures );
	atlasTexture  = textures[ 0 ];
	symbolTexture = textures[ 1 ];
	blankTexture  = textures[ 2 ];
	ballTexture   = textures[ 3 ];
	glBindTexture( GL_TEXTURE_2D, ballTexture );
	glTexImage2D( GL_TEXTURE_2D, 0, GL_RGBA32F, kBallSlots, 3, 0, GL_RGBA, GL_FLOAT, nullptr );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST );
	const unsigned char grey[ 4 ] = { 200, 200, 200, 255 };
	glBindTexture( GL_TEXTURE_2D, blankTexture );
	glTexImage2D( GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, grey );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR );
	glBindTexture( GL_TEXTURE_2D, 0 );

	//The shower's instances: a unit quad, and a buffer of per-piece data.
	glGenVertexArrays( 1, &showerVao );
	glGenBuffers( 1, &showerVbo );
	glGenBuffers( 1, &quadVbo );
	glBindVertexArray( showerVao );
	const float corners[ 8 ] = { -1, -1, 1, -1, -1, 1, 1, 1 };
	glBindBuffer( GL_ARRAY_BUFFER, quadVbo );
	glBufferData( GL_ARRAY_BUFFER, sizeof( corners ), corners, GL_STATIC_DRAW );
	glEnableVertexAttribArray( 0 );
	glVertexAttribPointer( 0, 2, GL_FLOAT, GL_FALSE, 0, nullptr );
	glBindBuffer( GL_ARRAY_BUFFER, showerVbo );
	glBufferData( GL_ARRAY_BUFFER, static_cast< GLsizeiptr >( Shower::kMaxPieces * 12 * sizeof( float ) ), nullptr, GL_DYNAMIC_DRAW );
	for( int a = 0; a < 3; ++a )
	{
		glEnableVertexAttribArray( static_cast< GLuint >( 1 + a ) );
		glVertexAttribPointer( static_cast< GLuint >( 1 + a ), 4, GL_FLOAT, GL_FALSE, 12 * sizeof( float ), reinterpret_cast< void* >( static_cast< uintptr_t >( a * 4 * sizeof( float ) ) ) );
		glVertexAttribDivisor( static_cast< GLuint >( 1 + a ), 1 );
	}
	glBindVertexArray( 0 );
	glBindBuffer( GL_ARRAY_BUFFER, 0 );

	fontDirty = atlasUploadDirty = symbolsUploadDirty = true;
	diag::info( isEffect ? "initialised (Over)" : "initialised (source)" );
	return CFFGLPlugin::InitGL( vp );
}

FFResult JackpotPlugin::DeInitGL()
{
	for( auto& p : programs )
		p.FreeGLResources();
	showerProgram.FreeGLResources();
	compositeProgram.FreeGLResources();
	quad.Release();
	GLuint textures[ 5 ] = { atlasTexture, symbolTexture, blankTexture, showerColour, ballTexture };
	for( GLuint t : textures )
		if( t != 0 )
			glDeleteTextures( 1, &t );
	if( showerDepth )
		glDeleteRenderbuffers( 1, &showerDepth );
	if( showerFbo )
		glDeleteFramebuffers( 1, &showerFbo );
	if( showerVbo )
		glDeleteBuffers( 1, &showerVbo );
	if( quadVbo )
		glDeleteBuffers( 1, &quadVbo );
	if( showerVao )
		glDeleteVertexArrays( 1, &showerVao );
	atlasTexture = symbolTexture = blankTexture = showerColour = showerDepth = showerFbo = showerVbo = quadVbo = showerVao = ballTexture = 0;
	showerWidth = showerHeight = 0;
	return FF_SUCCESS;
}

//---------------------------------------------------------------------------
void JackpotPlugin::UpdateClock()
{
	const double wallNow = wallSeconds();
	if( wallStart < 0.0 )
		wallStart = wallNow;
	const double raw = hostTime;

	//Resolume has been seen sending seconds and milliseconds through SetTime:
	//vote on the unit against the wall clock (rosette's code, via boreal).
	if( clockScale == 0.0 && raw >= 0.0 && lastRawTime >= 0.0 && lastWallTime >= 0.0 )
	{
		const double hostDelta = raw - lastRawTime;
		const double wallDelta = wallNow - lastWallTime;
		if( hostDelta > 0.0 && wallDelta >= 0.0005 )
		{
			const double ratio = hostDelta / wallDelta;
			if( ratio > 0.1 && ratio < 10.0 )
				++secondsVotes;
			else if( ratio > 100.0 && ratio < 10000.0 )
				++millisVotes;
			if( secondsVotes >= kClockVotes || millisVotes >= kClockVotes )
			{
				clockScale = millisVotes > secondsVotes ? 0.001 : 1.0;
				lastNow    = -1.0;
			}
		}
	}
	if( raw >= 0.0 )
		lastRawTime = raw;
	lastWallTime = wallNow;
	now          = ( raw >= 0.0 && clockScale != 0.0 ) ? raw * clockScale : wallNow - wallStart;
}

Game JackpotPlugin::CurrentGame() const
{
	return static_cast< Game >( OptionIndex( params[ PT_GAME ], static_cast< int >( Game::Count ) ) );
}

double JackpotPlugin::PlayDuration() const
{
	const double spin = SpinTimeFromParam( params[ PT_SPIN_TIME ] );
	const LandOn land = static_cast< LandOn >( OptionIndex( params[ PT_LAND_ON ], static_cast< int >( LandOn::Count ) ) );
	if( land == LandOn::Time )
		return spin;
	//The first beat (or bar) boundary at least Spin Time away.
	return transport.SecondsToNext( land == LandOn::Bar, spin );
}

slots::Request JackpotPlugin::SlotsRequest() const
{
	slots::Request r;
	r.set      = static_cast< SymbolSet >( OptionIndex( params[ PT_SYMBOLS ], isEffect ? 3 : 2 ) );
	r.reels    = OptionIndex( params[ PT_REELS ], 2 ) == 1 ? 5 : 3;
	r.strip    = static_cast< Strip >( OptionIndex( params[ PT_STRIP ], static_cast< int >( Strip::Count ) ) );
	r.result   = static_cast< Result >( OptionIndex( params[ PT_RESULT ], static_cast< int >( Result::Count ) ) );
	r.fixed    = static_cast< int >( std::lround( params[ PT_FIXED_NUMBER ] ) );
	r.seed     = static_cast< uint32_t >( std::clamp( std::lround( params[ PT_SEED ] ), 0L, 9999L ) );
	r.play     = playIndex;
	r.duration = PlayDuration();
	r.damping  = ReelDampingFromParam( params[ PT_REEL_BOUNCE ] );
	r.from     = reelStops;
	//A pull while the reels still run starts from where they are, not from
	//where the last play left them (which jumped the reels back).
	if( play.game == Game::Slots && !play.slots.idle && clock - playStart < play.slots.playback.duration )
		for( int i = 0; i < slots::kMaxReels; ++i )
			r.from[ static_cast< size_t >( i ) ] = play.slots.Position( i, clock - playStart );
	r.uniformVirtual = flags.uniformVirtual;
	return r;
}

roulette::Request JackpotPlugin::RouletteRequest() const
{
	roulette::Request r;
	r.wheel      = static_cast< RouletteWheel >( OptionIndex( params[ PT_WHEEL ], static_cast< int >( RouletteWheel::Count ) ) );
	r.result     = static_cast< Result >( OptionIndex( params[ PT_RESULT ], static_cast< int >( Result::Count ) ) );
	r.bet        = static_cast< int >( std::lround( params[ PT_FIXED_NUMBER ] ) );
	r.seed       = static_cast< uint32_t >( std::clamp( std::lround( params[ PT_SEED ] ), 0L, 9999L ) );
	r.play       = playIndex;
	r.duration   = PlayDuration();
	r.rotorSpeed = RotorSpeedFromParam( params[ PT_ROTOR_SPEED ] );
	r.deflectors = params[ PT_DEFLECTORS ] > 0.5f;
	if( play.game == Game::Roulette )
	{
		const double t  = clock - playStart;
		r.paintNow      = play.roulette.PaintAngle( t );
		r.paintSpeedNow = play.roulette.PaintSpeed( t );
	}
	else
	{
		r.paintNow      = rotorAngle;
		r.paintSpeedNow = rotorSpeed;
	}
	r.noShift    = flags.noShift;
	r.noWarp     = flags.noWarp;
	r.biasedDraw = flags.biasedDraw;
	return r;
}

wheel::Request JackpotPlugin::WheelRequest() const
{
	wheel::Request r;
	r.result    = static_cast< Result >( OptionIndex( params[ PT_RESULT ], static_cast< int >( Result::Count ) ) );
	r.bet       = static_cast< int >( std::lround( params[ PT_FIXED_NUMBER ] ) );
	r.seed      = static_cast< uint32_t >( std::clamp( std::lround( params[ PT_SEED ] ), 0L, 9999L ) );
	r.play      = playIndex;
	r.duration  = PlayDuration();
	r.stiffness = ClapperFromParam( params[ PT_CLAPPER ] );
	r.paintNow  = play.game == Game::MoneyWheel ? play.wheel.PaintAngle( clock - playStart ) : wheelAngle;
	r.noShift   = flags.noShift;
	r.noWarp    = flags.noWarp;
	r.biasedDraw = flags.biasedDraw;
	return r;
}

craps::Request JackpotPlugin::CrapsRequest() const
{
	craps::Request r;
	r.result   = static_cast< Result >( OptionIndex( params[ PT_RESULT ], static_cast< int >( Result::Count ) ) );
	r.fixed    = static_cast< int >( std::lround( params[ PT_FIXED_NUMBER ] ) );
	r.point    = crapsPoint;
	r.seed     = static_cast< uint32_t >( std::clamp( std::lround( params[ PT_SEED ] ), 0L, 9999L ) );
	r.play     = playIndex;
	r.duration = PlayDuration();
	r.pyramids = params[ PT_PYRAMIDS ] > 0.5f;
	if( play.game == Game::Craps )
	{
		const double t = clock - playStart;
		for( int i = 0; i < 2; ++i )
		{
			V3 x;
			M3 rot;
			if( play.craps.DieAt( i, t, x, rot ) )
			{
				r.previous[ static_cast< size_t >( i ) ] = { x, dice::FromMatrix( rot ) };
				r.havePrevious = true;
			}
		}
	}
	r.noWarp      = flags.noWarp;
	r.restitution = flags.restitution;
	return r;
}

lottery::Request JackpotPlugin::LotteryRequest() const
{
	lottery::Request r;
	r.balls    = std::clamp( static_cast< int >( std::lround( params[ PT_BALLS ] ) ), kMinBalls, kMaxBalls );
	r.draw     = std::clamp( static_cast< int >( std::lround( params[ PT_DRAW ] ) ), 1, kMaxDraw );
	r.result   = static_cast< Result >( OptionIndex( params[ PT_RESULT ], static_cast< int >( Result::Count ) ) );
	r.bet      = static_cast< int >( std::lround( params[ PT_FIXED_NUMBER ] ) );
	r.seed     = static_cast< uint32_t >( std::clamp( std::lround( params[ PT_SEED ] ), 0L, 9999L ) );
	r.play     = playIndex;
	r.duration = PlayDuration();
	r.air      = AirFromParam( params[ PT_AIR ] );
	if( play.game == Game::Lottery )
	{
		//Whatever is in the drum now drains first; what is on the rack rolls off.
		const std::vector< lottery::BallState > balls = play.lottery.Balls( clock - playStart );
		for( const lottery::BallState& b : balls )
		{
			const bool racked = b.x.z > lottery::kDrum + 0.05;
			if( racked )
				r.previousRack.push_back( b.number );
			else
				r.previousDrum.push_back( b );
		}
	}
	r.noWarp      = flags.noWarp;
	r.biasedDraw  = flags.biasedDraw;
	r.restitution = flags.restitution;
	return r;
}

Play JackpotPlugin::MakePlayFor( Game game, bool rest ) const
{
	Play p;
	p.game = game;
	switch( game )
	{
	case Game::Roulette: p.roulette = rest ? roulette::MakeRest( RouletteRequest() ) : roulette::MakePlan( RouletteRequest() ); break;
	case Game::MoneyWheel: p.wheel = rest ? wheel::MakeRest( WheelRequest() ) : wheel::MakePlan( WheelRequest() ); break;
	case Game::Craps: p.craps = rest ? craps::MakeRest( CrapsRequest() ) : craps::MakePlan( CrapsRequest() ); break;
	case Game::Lottery: p.lottery = rest ? lottery::MakeRest( LotteryRequest() ) : lottery::MakePlan( LotteryRequest() ); break;
	case Game::ShowerOnly: break;
	default: p.slots = rest ? slots::MakeRest( SlotsRequest() ) : slots::MakePlan( SlotsRequest() ); break;
	}
	return p;
}

void JackpotPlugin::ReapAbandoned( bool wait )
{
	for( size_t i = 0; i < abandoned.size(); )
	{
		if( wait || abandoned[ i ].wait_for( std::chrono::seconds( 0 ) ) == std::future_status::ready )
		{
			if( wait )
				abandoned[ i ].wait();
			abandoned.erase( abandoned.begin() + static_cast< long >( i ) );
			abandonedCancel.erase( abandonedCancel.begin() + static_cast< long >( i ) );
		}
		else
			++i;
	}
}

void JackpotPlugin::StartPlay()
{
	const Game game = CurrentGame();
	if( game == Game::ShowerOnly )
	{
		//Play on the shower alone is the shower.
		++showerPresses;
		return;
	}
	++playIndex;
	lastPlayRequest = clock;
	if( pending.valid() )
	{
		pendingCancel->store( true );
		abandoned.push_back( std::move( pending ) );
		abandonedCancel.push_back( pendingCancel );
	}
	//The slot machine's plan is a schedule, not a simulation: microseconds.
	//Everything else plans on a worker unless the harness asks otherwise.
	if( synchronous || game == Game::Slots )
	{
		if( game == Game::Slots )
			credits = std::max( 0, credits - 1 );
		play        = MakePlayFor( game, false );
		playStart   = clock;
		restHandled = false;
		laidOut     = true;
		layoutGame  = static_cast< int >( game );
		++playsStarted;
		return;
	}
	//The requests read the current play for continuity (the rotor's angle,
	//the dice on the table), so they are built here, on the render thread.
	Play seed;
	seed.game = game;
	pendingCancel = std::make_shared< std::atomic< bool > >( false );
	auto cancel   = pendingCancel;
	const double pressed = clock;
	switch( game )
	{
	case Game::Roulette:
	{
		const roulette::Request r = RouletteRequest();
		pending = std::async( std::launch::async, [ r, cancel, pressed ]() { Play p; p.game = Game::Roulette; p.roulette = roulette::MakePlan( r, cancel.get() ); (void)pressed; return p; } );
		break;
	}
	case Game::MoneyWheel:
	{
		const wheel::Request r = WheelRequest();
		pending = std::async( std::launch::async, [ r, cancel ]() { Play p; p.game = Game::MoneyWheel; p.wheel = wheel::MakePlan( r, cancel.get() ); return p; } );
		break;
	}
	case Game::Craps:
	{
		const craps::Request r = CrapsRequest();
		pending = std::async( std::launch::async, [ r, cancel ]() { Play p; p.game = Game::Craps; p.craps = craps::MakePlan( r, cancel.get() ); return p; } );
		break;
	}
	case Game::Lottery:
	default:
	{
		const lottery::Request r = LotteryRequest();
		pending = std::async( std::launch::async, [ r, cancel ]() { Play p; p.game = Game::Lottery; p.lottery = lottery::MakePlan( r, cancel.get() ); return p; } );
		break;
	}
	}
	pendingPressed = pressed;
}

void JackpotPlugin::Relayout()
{
	if( pending.valid() )
	{
		pendingCancel->store( true );
		abandoned.push_back( std::move( pending ) );
		abandonedCancel.push_back( pendingCancel );
	}
	//Carry the turning wheels across a change of game.
	if( play.game == Game::Roulette )
	{
		rotorAngle = play.roulette.PaintAngle( clock - playStart );
		rotorSpeed = play.roulette.PaintSpeed( clock - playStart );
	}
	if( play.game == Game::MoneyWheel )
		wheelAngle = play.wheel.PaintAngle( clock - playStart );
	const Game game = CurrentGame();
	play            = MakePlayFor( game, true );
	playStart       = clock;
	restHandled     = true;
	laidOut         = true;
	layoutGame      = static_cast< int >( game );
	layoutKey[ 0 ]  = params[ PT_REELS ];
	layoutKey[ 1 ]  = params[ PT_SYMBOLS ];
	layoutKey[ 2 ]  = params[ PT_WHEEL ];
	layoutKey[ 3 ]  = params[ PT_BALLS ];
	layoutKey[ 4 ]  = params[ PT_RESULT ];
	layoutKey[ 5 ]  = params[ PT_FIXED_NUMBER ];
}

void JackpotPlugin::OnRest()
{
	restHandled = true;
	++restsSeen;
	const Outcome& o = play.Result();
	switch( play.game )
	{
	case Game::Slots:
		for( int i = 0; i < slots::kMaxReels; ++i )
			reelStops[ static_cast< size_t >( i ) ] = play.slots.stops[ static_cast< size_t >( i ) ];
		winFrom = 0;
		if( o.payout > 0 )
		{
			credits += o.payout;
			winSince = clock;
		}
		break;
	case Game::Craps:
		crapsPoint = play.craps.pointAfter;
		if( o.win )
			winSince = clock;
		break;
	default:
		if( o.win )
			winSince = clock;
		break;
	}
	const ShowerWhen when = static_cast< ShowerWhen >( OptionIndex( params[ PT_SHOWER ], static_cast< int >( ShowerWhen::Count ) ) );
	if( ( when == ShowerWhen::OnWin && o.win ) || ( when == ShowerWhen::OnJackpot && o.jackpot ) )
		shower.Start( o.jackpot ? 3.5 : 2.5, o.jackpot ? 2.0 : 1.0 );
	diag::info( std::string( kGameNames[ static_cast< int >( play.game ) ] ) + " play " + std::to_string( playIndex ) + ": " + o.text
	            + ( o.jackpot ? " (JACKPOT)" : o.win ? " (win)" : o.nearMiss ? " (near miss)" : "" ) + ", warp "
	            + std::to_string( play.Clock().warp ) );
}

void JackpotPlugin::Tick( double dt )
{
	clock += dt;
	frameDt = dt > 0.0 ? dt : frameDt;
	const Game game = CurrentGame();

	//A new game, or a change that alters what is at rest before the first play,
	//lays the table out again at once.
	const bool idle    = play.Idle() && !pending.valid();
	const bool changed = params[ PT_REELS ] != layoutKey[ 0 ] || params[ PT_SYMBOLS ] != layoutKey[ 1 ] || params[ PT_WHEEL ] != layoutKey[ 2 ]
	                     || params[ PT_BALLS ] != layoutKey[ 3 ] || ( idle && game == Game::Craps && ( params[ PT_RESULT ] != layoutKey[ 4 ] || params[ PT_FIXED_NUMBER ] != layoutKey[ 5 ] ) );
	if( !laidOut || static_cast< int >( game ) != layoutGame || ( idle && changed ) )
		Relayout();
	else if( changed )
	{
		layoutKey[ 0 ] = params[ PT_REELS ];
		layoutKey[ 1 ] = params[ PT_SYMBOLS ];
		layoutKey[ 2 ] = params[ PT_WHEEL ];
		layoutKey[ 3 ] = params[ PT_BALLS ];
	}

	if( playPresses > 0 )
	{
		playPresses = 0;
		StartPlay();
	}
	const bool autoOn = params[ PT_AUTO_PLAY ] > 0.5f;
	if( autoOn && !autoWas )
		lastPlayRequest = -1e30;
	autoWas = autoOn;
	if( autoOn && !pending.valid() && clock - lastPlayRequest >= IntervalFromParam( params[ PT_INTERVAL ] ) )
		StartPlay();

	if( pending.valid() && pending.wait_for( std::chrono::seconds( 0 ) ) == std::future_status::ready )
	{
		Play next = pending.get();
		pendingCancel.reset();
		if( next.game == CurrentGame() )
		{
			play = std::move( next );
			//Back-dated to the press, so a play sized to land on the beat still
			//does, however long it took to plan.
			playStart   = pendingPressed;
			restHandled = false;
			++playsStarted;
		}
	}
	ReapAbandoned( false );

	if( !restHandled && !play.Idle() && clock - playStart >= play.Clock().duration )
		OnRest();

	//The shower: settings every frame, then the physics.
	shower.settings.pattern = static_cast< Pattern >( OptionIndex( params[ PT_PATTERN ], static_cast< int >( Pattern::Count ) ) );
	shower.settings.pieces  = static_cast< Pieces >( OptionIndex( params[ PT_PIECES ], static_cast< int >( Pieces::Count ) ) );
	shower.settings.rate    = AmountFromParam( params[ PT_AMOUNT ] );
	shower.settings.size    = PieceSizeFromParam( params[ PT_PIECE_SIZE ] );
	shower.settings.pile    = params[ PT_PILE ] > 0.5f;
	shower.settings.restitution = flags.restitution;
	shower.seed             = static_cast< uint32_t >( std::clamp( std::lround( params[ PT_SEED ] ), 0L, 9999L ) );
	if( showerPresses > 0 )
	{
		showerPresses = 0;
		shower.Start( 2.5 );
	}
	const ShowerWhen when = static_cast< ShowerWhen >( OptionIndex( params[ PT_SHOWER ], static_cast< int >( ShowerWhen::Count ) ) );
	shower.Advance( dt, when == ShowerWhen::Always );
}

//---------------------------------------------------------------------------
// Fonts.
//---------------------------------------------------------------------------
void JackpotPlugin::ResolveFont()
{
	std::string file, name;
	bool nameSet = false;
	{
		std::lock_guard< std::mutex > lock( textMutex );
		file        = fontFilePath;
		name        = fontName;
		nameSet     = fontNameSet;
		fontNameSet = false;
	}
	const std::vector< FontFile >& fonts = InstalledFonts();
	bool fromName = false, loaded = false;
	if( !file.empty() && typeface.Load( file, 0 ) )
		loaded = true;
	else
	{
		//The dropdown stores an INDEX into this machine's font list; Font Name
		//stores the family, and wins when a composition restores both.
		int chosen = -1;
		if( nameSet )
		{
			fromName = true;
			chosen   = name.empty() ? -1 : FindFontByFamily( name );
			if( !name.empty() && chosen < 0 )
				diag::warn( "font '" + name + "' is not installed here; using the built-in face" );
		}
		else
			chosen = OptionIndex( params[ PT_FONT ], static_cast< int >( fonts.size() ) + 1 ) - 1;
		if( chosen >= 0 && typeface.Load( fonts[ static_cast< size_t >( chosen ) ].path, fonts[ static_cast< size_t >( chosen ) ].collectionIndex ) )
			loaded = true;
		else
			typeface.UseBuiltin();
	}
	if( !loaded )
		typeface.UseBuiltin();
	atlas          = typeface.Build();
	resolvedFamily = atlas.family;
	const int index = resolvedFamily.empty() ? 0 : FindFontByFamily( resolvedFamily ) + 1;
	if( index > 0 || resolvedFamily.empty() )
		params[ PT_FONT ] = static_cast< float >( std::max( index, 0 ) );
	{
		std::lock_guard< std::mutex > lock( textMutex );
		if( !( fromName && !name.empty() && resolvedFamily.empty() ) )
			fontName = resolvedFamily;
	}
	diag::info( "text in " + ( resolvedFamily.empty() ? std::string( "the built-in face" ) : resolvedFamily ) );
	fontDirty        = false;
	atlasUploadDirty = true;
}

void JackpotPlugin::UploadAtlas()
{
	GLint alignment = 4;
	glGetIntegerv( GL_UNPACK_ALIGNMENT, &alignment );
	glPixelStorei( GL_UNPACK_ALIGNMENT, 1 );
	glBindTexture( GL_TEXTURE_2D, atlasTexture );
	glTexImage2D( GL_TEXTURE_2D, 0, GL_R8, GlyphAtlas::kWidth, GlyphAtlas::kHeight, 0, GL_RED, GL_UNSIGNED_BYTE, atlas.pixels.data() );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );
	glBindTexture( GL_TEXTURE_2D, 0 );
	glPixelStorei( GL_UNPACK_ALIGNMENT, alignment );
	atlasUploadDirty = false;
}

void JackpotPlugin::UploadSymbols()
{
	GLint alignment = 4;
	glGetIntegerv( GL_UNPACK_ALIGNMENT, &alignment );
	glPixelStorei( GL_UNPACK_ALIGNMENT, 1 );
	glBindTexture( GL_TEXTURE_2D, symbolTexture );
	glTexImage2D( GL_TEXTURE_2D, 0, GL_RGBA8, SymbolAtlas::kWidth, SymbolAtlas::kHeight, 0, GL_RGBA, GL_UNSIGNED_BYTE, symbols.rgba.data() );
	glGenerateMipmap( GL_TEXTURE_2D );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );
	glBindTexture( GL_TEXTURE_2D, 0 );
	glPixelStorei( GL_UNPACK_ALIGNMENT, alignment );
	symbolsUploadDirty = false;
}

//---------------------------------------------------------------------------
// Uniforms.
//---------------------------------------------------------------------------
void JackpotPlugin::SetCommonUniforms( FFGLShader& shader, int width, int height, const FFGLTextureStruct* input )
{
	float felt[ 3 ], accent[ 3 ];
	linearColour( &params[ PT_FELT_R ], felt );
	linearColour( &params[ PT_ACCENT_R ], accent );
	shader.Set( "Resolution", static_cast< float >( width ), static_cast< float >( height ) );
	shader.Set( "Zoom", static_cast< float >( ZoomFromParam( params[ PT_ZOOM ] ) ) );
	shader.Set( "Time", static_cast< float >( std::fmod( clock, 3600.0 ) ) );
	shader.Set( "BackdropKind", OptionIndex( params[ PT_BACKDROP ], static_cast< int >( Backdrop::Count ) ) );
	shader.Set( "Felt", felt[ 0 ], felt[ 1 ], felt[ 2 ] );
	shader.Set( "Accent", accent[ 0 ], accent[ 1 ], accent[ 2 ] );
	shader.Set( "Lights", std::clamp( params[ PT_LIGHTS ], 0.0f, 1.0f ) );
	shader.Set( "LightAngle", static_cast< float >( LightAngleFromParam( params[ PT_LIGHT_ANGLE ] ) * kPi / 180.0 ) );
	shader.Set( "IsEffect", isEffect ? 1 : 0 );
	shader.Set( "Mix", std::clamp( params[ PT_MIX ], 0.0f, 1.0f ) );
	shader.Set( "TestFlat", flat );
	if( input != nullptr )
	{
		const FFGLTexCoords maxCoords = GetMaxGLTexCoords( *input );
		shader.Set( "ClipScale", maxCoords.s, maxCoords.t );
	}
	else
		shader.Set( "ClipScale", 1.0f, 1.0f );
	shader.Set( "Clip", 0 );
	shader.Set( "Atlas", 1 );
}

/// The game's own spans, and after them the result board's words: shown at
/// rest after a play with Display on, faded in over a third of a second.
void JackpotPlugin::SetTextUniforms( FFGLShader& shader, std::vector< std::string > spans )
{
	int resultSpan  = -1;
	float amount    = 0.0f;
	int tone        = 0;
	const Game game = play.game;
	if( game != Game::Slots && game != Game::ShowerOnly && !play.Idle() && params[ PT_DISPLAY ] > 0.5f )
	{
		const double since = clock - playStart - play.Clock().duration;
		if( since >= 0.0 && !play.Result().text.empty() )
		{
			resultSpan = static_cast< int >( spans.size() );
			spans.push_back( play.Result().text );
			amount = static_cast< float >( Smooth( since / 0.33 ) );
			tone   = play.Result().jackpot ? 2 : play.Result().win ? 1 : 0;
		}
	}
	shader.Set( "ResultSpan", resultSpan );
	shader.Set( "ResultAmount", amount );
	shader.Set( "ResultTone", tone );

	std::vector< GLint > chars;
	std::vector< GLint > span;
	std::vector< float > ink;
	for( const std::string& s : spans )
	{
		span.push_back( static_cast< GLint >( chars.size() ) );
		span.push_back( static_cast< GLint >( s.size() ) );
		float pen = 0.0f, lo = 1e9f, hi = -1e9f;
		for( char c : s )
		{
			const int g = GlyphIndex( c );
			chars.push_back( g );
			if( g >= 0 )
			{
				lo = std::min( lo, pen + atlas.glyph[ g ].inkX0 );
				hi = std::max( hi, pen + atlas.glyph[ g ].inkX1 );
				pen += atlas.glyph[ g ].advance;
			}
			else
				pen += kSpace;
		}
		ink.push_back( hi > lo ? lo : 0.0f );
		ink.push_back( hi > lo ? hi - lo : 0.0f );
	}
	chars.resize( std::min< size_t >( chars.size(), 192 ) );
	float boxes[ kGlyphCount * 4 ], advances[ kGlyphCount ];
	for( int g = 0; g < kGlyphCount; ++g )
	{
		boxes[ g * 4 + 0 ] = atlas.glyph[ g ].originX;
		boxes[ g * 4 + 1 ] = atlas.glyph[ g ].originY;
		boxes[ g * 4 + 2 ] = atlas.glyph[ g ].inkX0;
		boxes[ g * 4 + 3 ] = atlas.glyph[ g ].inkX1;
		advances[ g ]      = atlas.glyph[ g ].advance;
	}
	glUniform4fv( shader.FindUniform( "GlyphBox" ), kGlyphCount, boxes );
	glUniform1fv( shader.FindUniform( "GlyphAdv" ), kGlyphCount, advances );
	if( !chars.empty() )
		glUniform1iv( shader.FindUniform( "Chars" ), static_cast< GLsizei >( chars.size() ), chars.data() );
	if( !spans.empty() )
	{
		glUniform2iv( shader.FindUniform( "Span" ), static_cast< GLsizei >( spans.size() ), span.data() );
		glUniform2fv( shader.FindUniform( "SpanInk" ), static_cast< GLsizei >( spans.size() ), ink.data() );
	}
	shader.Set( "AtlasPx", atlas.pxPerUnit );
	shader.Set( "AtlasSize", static_cast< float >( GlyphAtlas::kWidth ), static_cast< float >( GlyphAtlas::kHeight ) );
	shader.Set( "Weight", 0.0f );
}

void JackpotPlugin::DrawSlots( FFGLShader& shader, double t )
{
	const slots::Plan& p = play.slots;
	const int reels      = p.request.reels;
	const double shutter = BlurFromParam( params[ PT_BLUR ] ) * frameDt;
	float a[ 5 ], b[ 5 ];
	for( int i = 0; i < 5; ++i )
	{
		b[ i ] = static_cast< float >( std::fmod( p.Position( i, t ) + flags.reelOffset, static_cast< double >( slots::kStops ) ) );
		a[ i ] = static_cast< float >( std::fmod( p.Position( i, t - shutter ), static_cast< double >( slots::kStops ) ) );
		//The shutter's travel, unwrapped: a reel crossing stop 0 must not
		//integrate backwards round the whole strip.
		const double travel = p.Position( i, t ) - p.Position( i, t - shutter );
		a[ i ]              = b[ i ] - static_cast< float >( travel );
	}
	const auto& strips = slots::Reels( p.request.set );
	GLint strip[ 110 ];
	for( int i = 0; i < 5; ++i )
		for( int k = 0; k < slots::kStops; ++k )
			strip[ i * slots::kStops + k ] = strips[ static_cast< size_t >( i ) ].symbol[ static_cast< size_t >( k ) ];

	const bool atRest      = p.idle || t >= p.playback.duration;
	const double sinceWin  = clock - winSince;
	const bool showWin     = atRest && !p.idle && p.outcome.payout > 0 && sinceWin >= 0.0;
	const int shownWin     = showWin ? static_cast< int >( std::lround( p.outcome.payout * Smooth( sinceWin / kCountUp ) ) ) : 0;
	const float glow       = showWin && sinceWin < kBannerFor ? static_cast< float >( 0.5 + 0.5 * std::sin( sinceWin * 9.0 ) ) : 0.0f;
	const bool banner      = params[ PT_DISPLAY ] > 0.5f && showWin && sinceWin < kBannerFor;
	const int celebrate    = showWin && sinceWin < kBannerFor ? ( p.outcome.jackpot ? 2 : 1 ) : 0;
	const int creditsShown = std::max( 0, credits - p.outcome.payout + shownWin );

	glUniform1fv( shader.FindUniform( "ReelA" ), 5, a );
	glUniform1fv( shader.FindUniform( "ReelB" ), 5, b );
	glUniform1iv( shader.FindUniform( "StripSym" ), 110, strip );
	shader.Set( "Reels", reels );
	shader.Set( "Handle", static_cast< float >( p.Handle( t ) ) );
	shader.Set( "Credits", showWin ? creditsShown : credits );
	shader.Set( "WinMeter", shownWin );
	shader.Set( "WinGlow", glow );
	shader.Set( "Banner", banner ? ( p.outcome.jackpot ? 2 : 1 ) : 0 );
	shader.Set( "BannerAmount", banner ? static_cast< float >( Smooth( sinceWin / 0.25 ) * ( 1.0 - Smooth( ( sinceWin - kBannerFor + 0.4 ) / 0.4 ) ) ) : 0.0f );
	shader.Set( "Celebrate", celebrate );
	shader.Set( "Symbols", 2 );
	shader.Set( "SymbolsSize", static_cast< float >( SymbolAtlas::kWidth ), static_cast< float >( SymbolAtlas::kHeight ) );
	const std::string bannerText = p.outcome.jackpot ? "JACKPOT!" : p.outcome.payout >= 40 ? "BIG WIN" : "WINNER";
	SetTextUniforms( shader, { "JACKPOT", "CREDITS", "WIN", bannerText, "SPIN" } );
}

//---------------------------------------------------------------------------
// The 3D games' camera: to the south, Tilt above the horizon, looking at
// `target`, backed off so a disc of `radius` (rising `rise` above it) fills the
// frame at Zoom 1 whatever the aspect. A right-handed view: right x up points
// at the viewer (polyhedral's trap: the other way mirrors the picture).
//---------------------------------------------------------------------------
V3 JackpotPlugin::Camera::Project( V3 p ) const
{
	const V3 d         = p - pos;
	const double depth = Dot( d, forward );
	if( depth <= 1e-9 )
		return { 0.0, 0.0, -1.0 };
	const double sx = Dot( d, right ) * focal / depth, sy = Dot( d, up ) * focal / depth;
	return { 0.5 * ( sx * height + width ), 0.5 * ( sy * height + height ), depth };
}

JackpotPlugin::Camera JackpotPlugin::SetCamera( FFGLShader& shader, int width, int height, double radius, double rise, V3 target, double lowerBy )
{
	const double e        = std::max( 5.0, TiltFromParam( params[ PT_TILT ] ) - lowerBy ) * kPi / 180.0;
	const double aspect   = static_cast< double >( width ) / height;
	const double unzoomed = 1.0 / std::tan( 14.0 * kPi / 180.0 );
	const double tall     = radius * std::sin( e ) + rise * std::cos( e );
	const double half     = std::max( tall, radius / aspect ) * 1.1;
	const double distance = half * unzoomed;
	const V3 forward      = { 0.0, std::cos( e ), -std::sin( e ) };
	const V3 up           = { 0.0, std::sin( e ), std::cos( e ) };
	const V3 right        = { 1.0, 0.0, 0.0 };
	const V3 pos          = target - forward * distance;
	shader.Set( "CamPos", static_cast< float >( pos.x ), static_cast< float >( pos.y ), static_cast< float >( pos.z ) );
	shader.Set( "CamRight", static_cast< float >( right.x ), static_cast< float >( right.y ), static_cast< float >( right.z ) );
	shader.Set( "CamUp", static_cast< float >( up.x ), static_cast< float >( up.y ), static_cast< float >( up.z ) );
	shader.Set( "CamForward", static_cast< float >( forward.x ), static_cast< float >( forward.y ), static_cast< float >( forward.z ) );
	shader.Set( "CamFocal", static_cast< float >( unzoomed * ZoomFromParam( params[ PT_ZOOM ] ) ) );
	//Four rays a pixel up to 1080p; above, four only on the edges the 2x2
	//quads can see (a third of the cost at 4K, where a pixel is small).
	shader.Set( "Samples", flat ? 1 : height > 1200 ? 2 : 4 );
	Camera camera;
	camera.pos     = pos;
	camera.right   = right;
	camera.up      = up;
	camera.forward = forward;
	camera.focal   = unzoomed * ZoomFromParam( params[ PT_ZOOM ] );
	camera.width   = width;
	camera.height  = height;
	lastCamera     = camera;
	return camera;
}

void JackpotPlugin::DrawRoulette( FFGLShader& shader, double t, int width, int height )
{
	const roulette::Plan& p          = play.roulette;
	const std::vector< int >& numbers = roulette::Numbers( p.request.wheel );
	const int N                      = static_cast< int >( numbers.size() );
	GLint pocketNumber[ 38 ]         = {};
	for( int i = 0; i < N; ++i )
		pocketNumber[ i ] = numbers[ static_cast< size_t >( i ) ];
	//The ring's angle reduced in double: it keeps turning between plays.
	const double ring = std::fmod( p.PaintAngle( t ), 2.0 * kPi );
	V3 ball;
	const bool hasBall = p.Ball( t, ball );
	int highlight      = -1;
	if( !p.idle && t >= p.playback.duration && p.sim.settled )
		for( int i = 0; i < N; ++i )
			if( numbers[ static_cast< size_t >( i ) ] == p.outcome.value )
				highlight = i;

	SetCamera( shader, width, height, 0.47, 0.09, { 0.0, 0.0, 0.0 } );
	shader.Set( "RingAngle", static_cast< float >( ring ) );
	shader.Set( "Pockets", N );
	glUniform1iv( shader.FindUniform( "PocketNumber" ), 38, pocketNumber );
	shader.Set( "Deflectors", params[ PT_DEFLECTORS ] > 0.5f ? 1 : 0 );
	shader.Set( "BallAt", static_cast< float >( ball.x ), static_cast< float >( ball.y ), static_cast< float >( ball.z ), hasBall ? 1.0f : 0.0f );
	shader.Set( "Highlight", highlight );
	SetTextUniforms( shader, {} );
}
void JackpotPlugin::DrawWheel( FFGLShader& shader, double t )
{
	const wheel::Plan& p           = play.wheel;
	const std::vector< int >& layout = wheel::Layout();
	GLint values[ wheel::kSegments ];
	for( int i = 0; i < wheel::kSegments; ++i )
		values[ i ] = layout[ static_cast< size_t >( i ) ];
	const bool atRest  = !p.idle && t >= p.playback.duration && p.sim.settled;
	const int shown    = ( ( p.sim.segment - p.shift ) % wheel::kSegments + wheel::kSegments ) % wheel::kSegments;
	const double since = clock - winSince;
	shader.Set( "WheelAngle", static_cast< float >( std::fmod( p.PaintAngle( t ), 2.0 * kPi ) ) );
	shader.Set( "ClapperAngle", static_cast< float >( p.Clapper( t ) ) );
	glUniform1iv( shader.FindUniform( "Layout" ), wheel::kSegments, values );
	shader.Set( "Highlight", atRest ? shown : -1 );
	shader.Set( "Celebrate", atRest && p.outcome.win && since >= 0.0 && since < kBannerFor ? 1 : 0 );
	//= mirrored in Shaders.cpp (kWheel's SPAN_ constants).
	SetTextUniforms( shader, { "$1", "$2", "$5", "$10", "$20", "JOKER", "JACKPOT" } );
}
namespace
{
/// The dice engine's axes (y up, the back wall at z = -0.20) into the drawn
/// scene's (z up, the back wall at y = +0.20): (x, y, z) -> (x, -z, y), a
/// proper rotation, so R becomes P R P^T and handedness is kept.
V3 ToScene( V3 v )
{
	return { v.x, -v.z, v.y };
}

void ToSceneMatrix( const M3& r, float out[ 9 ] )
{
	//Columns of P R P^T: the images of the scene's axes.
	const V3 axes[ 3 ] = { { 1, 0, 0 }, { 0, 0, -1 }, { 0, 1, 0 } };//P^T e_x, P^T e_y, P^T e_z
	for( int c = 0; c < 3; ++c )
	{
		const V3 col = ToScene( r * axes[ c ] );
		out[ c * 3 + 0 ] = static_cast< float >( col.x );
		out[ c * 3 + 1 ] = static_cast< float >( col.y );
		out[ c * 3 + 2 ] = static_cast< float >( col.z );
	}
}
} // namespace

void JackpotPlugin::DrawCraps( FFGLShader& shader, double t, int width, int height )
{
	const craps::Plan& p   = play.craps;
	const dice::geo::Solid& cube = dice::geo::GetSolid( dice::geo::Shape::Cube );
	const dice::geo::Labelling& labels = dice::geo::GetLabelling( dice::geo::DieType::D6, 0 );
	float rot[ 4 * 9 ] = {};
	float pos[ 4 * 3 ] = {};
	GLint on[ 4 ]      = {};
	for( int i = 0; i < 4; ++i )
	{
		V3 x;
		M3 r;
		const bool shown = i < 2 ? p.DieAt( i, t, x, r ) : p.OldDieAt( i - 2, t, x, r );
		if( !shown )
			continue;
		on[ i ]        = 1;
		const V3 at    = ToScene( x );
		pos[ i * 3 + 0 ] = static_cast< float >( at.x );
		pos[ i * 3 + 1 ] = static_cast< float >( at.y );
		pos[ i * 3 + 2 ] = static_cast< float >( at.z );
		ToSceneMatrix( r, &rot[ i * 9 ] );
	}
	//Which value is on which face, in the scene's body axes (P n).
	GLint faceValue[ 6 ] = { 1, 6, 2, 5, 3, 4 };
	for( size_t f = 0; f < cube.faces.size() && f < labels.digit.size(); ++f )
	{
		const V3 n = ToScene( cube.faces[ f ].normal );
		const double a[ 3 ] = { n.x, n.y, n.z };
		int axis = 0;
		for( int k = 1; k < 3; ++k )
			if( std::fabs( a[ k ] ) > std::fabs( a[ axis ] ) )
				axis = k;
		faceValue[ axis * 2 + ( a[ axis ] > 0.0 ? 0 : 1 ) ] = labels.digit[ f ];
	}
	const double since = clock - winSince;
	SetCamera( shader, width, height, 0.26, 0.06, { 0.0, 0.0, 0.0 } );
	glUniformMatrix3fv( shader.FindUniform( "DieRot" ), 4, GL_FALSE, rot );
	glUniform3fv( shader.FindUniform( "DiePos" ), 4, pos );
	glUniform1iv( shader.FindUniform( "DieOn" ), 4, on );
	shader.Set( "DieHalf", static_cast< float >( cube.inradius ) );
	glUniform1iv( shader.FindUniform( "FaceValue" ), 6, faceValue );
	shader.Set( "Point", crapsPoint );
	shader.Set( "Pyramids", params[ PT_PYRAMIDS ] > 0.5f ? 1 : 0 );
	shader.Set( "Puck", params[ PT_PUCK ] > 0.5f ? 1 : 0 );
	shader.Set( "Celebrate", !p.idle && t >= p.playback.duration && p.outcome.win && since >= 0.0 && since < kBannerFor ? 1 : 0 );
	//= mirrored in Shaders.cpp (kCraps's SP_ constants).
	SetTextUniforms( shader, { "SIX", "NINE", "COME", "FIELD", "PASS LINE", "DON'T PASS BAR", "ON", "OFF", "2 3 4 9 10 11 12" } );
}
void JackpotPlugin::DrawLottery( FFGLShader& shader, double t, int width, int height )
{
	const lottery::Plan& p = play.lottery;
	//A lottery machine is seen from in front: the same Tilt, 35 degrees lower.
	const Camera camera    = SetCamera( shader, width, height, 0.40, 0.36, { 0.07, 0.0, 0.0 }, 35.0 );
	const std::vector< lottery::BallState > balls = p.Balls( t );
	//Rows: (x y z number), (quaternion w x y z), and the ball's disc on the
	//frame (x y radius in pixels), which is how a pixel finds its few balls.
	const int count = std::min( static_cast< int >( balls.size() ), kBallSlots );
	std::vector< float > data( static_cast< size_t >( kBallSlots ) * 3 * 4, 0.0f );
	for( int i = 0; i < count; ++i )
	{
		const lottery::BallState& b = balls[ static_cast< size_t >( i ) ];
		float* row0 = &data[ static_cast< size_t >( i ) * 4 ];
		float* row1 = &data[ ( static_cast< size_t >( kBallSlots ) + static_cast< size_t >( i ) ) * 4 ];
		float* row2 = &data[ ( 2 * static_cast< size_t >( kBallSlots ) + static_cast< size_t >( i ) ) * 4 ];
		row0[ 0 ] = static_cast< float >( b.x.x );
		row0[ 1 ] = static_cast< float >( b.x.y );
		row0[ 2 ] = static_cast< float >( b.x.z );
		row0[ 3 ] = static_cast< float >( b.number );
		row1[ 0 ] = static_cast< float >( b.q.w );
		row1[ 1 ] = static_cast< float >( b.q.x );
		row1[ 2 ] = static_cast< float >( b.q.y );
		row1[ 3 ] = static_cast< float >( b.q.z );
		const V3 at = camera.Project( b.x );
		if( at.z > 0.0 )
		{
			//The disc's radius with room for the perspective's stretch at the
			//frame's edge and the four rays' spread.
			row2[ 0 ] = static_cast< float >( at.x );
			row2[ 1 ] = static_cast< float >( at.y );
			row2[ 2 ] = static_cast< float >( lottery::kBall * camera.focal / at.z * 0.5 * height * 1.25 + 2.0 );
		}
	}
	glActiveTexture( GL_TEXTURE3 );
	glBindTexture( GL_TEXTURE_2D, ballTexture );
	glTexSubImage2D( GL_TEXTURE_2D, 0, 0, 0, kBallSlots, 3, GL_RGBA, GL_FLOAT, data.data() );
	glActiveTexture( GL_TEXTURE0 );
	shader.Set( "BallData", 3 );
	shader.Set( "BallCount", count );
	shader.Set( "Air", static_cast< float >( p.Air( t ) ) );
	SetTextUniforms( shader, {} );
}
void JackpotPlugin::DrawShower( int width, int height, GLint hostFbo )
{
	const std::vector< Piece >& pieces = shower.Pieces();
	const int count = std::min( static_cast< int >( pieces.size() ), Shower::kMaxPieces );
	std::vector< float > data( static_cast< size_t >( count ) * 12 );
	for( int i = 0; i < count; ++i )
	{
		const Piece& p = pieces[ static_cast< size_t >( i ) ];
		float* d       = &data[ static_cast< size_t >( i ) * 12 ];
		d[ 0 ]  = static_cast< float >( p.x.x );
		d[ 1 ]  = static_cast< float >( p.x.y );
		d[ 2 ]  = static_cast< float >( p.x.z );
		d[ 3 ]  = static_cast< float >( p.radius );
		d[ 4 ]  = static_cast< float >( p.q.w );
		d[ 5 ]  = static_cast< float >( p.q.x );
		d[ 6 ]  = static_cast< float >( p.q.y );
		d[ 7 ]  = static_cast< float >( p.q.z );
		d[ 8 ]  = static_cast< float >( p.half );
		d[ 9 ]  = p.kind == PieceKind::Coin ? 1.0f : 0.0f;
		d[ 10 ] = static_cast< float >( p.colour );
		d[ 11 ] = static_cast< float >( std::clamp( p.fade, 0.0, 1.0 ) );
	}
	const GLint* hostViewport = nullptr;
	GLint viewport[ 4 ];
	glGetIntegerv( GL_VIEWPORT, viewport );
	hostViewport = viewport;

	//The pieces, into their own buffer with depth.
	glBindFramebuffer( GL_FRAMEBUFFER, showerFbo );
	glViewport( 0, 0, width, height );
	glClearColor( 0.0f, 0.0f, 0.0f, 0.0f );
	glClearDepth( 1.0 );
	glClear( GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT );
	glEnable( GL_DEPTH_TEST );
	glDepthFunc( GL_LESS );
	glDepthMask( GL_TRUE );
	glDisable( GL_BLEND );
	glBindBuffer( GL_ARRAY_BUFFER, showerVbo );
	glBufferSubData( GL_ARRAY_BUFFER, 0, static_cast< GLsizeiptr >( data.size() * sizeof( float ) ), data.data() );
	glBindBuffer( GL_ARRAY_BUFFER, 0 );
	{
		ScopedShaderBinding binding( showerProgram.GetGLID() );
		float felt[ 3 ], accent[ 3 ];
		linearColour( &params[ PT_FELT_R ], felt );
		linearColour( &params[ PT_ACCENT_R ], accent );
		showerProgram.Set( "Resolution", static_cast< float >( width ), static_cast< float >( height ) );
		showerProgram.Set( "LightAngle", static_cast< float >( LightAngleFromParam( params[ PT_LIGHT_ANGLE ] ) * kPi / 180.0 ) );
		showerProgram.Set( "Lights", std::clamp( params[ PT_LIGHTS ], 0.0f, 1.0f ) );
		showerProgram.Set( "TestFlat", flat != 0 ? 1 : 0 );
		showerProgram.Set( "Time", static_cast< float >( std::fmod( clock, 3600.0 ) ) );
		//Seen from a little above (15 degrees), so a piece lying in the pile
		//shows its face, lifted so the pile sits along the frame's foot.
		const double a        = 15.0 * kPi / 180.0;
		const float view[ 9 ] = { 1.0f, 0.0f, 0.0f,
		                          0.0f, static_cast< float >( std::cos( a ) ), static_cast< float >( std::sin( a ) ),
		                          0.0f, static_cast< float >( -std::sin( a ) ), static_cast< float >( std::cos( a ) ) };
		glUniformMatrix3fv( showerProgram.FindUniform( "View" ), 1, GL_FALSE, view );
		showerProgram.Set( "ViewLift", 0.05f );
		glBindVertexArray( showerVao );
		glDrawArraysInstanced( GL_TRIANGLE_STRIP, 0, 4, count );
		glBindVertexArray( 0 );
	}
	glDisable( GL_DEPTH_TEST );

	//Over the frame, premultiplied.
	glBindFramebuffer( GL_FRAMEBUFFER, static_cast< GLuint >( hostFbo ) );
	glViewport( hostViewport[ 0 ], hostViewport[ 1 ], hostViewport[ 2 ], hostViewport[ 3 ] );
	glEnable( GL_BLEND );
	glBlendFunc( GL_ONE, GL_ONE_MINUS_SRC_ALPHA );
	{
		ScopedShaderBinding binding( compositeProgram.GetGLID() );
		compositeProgram.Set( "Pieces", 0 );
		compositeProgram.Set( "Strength", isEffect ? std::clamp( params[ PT_MIX ], 0.0f, 1.0f ) : 1.0f );
		bindUnit( 0, showerColour );
		quad.Draw();
		unbindTextureUnits( 1 );
	}
	glDisable( GL_BLEND );
}

//---------------------------------------------------------------------------
FFResult JackpotPlugin::ProcessOpenGL( ProcessOpenGLStruct* pgl )
{
	const Game game = CurrentGame();
	if( pgl == nullptr || programs[ static_cast< int >( game ) ].GetGLID() == 0 )
		return FF_FAIL;
	const FFGLTextureStruct* input = nullptr;
	if( isEffect )
	{
		if( pgl->numInputTextures < 1 || pgl->inputTextures[ 0 ] == nullptr )
			return FF_FAIL;
		input = pgl->inputTextures[ 0 ];
	}

	ScopedGLState restore;
	const GLint* hostViewport = restore.saved.viewport;
	const int width           = input ? static_cast< int >( input->Width ) : hostViewport[ 2 ];
	const int height          = input ? static_cast< int >( input->Height ) : hostViewport[ 3 ];
	if( width <= 0 || height <= 0 )
		return FF_FAIL;
	glDisable( GL_BLEND );

	//Time: frame-relative, so Resolume's huge float clock never reaches the
	//shader; a backwards or large step (a clip trigger, a scrub) passes none.
	UpdateClock();
	double dt = 0.0;
	if( lastNow >= 0.0 )
	{
		const double step = now - lastNow;
		if( step > 0.0 && step <= kMaxFrameDelta )
			dt = step;
	}
	lastNow = now;
	transport.Advance( dt );
	shower.settings.aspect = static_cast< double >( width ) / height;
	Tick( dt );

	//Everything allocated and uploaded before anything is bound to draw.
	if( fontDirty )
		ResolveFont();
	if( atlasUploadDirty )
		UploadAtlas();
	if( symbolsUploadDirty )
		UploadSymbols();
	const bool showerOn = !shower.Pieces().empty();
	if( showerOn && ( showerWidth != width || showerHeight != height || showerFbo == 0 ) )
	{
		if( showerColour )
			glDeleteTextures( 1, &showerColour );
		if( showerDepth )
			glDeleteRenderbuffers( 1, &showerDepth );
		if( showerFbo )
			glDeleteFramebuffers( 1, &showerFbo );
		glGenTextures( 1, &showerColour );
		glBindTexture( GL_TEXTURE_2D, showerColour );
		glTexImage2D( GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr );
		glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST );
		glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST );
		glBindTexture( GL_TEXTURE_2D, 0 );
		glGenRenderbuffers( 1, &showerDepth );
		glBindRenderbuffer( GL_RENDERBUFFER, showerDepth );
		glRenderbufferStorage( GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, width, height );
		glBindRenderbuffer( GL_RENDERBUFFER, 0 );
		glGenFramebuffers( 1, &showerFbo );
		glBindFramebuffer( GL_FRAMEBUFFER, showerFbo );
		glFramebufferTexture2D( GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, showerColour, 0 );
		glFramebufferRenderbuffer( GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, showerDepth );
		if( glCheckFramebufferStatus( GL_FRAMEBUFFER ) != GL_FRAMEBUFFER_COMPLETE )
			diag::error( "the shower's framebuffer is incomplete" );
		glBindFramebuffer( GL_FRAMEBUFFER, pgl->HostFBO );
		showerWidth  = width;
		showerHeight = height;
	}

	//-------------------------------------------------------------------
	// The game, straight onto the host's framebuffer.
	//-------------------------------------------------------------------
	const double t = clock - playStart;
	{
		FFGLShader& shader = programs[ static_cast< int >( game ) ];
		ScopedShaderBinding shaderBinding( shader.GetGLID() );
		SetCommonUniforms( shader, width, height, input );
		switch( play.game == game ? game : Game::ShowerOnly )
		{
		case Game::Slots: DrawSlots( shader, t ); break;
		case Game::Roulette: DrawRoulette( shader, t, width, height ); break;
		case Game::MoneyWheel: DrawWheel( shader, t ); break;
		case Game::Craps: DrawCraps( shader, t, width, height ); break;
		case Game::Lottery: DrawLottery( shader, t, width, height ); break;
		default: break;
		}
		bindUnit( 0, input != nullptr ? input->Handle : blankTexture );
		bindUnit( 1, atlasTexture );
		bindUnit( 2, symbolTexture );
		glViewport( hostViewport[ 0 ], hostViewport[ 1 ], width, height );
		quad.Draw();
		unbindTextureUnits( 4 );
		glActiveTexture( GL_TEXTURE0 );
	}

	if( showerOn )
		DrawShower( width, height, static_cast< GLint >( pgl->HostFBO ) );
	return FF_SUCCESS;
}

//---------------------------------------------------------------------------
FFResult JackpotPlugin::SetTime( double time )
{
	hostTime = time;
	return FF_SUCCESS;
}

void JackpotPlugin::SetBeatInfo( float bpm, float barPhase )
{
	CFFGLPlugin::SetBeatInfo( bpm, barPhase );
	transport.SetBeatInfo( bpm, barPhase );
}

FFResult JackpotPlugin::SetFloatParameter( unsigned int index, float value )
{
	if( index >= ParamCount() )
		return FF_FAIL;
	if( index >= PT_ABOUT_TEXT && index < PT_SOURCE_COUNT )
		return stoatworks::about::handleParam( index - PT_ABOUT_TEXT, value ) ? FF_SUCCESS : FF_FAIL;

	//Events act on the rising edge, once.
	if( index == PT_PLAY )
	{
		const bool down = value >= 0.5f;
		if( down && !playHeld )
			++playPresses;
		playHeld = down;
	}
	if( index == PT_SHOWER_NOW )
	{
		const bool down = value >= 0.5f;
		if( down && !showerHeld )
			++showerPresses;
		showerHeld = down;
	}
	const float previous = params[ index ];
	params[ index ]      = value;
	if( value != previous && index == PT_FONT )
		fontDirty = true;
	return FF_SUCCESS;
}

float JackpotPlugin::GetFloatParameter( unsigned int index )
{
	return index < ParamCount() ? params[ index ] : 0.0f;
}

FFResult JackpotPlugin::SetTextParameter( unsigned int index, const char* value )
{
	//Display-only, and it MUST still succeed: instantiateGL pushes every
	//declared default back through the setters and deletes the instance if
	//one fails.
	if( index == PT_ABOUT_TEXT )
		return FF_SUCCESS;
	const std::string incoming = value != nullptr ? value : "";
	std::lock_guard< std::mutex > lock( textMutex );
	switch( index )
	{
	case PT_FONT_FILE:
		if( incoming != fontFilePath )
		{
			fontFilePath = incoming;
			fontDirty    = true;
		}
		return FF_SUCCESS;
	case PT_FONT_NAME:
		if( incoming != fontName )
		{
			fontName    = incoming;
			fontNameSet = true;
			fontDirty   = true;
		}
		return FF_SUCCESS;
	default:
		return FF_FAIL;
	}
}

char* JackpotPlugin::GetTextParameter( unsigned int index )
{
	if( index == PT_ABOUT_TEXT )
	{
		static const std::string aboutLine = stoatworks::about::textParam( 0 );
		return const_cast< char* >( aboutLine.c_str() );
	}
	std::lock_guard< std::mutex > lock( textMutex );
	const std::string* source = nullptr;
	switch( index )
	{
	case PT_FONT_FILE: source = &fontFilePath; break;
	case PT_FONT_NAME: source = &fontName; break;
	default: break;
	}
	textReturn[ 0 ] = '\0';
	if( source != nullptr )
	{
		const size_t length = std::min( source->size(), sizeof( textReturn ) - 1 );
		std::memcpy( textReturn, source->data(), length );
		textReturn[ length ] = '\0';
	}
	return textReturn;
}

} // namespace jackpot
