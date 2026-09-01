#pragma once

#include <FFGLSDK.h>

#include <array>
#include <string>

#include "Controls.h"
#include "Dispersion.h"
#include "Drive.h"
#include "PassBuffer.h"
#include "Presets.h"
#include "StoatworksAboutParams.h"

/**
    Abomerration -- sound-reactive chromatic aberration for Resolume.

    Chromatic aberration is a lens failing to bring every wavelength to the same
    place. Photographers spent a century and a great deal of money getting rid of
    it; this puts it back, wires it to the music, and then keeps going well past
    anything glass could do.

    ------------------------------------------------------------- the one idea

    **A lens does not split a picture into three channels. It smears the whole
    spectrum along a path, and the sensor integrates it.** So this plugin
    displaces the picture once per *wavelength sample* and adds the results up
    through spectral weights -- and the familiar hard red/blue split is not a
    separate mode, it is what the same code does when you ask for three samples,
    because the three-sample weight table is the identity.

    That is why one control spans "cheap channel offset" and "real prismatic
    fringe" with nothing else changing, and why the expensive settings cost
    exactly what they look like they cost. See Dispersion.h.

    ------------------------------------------------------- two halves again

    The field says which way and how far, per pixel (`Dispersion.cpp`, mirrored
    in `Field.cpp`). The weight table says what each sample counts as
    (`Dispersion.cpp`, not mirrored -- it is uploaded as a uniform). Neither
    knows about the other, which is why four geometries times four spectrum
    settings is eight pieces of code rather than sixteen.

    --------------------------------------------------------------- the clock

    Unlike most of the fleet this plugin has two reasons to want time -- the
    turbulence drifts and the beat envelope fires -- and they want it differently.

    **Drift is integrated**, never computed as `time * rate`. The obvious form is
    wrong: rescaling the clock rewrites the whole history, so the noise field
    jumps to a different configuration the instant the control is touched, which
    is precisely when somebody is nudging it and watching.

    **The beat envelope is absolute**, recovered from the host's tempo and bar
    position every frame. The entire point of Beat and Bar mode is that the pulse
    lands on the grid, and an integrated phase drifts off it.

    ---------------------------------------------------- units, and the trap

    **Resolume sends SetTime in MILLISECONDS.** The FFGL header never says, the
    SDK's own Particles sample divides by 1000, and this repo's harness sends
    seconds -- so a plugin that consumes `hostTime` raw runs a thousand times fast
    in a real host, or freezes solid if it is doing beat arithmetic, and no
    offline test can catch it. The unit is detected from the first plausible frame
    delta and normalised; see `clockScale`.

    See Shaders.h for the passes and AGENTS.md for the rest of the traps.
*/
class Abomerration : public CFFGLPlugin
{
public:
	/// Clock test hook. The offline harness DECLARES its unit rather than
	/// leaving the calibration to infer one -- an absolute time handed over in
	/// a single frame is genuinely ambiguous, and an implicit unit is what let
	/// the millisecond bug through in the first place.
	void SetClockScaleForTest( double scale );

	Abomerration();

	//CFFGLPlugin
	FFResult InitGL( const FFGLViewportStruct* vp ) override;
	FFResult ProcessOpenGL( ProcessOpenGLStruct* pGL ) override;
	FFResult DeInitGL() override;

	FFResult SetFloatParameter( unsigned int index, float value ) override;
	float GetFloatParameter( unsigned int index ) override;

	FFResult SetTime( double time ) override;

	char* GetTextParameter( unsigned int index ) override;

	/// What a control MEANS, in the sixteen characters FFGL allows.
	///
	/// Edges is the one that needs it: it reads backwards without a word next to
	/// it, because turning it UP makes the effect subtler rather than stronger.
	/// See the comment on the PT_EDGES case for why that is correct.
	char* GetParameterDisplay( unsigned int index ) override;

	/// The fallback for everything GetParameterDisplay does not word itself.
	///
	/// **Not** a call to `CFFGLPlugin::GetParameterDisplay`: the base class
	/// reaches through `m_pPlugin`, which a harness-constructed instance does
	/// not have, and segfaults. The fleet hit this in cogwheel first.
	char* PlainDisplay( unsigned int index );

	/// Display-only text still needs this.
	///
	/// The SDK's `instantiateGL` sets EVERY parameter's default on a fresh
	/// instance and **deletes the instance if any set returns FF_FAIL** -- and
	/// the base class's SetTextParameter is a stub that returns exactly that. So
	/// declaring the About block without overriding this means no real host can
	/// instantiate the plugin at all, while every harness that drives the plugin
	/// class directly passes, because they bypass plugMain.
	FFResult SetTextParameter( unsigned int index, const char* value ) override;

	/// Everything the operator can reach, in the order Resolume shows it: what
	/// the lens does, what each channel does on top, what the music is allowed to
	/// do to both, and how much of the result to keep.
	///
	/// Public because the harness drives the plugin by parameter id and needs
	/// PT_COUNT to enumerate them.
	///
	/// Nothing here is appended out of place. The fleet's rule is that a
	/// *released* plugin must never renumber its ParamIDs, because saved
	/// compositions refer to them -- so tinsel's Sync and Audio sit awkwardly at
	/// the end. This plugin has never shipped, so its ids are still free to be in
	/// the order the inspector should show them, and PT_AUDIO is where it
	/// belongs. From v0.1.0 onward that freedom is gone.
	enum ParamID : FFUInt32
	{
		//Aberration
		PT_GEOMETRY,
		PT_AMOUNT,
		PT_CENTRE_X,
		PT_CENTRE_Y,
		PT_ANGLE,
		PT_FALLOFF,
		PT_SPECTRUM,
		PT_TURBULENCE,
		PT_DRIFT,

