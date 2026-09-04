#include "Abomerration.h"

//The SDK's umbrella FFGLSDK.h pulls in every other scoped binding but leaves
//this one out (SDK b1afaf9), so it has to be reached for by hand.
#include <ffglex/FFGLScopedFBOBinding.h>

#include "Diag.h"
#include "Shaders.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <string>

using namespace ffglex;
using namespace abomerration;

static CFFGLPluginInfo PluginInfo(
	PluginFactory< Abomerration >,                    // Create method
	"AB01",                                           // Plugin unique ID of maximum length 4
	"Abomerration",                                   // Plugin name
	2,                                                // API major version number
	1,                                                // API minor version number
	0,                                                // Plugin major version number
	1,                                                // Plugin minor version number
	FF_EFFECT,                                        // Plugin type
	"Sound-reactive chromatic aberration",            // Plugin description
	"Abomerration FFGL effect"                        // About
);

namespace
{
/// The longest frame delta the drift integrator will accept. A host that stalls
/// -- loading a clip, or the operator dragging the transport -- hands over a
/// enormous delta on the next frame, and integrating it advances the noise field
/// by a visible jump. Clamping costs a little accuracy on a genuinely slow frame
/// and buys not lurching after every hiccup.
constexpr double kMaxFrameDelta = 0.1;

/// Wall clock, for hosts that never call SetTime. Steady rather than system, so
/// it cannot go backwards when the machine's clock is corrected mid-show.
/// Frames that must agree before the host's clock unit is settled.
constexpr int kClockVotes = 4;

double wallSeconds()
{
	using namespace std::chrono;
	static const auto origin = steady_clock::now();
	return duration_cast< duration< double > >( steady_clock::now() - origin ).count();
}

/// glGetString returns nullptr when there is no current context, and feeding
/// that to std::string is undefined behaviour. A logging call must never be the
/// thing that brings the host down.
std::string glStringOrUnknown( GLenum name )
{
	const GLubyte* value = glGetString( name );
	return value ? reinterpret_cast< const char* >( value ) : "unknown";
}
} // namespace

