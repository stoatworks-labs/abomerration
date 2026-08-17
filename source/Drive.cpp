#include "Drive.h"

#include <algorithm>
#include <cmath>

namespace abomerration
{
namespace drive
{
namespace
{
/// Band edges as fractions of the bin count. Logarithmic rather than equal
/// thirds -- see the header for why equal thirds puts the entire mix in "bass".
///
/// Bass is the bottom sixteenth, mid the next quarter, treble everything above.
/// With 64 bins that is bins 0-3, 4-15 and 16-63, which is about two octaves
/// each if the buffer is linear in frequency.
constexpr float kBassEnd = 1.0f / 16.0f;
constexpr float kMidEnd  = 1.0f / 4.0f;

float meanOver( const float* bins, int from, int to )
{
	if( to <= from )
		return 0.0f;

	float sum = 0.0f;
	for( int i = from; i < to; ++i )
		sum += std::max( 0.0f, bins[ i ] );

	return sum / static_cast< float >( to - from );
}
} // namespace

void bands( const float* bins, int binCount, float* outBass, float* outMid, float* outHigh )
{
	*outBass = 0.0f;
	*outMid  = 0.0f;
	*outHigh = 0.0f;

	if( bins == nullptr || binCount <= 0 )
		return;

	const float n = static_cast< float >( binCount );

	// At least one bin per band however few bins the host supplies, and the
	// edges kept in order, so a host handing over eight bins still gets three
	// distinct bands rather than two empty ones and a mean of everything.
	const int bassEnd = std::clamp( static_cast< int >( std::lround( n * kBassEnd ) ), 1, binCount );
	const int midEnd  = std::clamp( static_cast< int >( std::lround( n * kMidEnd ) ), bassEnd + 1, binCount );

	*outBass = meanOver( bins, 0, bassEnd );
	*outMid  = meanOver( bins, bassEnd, midEnd );
	*outHigh = meanOver( bins, midEnd, binCount );
}

Output compute( const Settings& settings, const Input& in )
{
	Output out;

	bands( in.bins, in.binCount, &out.bass, &out.mid, &out.high );

	// The level is the mean of every bin, not the mean of the three bands: the
	// bands cover wildly different numbers of bins, so averaging them would
	// weight four bass bins the same as forty-eight treble ones and the "level"
	// would follow the kick drum -- which is what Bass Only is for, and is not
	// what an overall level should mean.
	out.level = in.bins != nullptr && in.binCount > 0
	                ? meanOver( in.bins, 0, in.binCount )
	                : 0.0f;

	//---------------------------------------------------------------------
	// The beat envelope.
	//---------------------------------------------------------------------
	if( settings.sync != kSyncFree )
	{
		// The host gives a tempo and a position within the current bar, never
		// which bar it is. Recover a continuous count without keeping any state:
		// the clock estimates how many bars have passed, barPhase is the exact
		// position inside this one, and the whole number reconciling them is
		// round( estimate - barPhase ). Continuous across the bar line, because
		// as barPhase wraps from 1 to 0 the rounded integer steps up at the same
		// instant, and exact for as long as the clock estimate stays within half
		// a bar of the truth. The same recovery the rest of the fleet uses.
		const double tempo      = in.bpm > 1.0f ? static_cast< double >( in.bpm ) : 120.0;
		const double barSeconds = 240.0 / tempo;// four beats to the bar
		const double estimate   = in.seconds / barSeconds;
		const double within     = std::clamp( static_cast< double >( in.barPhase ), 0.0, 1.0 );

		const double bars  = within + std::round( estimate - within );
		const double beats = bars * 4.0;

		//Beats divided by the division, and nothing else. See Drive.h on why there
		//is no second branch here for a Bar mode.
		const double division = std::max( 0.125, static_cast< double >( settings.beatDivision ) );
		const double position = beats / division;

		// Fractional part of the position: 0 at the instant the division lands,
		// approaching 1 just before the next one. std::floor and not a cast --
		// a cast truncates toward zero, so every division before the host's zero
		// point would ramp the wrong way, and a host that reports a negative
		// transport position is a scrub backwards, which is a thing operators do
		// constantly.
		const double frac = position - std::floor( position );

		// Sharp attack on the division, decay across it. The exponent is the
		// Decay control: 1 is a linear ramp, 16 is a click.
		const float decay = std::clamp( settings.beatDecay, 1.0f, 16.0f );
		out.beat          = std::pow( 1.0f - static_cast< float >( frac ), decay );
	}

	//---------------------------------------------------------------------
	// The overall scale.
	//
	// The reactive part is not added on top of the manual amount, it is carved
	// out of it: whatever depth is handed to the music is taken away from the
	// always-on part. So beat depth 1 means silence renders a clean picture and
	// the beat renders the full Amount, which is the effect everybody actually
	// wants from a control called Depth -- while depth 0 leaves scale at exactly
	// 1.0 and the plugin is a manual lens.
	//
	// The depths are clamped as a *sum*. Two sources at 0.8 each would otherwise
	// leave the always-on part at -0.6, and a negative scale flips the direction
	// of the dispersion -- so a plugin set up to react hard to two things at
	// once would invert instead of saturating.
	//---------------------------------------------------------------------
	const float beatDepth  = std::clamp( settings.beatDepth, 0.0f, 1.0f );
	const float levelDepth = std::clamp( settings.levelDepth, 0.0f, 1.0f );
	const float handedOver = std::min( 1.0f, beatDepth + levelDepth );

	out.scale = ( 1.0f - handedOver )
	            + beatDepth * out.beat
	            + levelDepth * std::clamp( out.level, 0.0f, 1.0f );

	//---------------------------------------------------------------------
	// Per-channel push from the bands.
	//---------------------------------------------------------------------
	const float bandDepth = std::clamp( settings.bandDepth, 0.0f, 1.0f );
	if( bandDepth > 0.0f )
	{
		const float bass = std::clamp( out.bass, 0.0f, 1.0f );
		const float mid  = std::clamp( out.mid, 0.0f, 1.0f );
		const float high = std::clamp( out.high, 0.0f, 1.0f );

		switch( settings.route )
		{
			case kRouteInverted:
				out.push[ 0 ] = -bass;
				out.push[ 1 ] = -mid * 0.25f;
				out.push[ 2 ] = +high;
				break;

			case kRouteBass:
				out.push[ 0 ] = bass;
				out.push[ 1 ] = bass;
				out.push[ 2 ] = bass;
				break;

			case kRouteTreble:
				out.push[ 0 ] = high;
				out.push[ 1 ] = high;
				out.push[ 2 ] = high;
				break;

			case kRouteNatural:
			default:
				// Mid gets a quarter of the swing the other two get. Not a
				// fudge: red and blue are the ends of the path and green sits
				// at the middle of it, so pushing green as hard as the other
				// two moves the whole picture rather than spreading it, and the
				// result reads as a wobble instead of an aberration.
				out.push[ 0 ] = +bass;
				out.push[ 1 ] = +mid * 0.25f;
				out.push[ 2 ] = -high;
				break;
		}

		for( float& p : out.push )
			p *= bandDepth;
	}

	return out;
}

} // namespace drive
} // namespace abomerration