		//Channels
		PT_RED_PUSH,
		PT_GREEN_PUSH,
		PT_BLUE_PUSH,

		//Reaction. PT_AUDIO is an FFT buffer (FF_TYPE_BUFFER, FF_USAGE_FFT):
		//Resolume renders it as an audio-source picker -- Local, Composition or
		//External -- and writes one spectrum bin per element, low frequencies
		//first.
		PT_AUDIO,
		PT_SYNC,
		PT_BEAT_DEPTH,
		PT_BEAT_DECAY,
		PT_BEAT_DIVISION,
		PT_LEVEL_DEPTH,
		PT_BAND_DEPTH,
		PT_ROUTE,

		//Look
		PT_EDGES,
		PT_FRINGE,

		//Output
		PT_SHOW_FIELD,
		PT_MIX,

		//Preset. Declared after the real controls so their IDs -- which a saved
		//composition refers to -- do not shift under existing users.
		PT_PRESET,

		//About. FFGL has no window, so the name, the version and the links are
		//parameters the host draws. See StoatworksAboutParams.h.
		PT_ABOUT_FIRST,
		PT_COUNT = PT_ABOUT_FIRST + stoatworks::about::kParamCount
	};

	/// params[] as the shared control struct, so FFGL and OFX ask the same
	/// question of the same code. Public for the harness.
	abomerration::controls::HostValues hostValues() const;

	/// What the music did on the frame just rendered. Public so the harness can
	/// assert on the reaction without re-deriving it -- and so a failing
	/// assertion is about the plugin's own arithmetic rather than about a second
	/// copy of it in the test.
	///
	/// Zero-valued before the first ProcessOpenGL.
	const abomerration::drive::Output& lastDrive() const
	{
		return driveOut;
	}

	/// The integrated turbulence phase, for the same reason. A drift that does
	/// not advance and a drift that advances wrongly look identical in a single
	/// frame.
	float driftPhaseForTest() const
	{
		return static_cast< float >( driftPhase );
	}

private:
	/// The ParamID each presets::Param drives, in presets::Param order. The
	/// preset table stays host-agnostic; this is the FFGL binding of it.
	static constexpr unsigned int kPresetParamIDs[ abomerration::presets::kParamCount ] = {
		PT_GEOMETRY, PT_AMOUNT, PT_ANGLE, PT_FALLOFF, PT_SPECTRUM, PT_TURBULENCE,
		PT_DRIFT, PT_RED_PUSH, PT_GREEN_PUSH, PT_BLUE_PUSH, PT_BEAT_DEPTH,
		PT_BEAT_DECAY, PT_BEAT_DIVISION, PT_LEVEL_DEPTH, PT_BAND_DEPTH, PT_ROUTE,
		PT_EDGES, PT_FRINGE
	};

	/// Copy a factory preset's values into params[] and raise value events so the
	/// host re-reads the sliders. `presetIndex` is 1-based; 0 is Custom.
	void applyPreset( int presetIndex );

	bool compileShaders();

	/// Fold the host's spectrum buffer into `audioLevel` through an attack and
	/// release filter.
	void updateAudio();

	ffglex::FFGLShader copyShader;
	ffglex::FFGLShader edgeShader;
	ffglex::FFGLShader disperseShader;
	ffglex::FFGLScreenQuad quad;

	/// The picture, as ours, mipmapped. Every wavelength sample reads from here
	/// rather than from the host's texture, at a mip level covering the gap to the
	/// next sample -- which is what stops a sparse quadrature aliasing the
	/// picture. See Copy.cpp for the numbers.
	abomerration::PassBuffer copyBuffer;

	/// Local luminance gradient, for the Edges control. Allocated only when
	/// Edges is above zero, which is why the sampler is bound to the input
	/// otherwise -- see Shaders.h.
	abomerration::PassBuffer edgeBuffer;

	//---------------------------------------------------------------------
	// Time. See the class comment: drift integrates, the beat is absolute, and
	// the host's unit is not knowable in advance.
	//---------------------------------------------------------------------
	double hostTime     = -1.0;
	double lastHostTime = -1.0;
	double driftPhase   = 0.0;

	double clockScale  = 0.0;///< 0 until decided; then 1.0 or 0.001
	double lastWallTime = -1.0;
	double wallStart    = -1.0;
	int secondsVotes    = 0;
	int millisVotes     = 0;
	double lastRawTime = -1.0;

	/// Counts frames so the sixtieth can log what the host's clock actually looks
	/// like. One line, once, in the diag log -- and the only way to find out
	/// after the fact which unit a given host was really sending.
	int clockFrames = 0;

	//---------------------------------------------------------------------
	// Audio.
	//---------------------------------------------------------------------
	std::array< float, abomerration::drive::kAudioBins > audioLevel = {};
	double audioClock = -1.0;

	abomerration::drive::Output driveOut;

	/// Zero-initialised: the constructor writes a default for every real
	/// control, but the About block's ids are never stored to -- pressing a
	/// button opens a browser and returns -- so without this GetFloatParameter
	/// hands the host whatever was on the stack for them.
	float params[ PT_COUNT ] = {};

	/// GetTextParameter hands the host a bare pointer, so the string has to
	/// outlive the call.
	std::string aboutText;

	/// Same reason as aboutText: GetParameterDisplay returns a bare pointer.
	std::string displayValue;
};
