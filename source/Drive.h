#pragma once

namespace abomerration
{
/**
    What the music does to the lens.

    Everything reactive in this plugin is decided here, in plain C++ with no GL
    and no host in sight: bins and a tempo go in, a handful of scalars come out,
    and the render multiplies by them. That is deliberate. Reaction is the part
    of an audio-driven effect that is hardest to judge by eye -- "it moves when
    the music moves" is true of almost any wrong answer -- so it is kept where a
    test can state what it should be and check that it is.

    ----------------------------------------------------------- opt in, always

    Every source defaults to depth zero, and at depth zero the output is exactly
    1.0 with no per-channel push. Dropped on a layer with no audio routed and
    nothing touched, this plugin is an ordinary manual chromatic aberration lens
    and behaves like one. That is not a fallback for when the audio is missing,
    it is the intended way to use it half the time.

    -------------------------------------------------------------- FFGL only

    Beat information and the FFT buffer are FFGL features. OFX has neither, so
    the OFX build fills a DriveInput with zeros and gets the manual lens -- the
    same code, the same arithmetic, no second implementation to keep in step.
*/
namespace drive
{
/// Spectrum bins the plugin asks the host for. Nothing on the GPU has a matching
/// array, and deliberately so: the drive collapses the spectrum to three band
/// energies and a level here, on the CPU, so the bins never reach a shader and
/// there is no uniform array length to keep in step with this number.
constexpr int kAudioBins = 64;

/// How the host's transport is being followed.
enum Sync
{
	/// No grid. The beat envelope is flat zero in this mode, so Beat Depth
	/// genuinely does nothing -- which is why tools/sweep.py sweeps it from a
	/// context that has already left Free.
	kSyncFree = 0,

	/// Locked to the host's transport. Which division it fires on is the Division
	/// control's job, not this one's.
	///
	/// There were three modes here -- Free, Beat and Bar -- for about an hour,
	/// copying the shape the rest of the fleet uses. It was wrong twice over.
	/// Elsewhere Sync picks the UNIT a Speed control is measured in, so Beat and
	/// Bar are genuinely different; here there is an explicit Division dropdown
	/// spanning a quarter beat to two bars, which already says everything Bar
	/// would have said. And because Bar's arithmetic worked out to `bars * 4`,
	/// which is exactly the beat count, the two options were byte-identical code
	/// paths -- two dropdown entries that rendered the same picture. Found by
	/// tools/sweep.py, which reported Sync dead and was right.
	kSyncLocked,

	kSyncCount
};

/// Which band pushes which channel. Bands are additive on top of the spectral
/// dispersion, so a route is a set of three signed gains.
enum Route
{
	/// Bass pushes red one way, treble pushes blue the other, mid stays near
	/// the middle. The spectrum ends up spread the same way a lens spreads it,
	/// which is why this one still reads as a lens when it is working hard.
	kRouteNatural = 0,

	/// The same, reversed. Reads as wrong on purpose: the fringe moves against
	/// the picture instead of with it.
	kRouteInverted,

	/// Bass alone, all three channels together. No colour separation from the
	/// bands at all -- it pumps the whole dispersion instead, which is the one
	/// route that stays legible on a busy mix.
	kRouteBass,

	/// Treble alone, all three channels. Hi-hats and cymbals only. Nearly
	/// silent on a bass-heavy track, and that is the point of having it.
	kRouteTreble,

	kRouteCount
};

/// The reactive controls, in physical units.
struct Settings
{
	int sync  = kSyncFree;
	int route = kRouteNatural;

	/// 0..1 each. The sum of beat and level depth is how much of the dispersion
	/// is handed over to the music; what is left is always on. See `scale`.
	float beatDepth  = 0.0f;
	float levelDepth = 0.0f;
	float bandDepth  = 0.0f;

	/// Exponent on the beat envelope's decay, 1..16. 1 is a linear ramp down
	/// over the whole beat, 16 is a click.
	float beatDecay = 4.0f;

	/// Musical division the beat envelope fires on, in beats. 1 is every beat,
	/// 4 is every bar.
	float beatDivision = 1.0f;
};

/// Everything the drive reads from the host this frame.
struct Input
{
	/// Smoothed spectrum, low frequencies first. Null or zero-length is a host
	/// with no audio routed, which is not an error.
	const float* bins = nullptr;
	int binCount      = 0;

	float bpm      = 120.0f;
	float barPhase = 0.0f;

	/// The host clock, already normalised to seconds. See Abomerration.h for why
	/// that normalisation is not something a plugin can skip.
	double seconds = 0.0;
};

/// What the render multiplies by.
struct Output
{
	/// Multiplies the Amount control. 1.0 when nothing is reacting.
	float scale = 1.0f;

	/// Extra displacement per channel, in the same -1..+1 units as a spectral
	/// sample's position. Zero when the bands are not driving.
	float push[ 3 ] = { 0.0f, 0.0f, 0.0f };

	/// The three band energies and the overall level, 0..1. Outputs rather than
	/// internals because the harness asserts on them directly and because Show
	/// Field draws them, which makes an operator's "is it even hearing
	/// anything?" answerable without leaving Resolume.
	float bass  = 0.0f;
	float mid   = 0.0f;
	float high  = 0.0f;
	float level = 0.0f;

	/// The beat envelope, 0..1. Zero in Free mode.
	float beat = 0.0f;
};

/**
    Split the spectrum into three bands.

    ------------------------------------------------------------------ the trap

    Resolume does not document what frequency range its FFT buffer spans, and
    the obvious split -- a third of the bins each -- is wrong for any buffer
    that is linear in frequency. With 64 linear bins over a normal sample rate,
    one bin is several hundred hertz: a "bass third" then reaches past 7 kHz, so
    everything anybody would call music lands in it and the other two bands sit
    at nearly zero all night. The effect looks like it is only hearing the kick
    drum, and the natural conclusion is that the FFT is broken rather than that
    the arithmetic dividing it up is.

    So the split is logarithmic in bin index -- roughly two octaves per band --
    which is right for a linear-in-frequency buffer and merely differently
    weighted for an already-log-spaced one. Stated as an assumption because that
    is what it is: the host's actual mapping is undocumented, the Band Route
    control is how an operator compensates if a given host disagrees, and none
    of this is a claim about Resolume's internals.

    Each band is a mean over its bins, not a peak, so one loud bin cannot carry
    a band on its own.
*/
void bands( const float* bins, int binCount, float* outBass, float* outMid, float* outHigh );

/// Fold the frame's audio and transport into the scalars the render wants.
Output compute( const Settings& settings, const Input& in );

} // namespace drive
} // namespace abomerration
