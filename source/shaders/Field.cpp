#include "../Shaders.h"

namespace abomerration::shaders
{
/*
    The dispersion field, in GLSL.

    This is the ONLY mirrored file in the repo: every block marked `//= mirrored`
    below has a twin in Dispersion.cpp, and `abomtest --field` compares them
    across all four geometries at a few thousand points. Change one, change the
    other, run the test.

    --------------------------------------------------------------- the v axis

    Dispersion.cpp works in picture space with **v running down**, because that
    is how an operator reads a frame and how the Centre Y control is labelled: 0
    is the top. GL hands the fragment shader a UV with **v running up**. The flip
    happens once, in each main(), and the returned offset is therefore also in
    picture space -- so `fetchInput` flips it back on the way to a texture
    coordinate, and nothing in between has to think about it.

    Getting this wrong is invisible on a centred radial field and wrong the
    moment anybody drags the centre off the middle.

    -------------------------------------------------------------- the noise

    An integer hash, not `fract( sin( x ) * 43758.5453 )`. The sine trick relies
    on the precision of sin() far outside the range any spec pins down, so it is
    a different function on different drivers -- which for a noise-directed field
    means the picture differs between machines and the field check passes only on
    the machine that wrote it. This hash is a 32-bit integer bijection and one
    divide, and it is bit-identical anywhere with 32-bit unsigned arithmetic.
*/
const char* const kFieldFunctions = R"(
uniform int Geometry;
uniform vec2 Centre;
uniform float Angle;
uniform float Amount;
uniform float Falloff;
uniform float Turbulence;
uniform float Drift;
uniform float FrameAspect;

const float kPi = 3.14159265358979;

//= mirrored: Dispersion.cpp hash()
uint hash( uint x )
{
	x ^= x >> 16;
	x *= 0x7feb352du;
	x ^= x >> 15;
	x *= 0x846ca68bu;
	x ^= x >> 16;
	return x;
}

//= mirrored: Dispersion.cpp hash2()
float hash2( int ix, int iy )
{
	uint h = hash( uint( ix ) * 0x27d4eb2du ^ hash( uint( iy ) ) );
	return float( h ) * ( 1.0 / 4294967296.0 );
}

//= mirrored: Dispersion.cpp noise2D()
float noise2D( float x, float y )
{
	//floor and not a cast: a cast truncates toward zero, so every cell on the
	//negative side of the origin would be twice as wide as the others and the
	//noise would have a seam through the middle of the frame -- which is exactly
	//where the optical centre usually sits, and so exactly where it would be
	//looked at.
	float fx = floor( x );
	float fy = floor( y );

	int ix = int( fx );
	int iy = int( fy );

	float tx = x - fx;
	float ty = y - fy;
	tx = tx * tx * ( 3.0 - 2.0 * tx );
	ty = ty * ty * ( 3.0 - 2.0 * ty );

	float a = hash2( ix, iy );
	float b = hash2( ix + 1, iy );
	float c = hash2( ix, iy + 1 );
	float d = hash2( ix + 1, iy + 1 );

	float top = a + ( b - a ) * tx;
	float bottom = c + ( d - c ) * tx;

	return top + ( bottom - top ) * ty;
}

//= mirrored: Dispersion.cpp offsetAt()
//Takes a point in PICTURE space (v down) and returns the displacement of the far
//end of the spectrum, also in picture space. The near end is its negative.
vec2 offsetAt( vec2 p )
{
	float aspect = FrameAspect > 0.0 ? FrameAspect : 1.0;

	//Frame-height units: x stretched by the aspect ratio here and squashed back
	//at the end, which is what makes an Amount displace by the same visible
	//distance whatever shape the composition is.
	float x = ( p.x - Centre.x ) * aspect;
	float y = ( p.y - Centre.y );

	float r = sqrt( x * x + y * y );

	vec2 dir = vec2( 0.0 );
	float mag = 0.0;

	if( Geometry == 1 )
	{
		//Linear. No r term, and no optical centre at all -- Centre X and Centre
		//Y genuinely do nothing here, which is why the sweep is told so instead
		//of reporting two dead controls.
		dir = vec2( cos( Angle ), sin( Angle ) );
		mag = 1.0;
	}
	else if( Geometry == 2 )
	{
		//Tangential. The guard matters more than in the radial case: a tangent
		//has no limit at r = 0, so an unguarded normalise leaves one pixel of
		//garbage exactly at the optical centre, and that pixel then survives
		//every average anybody takes of the frame.
		if( r > 1e-6 )
			dir = vec2( -y / r, x / r );
		mag = pow( r, Falloff );
	}
	else if( Geometry == 3 )
	{
		//Turbulent. Direction from noise, magnitude constant. Varying both was
		//the obvious thing and it looks worse: magnitude noise leaves flat
		//patches where the effect simply stops, and those read as the plugin
		//failing rather than as texture.
		float n = noise2D( x * Turbulence + Drift, y * Turbulence - Drift * 0.7 );

		float a = n * 2.0 * kPi + Angle;
		dir = vec2( cos( a ), sin( a ) );
		mag = 1.0;
	}
	else
	{
		//Radial.
		if( r > 1e-6 )
			dir = vec2( x / r, y / r );
		mag = pow( r, Falloff );
	}

	float scale = Amount * mag;

	return vec2( dir.x * scale / aspect, dir.y * scale );
}
//= end mirrored
)";
} // namespace abomerration::shaders
