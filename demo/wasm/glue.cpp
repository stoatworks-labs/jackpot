/*
    The page's way in to the plugin: what an FFGL host does to a plugin
    instance, as C functions JavaScript can call.

    Nothing here is the plugin, and nothing here does the plugin's work. The
    plugin is `source/Jackpot.cpp` and everything it calls, compiled UNMODIFIED
    into the same WebAssembly module as this file (see
    demo/tools/build-wasm.sh for the list). This file only plays the host:

      - it constructs a `JackpotPlugin`, as SourcePlugin.cpp / EffectPlugin.cpp's
        registrations would, and calls InitGL / DeInitGL on it;
      - it reads the parameter declarations back through the FFGL SDK's own
        host-facing getters (CFFGLPluginManager::GetParamName and the rest,
        the SDK compiled unmodified too), so the page's panel is built from
        the plugin's constructor rather than from a copy of it;
      - it forwards the host's calls: SetFloatParameter, SetTextParameter,
        SetTime, ProcessOpenGL with a viewport (the source) or one input
        texture (the Over), exactly as `tools/jptest`'s rig does;
      - it answers the page's read-outs through the plugin's own accessors --
        the ones the harness uses (CurrentPlay, PlaybackSeconds, PlaysStarted,
        BoardText, Credits, CrapsPoint, CurrentShower, FontFamily,
        WheelFileStatus) -- and Controls.cpp's conversions for the number
        beside each slider.

    Two choices here are the page's and not the plugin's, and the page says so:

      - `SetSynchronousForTest( true )`: the plugin plans roulette, the money
        wheel, craps and the lottery on a worker thread (std::async) and starts
        the play a frame or two after the press, back-dated to it. This module
        is built without threads, so it plans on the page's thread, as the
        plugin's own harness does, and the frame after a press waits for the
        planner. The slot machine plans on the render thread in the plugin too.
      - The clock is the page's: SetTime is handed the kit's seconds, and the
        plugin's own vote decides the unit (it settles on seconds within four
        frames, as it would in Arena). There is no host transport: nothing
        calls SetBeatInfo, so Land On's Beat and Bar fall on the plugin's own
        bar clock at the 120 BPM it starts with (Transport.h), as they would
        in a host that never sends one.

    GL calls go to the page's WebGL2 context through emscripten's GL library;
    demo/wasm/gl_shim.cpp replaces the four entry points that cannot be passed
    straight through.
*/
#include "Controls.h"
#include "Diag.h"
#include "Jackpot.h"

#include <emscripten/emscripten.h>

#include <sys/stat.h>

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>

using namespace jackpot;

namespace
{
/// One plugin instance and the host-side structs it is handed.
struct Instance
{
	explicit Instance( bool effect ) : plugin( effect )
	{
		plugin.SetSynchronousForTest( true );
	}

	JackpotPlugin plugin;
	bool initialised = false;

	FFGLTextureStruct input {};
	FFGLTextureStruct* inputs[ 1 ] = { &input };
	ProcessOpenGLStruct process {};

	std::string scratch;///< a string handed back to JS lives here until the next call
};

float asFloat( FFMixed mixed )
{
	float value = 0.0f;
	static_assert( sizeof( value ) == sizeof( mixed.UIntValue ), "FFMixed carries a float in its bits" );
	std::memcpy( &value, &mixed.UIntValue, sizeof( value ) );
	return value;
}

const char* hold( Instance* instance, std::string text )
{
	instance->scratch = std::move( text );
	return instance->scratch.c_str();
}
} // namespace

