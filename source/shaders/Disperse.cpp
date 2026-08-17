#include "../Shaders.h"

#include "../Dispersion.h"

#include <string>

namespace abomerration::shaders
{
namespace
{
/// Uniforms and helpers both the production pass and the probe need. Kept apart
/// from the two mains so that neither can drift a declaration.
const char* const kDisperseCommon = R"(
uniform sampler2D InputTexture;
uniform sampler2D EdgeTexture;

uniform vec2 SourceHalfTexel;

uniform float EdgeWeight;

/// Frame height in pixels, and whether to prefilter. Together they turn a
/// displacement in frame-height units into a sample spacing in pixels, which is
/// what decides the mip level -- see `fetchInput`.
uniform float FrameHeightPx;
uniform bool Prefilter;

in vec2 uv;
out vec4 fragColor;

/// Fetch in PICTURE space (v down), at a mip level covering `spacingPx` pixels.
///
/// Two things happen here and both are bugs somebody has already shipped:
///
///   - the v flip, because the field works in picture space and a texture
///     coordinate does not;
///   - the half-texel inset, because GL_LINEAR at the very edge of the picture
///     takes half its weight from outside it.
///
/// There is no MaxUV. This reads our own copy of the picture, which the copy pass
/// already resolved -- and that is what makes the mip chain trustworthy, because
/// mip levels of a padded texture average the picture together with undrawn
/// padding. See Copy.cpp.
vec4 fetchInput( vec2 p, float spacingPx )
{
	vec2 g = vec2( p.x, 1.0 - p.y );
	vec2 t = clamp( g, SourceHalfTexel, vec2( 1.0 ) - SourceHalfTexel );

	//TWICE the gap between neighbouring samples, not the gap itself.
	//
	//Sampling theory, and the factor of two is not a fudge. Samples spaced d apart
	//can only carry frequencies below the Nyquist limit 1/(2d), so the prefilter
	//has to reach that far. A box filter of width d has its first zero at 1/d --
	//an octave too high, so it passes everything between 1/(2d) and 1/d straight
	//into the sum, where it folds down as ripple. A box of width 2d puts its first
	//zero exactly on Nyquist.
	//
	//The first version of this used the gap itself and measurably under-filtered.
	//`abomtest --quadrature` on a hard step edge, worst ripple of 255 for Prism
	//8 / 16 / 32:
	//
	//    width d   16.6   8.7   1.7
	//    width 2d   4.8   2.0   1.2
	//
	//The cost is one extra octave of softening at a given sample count, which is
	//exactly the trade the Spectrum control exists to let somebody make.
	//
	//log2 because that is what a mip level is: level 0 covers one pixel, level 1
	//covers two. The max() keeps it off negative levels, which would be a request
	//to magnify.
	float lod = Prefilter ? log2( max( spacingPx * 2.0, 1.0 ) ) : 0.0;

	return textureLod( InputTexture, t, lod );
}

/// The dispersion at this pixel, after the edge weighting. Both mains want the
/// same answer, and --field would be checking the wrong number if the probe
/// skipped the weighting the picture gets.
vec2 dispersionAt( vec2 pic )
{
	vec2 offset = offsetAt( pic );

	if( EdgeWeight > 0.0 )
	{
		//Our own buffer: no padding, and v is GL's here because nothing wrote it
		//in picture space.
		float e = texture( EdgeTexture, uv ).r;
		offset *= mix( 1.0, e, EdgeWeight );
	}

	return offset;
}
)";

/// The production main.
const char* const kDisperseMain = R"(
uniform int SampleCount;
uniform vec4 Samples[ MAX_SAMPLES ];//s, then the r/g/b weights
uniform vec3 Push;
uniform bool UniformPush;
uniform float Fringe;
uniform float MixAmount;
uniform bool ShowField;

//Show Field only.
uniform float AmountRef;
uniform vec4 Meters;//bass, mid, high, beat

void main()
{
	vec2 pic = vec2( uv.x, 1.0 - uv.y );

	vec2 offset = dispersionAt( pic );

	//Spacing between neighbouring wavelength samples, in pixels, at THIS pixel.
	//Per pixel and not per frame on purpose: a radial field is nearly still in
	//the middle of the frame and largest in the corners, so a single spacing for
	//the whole draw would prefilter the middle for a displacement it does not
	//have and soften a picture that should be sharp there.
	//
	//length(offset) is in picture units, where v spans the frame height -- so
	//multiplying by the frame height in pixels gives the path length in pixels
	//regardless of the composition's shape.
	float spacingPx = SampleCount > 1
	                  ? length( offset ) * FrameHeightPx / float( SampleCount - 1 )
	                  : 0.0;

	//Level 0, always. The undispersed picture is what Mix blends back to and what
	//the fringe boost measures against; prefiltering it would soften the dry
	//signal and make Mix at 0 differ from bypass.
	vec4 original = fetchInput( pic, 0.0 );

	vec3 colour = vec3( 0.0 );
	float alpha = 0.0;

	if( UniformPush )
	{
		//One fetch per sample, all three channels taken from it. This is the
		//common case -- the channel trims default to centred and the bands
		//default to off -- and it is three times cheaper than the branch below.
		//UniformPush is a uniform, so the branch is coherent across the whole
		//draw and costs nothing in divergence.
		float p = Push.r;

		for( int i = 0; i < SampleCount; ++i )
		{
			vec4 s = Samples[ i ];
			vec4 c = fetchInput( pic + offset * ( s.x + p ) * 0.5, spacingPx );

			colour += c.rgb * s.yzw;
			//Alpha is achromatic, so it takes the achromatic response: the mean
			//of the three weights. That mean sums to exactly 1 over the table,
			//because each channel's weights do, so a fully opaque picture stays
			//fully opaque however the spectrum is set.
			alpha += c.a * ( s.y + s.z + s.w ) * ( 1.0 / 3.0 );
		}
	}
	else
	{
		for( int i = 0; i < SampleCount; ++i )
		{
			vec4 s = Samples[ i ];

			vec4 cr = fetchInput( pic + offset * ( s.x + Push.r ) * 0.5, spacingPx );
			vec4 cg = fetchInput( pic + offset * ( s.x + Push.g ) * 0.5, spacingPx );
			vec4 cb = fetchInput( pic + offset * ( s.x + Push.b ) * 0.5, spacingPx );

			colour += vec3( cr.r * s.y, cg.g * s.z, cb.b * s.w );
			alpha += ( cr.a * s.y + cg.a * s.z + cb.a * s.w ) * ( 1.0 / 3.0 );
		}
	}

	//The fringe boost pushes what the dispersion already separated further apart
	//without moving anything: it is the difference from the undispersed picture,
	//amplified. So it cannot invent a fringe where there is no dispersion, which
	//is the property that makes it safe to leave up while Amount is automated.
	if( Fringe > 0.0 )
		colour += ( colour - original.rgb ) * Fringe;

	vec4 result = vec4( colour, alpha );

	if( ShowField )
	{
		//A dim monochrome picture with the dispersion magnitude painted over it.
		//Deliberately does NOT show direction: direction is legible from the
		//effect itself, magnitude is the thing an operator cannot see -- because
		//a flat region with an enormous displacement looks exactly like a flat
		//region with none, which is the whole reason Edges exists and the single
		//most confusing thing about setting this plugin up.
		float grey = dot( original.rgb, vec3( 0.2126, 0.7152, 0.0722 ) ) * 0.25;

		float norm = clamp( length( offset ) / max( AmountRef, 1e-6 ), 0.0, 1.0 );

		//Blue - cyan - yellow - red. Monotonic in brightness as well as hue, so
		//it survives being looked at on a badly set up monitor.
		vec3 ramp = clamp( vec3( norm * 2.0 - 0.6, 1.0 - abs( norm - 0.5 ) * 2.2, 1.0 - norm * 2.2 ),
		                   vec3( 0.0 ), vec3( 1.0 ) );

		result = vec4( vec3( grey ) + ramp * 0.85, 1.0 );

		//Four meters along the bottom: bass, mid, treble, beat. This is the only
		//place in Resolume an operator can find out whether the plugin is
		//hearing anything at all -- with no audio routed the picture simply does
		//not move, which is indistinguishable from a depth set to zero, a route
		//pointing at a silent band, or a host that never sent a buffer.
		if( pic.y > 0.90 && pic.x > 0.02 && pic.x < 0.42 )
		{
			float slot = ( pic.x - 0.02 ) / 0.10;
			int which = int( slot );

			//A gap between the bars so four meters read as four and not as one
			//wide graph with steps in it.
			if( fract( slot ) < 0.8 && which >= 0 && which < 4 )
			{
				float value = which == 0 ? Meters.x : ( which == 1 ? Meters.y : ( which == 2 ? Meters.z : Meters.w ) );
				float fill = ( 0.98 - pic.y ) / 0.08;

				vec3 bar = which == 3 ? vec3( 1.0, 1.0, 1.0 ) : vec3( 0.2, 1.0, 0.4 );
				result = vec4( fill < clamp( value, 0.0, 1.0 ) ? bar : vec3( 0.10 ), 1.0 );
			}
		}
	}
	else
	{
		result = mix( original, result, MixAmount );
	}

	fragColor = result;
}
)";

/// The probe main. Writes the field itself, so a test can compare it against
/// Dispersion.cpp without the spectral integration in the way.
const char* const kFieldProbeMain = R"(
void main()
{
	vec2 pic = vec2( uv.x, 1.0 - uv.y );

	vec2 offset = dispersionAt( pic );

	//Picture space, unscaled and unbiased. The buffer this lands in is RGBA32F
	//precisely so that no encoding sits between the shader and the comparison --
	//a signed field packed into 0..1 would make the test's tolerance a statement
	//about the packing rather than about the arithmetic.
	fragColor = vec4( offset.x, offset.y, length( offset ), 1.0 );
}
)";

std::string header()
{
	return std::string( "#version 410 core\n#define MAX_SAMPLES " )
	       + std::to_string( dispersion::kMaxSamples ) + "\n";
}
} // namespace

std::string DisperseFragment()
{
	return header() + kFieldFunctions + kDisperseCommon + kDisperseMain;
}

std::string FieldProbeFragment()
{
	return header() + kFieldFunctions + kDisperseCommon + kFieldProbeMain;
}

} // namespace abomerration::shaders