Abomerration::Abomerration()
{
	SetMinInputs( 1 );
	SetMaxInputs( 1 );

	//Without this the host is entitled never to call SetTime, and the turbulence
	//would sit still in a host that would happily have driven it.
	SetTimeSupported( true );

	//---------------------------------------------------------------------
	// Defaults. SetParamInfof reads each one back out of GetFloatParameter, so
	// these assignments are what the host is told the defaults are.
	//
	// They are set to a lens that is visibly doing something the moment it is
	// dropped on a layer -- a radial dispersion with a real spectrum behind it --
	// and to a plugin that is NOT yet reacting to anything. Both halves matter.
	// An effect that does nothing until six sliders move is an effect nobody
	// finds out is any good; and an effect that starts out gated by audio nobody
	// has routed yet looks broken in exactly the same way.
	//---------------------------------------------------------------------
	const controls::HostValues defaults;

	params[ PT_GEOMETRY ]   = defaults.geometry;
	params[ PT_AMOUNT ]     = defaults.amount;
	params[ PT_CENTRE_X ]   = defaults.centreX;
	params[ PT_CENTRE_Y ]   = defaults.centreY;
	params[ PT_ANGLE ]      = defaults.angle;
	params[ PT_FALLOFF ]    = defaults.falloff;
	params[ PT_SPECTRUM ]   = defaults.spectrum;
	params[ PT_TURBULENCE ] = defaults.turbulence;
	params[ PT_DRIFT ]      = defaults.drift;

	params[ PT_RED_PUSH ]   = defaults.redPush;
	params[ PT_GREEN_PUSH ] = defaults.greenPush;
	params[ PT_BLUE_PUSH ]  = defaults.bluePush;

	params[ PT_AUDIO ]         = 0.0f;
	params[ PT_SYNC ]          = defaults.sync;
	params[ PT_BEAT_DEPTH ]    = defaults.beatDepth;
	params[ PT_BEAT_DECAY ]    = defaults.beatDecay;
	params[ PT_BEAT_DIVISION ] = defaults.beatDivision;
	params[ PT_LEVEL_DEPTH ]   = defaults.levelDepth;
	params[ PT_BAND_DEPTH ]    = defaults.bandDepth;
	params[ PT_ROUTE ]         = defaults.route;

	params[ PT_EDGES ]  = defaults.edges;
	params[ PT_FRINGE ] = defaults.fringe;

	params[ PT_SHOW_FIELD ] = defaults.showField;
	params[ PT_MIX ]        = defaults.mix;

	params[ PT_PRESET ] = 0.0f;//Custom: the sliders are the truth

	//---------------------------------------------------------------------
	// Declaration. The groups matter: this is twenty-four parameters, and an
	// ungrouped list of twenty-four in somebody else's inspector is unusable.
	//---------------------------------------------------------------------
	SetOptionParamInfo( PT_GEOMETRY, "Geometry", controls::geometryCount(), params[ PT_GEOMETRY ] );
	for( int i = 0; i < controls::geometryCount(); ++i )
		SetParamElementInfo( PT_GEOMETRY, i, controls::geometryLabel( i ), static_cast< float >( i ) );

	SetParamInfof( PT_AMOUNT, "Amount", FF_TYPE_STANDARD );
	SetParamInfof( PT_CENTRE_X, "Centre X", FF_TYPE_STANDARD );
	SetParamInfof( PT_CENTRE_Y, "Centre Y", FF_TYPE_STANDARD );
	SetParamInfof( PT_ANGLE, "Angle", FF_TYPE_STANDARD );
	SetParamInfof( PT_FALLOFF, "Falloff", FF_TYPE_STANDARD );

	SetOptionParamInfo( PT_SPECTRUM, "Spectrum", controls::spectrumCount(), params[ PT_SPECTRUM ] );
	for( int i = 0; i < controls::spectrumCount(); ++i )
		SetParamElementInfo( PT_SPECTRUM, i, controls::spectrumLabel( i ), static_cast< float >( i ) );

	SetParamInfof( PT_TURBULENCE, "Turbulence", FF_TYPE_STANDARD );
	SetParamInfof( PT_DRIFT, "Drift", FF_TYPE_STANDARD );

	SetParamInfof( PT_RED_PUSH, "Red Push", FF_TYPE_STANDARD );
	SetParamInfof( PT_GREEN_PUSH, "Green Push", FF_TYPE_STANDARD );
	SetParamInfof( PT_BLUE_PUSH, "Blue Push", FF_TYPE_STANDARD );

	// An FFT buffer. Resolume renders it as an audio-source picker and writes one
	// spectrum bin per element; the elements are declared with a default of 0 so
	// a host that shows the picker but has nothing routed reads as silence rather
	// than as noise.
	SetBufferParamInfo( PT_AUDIO, "Audio", drive::kAudioBins, FF_USAGE_FFT );
	for( int i = 0; i < drive::kAudioBins; ++i )
		SetParamElementInfo( PT_AUDIO, i, "", 0.0f );

	SetOptionParamInfo( PT_SYNC, "Sync", controls::syncCount(), params[ PT_SYNC ] );
	for( int i = 0; i < controls::syncCount(); ++i )
		SetParamElementInfo( PT_SYNC, i, controls::syncLabel( i ), static_cast< float >( i ) );

	SetParamInfof( PT_BEAT_DEPTH, "Beat Depth", FF_TYPE_STANDARD );
	SetParamInfof( PT_BEAT_DECAY, "Beat Decay", FF_TYPE_STANDARD );

	SetOptionParamInfo( PT_BEAT_DIVISION, "Division", controls::divisionCount(), params[ PT_BEAT_DIVISION ] );
	for( int i = 0; i < controls::divisionCount(); ++i )
		SetParamElementInfo( PT_BEAT_DIVISION, i, controls::divisionLabel( i ), static_cast< float >( i ) );

	SetParamInfof( PT_LEVEL_DEPTH, "Level Depth", FF_TYPE_STANDARD );
	SetParamInfof( PT_BAND_DEPTH, "Band Depth", FF_TYPE_STANDARD );

	SetOptionParamInfo( PT_ROUTE, "Band Route", controls::routeCount(), params[ PT_ROUTE ] );
	for( int i = 0; i < controls::routeCount(); ++i )
		SetParamElementInfo( PT_ROUTE, i, controls::routeLabel( i ), static_cast< float >( i ) );

	SetParamInfof( PT_EDGES, "Edges", FF_TYPE_STANDARD );
	SetParamInfof( PT_FRINGE, "Fringe", FF_TYPE_STANDARD );

	SetParamInfof( PT_SHOW_FIELD, "Show Field", FF_TYPE_BOOLEAN );
	SetParamInfof( PT_MIX, "Mix", FF_TYPE_STANDARD );

	// Factory presets. Element 0 is Custom; picking anything else copies that
	// preset's values into the covered parameters and raises value events so the
	// host re-reads the sliders. Editing a covered slider flips back to Custom.
	SetOptionParamInfo( PT_PRESET, "Preset", 1 + presets::kCount, params[ PT_PRESET ] );
	SetParamElementInfo( PT_PRESET, 0, "Custom", 0.0f );
	for( int i = 0; i < presets::kCount; ++i )
		SetParamElementInfo( PT_PRESET, 1 + i, presets::kPresets[ i ].name, static_cast< float >( 1 + i ) );

	// The About block. Inline rather than through a helper: SetParamInfo is
	// protected on CFFGLPlugin, so nothing outside the class can call it.
	SetParamInfo( PT_ABOUT_FIRST, "About", FF_TYPE_TEXT, stoatworks::about::defaultText() );
	{
		FFUInt32 aboutId = PT_ABOUT_FIRST + 1;
		for( const auto& b : stoatworks::about::buttons() )
			SetParamInfo( aboutId++, b.label, FF_TYPE_EVENT, false );
	}

	for( FFUInt32 i = PT_GEOMETRY; i <= PT_DRIFT; ++i )
		SetParamGroup( i, "Aberration" );
	for( FFUInt32 i = PT_RED_PUSH; i <= PT_BLUE_PUSH; ++i )
		SetParamGroup( i, "Channels" );
	for( FFUInt32 i = PT_AUDIO; i <= PT_ROUTE; ++i )
		SetParamGroup( i, "Reaction" );
	for( FFUInt32 i = PT_EDGES; i <= PT_FRINGE; ++i )
		SetParamGroup( i, "Look" );
	for( FFUInt32 i = PT_SHOW_FIELD; i <= PT_MIX; ++i )
		SetParamGroup( i, "Output" );

	SetParamGroup( PT_PRESET, "Preset" );

	FFGLLog::LogToHost( "Created Abomerration effect" );

	diag::init();
}

