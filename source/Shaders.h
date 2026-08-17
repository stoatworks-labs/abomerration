#pragma once

#include <string>

/**
    The GLSL.

    Abomerration has three passes, and the last one does nearly all of it:

      Copy      The picture, into a buffer of ours, mipmapped. Not an
                optimisation and not tidiness -- Copy.cpp has the measurements
                showing the dispersion aliases its source without it.
      Edge      Luminance gradient magnitude. Only run when the Edges control is
                above zero -- see below, because "only run" has a consequence.
      Disperse  The dispersion field, the spectral integration, the fringe
                boost, the mix, and the Show Field meters. Draws straight to the
                host's framebuffer.

    There is deliberately no composite pass, where the obvious build would have
    had one. The reason is that a composite has nothing to do that the dispersion
    pass does not already have in hand: Mix needs the original picture, the fringe
    boost needs the original picture, and the dispersion pass has the original
    picture bound because it is the thing being dispersed. Splitting them would
    have meant writing a full-resolution intermediate and reading it back for no
    arithmetic that could not happen a line earlier.

    ------------------------------------------------------------- the mirror

    `Field.cpp` carries the same arithmetic as `Dispersion.cpp` -- the hash, the
    noise, and `offsetAt`. It has to: the field is a function of every pixel, so
    the GPU has to evaluate it, while the OFX build and the harness need the C++
    one. Every mirrored block is marked `//= mirrored` in both files.

    What makes the mirror survivable is that it is one string and both consumers
    are assembled around it. `DisperseFragment()` builds the production pass and
    `FieldProbeFragment()` builds the test probe, and both concatenate the
    *same* `kFieldFunctions` -- so `abomtest --field` is not checking a copy of
    the shader that resembles the real one, it is checking the real one. Nothing
    else here is mirrored.

    ------------------------------------------------- the pass that may not run

    The Edge pass is skipped when Edges is 0, which is the default -- so most of
    the time there is no edge texture holding anything meaningful. The
    `EdgeTexture` sampler is still bound, to the input, because a sampler left
    pointing at a deleted texture is undefined behaviour rather than a harmless
    no-op, and `EdgeWeight` being 0 is what stops the shader reading it. Both
    halves of that are load bearing: dropping the bind crashes some drivers, and
    dropping the guard silently weights the dispersion by the input's own red
    channel.
*/
namespace abomerration::shaders
{
/// Shared by both passes: draws the screen quad and scales UVs by MaxUV.
///
/// MaxUV is ALWAYS set to 1 here and the scaling is done at each fetch instead,
/// because the dispersion pass reads a host texture (which may be padded) and
/// one of our own buffers (which is not) in the same program, and a single
/// vertex-stage scale cannot serve both.
extern const char* const kVertex;

extern const char* const kCopyFragment;
extern const char* const kEdgeFragment;

/// The mirrored block: uniforms, hash, noise, and `offsetAt`. Not a complete
/// shader -- no `#version` and no `main` -- because it is concatenated into two
/// of them.
extern const char* const kFieldFunctions;

/// The production dispersion pass.
std::string DisperseFragment();

/// The test probe. Writes the raw picture-space offset into the red and green
/// channels and its magnitude into blue, so `--field` can compare what the GPU
/// computed against `Dispersion.cpp` directly rather than inferring it from a
/// finished picture.
///
/// Inferring it was the alternative and it is a worse test: the spectral
/// integration would sit between the thing under test and the measurement, so a
/// wrong field and a wrong weight table would be indistinguishable.
std::string FieldProbeFragment();

} // namespace abomerration::shaders
