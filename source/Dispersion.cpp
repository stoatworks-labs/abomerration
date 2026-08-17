#include "Dispersion.h"

#include <algorithm>
#include <cmath>

namespace abomerration
{
namespace dispersion
{
namespace
{
constexpr float kPi = 3.14159265358979f;

//= mirrored -- Disperse.cpp hash()
//
// An integer hash, not fract(sin(x)*43758.5453). The sine trick is a different
// function on every driver -- it depends on the precision of sin() outside the
// range anybody sanctions -- so a noise-directed field would drift between
// machines and the field check would pass on the machine that wrote it and
// nowhere else. This is a 32-bit integer bijection followed by one divide, and
// it is bit-identical anywhere that has 32-bit unsigned arithmetic.
unsigned int hash( unsigned int x )
{
	x ^= x >> 16;
	x *= 0x7feb352du;
	x ^= x >> 15;
	x *= 0x846ca68bu;
	x ^= x >> 16;
	return x;
}

//= mirrored -- Disperse.cpp hash2()
float hash2( int ix, int iy )
{
	const unsigned int h = hash( static_cast< unsigned int >( ix ) * 0x27d4eb2du
	                             ^ hash( static_cast< unsigned int >( iy ) ) );
	return static_cast< float >( h ) * ( 1.0f / 4294967296.0f );
}

float smoothstep01( float t )
{
	return t * t * ( 3.0f - 2.0f * t );
}
} // namespace

//= mirrored -- Disperse.cpp noise2D()
float noise2D( float x, float y )
{
	// floor and not a cast: a cast truncates toward zero, so every cell on the
	// negative side of the origin is twice as wide as the others and the noise
	// has a visible seam through the middle of the frame. The centre of the
	// frame is exactly where the optical centre usually is, so the seam lands
	// where it is most likely to be looked at.
	const float fx = std::floor( x );
	const float fy = std::floor( y );

	const int ix = static_cast< int >( fx );
	const int iy = static_cast< int >( fy );

	const float tx = smoothstep01( x - fx );
	const float ty = smoothstep01( y - fy );

	const float a = hash2( ix, iy );
	const float b = hash2( ix + 1, iy );
	const float c = hash2( ix, iy + 1 );
	const float d = hash2( ix + 1, iy + 1 );

	const float top    = a + ( b - a ) * tx;
	const float bottom = c + ( d - c ) * tx;

	return top + ( bottom - top ) * ty;
}

//= mirrored -- Disperse.cpp offsetAt()
void offsetAt( const Field& field, float u, float v, float* outU, float* outV )
{
	// Frame-height units. x is stretched by the aspect ratio here and squashed
	// back at the end, which is what makes an Amount mean the same visible
	// distance whatever shape the composition is.
	const float aspect = field.aspectRatio > 0.0f ? field.aspectRatio : 1.0f;

	const float x = ( u - field.centreU ) * aspect;
	const float y = ( v - field.centreV );

	const float r = std::sqrt( x * x + y * y );

	float dirX = 0.0f;
	float dirY = 0.0f;
	float mag  = 0.0f;

	switch( field.geometry )
	{
		case kLinear:
			// No r term. The whole frame moves the same way by the same amount,
			// which is the only geometry here with no optical centre -- Centre X
			// and Centre Y genuinely do nothing in this mode, and the parameter
			// sweep is told so rather than reporting two dead controls.
			dirX = std::cos( field.angle );
			dirY = std::sin( field.angle );
			mag  = 1.0f;
			break;

		case kTangential:
		{
			// Perpendicular to the radius. Guarded at the centre for the same
			// reason as kRadial, and the guard matters more here: a tangent has
			// no limit at r = 0, so an unguarded normalise puts a single pixel
			// of garbage exactly at the optical centre, which then survives
			// every average anybody takes of the frame.
			if( r > 1e-6f )
			{
				dirX = -y / r;
				dirY = x / r;
			}
			mag = std::pow( r, field.falloff );
			break;
		}

		case kTurbulent:
		{
			// Direction from noise, magnitude constant. Varying both was the
			// obvious thing and it looks worse: the magnitude noise puts flat
			// patches in the frame where the effect simply stops, and a viewer
			// reads those as the plugin failing rather than as texture.
			const float n = noise2D( x * field.turbulence + field.drift,
			                         y * field.turbulence - field.drift * 0.7f );

			const float a = n * 2.0f * kPi + field.angle;
			dirX          = std::cos( a );
			dirY          = std::sin( a );
			mag           = 1.0f;
			break;
		}

		case kRadial:
		default:
			if( r > 1e-6f )
			{
				dirX = x / r;
				dirY = y / r;
			}
			mag = std::pow( r, field.falloff );
			break;
	}

	const float scale = field.amount * mag;

	// Back into picture space, where a uv lives.
	*outU = dirX * scale / aspect;
	*outV = dirY * scale;
}
//= end mirrored

int weights( int count, Sample* out )
{
	const int n = std::clamp( count, 1, kMaxSamples );

	if( n == 1 )
	{
		// Degenerate but reachable: one sample is the undisplaced picture, and
		// it has to be the undisplaced picture exactly, or Amount would tint a
		// setting that by definition cannot disperse anything.
		out[ 0 ] = { 0.0f, 1.0f, 1.0f, 1.0f };
		return 1;
	}

	if( n == 3 )
	{
		// The exact hard split. See the header: this setting exists to reproduce
		// something specific and well known, so it returns that rather than an
		// approximation of it.
		out[ 0 ] = { +1.0f, 1.0f, 0.0f, 0.0f };
		out[ 1 ] = { 0.0f, 0.0f, 1.0f, 0.0f };
		out[ 2 ] = { -1.0f, 0.0f, 0.0f, 1.0f };
		return 3;
	}

	// Wavelength runs 380 nm at one end to 700 nm at the other. Which end is
	// pushed which way is a sign convention and this one matches a real lens:
	// blue is refracted more, so blue lands further from where the lens is
	// focused. s = +1 carries the long wavelengths.
	constexpr float kLambdaMin = 380.0f;
	constexpr float kLambdaMax = 700.0f;

	// Gaussian responses near each primary's dominant wavelength. An
	// approximation of a real observer, not the CIE functions -- see the header.
	struct Response
	{
		float centre;
		float width;
	};
	constexpr Response kResponse[ 3 ] = {
		{ 611.0f, 55.0f },// red
		{ 549.0f, 50.0f },// green
		{ 464.0f, 45.0f },// blue
	};

	float sumR = 0.0f;
	float sumG = 0.0f;
	float sumB = 0.0f;

	for( int i = 0; i < n; ++i )
	{
		const float t      = static_cast< float >( i ) / static_cast< float >( n - 1 );
		const float lambda = kLambdaMin + ( kLambdaMax - kLambdaMin ) * t;

		float rgb[ 3 ];
		for( int c = 0; c < 3; ++c )
		{
			const float d = ( lambda - kResponse[ c ].centre ) / kResponse[ c ].width;
			rgb[ c ]      = std::exp( -0.5f * d * d );
		}

		// t runs 0..1 with the short wavelengths first, so s runs -1..+1 with
		// the long ones at +1.
		out[ i ].s = t * 2.0f - 1.0f;
		out[ i ].r = rgb[ 0 ];
		out[ i ].g = rgb[ 1 ];
		out[ i ].b = rgb[ 2 ];

		sumR += rgb[ 0 ];
		sumG += rgb[ 1 ];
		sumB += rgb[ 2 ];
	}

	// Normalise per channel. This is the line that keeps Spectrum from being a
	// tint control -- see the header, and --spectrum, which fails without it.
	//
	// The sums cannot be zero: every response is a Gaussian evaluated over a
	// range that contains its centre, so each is strictly positive. Guarded
	// anyway, because the alternative to a guard here is a NaN reaching a
	// uniform, and a NaN in a weight table renders the frame black with no
	// message anywhere.
	const float invR = sumR > 1e-8f ? 1.0f / sumR : 0.0f;
	const float invG = sumG > 1e-8f ? 1.0f / sumG : 0.0f;
	const float invB = sumB > 1e-8f ? 1.0f / sumB : 0.0f;

	for( int i = 0; i < n; ++i )
	{
		out[ i ].r *= invR;
		out[ i ].g *= invG;
		out[ i ].b *= invB;
	}

	return n;
}

} // namespace dispersion
} // namespace abomerration