controls::HostValues Abomerration::hostValues() const
{
	controls::HostValues out;

	out.geometry   = params[ PT_GEOMETRY ];
	out.amount     = params[ PT_AMOUNT ];
	out.centreX    = params[ PT_CENTRE_X ];
	out.centreY    = params[ PT_CENTRE_Y ];
	out.angle      = params[ PT_ANGLE ];
	out.falloff    = params[ PT_FALLOFF ];
	out.spectrum   = params[ PT_SPECTRUM ];
	out.turbulence = params[ PT_TURBULENCE ];
	out.drift      = params[ PT_DRIFT ];

	out.redPush   = params[ PT_RED_PUSH ];
	out.greenPush = params[ PT_GREEN_PUSH ];
	out.bluePush  = params[ PT_BLUE_PUSH ];

	out.sync         = params[ PT_SYNC ];
	out.beatDepth    = params[ PT_BEAT_DEPTH ];
	out.beatDecay    = params[ PT_BEAT_DECAY ];
	out.beatDivision = params[ PT_BEAT_DIVISION ];
	out.levelDepth   = params[ PT_LEVEL_DEPTH ];
	out.bandDepth    = params[ PT_BAND_DEPTH ];
	out.route        = params[ PT_ROUTE ];

	out.edges  = params[ PT_EDGES ];
	out.fringe = params[ PT_FRINGE ];

	out.showField = params[ PT_SHOW_FIELD ];
	out.mix       = params[ PT_MIX ];

	return out;
}

bool Abomerration::compileShaders()
{
	struct Stage
	{
		FFGLShader* shader;
		std::string fragment;
		const char* name;
	};

	const Stage stages[] = {
		{ &copyShader, shaders::kCopyFragment, "copy" },
		{ &edgeShader, shaders::kEdgeFragment, "edge" },
		{ &disperseShader, shaders::DisperseFragment(), "disperse" },
	};

	for( const Stage& stage : stages )
	{
		if( !stage.shader->Compile( shaders::kVertex, stage.fragment.c_str() ) )
		{
			//Returning FF_FAIL from InitGL is invisible to the operator: the
			//effect simply does nothing in Resolume, with no message anywhere.
			//This line is the only record of which stage it was -- and for the
			//disperse pass it is a shader assembled from three strings at
			//runtime, so any line number the driver reports refers to a file that
			//does not exist.
			diag::error( std::string( "the " ) + stage.name
			             + " shader failed to compile - the effect will do nothing" );
			FFGLLog::LogToHost( "Abomerration: shader failed to compile" );
			return false;
		}
	}

	return true;
}

FFResult Abomerration::InitGL( const FFGLViewportStruct* vp )
{
	//The GL strings first, and unconditionally. When a shader will not compile it
	//is almost always the driver or the GL version, and knowing which machine
	//reported what is the whole diagnosis.
	diag::info( std::string( "GL vendor=" ) + glStringOrUnknown( GL_VENDOR )
	            + " renderer=" + glStringOrUnknown( GL_RENDERER )
	            + " version=" + glStringOrUnknown( GL_VERSION ) );

	if( !compileShaders() )
	{
		DeInitGL();
		return FF_FAIL;
	}

	if( !quad.Initialise() )
	{
		diag::error( "quad geometry failed to initialise" );
		FFGLLog::LogToHost( "Abomerration: quad geometry failed to initialise" );
		DeInitGL();
		return FF_FAIL;
	}

	diag::info( "initialised" );

	//Use the base class init as the success result so it retains the viewport.
	return CFFGLPlugin::InitGL( vp );
}

