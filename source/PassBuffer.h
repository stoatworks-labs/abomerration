#pragma once

#include <FFGLSDK.h>

namespace abomerration
{
/**
    An off-screen buffer for one stage of the chain.

    Two things on top of the SDK's FFGLFBO.

    **It reallocates only when it has to.** `Ensure()` is called every frame and
    is a no-op in the overwhelming majority of them. Abomerration's buffers are sized
    off the composition, so they change when somebody drags the composition
    resolution and at no other time.

    **It actually frees its colour texture.** `ffglex::FFGLFBO::Release()`
    deletes the framebuffer and the depth renderbuffer, then tests
    `depthBufferID` a second time where it plainly meant `colorTextureID` -- so
    the colour texture is leaked on every release (SDK b1afaf9, `FFGLFBO.cpp`).

    ---------------------------------------------------------------- the trap

    `FFGLFBO::Initialise` sizes its new colour texture inside a
    `ScopedTextureBinding`, and every `ffglex::Scoped*` binding **clears to 0 on
    scope exit rather than restoring** -- so allocating a buffer silently
    unbinds whatever was on the active texture unit. Anything that binds its
    input before calling `Ensure()` reads texture 0 for exactly the frames on
    which an allocation happened: the frame after load, and one frame each time
    a resolution drag reallocates. Correct on every frame except those.

    Abomerration is safe by construction -- `ProcessOpenGL` calls every `Ensure()`
    before it binds anything -- and `Ensure()` also saves and restores
    `GL_TEXTURE_BINDING_2D` so that stops being a thing a future edit can undo
    by moving one line.
*/
class PassBuffer : public ffglex::FFGLFBO
{
public:
	~PassBuffer();

	/// Allocate at this size and format, reusing the existing buffer if it
	/// already matches. Newly allocated buffers are cleared: a pass that reads
	/// a buffer before anything has written it reads this.
	bool Ensure( GLsizei requestedWidth, GLsizei requestedHeight, GLint format );

	/// Release everything, including the colour texture the SDK forgets.
	void Destroy();

	/**
	    Build the mip chain over whatever was just rendered, and switch the
	    sampler to read it.

	    `FFGLFBO` creates its colour texture with `GL_LINEAR` and no mip levels,
	    so a `textureLod` fetch above level 0 would come back black without this.

	    Called once per frame on the buffer the dispersion samples from. That is
	    not a nicety either: the quadrature takes a handful of point samples along
	    a path, and once they are more than a pixel apart it stops averaging the
	    picture and starts aliasing it -- so each sample reads from the mip level
	    that covers the gap to the next one. See Disperse.cpp for the measurements
	    and `abomtest --banding` for the check.
	*/
	void GenerateMipmaps();

	bool IsValid() const
	{
		return GetGLID() != 0;
	}
};

} // namespace abomerration