extern "C"
{
//---------------------------------------------------------------------------
// Lifetime.
//---------------------------------------------------------------------------
EMSCRIPTEN_KEEPALIVE Instance* jp_new( int effect )
{
	return new Instance( effect != 0 );
}

EMSCRIPTEN_KEEPALIVE void jp_delete( Instance* instance )
{
	if( instance == nullptr )
		return;
	if( instance->initialised )
		instance->plugin.DeInitGL();
	delete instance;
}

/// InitGL, with the viewport a host would pass. 1 on success.
EMSCRIPTEN_KEEPALIVE int jp_init_gl( Instance* instance, int width, int height )
{
	FFGLViewportStruct viewport {};
	viewport.width  = static_cast< GLuint >( width );
	viewport.height = static_cast< GLuint >( height );
	instance->initialised = instance->plugin.InitGL( &viewport ) == FF_SUCCESS;
	return instance->initialised ? 1 : 0;
}

//---------------------------------------------------------------------------
// The declarations, read as a host reads them.
//---------------------------------------------------------------------------
EMSCRIPTEN_KEEPALIVE int jp_param_count( Instance* instance )
{
	return static_cast< int >( instance->plugin.GetNumParams() );
}

EMSCRIPTEN_KEEPALIVE const char* jp_param_name( Instance* instance, int index )
{
	const char* name = instance->plugin.GetParamName( static_cast< unsigned int >( index ) );
	return name ? name : "";
}

EMSCRIPTEN_KEEPALIVE int jp_param_type( Instance* instance, int index )
{
	return static_cast< int >( instance->plugin.GetParamType( static_cast< unsigned int >( index ) ) );
}

EMSCRIPTEN_KEEPALIVE const char* jp_param_group( Instance* instance, int index )
{
	return hold( instance, instance->plugin.GetParamGroup( static_cast< unsigned int >( index ) ) );
}

/// The declared default of a numeric parameter (FF_TYPE_STANDARD already
/// clamped into 0..1 by the SDK, as a host sees it).
EMSCRIPTEN_KEEPALIVE float jp_param_default( Instance* instance, int index )
{
	return asFloat( instance->plugin.GetParamDefault( static_cast< unsigned int >( index ) ) );
}

/// The declared default of a text or file parameter.
EMSCRIPTEN_KEEPALIVE const char* jp_param_default_text( Instance* instance, int index )
{
	const FFMixed mixed = instance->plugin.GetParamDefault( static_cast< unsigned int >( index ) );
	return mixed.PointerValue ? static_cast< const char* >( mixed.PointerValue ) : "";
}

EMSCRIPTEN_KEEPALIVE int jp_param_element_count( Instance* instance, int index )
{
	return static_cast< int >( instance->plugin.GetNumParamElements( static_cast< unsigned int >( index ) ) );
}

EMSCRIPTEN_KEEPALIVE const char* jp_param_element_name( Instance* instance, int index, int element )
{
	const char* name = instance->plugin.GetParamElementName( static_cast< unsigned int >( index ), static_cast< unsigned int >( element ) );
	return name ? name : "";
}

EMSCRIPTEN_KEEPALIVE float jp_param_element_value( Instance* instance, int index, int element )
{
	return asFloat( instance->plugin.GetParamElementDefault( static_cast< unsigned int >( index ), static_cast< unsigned int >( element ) ) );
}

EMSCRIPTEN_KEEPALIVE float jp_param_range_min( Instance* instance, int index )
{
	return instance->plugin.GetParamRange( static_cast< unsigned int >( index ) ).min;
}

EMSCRIPTEN_KEEPALIVE float jp_param_range_max( Instance* instance, int index )
{
	return instance->plugin.GetParamRange( static_cast< unsigned int >( index ) ).max;
}

EMSCRIPTEN_KEEPALIVE int jp_param_extension_count( Instance* instance, int index )
{
	return static_cast< int >( instance->plugin.GetNumFileParamExtensions( static_cast< unsigned int >( index ) ) );
}

EMSCRIPTEN_KEEPALIVE const char* jp_param_extension( Instance* instance, int index, int extension )
{
	const char* text = instance->plugin.GetFileParamExtension( static_cast< unsigned int >( index ), static_cast< unsigned int >( extension ) );
	return text ? text : "";
}

EMSCRIPTEN_KEEPALIVE int jp_max_inputs( Instance* instance )
{
	return static_cast< int >( instance->plugin.GetMaxInputs() );
}

//---------------------------------------------------------------------------
// The host's calls.
//---------------------------------------------------------------------------
EMSCRIPTEN_KEEPALIVE int jp_set_float( Instance* instance, int index, float value )
{
	return instance->plugin.SetFloatParameter( static_cast< unsigned int >( index ), value ) == FF_SUCCESS ? 1 : 0;
}

EMSCRIPTEN_KEEPALIVE float jp_get_float( Instance* instance, int index )
{
	return instance->plugin.GetFloatParameter( static_cast< unsigned int >( index ) );
}

EMSCRIPTEN_KEEPALIVE int jp_set_text( Instance* instance, int index, const char* value )
{
	return instance->plugin.SetTextParameter( static_cast< unsigned int >( index ), value ) == FF_SUCCESS ? 1 : 0;
}

EMSCRIPTEN_KEEPALIVE const char* jp_get_text( Instance* instance, int index )
{
	const char* text = instance->plugin.GetTextParameter( static_cast< unsigned int >( index ) );
	return text ? text : "";
}

/// One frame: SetTime, then ProcessOpenGL into whatever the page has bound.
/// The source reads its size from the viewport the page set; the Over is
/// handed one input of clipWidth x clipHeight whose GL name is `clip` (a
/// texture the page registered with emscripten's GL tables). 1 on success.
EMSCRIPTEN_KEEPALIVE int jp_process( Instance* instance, double seconds, int clip, int clipWidth, int clipHeight )
{
	instance->plugin.SetTime( seconds );
	if( instance->plugin.IsEffect() )
	{
		instance->input.Width = instance->input.HardwareWidth = static_cast< FFUInt32 >( clipWidth );
		instance->input.Height = instance->input.HardwareHeight = static_cast< FFUInt32 >( clipHeight );
		instance->input.Handle                                 = static_cast< GLuint >( clip );
		instance->process.numInputTextures                     = 1;
		instance->process.inputTextures                        = instance->inputs;
	}
	else
	{
		instance->process.numInputTextures = 0;
		instance->process.inputTextures    = nullptr;
	}
	instance->process.HostFBO = 0;
	return instance->plugin.ProcessOpenGL( &instance->process ) == FF_SUCCESS ? 1 : 0;
}

//---------------------------------------------------------------------------
// For the panel's read-outs: Controls.cpp's conversion for a parameter, the
// one Jackpot.cpp applies to it (its requests, SetCommonUniforms, SetCamera,
// DrawSlots and Tick are where each pairing is made). NaN where the plugin
// uses the host's 0..1 as it is (Lights, Mix).
//---------------------------------------------------------------------------
EMSCRIPTEN_KEEPALIVE double jp_convert( int index, float v )
{
	switch( index )
	{
	case PT_SPIN_TIME: return SpinTimeFromParam( v );
	case PT_INTERVAL: return IntervalFromParam( v );
	case PT_BLUR: return BlurFromParam( v );
	case PT_REEL_BOUNCE: return ReelDampingFromParam( v );
	case PT_ROTOR_SPEED: return RotorSpeedFromParam( v );
	case PT_CLAPPER: return ClapperFromParam( v );
	case PT_AIR: return AirFromParam( v );
	case PT_AMOUNT: return AmountFromParam( v );
	case PT_PIECE_SIZE: return PieceSizeFromParam( v );
	case PT_LIGHT_ANGLE: return LightAngleFromParam( v );
	case PT_TILT: return TiltFromParam( v );
	case PT_ZOOM: return ZoomFromParam( v );
	default: return std::numeric_limits< double >::quiet_NaN();
	}
}

/// The harness's clock switch (SetClockScaleForTest): host seconds taken as
/// seconds from the first frame, skipping the plugin's vote against the wall
/// clock. The page never calls it -- its clock is voted on as in Arena -- but
/// demo/tools/crosscheck does, to drive this build on jptest's own 60 fps
/// clock, where frames come faster than the vote can see.
EMSCRIPTEN_KEEPALIVE void jp_set_clock_scale_for_test( Instance* instance, double scale )
{
	instance->plugin.SetClockScaleForTest( scale );
}

/// Give the plugin's log (Diag.cpp) somewhere to go: a directory in the
/// page's in-memory file system, named through the override Diag.cpp reads
/// (it is called POLYHEDRAL_LOG_DIR there, inherited with polyhedral's code).
/// Its own `mkdir -p` goes through system(), which a browser does not have.
/// Call before the first InitGL, which is when the log opens.
EMSCRIPTEN_KEEPALIVE void jp_prepare_log()
{
	setenv( "POLYHEDRAL_LOG_DIR", "/jackpot/logs", 1 );
	mkdir( "/jackpot", 0755 );
	mkdir( "/jackpot/logs", 0755 );
}

/// The plugin's log file (Diag.cpp), in the page's in-memory file system.
EMSCRIPTEN_KEEPALIVE const char* jp_log_path()
{
	static std::string path;
	path = diag::logPath();
	return path.c_str();
}

//---------------------------------------------------------------------------
// The play being played, through the harness's own accessors: the same
// fields the plugin's log line reports when a play comes to rest.
//---------------------------------------------------------------------------
/// How many plays have started: the harness's counter, so the page can tell
/// a press that has been honoured from one still pending.
EMSCRIPTEN_KEEPALIVE int jp_plays_started( Instance* instance )
{
	return instance->plugin.PlaysStarted();
}

/// The game the current play is (Game's order), or -1 at rest before a play.
EMSCRIPTEN_KEEPALIVE int jp_play_game( Instance* instance )
{
	const Play& play = instance->plugin.CurrentPlay();
	return play.Idle() ? -1 : static_cast< int >( play.game );
}

/// 1 once the play has come to rest (playback has reached its duration).
EMSCRIPTEN_KEEPALIVE int jp_play_finished( Instance* instance )
{
	const Play& play = instance->plugin.CurrentPlay();
	return play.Idle() || play.Clock().Finished( instance->plugin.PlaybackSeconds() ) ? 1 : 0;
}

/// The outcome's words, as the board and the log write them ("17 RED").
EMSCRIPTEN_KEEPALIVE const char* jp_play_text( Instance* instance )
{
	return hold( instance, instance->plugin.CurrentPlay().Result().text );
}

/// 2 jackpot, 1 win, 3 near miss, 0 none.
EMSCRIPTEN_KEEPALIVE int jp_play_kind( Instance* instance )
{
	const Outcome& o = instance->plugin.CurrentPlay().Result();
	return o.jackpot ? 2 : o.win ? 1 : o.nearMiss ? 3 : 0;
}

EMSCRIPTEN_KEEPALIVE double jp_play_natural( Instance* instance )
{
	return instance->plugin.CurrentPlay().Clock().natural;
}

EMSCRIPTEN_KEEPALIVE double jp_play_duration( Instance* instance )
{
	return instance->plugin.CurrentPlay().Clock().duration;
}

EMSCRIPTEN_KEEPALIVE double jp_play_warp( Instance* instance )
{
	return instance->plugin.CurrentPlay().Clock().warp;
}

EMSCRIPTEN_KEEPALIVE double jp_playback_seconds( Instance* instance )
{
	return instance->plugin.PlaybackSeconds();
}

EMSCRIPTEN_KEEPALIVE int jp_credits( Instance* instance )
{
	return instance->plugin.Credits();
}

EMSCRIPTEN_KEEPALIVE int jp_craps_point( Instance* instance )
{
	return instance->plugin.CrapsPoint();
}

/// Pieces of the shower in flight or in the pile.
EMSCRIPTEN_KEEPALIVE int jp_shower_pieces( Instance* instance )
{
	return static_cast< int >( instance->plugin.CurrentShower().Pieces().size() );
}

/// Why the last Wheel File fell back to the Big Six; empty if it did not.
EMSCRIPTEN_KEEPALIVE const char* jp_wheel_file_status( Instance* instance )
{
	return hold( instance, instance->plugin.WheelFileStatus() );
}

EMSCRIPTEN_KEEPALIVE int jp_wheel_file_loaded( Instance* instance )
{
	return instance->plugin.WheelFileLoaded() ? 1 : 0;
}

} // extern "C"