FFResult Abomerration::ProcessOpenGL( ProcessOpenGLStruct* pGL )
{
	if( pGL->numInputTextures < 1 || pGL->inputTextures[ 0 ] == nullptr )
		return FF_FAIL;

	const FFGLTextureStruct& input = *pGL->inputTextures[ 0 ];

	//The host's viewport, not the one InitGL was handed: Resolume changes
	//composition resolution without reinitialising the plugin.
	//
	//It also has to be captured before any pass runs, because ScopedFBOBinding
	//restores the framebuffer binding and NOT the viewport -- so the edge pass's
	//ResizeViewPort() leaks into the dispersion pass, which draws to the host's
	//own framebuffer and has no buffer of its own to size itself from.
	GLint hostViewport[ 4 ] = { 0, 0, 0, 0 };
	glGetIntegerv( GL_VIEWPORT, hostViewport );
	const int frameW = std::max( 1, hostViewport[ 2 ] );
	const int frameH = std::max( 1, hostViewport[ 3 ] );

	const float frameWf     = static_cast< float >( frameW );
	const float frameHf     = static_cast< float >( frameH );
	const float aspectRatio = frameWf / frameHf;

	const controls::HostValues host = hostValues();

	//---------------------------------------------------------------------
	// Time. Normalise the host's clock to seconds first -- Resolume sends
	// milliseconds, this repo's harness sends seconds, and the FFGL header says
	// nothing at all. Until the first plausible frame delta decides, assume
	// seconds; the decision lands within two frames and the delta clamp below
	// absorbs the single mis-scaled step a late decision could produce.
	//---------------------------------------------------------------------
	// steady_clock says how much real time passed, the host says how much host
	// time passed, and the ratio names the unit outright -- 1 for seconds,
	// 1000 for milliseconds, and nothing plausible in between. This replaced a
	// guess made from the magnitude of a single frame delta, which had three
	// holes: a delta between 0.5 and 2.0 decided nothing, a burst of sub-0.5 ms
	// frames at load locked it to "seconds" for the session, and while
	// undecided it assumed seconds -- precisely the millisecond host's wrong
	// answer.
	const double wallNow = wallSeconds();
	if( wallStart < 0.0 )
		wallStart = wallNow;

	const double raw = hostTime;

	if( clockScale == 0.0 && raw >= 0.0 && lastRawTime >= 0.0 && lastWallTime >= 0.0 )
	{
		const double hostDelta = raw - lastRawTime;
		const double wallDelta = wallNow - lastWallTime;

		// A paused host, a looping clip or a stalled frame tells us nothing.
		if( hostDelta > 0.0 && wallDelta >= 0.0005 )
		{
			const double ratio = hostDelta / wallDelta;
			if( ratio > 0.1 && ratio < 10.0 )
				++secondsVotes;
			else if( ratio > 100.0 && ratio < 10000.0 )
				++millisVotes;

			// Several frames rather than one, so a single odd frame cannot
			// decide it alone.
			if( secondsVotes >= kClockVotes || millisVotes >= kClockVotes )
				clockScale = millisVotes > secondsVotes ? 0.001 : 1.0;
		}
	}

	if( raw >= 0.0 )
		lastRawTime = raw;
	lastWallTime = wallNow;

	// Until the unit is settled -- and for a host that never calls SetTime --
	// run on the real clock: wrong in origin but right in rate, where assuming
	// seconds would be a thousand times fast on Resolume.
	const double now = ( raw >= 0.0 && clockScale != 0.0 ) ? raw * clockScale
	                                                       : wallNow - wallStart;

	//Drift integrates. See Abomerration.h: `time * rate` rescales the history and
	//makes the noise field jump whenever the control is touched.
	if( lastHostTime >= 0.0 )
	{
		const double delta = std::clamp( now - lastHostTime, 0.0, kMaxFrameDelta );
		driftPhase += delta * static_cast< double >( controls::driftRate( host ) );
	}

	if( ++clockFrames == 60 )
		diag::info( "host clock at frame 60: raw=" + std::to_string( raw )
		            + " scale=" + std::to_string( clockScale )
		            + " seconds=" + std::to_string( now )
		            + " bpm=" + std::to_string( bpm )
		            + " barPhase=" + std::to_string( barPhase ) );

	lastHostTime = now;

	updateAudio();

	//---------------------------------------------------------------------
	// What the music is doing. All of it decided in Drive.cpp, on the CPU: the
	// spectrum never reaches a shader.
	//---------------------------------------------------------------------
	{
		drive::Input in;
		in.bins     = audioLevel.data();
		in.binCount = static_cast< int >( audioLevel.size() );
		in.bpm      = bpm;
		in.barPhase = barPhase;
		in.seconds  = now;

		driveOut = drive::compute( controls::driveSettings( host ), in );
	}

	const dispersion::Field field = controls::field( host, aspectRatio, driveOut.scale,
	                                                 static_cast< float >( driftPhase ) );
	const controls::Look look = controls::look( host, driveOut.push );

	//---------------------------------------------------------------------
	// Every allocation FIRST, before anything is bound.
	//
	// FFGLFBO::Initialise sizes its colour texture inside a scoped texture
	// binding, and those CLEAR to 0 on scope exit rather than restoring -- so an
	// Ensure() called after a texture was bound silently unbinds it, and the frame
	// that allocated renders black. PassBuffer::Ensure saves and restores around
	// it as well, but the ordering here is the real defence: do not move this
	// below the passes.
	//---------------------------------------------------------------------
	const bool needsEdges = look.edges > 0.0f;

	if( !copyBuffer.Ensure( frameW, frameH, GL_RGBA16F ) )
	{
		diag::error( "could not allocate the copy buffer" );
		return FF_FAIL;
	}

	if( needsEdges && !edgeBuffer.Ensure( frameW, frameH, GL_RGBA16F ) )
	{
		diag::error( "could not allocate the edge buffer" );
		return FF_FAIL;
	}

	const FFGLTexCoords maxCoords = GetMaxGLTexCoords( input );
	const float inputHalfTexelU   = 0.5f / std::max( 1.0f, static_cast< float >( input.Width ) );
	const float inputHalfTexelV   = 0.5f / std::max( 1.0f, static_cast< float >( input.Height ) );

	//Every pass does its geometry in picture space and applies MaxUV at the
	//fetch, so the vertex shader's scaling is always off.
	const float kNoScale = 1.0f;

	//Our own buffers carry no padding, so their MaxUV is 1 and their half texel is
	//off their own size -- which here is the composition's.
	const float ownHalfTexelU = 0.5f / frameWf;
	const float ownHalfTexelV = 0.5f / frameHf;

	//------------------------------------------------------------------
	// 1. The picture, into a buffer of ours, then mipmapped.
	//
	//    Both halves are load bearing. The copy resolves MaxUV so everything
	//    downstream works in plain 0..1 -- and so the mip chain averages picture
	//    pixels only, never the host's undrawn padding. The mip chain is what the
	//    dispersion's wavelength samples read from when they are more than a pixel
	//    apart, which is the difference between a spectrum and a staircase. See
	//    Copy.cpp.
	//------------------------------------------------------------------
	{
		ScopedFBOBinding fbo( copyBuffer.GetGLID(), ScopedFBOBinding::RB_REVERT );
		copyBuffer.ResizeViewPort();
		ScopedShaderBinding shader( copyShader.GetGLID() );
		ScopedSamplerActivation sampler( 0 );
		Scoped2DTextureBinding texture( input.Handle );

		copyShader.Set( "MaxUV", kNoScale, kNoScale );
		copyShader.Set( "InputTexture", 0 );
		copyShader.Set( "SourceMaxUV", maxCoords.s, maxCoords.t );
		copyShader.Set( "SourceHalfTexel", inputHalfTexelU, inputHalfTexelV );
		quad.Draw();
	}

	//Outside the scoped binding above: glGenerateMipmap needs the texture bound to
	//a unit, and every ffglex Scoped* binding clears to 0 on scope exit rather
	//than restoring, so doing it inside would fight the scope for the binding.
	copyBuffer.GenerateMipmaps();

	//------------------------------------------------------------------
	// 2. The edge weight. Skipped entirely when Edges is 0, which is the
	//    default -- so the dispersion pass binds the copy to EdgeTexture in
	//    that case rather than a buffer holding nothing meaningful.
	//------------------------------------------------------------------
	if( needsEdges )
	{
		ScopedFBOBinding fbo( edgeBuffer.GetGLID(), ScopedFBOBinding::RB_REVERT );
		edgeBuffer.ResizeViewPort();
		ScopedShaderBinding shader( edgeShader.GetGLID() );
		ScopedSamplerActivation sampler( 0 );
		//The copy, not the host's texture: one place resolves MaxUV and everything
		//after it works in plain 0..1.
		Scoped2DTextureBinding texture( copyBuffer.GetTextureInfo().Handle );

		edgeShader.Set( "MaxUV", kNoScale, kNoScale );
		edgeShader.Set( "InputTexture", 0 );
		edgeShader.Set( "SourceMaxUV", kNoScale, kNoScale );
		edgeShader.Set( "SourceHalfTexel", ownHalfTexelU, ownHalfTexelV );
		//One composition texel in picture space. Off the frame and not off the
		//input texture: the input can be larger than the picture, and a Sobel
		//stepping by a texel of the padded texture would measure a gradient over
		//the wrong distance near the edges.
		edgeShader.Set( "SourceTexel", 1.0f / frameWf, 1.0f / frameHf );
		quad.Draw();
	}

	//------------------------------------------------------------------
	// 3. The dispersion, straight into whatever the host handed us.
	//------------------------------------------------------------------
	{
		glBindFramebuffer( GL_FRAMEBUFFER, pGL->HostFBO );
		glViewport( hostViewport[ 0 ], hostViewport[ 1 ], hostViewport[ 2 ], hostViewport[ 3 ] );

		ScopedShaderBinding shader( disperseShader.GetGLID() );

		glActiveTexture( GL_TEXTURE0 );
		glBindTexture( GL_TEXTURE_2D, copyBuffer.GetTextureInfo().Handle );
		glActiveTexture( GL_TEXTURE1 );
		//Bound even when the shader will not read it: a sampler left pointing at
		//a deleted texture is undefined behaviour, not a harmless no-op.
		glBindTexture( GL_TEXTURE_2D,
		               needsEdges ? edgeBuffer.GetTextureInfo().Handle : copyBuffer.GetTextureInfo().Handle );
		glActiveTexture( GL_TEXTURE0 );

		disperseShader.Set( "MaxUV", kNoScale, kNoScale );
		disperseShader.Set( "InputTexture", 0 );
		disperseShader.Set( "EdgeTexture", 1 );
		disperseShader.Set( "SourceHalfTexel", ownHalfTexelU, ownHalfTexelV );

		//The frame height in pixels, so the shader can turn a displacement in
		//frame-height units into a sample spacing in pixels and pick a mip level
		//from it.
		disperseShader.Set( "FrameHeightPx", frameHf );

		disperseShader.Set( "Geometry", field.geometry );
		disperseShader.Set( "Centre", field.centreU, field.centreV );
		disperseShader.Set( "Angle", field.angle );
		disperseShader.Set( "Amount", field.amount );
		disperseShader.Set( "Falloff", field.falloff );
		disperseShader.Set( "Turbulence", field.turbulence );
		disperseShader.Set( "Drift", field.drift );
		disperseShader.Set( "FrameAspect", field.aspectRatio );

		disperseShader.Set( "EdgeWeight", look.edges );
		disperseShader.Set( "SampleCount", look.sampleCount );
		disperseShader.Set( "Push", look.push[ 0 ], look.push[ 1 ], look.push[ 2 ] );
		disperseShader.Set( "Fringe", look.fringe );
		disperseShader.Set( "MixAmount", look.mix );

		//`FFGLShader::Set` has no bool overload and no integer-vector overload.
		//A bool uniform set through the float overload is a GL_INVALID_OPERATION
		//that leaves the uniform at zero with nothing anywhere the plugin can
		//see, so these two go through the raw call.
		glUniform1i( disperseShader.FindUniform( "UniformPush" ), look.uniformPush ? 1 : 0 );
		glUniform1i( disperseShader.FindUniform( "ShowField" ), look.showField ? 1 : 0 );
		glUniform1i( disperseShader.FindUniform( "Prefilter" ), look.prefilter ? 1 : 0 );

		//The weight table, as a flat array of vec4. Built in Dispersion.cpp and
		//uploaded rather than recomputed in GLSL -- one implementation, so the
		//two cannot drift.
		{
			float flat[ dispersion::kMaxSamples * 4 ] = {};
			for( int i = 0; i < look.sampleCount; ++i )
			{
				flat[ i * 4 + 0 ] = look.samples[ i ].s;
				flat[ i * 4 + 1 ] = look.samples[ i ].r;
				flat[ i * 4 + 2 ] = look.samples[ i ].g;
				flat[ i * 4 + 3 ] = look.samples[ i ].b;
			}
			glUniform4fv( disperseShader.FindUniform( "Samples" ), look.sampleCount, flat );
		}

		//Show Field only. The reference is the full unweighted displacement at
		//r = 1, so the ramp reads as a fraction of what this setting could do
		//rather than of some absolute scale nobody can see.
		disperseShader.Set( "AmountRef", std::fabs( field.amount ) );
		glUniform4f( disperseShader.FindUniform( "Meters" ),
		             driveOut.bass, driveOut.mid, driveOut.high, driveOut.beat );

		quad.Draw();

		glActiveTexture( GL_TEXTURE1 );
		glBindTexture( GL_TEXTURE_2D, 0 );
		glActiveTexture( GL_TEXTURE0 );
		glBindTexture( GL_TEXTURE_2D, 0 );
	}

	return FF_SUCCESS;
}

