#include "../Shaders.h"

namespace abomerration::shaders
{
/*
    Local luminance gradient, for the Edges control.

    -------------------------------------------------------------------- why

    Real lateral chromatic aberration is invisible in a flat area and obvious at
    a high-contrast edge, and the reason is not subtle: displacing a region of
    constant colour by a few pixels produces the same region of constant colour.
    A plugin that ignores this and disperses everything equally does not look
    like a lens fault, it looks like three misregistered copies of the picture --
    which is a real look, and is what Edges = 0 gives, but it is not the one
    anybody means by "chromatic aberration".

    So Edges weights the displacement by how much is going on locally. At 1 the
    dispersion only happens where there is an edge to fringe.

    -------------------------------------------------------------- Sobel, and why

    A 3x3 Sobel rather than two central differences. Not for accuracy -- for
    smoothness. A two-tap gradient responds to a single pixel, so the weight
    field carries per-pixel detail, and a displacement field with per-pixel
    detail in it produces a picture that shimmers on every frame the source
    moves. Shimmer reads as a bug rather than as an effect, and the fix is to
    have the gradient come from a neighbourhood rather than a pair. Sobel's
    smoothing is along the edge, which is exactly where smoothing is free.

    The magnitude is scaled by 0.25 and clamped. Sobel's raw output for a black
    to white step is 4, so an unscaled magnitude saturates the moment it meets a
    real edge and the control has no useful travel in the middle.
*/
const char* const kEdgeFragment = R"(#version 410 core
uniform sampler2D InputTexture;
uniform vec2 SourceMaxUV;
uniform vec2 SourceHalfTexel;
uniform vec2 SourceTexel;//one input texel, in picture-space units

in vec2 uv;
out vec4 fragColor;

float luma( vec2 p )
{
	vec2 t = p * SourceMaxUV;
	vec3 c = texture( InputTexture, clamp( t, SourceHalfTexel, SourceMaxUV - SourceHalfTexel ) ).rgb;
	//Rec.709. The green weight is most of it, which is the point: a gradient
	//taken on an unweighted mean treats a red-to-blue edge as flat, and those
	//are the edges an operator is most likely to be pointing this at.
	return dot( c, vec3( 0.2126, 0.7152, 0.0722 ) );
}

void main()
{
	vec2 d = SourceTexel;

	float tl = luma( uv + vec2( -d.x, -d.y ) );
	float tc = luma( uv + vec2( 0.0, -d.y ) );
	float tr = luma( uv + vec2( d.x, -d.y ) );
	float ml = luma( uv + vec2( -d.x, 0.0 ) );
	float mr = luma( uv + vec2( d.x, 0.0 ) );
	float bl = luma( uv + vec2( -d.x, d.y ) );
	float bc = luma( uv + vec2( 0.0, d.y ) );
	float br = luma( uv + vec2( d.x, d.y ) );

	float gx = ( tr + 2.0 * mr + br ) - ( tl + 2.0 * ml + bl );
	float gy = ( bl + 2.0 * bc + br ) - ( tl + 2.0 * tc + tr );

	float mag = clamp( sqrt( gx * gx + gy * gy ) * 0.25, 0.0, 1.0 );

	fragColor = vec4( mag, mag, mag, 1.0 );
}
)";
} // namespace abomerration::shaders
