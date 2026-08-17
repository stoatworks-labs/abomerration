#include "PassBuffer.h"

using namespace ffglex;

namespace abomerration
{
PassBuffer::~PassBuffer()
{
	//Nothing GL can be done here -- a destructor may well run without a current
	//context. DeInitGL is where the release happens; this is only a reminder
	//that it has to.
}

bool PassBuffer::Ensure( GLsizei requestedWidth, GLsizei requestedHeight, GLint format )
{
	if( requestedWidth <= 0 || requestedHeight <= 0 )
		return false;

	if( fboID != 0 && width == requestedWidth && height == requestedHeight && internalColorFormat == format )
		return true;

	//What the active texture unit had bound before we started. Initialise()
	//sizes its colour texture under a ScopedTextureBinding, and those clear to
	//0 on scope exit instead of restoring -- so without this, allocating a
	//buffer unbinds the caller's input texture and the frame that allocated
	//renders black. See the header.
	GLint previousTexture = 0;
	glGetIntegerv( GL_TEXTURE_BINDING_2D, &previousTexture );

	Destroy();

	if( !Initialise( requestedWidth, requestedHeight, format ) )
	{
		glBindTexture( GL_TEXTURE_2D, static_cast< GLuint >( previousTexture ) );
		return false;
	}

	//A freshly allocated framebuffer's contents are undefined, and undefined is
	//not a subtle artefact -- it is whatever texture memory the driver handed
	//back, which on a blur input is somebody else's picture.
	GLint previousFBO = 0;
	GLint previousViewport[ 4 ] = { 0, 0, 0, 0 };
	glGetIntegerv( GL_FRAMEBUFFER_BINDING, &previousFBO );
	glGetIntegerv( GL_VIEWPORT, previousViewport );

	glBindFramebuffer( GL_FRAMEBUFFER, fboID );
	glViewport( 0, 0, width, height );
	glClearColor( 0.0f, 0.0f, 0.0f, 0.0f );
	glClear( GL_COLOR_BUFFER_BIT );

	glBindFramebuffer( GL_FRAMEBUFFER, previousFBO );
	glViewport( previousViewport[ 0 ], previousViewport[ 1 ], previousViewport[ 2 ], previousViewport[ 3 ] );
	glBindTexture( GL_TEXTURE_2D, static_cast< GLuint >( previousTexture ) );

	return true;
}

void PassBuffer::GenerateMipmaps()
{
	if( colorTextureID == 0 )
		return;

	//Save and restore, for the same reason Ensure() does: leaving somebody else's
	//texture unbound from the active unit is a defect that only shows on the
	//frames where this ran.
	GLint previousTexture = 0;
	glGetIntegerv( GL_TEXTURE_BINDING_2D, &previousTexture );

	glBindTexture( GL_TEXTURE_2D, colorTextureID );

	//Set every frame rather than once at allocation. It is a cheap call, and the
	//alternative is a filter that depends on whether this buffer happened to be
	//reallocated since the last time somebody changed the sampler -- which is the
	//kind of state nobody reasons about correctly twice.
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );
	glGenerateMipmap( GL_TEXTURE_2D );

	glBindTexture( GL_TEXTURE_2D, static_cast< GLuint >( previousTexture ) );
}

void PassBuffer::Destroy()
{
	//The SDK's Release() leaves this behind. Delete it first, then let the base
	//class deal with the framebuffer and the depth buffer.
	if( colorTextureID != 0 )
	{
		glDeleteTextures( 1, &colorTextureID );
		colorTextureID = 0;
	}

	Release();
}

} // namespace abomerration