FFResult Abomerration::DeInitGL()
{
	copyShader.FreeGLResources();
	edgeShader.FreeGLResources();
	disperseShader.FreeGLResources();
	quad.Release();
	copyBuffer.Destroy();
	edgeBuffer.Destroy();

	return FF_SUCCESS;
}

FFResult Abomerration::SetTime( double time )
{
	hostTime = time;
	return FF_SUCCESS;
}

void Abomerration::updateAudio()
{
	const ParamInfo* info = FindParamInfo( PT_AUDIO );
	if( info == nullptr )
		return;

	// Frame delta for the release filter, off the same clock everything else runs
	// on -- lastHostTime is already normalised to seconds, so the milliseconds
	// question is settled before it gets here. First frame, or a clock that has
	// not moved, snaps instead.
	const double now = lastHostTime;
	const double dt  = ( audioClock >= 0.0 && now > audioClock ) ? now - audioClock : 0.0;
	audioClock       = now;

	// Fast up, slow down. A flash that arrives a frame late reads as broken,
	// while one that takes ~150 ms to die away reads as intended -- and for a
	// dispersion in particular the asymmetry is what stops the picture juddering
	// between frames on a busy mix.
	const float release = dt > 0.0 ? 1.0f - std::exp( static_cast< float >( -dt / 0.15 ) ) : 1.0f;

	const size_t bins = std::min( info->elements.size(), audioLevel.size() );
	for( size_t i = 0; i < bins; ++i )
	{
		// sqrt because bin magnitudes bunch up near zero: a spectrum used raw
		// reacts to the kick drum and to nothing else.
		const float raw = std::sqrt( std::max( 0.0f, info->elements[ i ].value ) );

		if( raw >= audioLevel[ i ] )
			audioLevel[ i ] = raw;
		else
			audioLevel[ i ] += ( raw - audioLevel[ i ] ) * release;
	}
}

