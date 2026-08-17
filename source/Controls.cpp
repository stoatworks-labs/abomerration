#include "Controls.h"

#include <algorithm>
#include <cmath>

namespace abomerration
{
namespace controls
{
namespace
{
constexpr float kPi = 3.14159265358979f;

float lerp( float a, float b, float t )
{
	return a + ( b - a ) * t;
}

/// A ratio control: 0.5 is unity, and equal distances either side are
/// reciprocal factors of each other. `span` is the factor at the top end.
float ratio( float x, float span )
{
	return std::exp( ( x - 0.5f ) * 2.0f * std::log( span ) );
}

const char* const kGeometryLabels[ dispersion::kGeometryCount ] = {
	"Radial",
	"Linear",
	"Tangential",
	"Turbulent"
};

struct SpectrumStep
{
	const char* label;
	int samples;
};

/// The sample counts are powers of two apart because the cost is linear in them
/// and anything finer would be a control whose steps nobody can see. 32 is the
/// ceiling because the fringe is already smooth there: past it the picture stops
/// changing and only the frame time keeps going up, which is a bad trade to
/// offer somebody in a dropdown.
const SpectrumStep kSpectrum[ kSpectrumCount ] = {
	{ "RGB Split", 3 },
	{ "Prism 8", 8 },
	{ "Prism 16", 16 },
	{ "Prism 32", 32 },
};

const char* const kSyncLabels[ drive::kSyncCount ] = {
	"Free",
	"Locked"
};

const char* const kRouteLabels[ drive::kRouteCount ] = {
	"Natural",
	"Inverted",
	"Bass Only",
	"Treble Only"
};

struct DivisionStep
{
	const char* label;
	float beats;
};

const DivisionStep kDivisions[] = {
	{ "1/4 Beat", 0.25f },
	{ "1/2 Beat", 0.5f },
	{ "Beat", 1.0f },
	{ "2 Beats", 2.0f },
	{ "Bar", 4.0f },
	{ "2 Bars", 8.0f },
};

constexpr int kDivisionCount = int( sizeof( kDivisions ) / sizeof( kDivisions[ 0 ] ) );
} // namespace

int geometryCount()
{
	return dispersion::kGeometryCount;
}

const char* geometryLabel( int index )
{
	if( index < 0 || index >= dispersion::kGeometryCount )
		return kGeometryLabels[ 0 ];
	return kGeometryLabels[ index ];
}

int spectrumCount()
{
	return kSpectrumCount;
}

const char* spectrumLabel( int index )
{
	if( index < 0 || index >= kSpectrumCount )
		return kSpectrum[ 0 ].label;
	return kSpectrum[ index ].label;
}

int spectrumSamples( int index )
{
	if( index < 0 || index >= kSpectrumCount )
		return kSpectrum[ 0 ].samples;
	return kSpectrum[ index ].samples;
}

int syncCount()
{
	return drive::kSyncCount;
}

const char* syncLabel( int index )
{
	if( index < 0 || index >= drive::kSyncCount )
		return kSyncLabels[ 0 ];
	return kSyncLabels[ index ];
}

int routeCount()
{
	return drive::kRouteCount;
}

const char* routeLabel( int index )
{
	if( index < 0 || index >= drive::kRouteCount )
		return kRouteLabels[ 0 ];
	return kRouteLabels[ index ];
}

int divisionCount()
{
	return kDivisionCount;
}

const char* divisionLabel( int index )
{
	if( index < 0 || index >= kDivisionCount )
		return kDivisions[ 2 ].label;
	return kDivisions[ index ].label;
}

float divisionValue( int index )
{
	if( index < 0 || index >= kDivisionCount )
		return 1.0f;
	return kDivisions[ index ].beats;
}

int option( float value, int elementCount )
{
	const int chosen = static_cast< int >( std::lround( value ) );
	return std::clamp( chosen, 0, std::max( 0, elementCount - 1 ) );
}

drive::Settings driveSettings( const HostValues& host )
{
	drive::Settings out;

	out.sync  = option( host.sync, drive::kSyncCount );
	out.route = option( host.route, drive::kRouteCount );

	out.beatDepth  = host.beatDepth;
	out.levelDepth = host.levelDepth;
	out.bandDepth  = host.bandDepth;

	//1 is a linear ramp down across the whole division, 16 is a click. Linear
	//in the exponent rather than a ratio curve: the visible difference between
	//1 and 2 is enormous and between 12 and 16 is nearly nothing, so an
	//exponential curve here would spend most of the slider on the end nobody
	//can distinguish.
	out.beatDecay = lerp( 1.0f, 16.0f, std::clamp( host.beatDecay, 0.0f, 1.0f ) );

	out.beatDivision = divisionValue( option( host.beatDivision, kDivisionCount ) );

	return out;
}

dispersion::Field field( const HostValues& host, float aspectRatio, float driveScale, float driftPhase )
{
	dispersion::Field out;

	out.geometry = option( host.geometry, dispersion::kGeometryCount );

	out.centreU = host.centreX;
	out.centreV = host.centreY;

	//A full turn, unlike a symmetric band's half turn: a dispersion path is not
	//its own mirror image, because the two ends of it are different colours.
	//Reversing the direction swaps which end of the spectrum goes which way and
	//that is a visibly different picture, so all 360 degrees are distinct and a
	//sweep from one end of the slider to the other never repeats itself.
	out.angle = ( host.angle - 0.5f ) * 2.0f * kPi;

	//Fifteen per cent of the frame height between the ends of the spectrum is
	//far past anything optical -- a real lens is a fraction of a per cent -- and
	//that is the point of the top of the range. The bottom is zero, not a small
	//number, so Amount at 0 is genuinely bypassed.
	out.amount = host.amount * 0.15f * driveScale;

	//1 is linear in radius, 2 concentrates it in the corners, and the range
	//reaches 0.25 at the bottom so the effect can be made to work in the middle
	//of the frame, which no lens does and which is exactly why it is available.
	out.falloff = ratio( host.falloff, 4.0f );

	//Cycles per frame height. Two is a couple of big lobes across the picture;
	//twenty is a fine churn. Below one the whole frame is inside a single noise
	//cell and the geometry stops looking turbulent at all, so the range starts
	//at one.
	out.turbulence = lerp( 1.0f, 20.0f, std::clamp( host.turbulence, 0.0f, 1.0f ) );
	out.drift      = driftPhase;

	out.aspectRatio = aspectRatio;

	return out;
}

float driftRate( const HostValues& host )
{
	//Noise units per second. Zero at the bottom of the slider means a frozen
	//field, which is worth having: a static turbulent field is a fixed lens
	//fault, and an operator who wants that should not have to fight an animation
	//they cannot switch off.
	return std::clamp( host.drift, 0.0f, 1.0f ) * 2.0f;
}

Look look( const HostValues& host, const float* drivePush )
{
	Look out;

	const int mode  = option( host.spectrum, kSpectrumCount );
	out.sampleCount = dispersion::weights( spectrumSamples( mode ), out.samples );

	//RGB Split is three hard copies by design, so it never prefilters. Every
	//Prism setting does -- see Copy.cpp for what happens without it.
	out.prefilter = mode != kSpectrumSplit;

	//The manual trims are centred controls: 0.5 is no extra push. They reach
	//half the spectral path either way, which is enough to put a channel well
	//outside the spread the Spectrum setting produced -- the point of having
	//them is to break the physical relationship, not to fine-tune it.
	const float manual[ 3 ] = {
		( host.redPush - 0.5f ) * 2.0f,
		( host.greenPush - 0.5f ) * 2.0f,
		( host.bluePush - 0.5f ) * 2.0f,
	};

	for( int c = 0; c < 3; ++c )
		out.push[ c ] = manual[ c ] + ( drivePush != nullptr ? drivePush[ c ] : 0.0f );

	//Exact comparison, deliberately. This decides which of two code paths the
	//shader takes, and both paths compute the same thing -- so the only cost of
	//being wrong by a float epsilon is one wasted fetch per sample, while a
	//tolerance would need a justification for its size that nothing here can
	//supply.
	out.uniformPush = ( out.push[ 0 ] == out.push[ 1 ] ) && ( out.push[ 1 ] == out.push[ 2 ] );

	out.edges  = std::clamp( host.edges, 0.0f, 1.0f );
	out.fringe = std::clamp( host.fringe, 0.0f, 1.0f ) * 3.0f;

	out.showField = host.showField >= 0.5f;
	out.mix       = std::clamp( host.mix, 0.0f, 1.0f );

	return out;
}

} // namespace controls
} // namespace abomerration
