#pragma once

#include "Dispersion.h"
#include "Drive.h"

namespace abomerration
{
/**
    The one place a slider position becomes a physical quantity.

    -------------------------------------------------------------------- why

    Two reasons, and the second is the one that bites.

    **A ranged FF_TYPE_STANDARD parameter cannot have a ranged default.** The
    SDK's `SetParamInfo` clamps a default into 0..1 *before* returning, and
    `SetParamRange` can only be called afterwards because it finds the parameter
    by id. There is no `SetParamDefault`. So a control declared in degrees
    cannot declare a default in degrees -- 90 silently becomes 1. Every standard
    parameter here therefore lives in 0..1 and is converted on the way through,
    which is this file.

    **FFGL and OFX must agree.** They expose the same 0..1 controls and the same
    factory presets, so if the conversion lived in each host's glue there would
    be two copies of every curve and a preset would mean something slightly
    different in Resolume and in Resolve. Both builds fill a `HostValues` and
    ask here.

    ------------------------------------------------------------- the curves

    Anything that is a ratio -- the falloff exponent, the noise frequency --
    converts exponentially, so half a slider is unity and equal distances either
    side are reciprocal factors. Anything that is a position or an amount
    converts linearly. Anything centred on "no change" puts that at 0.5, which
    is why the three channel trims default there and not to zero.
*/
namespace controls
{
/// The controls exactly as the host holds them: every one 0..1, option
/// parameters holding their element index. Both builds fill this.
struct HostValues
{
	//Aberration
	float geometry   = 0.0f;
	float amount     = 0.28f;
	float centreX    = 0.5f;
	float centreY    = 0.5f;
	float angle      = 0.5f;
	float falloff    = 0.5f;
	float spectrum   = 1.0f;
	float turbulence = 0.35f;
	float drift      = 0.4f;

	//Channels
	float redPush   = 0.5f;
	float greenPush = 0.5f;
	float bluePush  = 0.5f;

	//Reaction. Every depth starts at zero: dropped on a layer with nothing
	//routed, this is a manual lens and looks like one.
	float sync         = 0.0f;
	float beatDepth    = 0.0f;
	float beatDecay    = 0.45f;
	//Element 2, "Beat" -- NOT element 0, which is a quarter beat. This defaulted
	//to 0 and the pulse fired four times per beat out of the box, which reads as
	//a twitchy effect rather than as a wrong default. Found by rendering the
	//project video against a static frame: the reaction repeated every 3.75
	//frames at 30fps where 120 bpm demands 15, and every preset in the table
	//says 2.0 because a beat is what anybody means.
	float beatDivision = 2.0f;
	float levelDepth   = 0.0f;
	float bandDepth    = 0.0f;
	float route        = 0.0f;

	//Look
	float edges  = 0.0f;
	float fringe = 0.0f;

	//Output
	float showField = 0.0f;
	float mix       = 1.0f;
};

/// Sampling density along the spectrum. The order is the dropdown's, so append
/// only.
enum SpectrumMode
{
	/// Three samples, and the weight table is the identity -- the hard channel
	/// offset everybody already knows. Cheapest, and not merely a low-quality
	/// version of the others: it is a different, specific look.
	kSpectrumSplit = 0,
	kSpectrumPrism8,
	kSpectrumPrism16,
	kSpectrumPrism32,
	kSpectrumCount
};

/// Everything the render needs that is not the dispersion field.
struct Look
{
	/// How many wavelength samples, and their weights. Built here so the shader
	/// receives a table and never computes one.
	int sampleCount = 3;
	dispersion::Sample samples[ dispersion::kMaxSamples ] = {};

	/// Extra per-channel displacement, in spectral-path units. The manual trims
	/// and the band drive summed -- the shader cannot tell which is which, and
	/// does not need to.
	float push[ 3 ] = { 0.0f, 0.0f, 0.0f };

	/// True when all three pushes are equal, which lets the shader take one
	/// fetch per sample instead of three. A uniform branch, so it costs nothing
	/// in divergence -- and it is the common case, because the default trims are
	/// all centred and the bands default to off.
	bool uniformPush = true;

	float edges  = 0.0f;
	float fringe = 0.0f;

	/// Whether each wavelength sample should read from a mip level covering the
	/// gap to its neighbour. True for every Prism setting and FALSE for RGB
	/// Split, which exists to produce three hard-edged copies -- prefiltering
	/// those would soften the one thing that setting is for. See Copy.cpp.
	bool prefilter = true;

	bool showField = false;
	float mix      = 1.0f;
};

/// Element labels for the host's dropdowns.
int geometryCount();
const char* geometryLabel( int index );

int spectrumCount();
const char* spectrumLabel( int index );
/// Wavelength sample count for a Spectrum dropdown index.
int spectrumSamples( int index );

int syncCount();
const char* syncLabel( int index );

int routeCount();
const char* routeLabel( int index );

int divisionCount();
const char* divisionLabel( int index );
/// Division in beats for a Division dropdown index.
float divisionValue( int index );

/// Read an option parameter. Option parameters hold the element value the
/// operator chose -- 0, 1, 2 -- not a 0..1 fraction, so they are rounded and
/// clamped rather than scaled. A stale composition naming an element that no
/// longer exists is why it clamps.
int option( float value, int elementCount );

/// The reactive settings these controls describe.
drive::Settings driveSettings( const HostValues& host );

/// The dispersion field these controls describe. `driveScale` is
/// `drive::Output::scale`; pass 1.0f for the manual lens.
///
/// `driftPhase` is the *already integrated* turbulence phase. It is not derived
/// from the clock here on purpose: integrating a rate and rescaling a clock are
/// different things, and `time * speed` means moving the Drift control rewrites
/// the whole history so the noise field jumps -- at exactly the moment somebody
/// is nudging the control and watching it.
dispersion::Field field( const HostValues& host, float aspectRatio, float driveScale, float driftPhase );

/// The look these controls describe. `drivePush` is `drive::Output::push`.
Look look( const HostValues& host, const float* drivePush );

/// Turbulence phase advance for one frame, in noise units. The caller
/// integrates it; see `field()`.
float driftRate( const HostValues& host );

} // namespace controls
} // namespace abomerration