void Abomerration::seedHostSaid()
{
	// Seeded on first parameter traffic rather than in the constructor, so the
	// whole mechanism stays in one place. It has to happen BEFORE applyPreset
	// can run: seeding afterwards would record the preset's own values as the
	// host's opening position, and the host's very next restatement would then
	// look like an edit -- which is the bug this exists to fix, reintroduced.
	if( hostSaidSeeded )
		return;

	for( unsigned int i = 0; i < PT_COUNT; ++i )
		hostSaid[ i ] = params[ i ];

	hostSaidSeeded = true;
}

float Abomerration::presetValue( int presetIndex, unsigned int id ) const
{
	if( presetIndex < 1 || presetIndex > presets::kCount )
		return -1.0f;

	const presets::Preset& preset = presets::kPresets[ presetIndex - 1 ];

	for( int i = 0; i < presets::kParamCount; ++i )
		if( kPresetParamIDs[ i ] == id )
			return preset.v[ i ];

	return -1.0f;
}

bool Abomerration::hostIsRestatingItself( unsigned int index, float value )
{
	const float lastFromHost = hostSaid[ index ];
	hostSaid[ index ]      = value;

	const float fromPreset =
		presetValue( static_cast< int >( std::lround( params[ PT_PRESET ] ) ), index );
	if( fromPreset < 0.0f )
		return false;

	// A quantisation allowance rather than a float epsilon. A host that keeps
	// its parameters shorter than a float -- or round-trips them through a UI,
	// a MIDI value or a saved composition -- hands back a number NEAR ours
	// rather than ours, and 1e-4 read that as an edit.
	constexpr float kSame = 1e-3f;

	if( std::fabs( value - fromPreset ) <= kSame )
	{
		// The host agreeing with the preset. Nothing to write -- and writing it
		// would actively hurt: a host that quantises hands back a ROUNDED copy
		// of our own value, params[] would take the rounding, and the "did a
		// covered parameter move?" test below works to a tighter tolerance than
		// this one and would read that rounding as an edit.
		return true;
	}

	if( std::fabs( value - lastFromHost ) > kSame )
		return false;//neither: the operator has taken over

	// Deliberately not logged. A host that pushes its parameters every frame
	// would put a line here every frame, and a log that scrolls is a log nobody
	// reads. The event worth recording is the fallback to Custom, which
	// happens once.
	return true;
}

