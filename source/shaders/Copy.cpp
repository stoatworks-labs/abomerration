#include "../Shaders.h"

namespace abomerration::shaders
{
/*
    The picture, as ours, so it can be mipmapped.

    ------------------------------------------------------------------- why

    Two reasons, and the second is the one that made this pass necessary rather
    than merely tidy.

    **The host's texture cannot be mipmapped.** `glGenerateMipmap` on a texture
    the host owns and reuses would be modifying somebody else's resource behind
    their back, and the dispersion needs mip levels -- see below.

    **A sparse quadrature aliases its source.** The dispersion takes N point
    samples along a path; a path of L pixels sampled N times has a step every L/N
    pixels, and once that gap is wider than a pixel the fringe stops being a
    spectrum and becomes a visible staircase. Measured by `abomtest --banding` on
    a 1280x720 step edge, second-difference energy across the fringe:

        path      Prism 8   Prism 16   Prism 32
         27 px        394        202         46
         65 px        394        376        288
        108 px        382        388        384

    The last row is the tell: at a 108-pixel path all three settings are equally
    bad and no longer even ordered, because every one of them is undersampling.
    Raising the sample count cannot fix it -- one sample per pixel of a 108-pixel
    path is 108 fetches, three times that when the channel pushes differ, and at
    4K that is not a thing anybody can run.

    What fixes it is the prefilter the quadrature is missing: each sample reads
    from the mip level that covers the gap to the next one, so the samples average
    the picture between them instead of skipping it. Same lesson as tilter's blur
    running on a box-downsampled copy, and the same shape of fix.

    ----------------------------------------------------- what else this buys

    Resolving MaxUV here means the copy holds exactly the picture and nothing
    else, so every fetch in the dispersion pass works in plain 0..1 with its own
    half texel -- no host padding to carry through the geometry. The mip chain
    would be wrong otherwise in a way that is very hard to see: level 1 of a
    padded texture averages picture pixels together with undrawn padding, and the
    error is confined to a one-pixel border that grows with each level.
*/
const char* const kCopyFragment = R"(#version 410 core
uniform sampler2D InputTexture;
uniform vec2 SourceMaxUV;
uniform vec2 SourceHalfTexel;

in vec2 uv;
out vec4 fragColor;

void main()
{
	vec2 t = uv * SourceMaxUV;
	fragColor = texture( InputTexture, clamp( t, SourceHalfTexel, SourceMaxUV - SourceHalfTexel ) );
}
)";
} // namespace abomerration::shaders
