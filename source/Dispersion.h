#pragma once

namespace abomerration
{
/**
    Where each wavelength lands, and what colour it counts as.

    ------------------------------------------------------------- the one idea

    A lens does not split a picture into three channels. It focuses every
    wavelength at a slightly different magnification, and the coloured fringe
    you see at the edge of a frame is the whole visible spectrum smeared along
    that path and then integrated by the sensor.

    So this plugin displaces the picture **once per wavelength sample** and adds
    the results up through spectral weights. The familiar hard red/blue split is
    not a separate mode or a different code path -- it is what this becomes when
    you ask for three samples, because the three-sample weight table is the
    identity. That is the whole reason one control spans "cheap 1990s channel
    offset" and "real prismatic fringe" without touching anything else.

    ---------------------------------------------------------------- two halves

    **The field** says which way and how far, per pixel: `offsetAt()`. Mirrored
    in GLSL, and the mirror is checked -- see below.

    **The weights** say what each sample counts as, per channel: `weights()`.
    NOT mirrored. It is computed here once and uploaded as a uniform array,
    because a table has no reason to exist twice and every mirror is somewhere
    for the two copies to drift apart.

    ------------------------------------------------------------ the one mirror

    `offsetAt()` has to run per pixel, so it exists in GLSL as well. Blocks that
    appear in both are marked `//= mirrored` in both files. Change one, change
    the other, then run `abomtest --field`, which does not compare the two
    sources -- it renders the field through a probe shader assembled from the
    *same string the plugin uses* and compares the pixels the GPU actually
    wrote against this file. A GPU-only defect is a real category (a float `mod`
    that returns its modulus instead of zero on exact multiples is the fleet's
    standing example) and reading the source cannot catch one.

    -------------------------------------------------------------------- units

    Everything is in **frame-height units**: x is multiplied by the aspect ratio
    on the way in and divided out on the way back, so a given Amount displaces
    by the same visible distance on a 16:9 composition and a 4:3 one. Get this
    wrong and it is invisible on a square render, which is why the field checks
    use 320x180.
*/
namespace dispersion
{
/// Which way the spectrum is smeared. The order is the dropdown's, so append
/// only -- an existing composition stores the element index.
enum Geometry
{
	/// Along the radius from the optical centre, growing as r^Falloff. This is
	/// the one a real lens does: lateral chromatic aberration is zero at the
	/// axis and worst in the corners.
	kRadial = 0,

	/// One constant direction everywhere. A prism in front of the lens, or a
	/// three-strip camera out of registration. No r dependence at all, which is
	/// what makes it read as a fault in the *recording* rather than in a lens.
	kLinear,

	/// Perpendicular to the radius, so each wavelength is rotated about the
	/// centre rather than pushed away from it. No lens does this. It reads as
	/// the picture being wrung out, and it is the first geometry that looks
	/// deliberately wrong rather than badly built.
	kTangential,

	/// Direction taken from a smooth noise field that drifts over time. The
	/// abomination the plugin is named for: the fringe wanders across the frame
	/// instead of obeying any optical centre.
	kTurbulent,

	kGeometryCount
};

/// The dispersion field, in physical units. Both builds fill one of these
/// through `controls::field()`.
struct Field
{
	int geometry = kRadial;

	/// The optical centre, in picture space 0..1. v = 0 is the top, matching
	/// the host's own idea of the frame and not GL's.
	float centreU = 0.5f;
	float centreV = 0.5f;

	/// Radians. The direction for kLinear, and a rotation of the noise field's
	/// output for kTurbulent. Unused by kRadial and kTangential, which take
	/// their direction from the geometry.
	float angle = 0.0f;

	/// How far apart the two ends of the spectrum land, in frame-height units,
	/// measured at r = 1. Signed: negative swaps which end of the spectrum goes
	/// which way, which is not the same picture mirrored.
	float amount = 0.0f;

	/// The exponent on r for the two radial geometries. 1 is linear in radius
	/// (what a mild lens looks like), 2 concentrates it in the corners (what a
	/// bad one looks like), below 1 spreads it toward the middle (what no lens
	/// looks like).
	float falloff = 1.0f;

	/// Noise frequency in cycles per frame height, and the phase it has drifted
	/// to. kTurbulent only.
	float turbulence = 0.0f;
	float drift      = 0.0f;

	float aspectRatio = 1.0f;
};

/// The displacement of the far end of the spectrum at one point, in **picture
/// space** -- ready to add to a uv. The near end is the negative of it; a
/// sample partway along the spectrum scales it. Returns through `outU`/`outV`
/// rather than a vector type because this header is shared with GLSL-adjacent
/// code that has no vec2 and with the OFX build that has its own.
void offsetAt( const Field& field, float u, float v, float* outU, float* outV );

/// Value noise, the same hash and the same interpolation as the shader's.
/// Exposed because the field check needs to feed the mirror identical noise,
/// and because a bug in here looks exactly like a bug in the geometry.
float noise2D( float x, float y );

//--------------------------------------------------------------------------
// Spectral weights.
//--------------------------------------------------------------------------

/// The most wavelength samples any Spectrum setting asks for. The shader's
/// uniform array is this long and its loop bound is a uniform, so raising this
/// costs nothing until somebody actually selects a longer setting.
constexpr int kMaxSamples = 32;

/// One wavelength sample: where it lands, and what it counts as.
struct Sample
{
	/// Position along the dispersion path, -1..+1. Zero is the wavelength that
	/// is not displaced at all -- the one the lens is actually focused for.
	float s = 0.0f;

	/// How much this sample contributes to each channel. Normalised so that
	/// every channel's weights sum to exactly 1 across the whole table.
	float r = 0.0f;
	float g = 0.0f;
	float b = 0.0f;
};

/**
    Build the weight table for `count` samples.

    ------------------------------------------------------- why it normalises

    Each channel's weights are scaled to sum to 1. That is not tidiness, it is
    what makes Spectrum a *quality* control instead of a tint control: with the
    weights left raw, moving from 8 samples to 32 changes how much total energy
    each channel collects, so the picture's colour balance shifts and the
    operator reasonably concludes the knob is a colour effect. Normalised, a
    flat field renders as the same flat field at every setting and at every
    Amount, and `abomtest --spectrum` asserts exactly that to within the fp16
    quantum.

    ------------------------------------------------- what the response curve is

    An approximation, and worth being plain about it: the per-channel responses
    are Gaussian bumps centred near each primary's dominant wavelength, not the
    CIE 1931 colour matching functions. It is the standard cheap prism response,
    and for smearing a picture along a line it is indistinguishable from the
    real thing -- but it is not colorimetric and nothing here should be read as
    a claim that it is.

    `count == 3` is a special case and returns the identity: pure red at s=+1,
    pure green at 0, pure blue at -1. Not an approximation of the general path
    but a deliberate exact answer, because that setting exists to reproduce the
    hard-edged channel offset everybody already knows, and a smooth spectrum
    that merely resembles it would be a worse version of a thing that is not
    hard to get exactly right.

    Writes `count` entries and returns how many it wrote (clamped to
    kMaxSamples, and at least 1).
*/
int weights( int count, Sample* out );

} // namespace dispersion
} // namespace abomerration