FFResult Abomerration::SetFloatParameter( unsigned int index, float value )
{
	if( index >= PT_COUNT )
		return FF_FAIL;

	// An About button is a press, not a value to keep: it opens a browser and
	// nothing about the effect changes. Handled before any of the bookkeeping
	// below, because pressing one is not the operator editing a control.
	if( index >= PT_ABOUT_FIRST )
		return stoatworks::about::handleParam( index - PT_ABOUT_FIRST, value ) ? FF_SUCCESS : FF_FAIL;

	seedHostSaid();

	if( index == PT_PRESET )
	{
		const int chosen = static_cast< int >( std::lround( value ) );
		if( chosen != static_cast< int >( std::lround( params[ PT_PRESET ] ) ) )
			applyPreset( chosen );
		return FF_SUCCESS;
	}

	// The host may be restating a value it still believes in rather than the
	// operator moving anything. Letting that through would overwrite the
	// preset's value in params[] AND read as an edit, dropping the dropdown
	// straight back to Custom -- which is what made presets look like they
	// could not be selected at all.
	if( hostIsRestatingItself( index, value ) )
		return FF_SUCCESS;

	// A slider moved while a preset is active means the operator has taken over:
	// the dropdown falls back to Custom. The tolerance here is deliberately
	// tighter than the quantisation allowance above: a restatement never
	// reaches this point, so anything that does is a real move.
	const float previous = params[ index ];
	params[ index ]      = value;

	const int active = static_cast< int >( std::lround( params[ PT_PRESET ] ) );
	if( active > 0 && std::fabs( value - previous ) > 1e-4f )
	{
		for( unsigned int id : kPresetParamIDs )
		{
			if( id == index )
			{
				params[ PT_PRESET ] = 0.0f;
				RaiseParamEvent( PT_PRESET, FF_EVENT_FLAG_VALUE );
				break;
			}
		}
	}

	return FF_SUCCESS;
}

const unsigned int* Abomerration::PresetParamIDsForTest( int& count )
{
	count = presets::kParamCount;
	return kPresetParamIDs;
}

void Abomerration::applyPreset( int presetIndex )
{
	params[ PT_PRESET ] = static_cast< float >( presetIndex );

	if( presetIndex <= 0 || presetIndex > presets::kCount )
		return;//Custom: the sliders keep whatever they said

	const presets::Preset& preset = presets::kPresets[ presetIndex - 1 ];
	for( int j = 0; j < presets::kParamCount; ++j )
	{
		const unsigned int id = kPresetParamIDs[ j ];
		if( std::fabs( params[ id ] - preset.v[ j ] ) <= 1e-6f )
			continue;

		// The copy is what changes the picture; the event only tells the host to
		// re-read the slider. A host that ignores it renders the preset correctly
		// and merely shows stale knobs.
		//
		// ☠️ `hostSaid[ id ]` is deliberately NOT written here. It records what
		// the HOST last said, and the host has not said anything yet -- it still
		// believes the values from before the preset was chosen. Recording the
		// preset's own values as the host's opening position makes the host's
		// very next restatement of what it believes look like an operator edit,
		// and the dropdown snaps straight back to Custom. `abomtest --hosts`
		// fails in the "ignores" column without this.
		params[ id ] = preset.v[ j ];
		RaiseParamEvent( id, FF_EVENT_FLAG_VALUE );
	}
}

float Abomerration::GetFloatParameter( unsigned int index )
{
	if( index >= PT_COUNT )
		return 0.0f;

	return params[ index ];
}

//---------------------------------------------------------------------------
// What a control means
//---------------------------------------------------------------------------
//
// ☠️ EVERY STRING BELOW MUST RENDER IN 16 CHARACTERS OR FEWER, AT ITS WIDEST
// VALUE. FF_GET_PARAMETER_DISPLAY hands the host a 16-byte buffer -- the SDK's
// own default writes into `static char s_DisplayValue[ 16 ]` -- and Resolume
// copies 16 bytes with no terminator. Nothing plugin-side notices; only the
// operator sees it cut. cogwheel shipped a release reading "0.63% of the she".
char* Abomerration::GetParameterDisplay( unsigned int index )
{
	if( index >= PT_COUNT )
		return nullptr;

	char buffer[ 64 ] = {};
	switch( index )
	{
	case PT_EDGES:
	{
		// Reported as inverted (#4), and it is not -- but nothing in the host
		// said so, and the slider genuinely reads backwards: turning it UP makes
		// the picture calmer. It weights the dispersion by local contrast, and
		// real lateral aberration is invisible in flat areas because displacing
		// a region of constant colour returns the same region. So 0 displaces
		// the WHOLE picture -- the misregistered-camera look, and much the
		// louder of the two -- and 1 confines it to edges, which is the
		// physically honest one. The OFX build has carried that sentence in its
		// parameter description since the beginning; FFGL has nowhere to put a
		// description, so it goes here, in the only sixteen characters there are.
		const float edges = params[ PT_EDGES ];
		if( edges <= 0.0f )
			std::snprintf( buffer, sizeof( buffer ), "whole picture" );//13
		else if( edges >= 1.0f )
			std::snprintf( buffer, sizeof( buffer ), "edges only" );//10
		else
			std::snprintf( buffer, sizeof( buffer ), "%.0f%% to edges", 100.0f * edges );//100% to edges = 13
		break;
	}
	default:
		return PlainDisplay( index );
	}

	displayValue = buffer;
	return const_cast< char* >( displayValue.c_str() );
}

char* Abomerration::PlainDisplay( unsigned int index )
{
	if( index >= PT_COUNT )
		return nullptr;

	const unsigned int type = GetParamType( index );
	if( type == FF_TYPE_TEXT || type == FF_TYPE_FILE )
		return GetTextParameter( index );

	char buffer[ 64 ] = {};
	if( type == FF_TYPE_OPTION )
	{
		// The element's NAME, not its index. An option's display is the one
		// place an operator can check that a dropdown is where they think it
		// is, and a bare "3" is not that.
		const unsigned int element = static_cast< unsigned int >(
			std::max( 0L, std::lround( params[ index ] ) ) );
		const char* name = element < GetNumParamElements( index )
		                     ? GetParamElementName( index, element )
		                     : nullptr;
		std::snprintf( buffer, sizeof( buffer ), "%s", name != nullptr ? name : "?" );
	}
	else if( type == FF_TYPE_BOOLEAN || type == FF_TYPE_EVENT )
	{
		std::snprintf( buffer, sizeof( buffer ), "%s", params[ index ] > 0.5f ? "on" : "off" );
	}
	else if( type == FF_TYPE_INTEGER )
	{
		std::snprintf( buffer, sizeof( buffer ), "%ld", std::lround( params[ index ] ) );
	}
	else
	{
		std::snprintf( buffer, sizeof( buffer ), "%.3f", params[ index ] );
	}

	displayValue = buffer;
	return const_cast< char* >( displayValue.c_str() );
}

char* Abomerration::GetTextParameter( unsigned int index )
{
	// The host is handed a bare pointer, so the string is kept as a member rather
	// than built on the stack here.
	if( index == PT_ABOUT_FIRST )
	{
		aboutText = stoatworks::about::textParam( 0 );
		return const_cast< char* >( aboutText.c_str() );
	}

	return CFFGLPlugin::GetTextParameter( index );
}

FFResult Abomerration::SetTextParameter( unsigned int index, const char* value )
{
	// See the declaration: the base class returns FF_FAIL, and instantiateGL
	// deletes the whole instance when setting any default fails. The About text is
	// display-only, so there is genuinely nothing to store -- but it has to say so
	// successfully.
	(void)value;

	if( index == PT_ABOUT_FIRST )
		return FF_SUCCESS;

	return CFFGLPlugin::SetTextParameter( index, value );
}

void Abomerration::SetClockScaleForTest( double scale )
{
	clockScale = scale;
}
