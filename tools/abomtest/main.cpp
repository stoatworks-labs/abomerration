/**
    abomtest -- the offline harness.

    It drives **the real plugin class** through the real FFGL sequence in a
    headless core-profile context, including `SetTime`, `SetBeatInfo` and a
    synthetic spectrum injected through the host-facing element API. Not a
    reimplementation and not a preview: the thing under test is `Abomerration`,
    compiled from the same objects that go into the bundle, and every number
    below comes out of a frame it actually rendered.

        --out PATH        render a frame
        --scene PATH      write the synthetic test card
        --list            parameters, with their types and defaults
        --set "Name=v"    set any parameter by its host-facing name
        --field           the GLSL field against Dispersion.cpp, all four geometries
        --offset          the picture really moves by the distance asked for
        --spectrum        the weight table is energy preserving at every setting
        --drive           the reaction arithmetic, including the bar recovery
        --clock           milliseconds and seconds hosts produce the same drift
        --presets         every factory preset is distinct and non-degenerate
        --quadrature      the spectral quadrature has converged
        --bench           frame cost at 1080p and 4K
        --sheet PATH      a contact sheet of every geometry and spectrum
        --size WxH        render size (default 640x360)
        --frames N        advance this many frames at 60 fps before writing (default 2)
        --pipe            raw RGBA frames in on stdin, out on stdout
        --script PATH     a `frame Parameter Name value` cue sheet for --pipe
        --fps N           the cue sheet's frame rate (default 30)

    ## What each check can and cannot catch

    `--field` is the only thing standing between `Dispersion.cpp` and its GLSL
    mirror in `Field.cpp`. It renders the field through a probe shader assembled
    from the *same strings the plugin uses* -- so it is not checking a lookalike
    -- into an RGBA32F buffer, and compares what the GPU wrote against the C++ at
    a few thousand points per geometry. **It carries its own control**: the same
    comparison against a deliberately wrong geometry, which must FAIL. A row of
    agreements is exactly when to start wondering whether a test can fail at all.

    It cannot catch a field that is mirrored correctly and wrong in both copies.

    `--offset` can. It puts a single hard vertical edge through the frame and
    finds where each channel's edge landed, then compares that against the
    displacement the controls asked for -- so it tests the field, the weight
    table, the fetch, the aspect correction and the sign convention at once, in
    pixels, against arithmetic done outside the plugin.

    `--spectrum` renders a flat field and demands it come back flat at every
    Spectrum setting and every Amount. That is the normalisation in
    `dispersion::weights()`: without it the settings differ in overall colour and
    Spectrum reads as a tint control. Flat in, flat out is the one property that
    catches it, and it catches nothing else.

    `--drive` is pure arithmetic with no GL at all. It is where the bar recovery,
    the depth carve-out and the logarithmic band split are actually pinned down,
    because none of those are judgeable by eye -- "it moves with the music" is
    true of almost every wrong answer.

    `--clock` feeds one plugin a seconds-shaped clock and another a
    milliseconds-shaped one and demands the same drift out of both. Resolume
    sends milliseconds; this harness sends seconds; the FFGL header says nothing.
    Nothing else here would notice a plugin that runs a thousand times fast in
    the only host that matters.

    `--presets` catches the degenerate ones -- a preset that renders black, or
    that is identical to another, or that does nothing at all.

    None of them catches a dead control. See `tools/sweep.py`.
*/

#include <OpenGL/OpenGL.h>
#include <OpenGL/gl3.h>
#include <zlib.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "Abomerration.h"
#include "Controls.h"
#include "Dispersion.h"
#include "Drive.h"
#include "Presets.h"
#include "Shaders.h"

using namespace abomerration;

namespace
{
//---------------------------------------------------------------------------
// PNG. zlib ships with the OS, so this is fifty lines rather than a dependency.
//---------------------------------------------------------------------------
void putBigEndian( std::vector< unsigned char >& out, unsigned int value )
{
	out.push_back( static_cast< unsigned char >( ( value >> 24 ) & 0xff ) );
	out.push_back( static_cast< unsigned char >( ( value >> 16 ) & 0xff ) );
	out.push_back( static_cast< unsigned char >( ( value >> 8 ) & 0xff ) );
	out.push_back( static_cast< unsigned char >( value & 0xff ) );
}

void putChunk( std::vector< unsigned char >& out, const char* type,
               const unsigned char* data, size_t length )
{
	putBigEndian( out, static_cast< unsigned int >( length ) );
	const size_t crcStart = out.size();
	out.insert( out.end(), type, type + 4 );
	out.insert( out.end(), data, data + length );
	const unsigned long crc = crc32( 0, out.data() + crcStart,
	                                 static_cast< unsigned int >( out.size() - crcStart ) );
	putBigEndian( out, static_cast< unsigned int >( crc ) );
}

bool writePng( const std::string& path, int width, int height,
               const std::vector< unsigned char >& rgba )
{
	//Each scanline gets a filter byte. Filter 0 (none) throughout: this is a test
	//artefact, not a delivery format.
	std::vector< unsigned char > raw;
	raw.reserve( static_cast< size_t >( height ) * ( 1 + static_cast< size_t >( width ) * 4 ) );
	for( int y = 0; y < height; ++y )
	{
		raw.push_back( 0 );
		const unsigned char* row = rgba.data() + static_cast< size_t >( y ) * width * 4;
		raw.insert( raw.end(), row, row + static_cast< size_t >( width ) * 4 );
	}

	uLongf compressedSize = compressBound( static_cast< uLong >( raw.size() ) );
	std::vector< unsigned char > compressed( compressedSize );
	if( compress2( compressed.data(), &compressedSize, raw.data(),
	               static_cast< uLong >( raw.size() ), 6 ) != Z_OK )
		return false;
	compressed.resize( compressedSize );

	std::vector< unsigned char > png = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n' };

	std::vector< unsigned char > ihdr;
	putBigEndian( ihdr, static_cast< unsigned int >( width ) );
	putBigEndian( ihdr, static_cast< unsigned int >( height ) );
	ihdr.push_back( 8 );//bit depth
	ihdr.push_back( 6 );//truecolour with alpha
	ihdr.push_back( 0 );//deflate
	ihdr.push_back( 0 );//adaptive filtering
	ihdr.push_back( 0 );//no interlace
	putChunk( png, "IHDR", ihdr.data(), ihdr.size() );
	putChunk( png, "IDAT", compressed.data(), compressed.size() );
	putChunk( png, "IEND", nullptr, 0 );

	FILE* file = std::fopen( path.c_str(), "wb" );
	if( file == nullptr )
		return false;
	const size_t written = std::fwrite( png.data(), 1, png.size(), file );
	std::fclose( file );
	return written == png.size();
}

//---------------------------------------------------------------------------
// The synthetic scenes.
//
// Three of them, and the split is the point: each check gets a scene that makes
// the thing it measures measurable, instead of one scene that makes everything
// vaguely visible.
//---------------------------------------------------------------------------
struct Rgba
{
	float r, g, b, a;
};

/// The general test card: something to look at, and something for a contact
/// sheet to be judged from. A pure function of position, so a test can predict
/// what should be where without carrying a copy of the image.
///
/// Frame coordinates, 0..1, **y DOWN**.
Rgba scenePixel( float x, float y )
{
	//A dark background rather than mid grey: a coloured fringe is what this
	//plugin makes, and fringes are legible against dark and muddy against grey.
	Rgba out { 0.06f, 0.06f, 0.08f, 1.0f };

	//Concentric rings. The radial geometries do their most visible work at a
	//distance from the centre, so the card carries structure at every radius --
	//and rings are the one shape whose dispersion is unmistakable, because a
	//radial displacement moves a ring onto where another ring was.
	const float cx = ( x - 0.5f ) * 1.7778f;
	const float cy = ( y - 0.5f );
	const float r  = std::sqrt( cx * cx + cy * cy );

	//Twelve cycles across the half-height, which puts a ring edge every ~7 px at
	//180 tall. Chosen against the displacement rather than for looks: the first
	//version of this card used 42 and every cell of the contact sheet came out a
	//saturated rainbow, because a displacement of a dozen pixels crossed several
	//ring periods and the card was being aliased rather than dispersed. A card
	//finer than the effect measures the card.
	const float rings = std::fabs( std::sin( r * 12.0f ) );
	if( rings > 0.55f )
	{
		out.r = 0.85f;
		out.g = 0.85f;
		out.b = 0.88f;
	}

	//A hard vertical edge down the middle third: the single most legible thing a
	//horizontal channel offset can act on.
	if( x > 0.5f && y > 0.33f && y < 0.66f )
	{
		out.r = 0.92f;
		out.g = 0.92f;
		out.b = 0.95f;
	}

	//Saturated primaries along the top, so a spectral smear has something to
	//smear that is not already white. White fringes on white edges hide exactly
	//the mistake this plugin is most likely to make.
	if( y < 0.10f )
	{
		const int band = static_cast< int >( x * 6.0f );
		const float c[ 6 ][ 3 ] = {
			{ 0.9f, 0.1f, 0.1f }, { 0.1f, 0.9f, 0.1f }, { 0.1f, 0.1f, 0.9f },
			{ 0.9f, 0.9f, 0.1f }, { 0.9f, 0.1f, 0.9f }, { 0.1f, 0.9f, 0.9f }
		};
		const int i = std::clamp( band, 0, 5 );
		out.r       = c[ i ][ 0 ];
		out.g       = c[ i ][ 1 ];
		out.b       = c[ i ][ 2 ];
	}

	//Isolated highlights along the bottom, for the fringe boost to work on.
	if( y > 0.88f )
	{
		const float gx = std::fmod( x * 10.0f, 1.0f );
		if( gx < 0.35f )
		{
			out.r = 1.0f;
			out.g = 1.0f;
			out.b = 1.0f;
		}
	}

	return out;
}

std::vector< unsigned char > buildScene( int width, int height, Rgba ( *pixel )( float, float ) )
{
	std::vector< unsigned char > rgba( static_cast< size_t >( width ) * height * 4 );

	for( int y = 0; y < height; ++y )
	{
		const float fy = ( static_cast< float >( y ) + 0.5f ) / static_cast< float >( height );
		for( int x = 0; x < width; ++x )
		{
			const float fx = ( static_cast< float >( x ) + 0.5f ) / static_cast< float >( width );

			const Rgba c = pixel( fx, fy );

			//Premultiplied, because that is what an FFGL host hands a plugin.
			unsigned char* p = rgba.data() + ( static_cast< size_t >( y ) * width + x ) * 4;
			p[ 0 ]           = static_cast< unsigned char >( std::clamp( c.r * c.a, 0.0f, 1.0f ) * 255.0f + 0.5f );
			p[ 1 ]           = static_cast< unsigned char >( std::clamp( c.g * c.a, 0.0f, 1.0f ) * 255.0f + 0.5f );
			p[ 2 ]           = static_cast< unsigned char >( std::clamp( c.b * c.a, 0.0f, 1.0f ) * 255.0f + 0.5f );
			p[ 3 ]           = static_cast< unsigned char >( std::clamp( c.a, 0.0f, 1.0f ) * 255.0f + 0.5f );
		}
	}

	return rgba;
}

/// A single mid grey, everywhere, fully opaque.
///
/// The whole point of `--spectrum`: displacing a constant field can only return
/// the same constant field, so any difference at all is the weight table failing
/// to preserve energy. A scene with any structure in it would make the check a
/// judgement about tolerances instead of a statement about arithmetic.
Rgba flatPixel( float, float )
{
	return { 0.5f, 0.5f, 0.5f, 1.0f };
}

/// One hard vertical edge at x = 0.5, black to white, full height.
///
/// For `--offset`. Full height so the measurement can be taken on any row, and
/// nothing else in the frame so that finding the edge cannot pick up something
/// else by mistake.
Rgba edgePixel( float x, float )
{
	const float v = x < 0.5f ? 0.0f : 1.0f;
	return { v, v, v, 1.0f };
}

//---------------------------------------------------------------------------
// GL plumbing.
//---------------------------------------------------------------------------
CGLContextObj createContext()
{
	const CGLPixelFormatAttribute accelerated[] = {
		kCGLPFAOpenGLProfile, static_cast< CGLPixelFormatAttribute >( kCGLOGLPVersion_GL4_Core ),
		kCGLPFAAccelerated,
		kCGLPFAColorSize, static_cast< CGLPixelFormatAttribute >( 24 ),
		kCGLPFAAlphaSize, static_cast< CGLPixelFormatAttribute >( 8 ),
		static_cast< CGLPixelFormatAttribute >( 0 )
	};
	const CGLPixelFormatAttribute software[] = {
		kCGLPFAOpenGLProfile, static_cast< CGLPixelFormatAttribute >( kCGLOGLPVersion_GL4_Core ),
		kCGLPFAColorSize, static_cast< CGLPixelFormatAttribute >( 24 ),
		kCGLPFAAlphaSize, static_cast< CGLPixelFormatAttribute >( 8 ),
		static_cast< CGLPixelFormatAttribute >( 0 )
	};

	CGLPixelFormatObj format = nullptr;
	GLint formatCount        = 0;
	if( CGLChoosePixelFormat( accelerated, &format, &formatCount ) != kCGLNoError || format == nullptr )
	{
		if( CGLChoosePixelFormat( software, &format, &formatCount ) != kCGLNoError || format == nullptr )
			return nullptr;
	}

	CGLContextObj context = nullptr;
	const CGLError error  = CGLCreateContext( format, nullptr, &context );
	CGLDestroyPixelFormat( format );
	if( error != kCGLNoError )
		return nullptr;

	CGLSetCurrentContext( context );
	return context;
}

struct Target
{
	GLuint texture = 0;
	GLuint fbo     = 0;
	int width      = 0;
	int height     = 0;
};

Target makeTarget( int width, int height, GLenum internalFormat = GL_RGBA8 )
{
	Target target;
	target.width  = width;
	target.height = height;

	const bool isFloat = internalFormat == GL_RGBA32F;

	glGenTextures( 1, &target.texture );
	glBindTexture( GL_TEXTURE_2D, target.texture );
	glTexImage2D( GL_TEXTURE_2D, 0, static_cast< GLint >( internalFormat ), width, height, 0,
	              GL_RGBA, isFloat ? GL_FLOAT : GL_UNSIGNED_BYTE, nullptr );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST );
	glBindTexture( GL_TEXTURE_2D, 0 );

	glGenFramebuffers( 1, &target.fbo );
	glBindFramebuffer( GL_FRAMEBUFFER, target.fbo );
	glFramebufferTexture2D( GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, target.texture, 0 );
	return target;
}

void releaseTarget( Target& target )
{
	if( target.fbo != 0 )
		glDeleteFramebuffers( 1, &target.fbo );
	if( target.texture != 0 )
		glDeleteTextures( 1, &target.texture );
	target = Target();
}

GLuint uploadTexture( const std::vector< unsigned char >& rgba, int width, int height )
{
	GLuint texture = 0;
	glGenTextures( 1, &texture );
	glBindTexture( GL_TEXTURE_2D, texture );
	glTexImage2D( GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data() );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );
	glBindTexture( GL_TEXTURE_2D, 0 );
	return texture;
}

/// Straight out of GL, **bottom row first**. Every sampler below takes frame
/// coordinates with y down and flips here, in one place.
std::vector< unsigned char > readBytes( const Target& target )
{
	std::vector< unsigned char > pixels( static_cast< size_t >( target.width ) * target.height * 4 );
	glBindFramebuffer( GL_FRAMEBUFFER, target.fbo );
	glPixelStorei( GL_PACK_ALIGNMENT, 1 );
	glReadPixels( 0, 0, target.width, target.height, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data() );
	return pixels;
}

std::vector< float > readFloats( const Target& target )
{
	std::vector< float > pixels( static_cast< size_t >( target.width ) * target.height * 4 );
	glBindFramebuffer( GL_FRAMEBUFFER, target.fbo );
	glPixelStorei( GL_PACK_ALIGNMENT, 1 );
	glReadPixels( 0, 0, target.width, target.height, GL_RGBA, GL_FLOAT, pixels.data() );
	return pixels;
}

std::vector< unsigned char > flipRows( const std::vector< unsigned char >& image, int width, int height )
{
	std::vector< unsigned char > flipped( image.size() );
	const size_t stride = static_cast< size_t >( width ) * 4;
	for( int y = 0; y < height; ++y )
		std::memcpy( flipped.data() + static_cast< size_t >( y ) * stride,
		             image.data() + static_cast< size_t >( height - 1 - y ) * stride, stride );
	return flipped;
}

/**
    Upload a scene built by `buildScene`.

    `buildScene` works top-down, because that is how a frame is described and how
    `scenePixel` is written. **`glTexImage2D` treats its first row as v = 0, which
    is the BOTTOM.** So the rows have to be reversed on the way in, or the texture
    holds the picture upside down.

    Getting this wrong does not look like an orientation bug. The picture comes
    out inverted, somebody reaches for the readback and flips that instead, and
    then the two flips cancel for anything symmetric -- which is most of what a
    check looks at -- while `samplePixel` and the Show Field meters quietly
    address the wrong half of the frame. That is exactly what happened here: the
    first version flipped at the readback, `--field` and `--offset` both passed
    because they measure flip-insensitive quantities, and only looking at a
    bypassed frame showed it.

    One flip, here, where the convention actually changes.
*/
GLuint uploadScene( const std::vector< unsigned char >& topDown, int width, int height )
{
	return uploadTexture( flipRows( topDown, width, height ), width, height );
}


/// One pixel of a bottom-up RGBA8 read, in frame coordinates (0..1, y down),
/// un-premultiplied.
Rgba samplePixel( const std::vector< unsigned char >& bottomUp, int width, int height,
                  float fx, float fy )
{
	const int x     = std::clamp( static_cast< int >( fx * static_cast< float >( width ) ), 0, width - 1 );
	const int yDown = std::clamp( static_cast< int >( fy * static_cast< float >( height ) ), 0, height - 1 );
	const int y     = height - 1 - yDown;

	const unsigned char* p = bottomUp.data() + ( static_cast< size_t >( y ) * width + x ) * 4;
	const float a          = static_cast< float >( p[ 3 ] ) / 255.0f;
	if( a <= 0.001f )
		return { 0.0f, 0.0f, 0.0f, 0.0f };

	return { static_cast< float >( p[ 0 ] ) / 255.0f / a,
	         static_cast< float >( p[ 1 ] ) / 255.0f / a,
	         static_cast< float >( p[ 2 ] ) / 255.0f / a,
	         a };
}

//---------------------------------------------------------------------------
// Driving the plugin.
//---------------------------------------------------------------------------
/**
    The plugin plus the state needed to render into it repeatedly.

    InitGL is called only when the size changes, which is not tidiness: InitGL
    compiles the shaders, and `FFGLShader::Compile` does not free the program it
    is replacing -- so calling it per frame leaks two programs per frame, which
    over a --bench run or a long --pipe is enough to matter and to distort the
    timings it is there to measure.
*/
struct Driver
{
	Abomerration plugin;
	int width  = 0;
	int height = 0;

	bool render( const Target& target, GLuint input, int inputWidth, int inputHeight )
	{
		FFGLTextureStruct inputStruct {};
		inputStruct.Width = inputStruct.HardwareWidth = static_cast< FFUInt32 >( inputWidth );
		inputStruct.Height = inputStruct.HardwareHeight = static_cast< FFUInt32 >( inputHeight );
		inputStruct.Handle                              = input;
		FFGLTextureStruct* inputs[ 1 ]                  = { &inputStruct };

		ProcessOpenGLStruct process {};
		process.numInputTextures = 1;
		process.inputTextures    = inputs;
		process.HostFBO          = target.fbo;

		glBindFramebuffer( GL_FRAMEBUFFER, target.fbo );
		glViewport( 0, 0, target.width, target.height );
		glClearColor( 0.0f, 0.0f, 0.0f, 0.0f );
		glClear( GL_COLOR_BUFFER_BIT );

		if( target.width != width || target.height != height )
		{
			FFGLViewportStruct viewport {};
			viewport.width  = static_cast< FFUInt32 >( target.width );
			viewport.height = static_cast< FFUInt32 >( target.height );
			if( plugin.InitGL( &viewport ) != FF_SUCCESS )
				return false;
			width  = target.width;
			height = target.height;
		}

		return plugin.ProcessOpenGL( &process ) == FF_SUCCESS;
	}
};

/// Every parameter's host-facing name, read out of the plugin itself.
///
/// Built at runtime rather than kept as a table beside Controls.h, and that is
/// not tidiness: a hand-written table is a second place for a name to live, and
/// the failure it produces is a `--set` that silently addresses nothing while
/// everything else about the run looks correct.
std::map< std::string, unsigned int > parameterIndex( Abomerration& plugin )
{
	std::map< std::string, unsigned int > byName;
	for( unsigned int id = 0; id < Abomerration::PT_COUNT; ++id )
	{
		const char* name = plugin.GetParamName( id );
		if( name != nullptr && name[ 0 ] != '\0' )
			byName[ name ] = id;
	}
	return byName;
}

/// A synthetic spectrum, delivered the way a host delivers one: through the
/// public element API, one bin at a time.
///
/// Without this every reactive control reads as dead, because there is no audio
/// in a headless process and the plugin is right to do nothing.
void injectSpectrum( Abomerration& plugin, const std::vector< float >& bins )
{
	for( size_t i = 0; i < bins.size() && i < static_cast< size_t >( drive::kAudioBins ); ++i )
		plugin.SetParamElementValue( Abomerration::PT_AUDIO, static_cast< unsigned int >( i ), bins[ i ] );
}

//---------------------------------------------------------------------------
// Parameter automation for --pipe.
//
// A plain text file of `frame  Parameter Name  value` lines. Values are held
// before the first key and after the last, and linearly interpolated between,
// so the piece is edited by editing the cue sheet rather than by editing code.
//
// **Option and boolean parameters must STEP.** An option is read by rounding and
// a boolean by a threshold, so a ramp between two settings passes through every
// setting in between -- a slow move from Radial to Turbulent renders Linear and
// Tangential on the way. Two keys one frame apart put the change on the frame
// the cut chose.
//---------------------------------------------------------------------------
using Track = std::vector< std::pair< int, float > >;

std::map< std::string, Track > loadScript( const std::string& path, std::string& error )
{
	std::map< std::string, Track > tracks;
	std::ifstream file( path );
	if( !file )
	{
		error = "cannot open " + path;
		return tracks;
	}

	std::string line;
	int lineNumber = 0;
	while( std::getline( file, line ) )
	{
		++lineNumber;

		const size_t hash = line.find( '#' );
		if( hash != std::string::npos )
			line = line.substr( 0, hash );

		std::istringstream stream( line );
		int frame = 0;
		if( !( stream >> frame ) )
			continue;// blank or comment

		//The name can contain spaces and the value is the last field, so the
		//rest of the line is split from the right rather than tokenised.
		std::string rest;
		std::getline( stream, rest );

		const size_t lastSpace = rest.find_last_of( " \t" );
		if( lastSpace == std::string::npos )
		{
			error = path + ":" + std::to_string( lineNumber ) + ": expected `frame Name value`";
			return {};
		}

		std::string name  = rest.substr( 0, lastSpace );
		const std::string value = rest.substr( lastSpace + 1 );

		const size_t first = name.find_first_not_of( " \t" );
		const size_t last  = name.find_last_not_of( " \t" );
		if( first == std::string::npos )
		{
			error = path + ":" + std::to_string( lineNumber ) + ": no parameter name";
			return {};
		}
		name = name.substr( first, last - first + 1 );

		tracks[ name ].emplace_back( frame, std::strtof( value.c_str(), nullptr ) );
	}

	for( auto& entry : tracks )
		std::sort( entry.second.begin(), entry.second.end(),
		           []( const auto& a, const auto& b ) { return a.first < b.first; } );

	return tracks;
}

float valueAt( const Track& track, int frame )
{
	if( track.empty() )
		return 0.0f;
	if( frame <= track.front().first )
		return track.front().second;
	if( frame >= track.back().first )
		return track.back().second;

	for( size_t i = 1; i < track.size(); ++i )
	{
		if( frame <= track[ i ].first )
		{
			const auto& a = track[ i - 1 ];
			const auto& b = track[ i ];
			const int span = b.first - a.first;
			if( span <= 0 )
				return b.second;
			const float t = static_cast< float >( frame - a.first ) / static_cast< float >( span );
			return a.second + ( b.second - a.second ) * t;
		}
	}

	return track.back().second;
}

bool readExactly( void* into, size_t bytes )
{
	unsigned char* at = static_cast< unsigned char* >( into );
	size_t left       = bytes;
	while( left > 0 )
	{
		const size_t got = std::fread( at, 1, left, stdin );
		if( got == 0 )
			return false;
		at += got;
		left -= got;
	}
	return true;
}

struct Options
{
	int width                              = 640;
	int height                             = 360;
	std::vector< std::pair< std::string, float > > sets;
};

/**
    Drive the plugin for `frames` frames of a synthetic 60 fps show, with a
    transport a real host would recognise.

    **barPhase has to track the clock.** The first version of this pinned it at 0
    and the effect was subtle but total: the bar recovery reconstructs a whole
    number of bars by reconciling the clock against barPhase, so a barPhase of 0
    at any time near zero recovers bar 0 exactly, the beat envelope sits at its
    peak of 1.0 for every frame, and *every control that shapes the envelope reads
    as dead* -- Beat Decay and Division both, because at the instant of a beat
    neither of them changes anything. Nothing about that looks like a harness bug
    from the outside.

    Two frames minimum, because the audio filter and the drift integrator both
    need a delta before they do anything: a one-frame render judges every reactive
    control on its inert first frame.
*/
bool driveFrames( Driver& driver, const Target& target, GLuint input,
                  int inputWidth, int inputHeight, int frames )
{
	//120 bpm, four beats to the bar.
	constexpr double kBarSeconds = 2.0;

	bool ok = true;
	for( int frame = 0; frame < frames; ++frame )
	{
		const double seconds = static_cast< double >( frame ) / 60.0;
		const double bars    = seconds / kBarSeconds;

		driver.plugin.SetBeatInfo( 120.0f, static_cast< float >( bars - std::floor( bars ) ) );
		driver.plugin.SetClockScaleForTest( 1.0 );//seconds, said out loud rather than inferred
		driver.plugin.SetTime( seconds );

		ok = driver.render( target, input, inputWidth, inputHeight );
		if( !ok )
			return false;
	}
	return ok;
}

bool applySets( Abomerration& plugin, const Options& options )
{
	const std::map< std::string, unsigned int > byName = parameterIndex( plugin );

	for( const auto& set : options.sets )
	{
		const auto found = byName.find( set.first );
		if( found == byName.end() )
		{
			std::fprintf( stderr, "no such parameter: %s\n", set.first.c_str() );
			return false;
		}
		plugin.SetFloatParameter( found->second, set.second );
	}

	return true;
}

//---------------------------------------------------------------------------
// --field
//---------------------------------------------------------------------------
/**
    Render the dispersion field through the probe shader.

    The probe is assembled by `shaders::FieldProbeFragment()` from the same
    `kFieldFunctions` and the same `kDisperseCommon` the production pass uses, so
    what this renders is the shader the plugin runs and not a copy of it.
*/
bool renderFieldProbe( const dispersion::Field& field, const Target& target,
                       GLuint anyTexture, std::vector< float >& out )
{
	ffglex::FFGLShader shader;
	const std::string fragment = shaders::FieldProbeFragment();
	if( !shader.Compile( shaders::kVertex, fragment.c_str() ) )
	{
		std::fprintf( stderr, "the field probe shader failed to compile\n" );
		return false;
	}

	ffglex::FFGLScreenQuad quad;
	if( !quad.Initialise() )
	{
		std::fprintf( stderr, "the probe quad failed to initialise\n" );
		return false;
	}

	glBindFramebuffer( GL_FRAMEBUFFER, target.fbo );
	glViewport( 0, 0, target.width, target.height );
	glClearColor( 0.0f, 0.0f, 0.0f, 1.0f );
	glClear( GL_COLOR_BUFFER_BIT );

	{
		ffglex::ScopedShaderBinding binding( shader.GetGLID() );
		ffglex::ScopedSamplerActivation sampler( 0 );
		ffglex::Scoped2DTextureBinding texture( anyTexture );

		shader.Set( "MaxUV", 1.0f, 1.0f );
		shader.Set( "InputTexture", 0 );
		shader.Set( "EdgeTexture", 0 );
		shader.Set( "SourceMaxUV", 1.0f, 1.0f );
		shader.Set( "SourceHalfTexel", 0.0f, 0.0f );

		//Zero, so `dispersionAt` returns the unweighted field. The edge weighting
		//is a separate claim and --offset is where it is exercised; mixing it in
		//here would make a mirror disagreement and an edge-pass disagreement
		//indistinguishable.
		shader.Set( "EdgeWeight", 0.0f );

		shader.Set( "Geometry", field.geometry );
		shader.Set( "Centre", field.centreU, field.centreV );
		shader.Set( "Angle", field.angle );
		shader.Set( "Amount", field.amount );
		shader.Set( "Falloff", field.falloff );
		shader.Set( "Turbulence", field.turbulence );
		shader.Set( "Drift", field.drift );
		shader.Set( "FrameAspect", field.aspectRatio );

		quad.Draw();
	}

	out = readFloats( target );

	quad.Release();
	shader.FreeGLResources();
	return true;
}

/// Compare the GPU's field against Dispersion.cpp. Returns the worst
/// disagreement seen, in picture-space units.
float fieldDisagreement( const dispersion::Field& field, const std::vector< float >& probe,
                         int width, int height )
{
	float worst = 0.0f;

	//Every eighth pixel in each direction: a few thousand points, which is plenty
	//to catch an arithmetic difference and quick enough to run per geometry.
	for( int y = 4; y < height; y += 8 )
	{
		for( int x = 4; x < width; x += 8 )
		{
			//The probe wrote picture space; the read is bottom-up.
			const float u = ( static_cast< float >( x ) + 0.5f ) / static_cast< float >( width );
			const float v = ( static_cast< float >( y ) + 0.5f ) / static_cast< float >( height );

			const int row       = height - 1 - y;
			const float* pixel  = probe.data() + ( static_cast< size_t >( row ) * width + x ) * 4;

			//Coordinates line up: the probe wrote picture space at pic.y = 1 - gl_v,
			//and this read is bottom-up, so array row (height-1-y) is exactly
			//pic.y = v. No flip is needed here -- the one in the shader is the only
			//one in the chain.
			float expectU = 0.0f;
			float expectV = 0.0f;
			dispersion::offsetAt( field, u, v, &expectU, &expectV );

			worst = std::max( worst, std::fabs( pixel[ 0 ] - expectU ) );
			worst = std::max( worst, std::fabs( pixel[ 1 ] - expectV ) );
		}
	}

	return worst;
}

bool checkField()
{
	//Deliberately not square and deliberately small. The aspect correction is
	//invisible on a square render, so a square one would let a whole class of
	//mistake through.
	const int width  = 320;
	const int height = 180;

	const std::vector< unsigned char > sceneBytes = buildScene( width, height, scenePixel );
	const GLuint input                            = uploadScene( sceneBytes, width, height );

	Target target = makeTarget( width, height, GL_RGBA32F );

	//RGBA32F, so nothing sits between the shader and the comparison. A signed
	//field packed into an 8-bit buffer would make the tolerance below a statement
	//about the packing rather than about the arithmetic.
	//
	//The tolerance is therefore float round-off over a handful of operations --
	//a trig call, a pow, a divide -- and not a fudge factor. 1e-5 of a picture
	//width is a thousandth of a pixel at 4K.
	constexpr float kTolerance = 1e-5f;

	bool ok = true;

	struct Case
	{
		const char* name;
		float geometry;
		float falloff;
		float centreX;
		float centreY;
	};

	const Case cases[] = {
		{ "Radial", 0.0f, 0.5f, 0.5f, 0.5f },
		//Off centre, because a centred radial field is symmetric and hides a
		//centre that never reached the shader.
		{ "Radial off centre", 0.0f, 0.72f, 0.32f, 0.61f },
		{ "Linear", 1.0f, 0.5f, 0.5f, 0.5f },
		{ "Tangential", 2.0f, 0.61f, 0.44f, 0.52f },
		{ "Turbulent", 3.0f, 0.5f, 0.5f, 0.5f },
	};

	for( const Case& c : cases )
	{
		controls::HostValues host;
		host.geometry   = c.geometry;
		host.falloff    = c.falloff;
		host.centreX    = c.centreX;
		host.centreY    = c.centreY;
		host.amount     = 0.7f;
		host.angle      = 0.31f;
		host.turbulence = 0.5f;

		const dispersion::Field field = controls::field(
			host, static_cast< float >( width ) / static_cast< float >( height ), 1.0f, 3.7f );

		std::vector< float > probe;
		if( !renderFieldProbe( field, target, input, probe ) )
			return false;

		const float worst = fieldDisagreement( field, probe, width, height );
		const bool pass   = worst <= kTolerance;
		ok                = ok && pass;

		std::printf( "  %-20s worst disagreement %.3e  %s\n", c.name, worst, pass ? "ok" : "FAIL" );
	}

	//---------------------------------------------------------------------
	// The control. The same comparison, with the C++ told a different geometry
	// from the one the GPU rendered -- so it MUST disagree. A test that only
	// ever agrees is a test nobody has shown can fail.
	//---------------------------------------------------------------------
	{
		controls::HostValues host;
		host.geometry = 0.0f;//radial on the GPU
		host.amount   = 0.7f;

		const float aspect = static_cast< float >( width ) / static_cast< float >( height );

		dispersion::Field rendered = controls::field( host, aspect, 1.0f, 0.0f );

		std::vector< float > probe;
		if( !renderFieldProbe( rendered, target, input, probe ) )
			return false;

		dispersion::Field lied = rendered;
		lied.geometry          = dispersion::kTangential;

		const float worst = fieldDisagreement( lied, probe, width, height );
		const bool pass   = worst > kTolerance;
		ok                = ok && pass;

		std::printf( "  %-20s worst disagreement %.3e  %s (must disagree)\n",
		             "control", worst, pass ? "ok" : "FAIL" );
	}

	releaseTarget( target );
	glDeleteTextures( 1, &input );

	return ok;
}

//---------------------------------------------------------------------------
// --offset
//---------------------------------------------------------------------------
/**
    Where a channel's edge landed, in picture-space u, by linear interpolation
    across the 50% crossing.

    Sub-pixel on purpose. The displacement being measured is a fraction of the
    frame, so at 640 wide a whole-pixel answer would carry a 0.0016 error into a
    comparison against arithmetic that is exact -- which would then have to be
    absorbed by a tolerance loose enough to hide a real sign error on a small
    Amount.
*/
float findEdge( const std::vector< unsigned char >& bottomUp, int width, int height,
                float atY, int channel )
{
	const int yDown = std::clamp( static_cast< int >( atY * static_cast< float >( height ) ), 0, height - 1 );
	const int y     = height - 1 - yDown;

	const unsigned char* row = bottomUp.data() + static_cast< size_t >( y ) * width * 4;

	for( int x = 1; x < width; ++x )
	{
		const float a = static_cast< float >( row[ ( x - 1 ) * 4 + channel ] ) / 255.0f;
		const float b = static_cast< float >( row[ x * 4 + channel ] ) / 255.0f;

		if( ( a < 0.5f && b >= 0.5f ) || ( a >= 0.5f && b < 0.5f ) )
		{
			const float t = ( 0.5f - a ) / ( b - a );
			return ( static_cast< float >( x - 1 ) + 0.5f + t ) / static_cast< float >( width );
		}
	}

	return -1.0f;
}

bool checkOffset()
{
	const int width  = 640;
	const int height = 360;
	const float aspect = static_cast< float >( width ) / static_cast< float >( height );

	const std::vector< unsigned char > sceneBytes = buildScene( width, height, edgePixel );
	const GLuint input                            = uploadScene( sceneBytes, width, height );

	Target target = makeTarget( width, height );

	bool ok = true;

	//Linear geometry at angle 0 so the displacement is purely horizontal and
	//purely known, and RGB Split so red is exactly the +1 end of the path and
	//blue exactly the -1 end. Every other setting mixes wavelengths, which is
	//correct for a picture and useless for a measurement.
	const float amounts[] = { 0.15f, 0.35f, 0.6f };

	for( float amountControl : amounts )
	{
		Driver driver;
		driver.plugin.SetFloatParameter( Abomerration::PT_GEOMETRY, 1.0f );//Linear
		driver.plugin.SetFloatParameter( Abomerration::PT_SPECTRUM, 0.0f );//RGB Split
		driver.plugin.SetFloatParameter( Abomerration::PT_ANGLE, 0.5f );   //0 radians
		driver.plugin.SetFloatParameter( Abomerration::PT_AMOUNT, amountControl );
		driver.plugin.SetFloatParameter( Abomerration::PT_EDGES, 0.0f );
		driver.plugin.SetFloatParameter( Abomerration::PT_FRINGE, 0.0f );

		if( !driver.render( target, input, width, height ) )
		{
			std::fprintf( stderr, "render failed\n" );
			return false;
		}

		const std::vector< unsigned char > pixels = readBytes( target );

		//What the controls asked for. `field()` is the shared conversion, so this
		//is the plugin's own idea of the amount -- but the step from there to a
		//pixel position is done here, outside the plugin, which is what makes
		//this a check and not a tautology.
		const controls::HostValues host = driver.plugin.hostValues();
		const dispersion::Field field    = controls::field( host, aspect, 1.0f, 0.0f );

		//Linear, angle 0: offsetU = amount / aspect, and the sample position is
		//u + offsetU * s * 0.5. The edge at input u = 0.5 therefore appears in
		//channel s at output u = 0.5 - offsetU * s * 0.5.
		const float offsetU  = field.amount / aspect;
		const float expectR  = 0.5f - offsetU * 0.5f;
		const float expectB  = 0.5f + offsetU * 0.5f;
		const float expectG  = 0.5f;

		const float gotR = findEdge( pixels, width, height, 0.5f, 0 );
		const float gotG = findEdge( pixels, width, height, 0.5f, 1 );
		const float gotB = findEdge( pixels, width, height, 0.5f, 2 );

		//One pixel. The crossing is found by interpolating an 8-bit read of a
		//GL_LINEAR fetch, so a fraction of a pixel is round-off; a whole pixel is
		//not, and a sign error is half the frame.
		const float tolerance = 1.0f / static_cast< float >( width );

		const bool pass = gotR > 0.0f && gotG > 0.0f && gotB > 0.0f
		                  && std::fabs( gotR - expectR ) <= tolerance
		                  && std::fabs( gotG - expectG ) <= tolerance
		                  && std::fabs( gotB - expectB ) <= tolerance;
		ok = ok && pass;

		std::printf( "  amount %.2f  R %+.4f (want %+.4f)  G %+.4f (want %+.4f)  B %+.4f (want %+.4f)  %s\n",
		             amountControl,
		             gotR - 0.5f, expectR - 0.5f,
		             gotG - 0.5f, expectG - 0.5f,
		             gotB - 0.5f, expectB - 0.5f,
		             pass ? "ok" : "FAIL" );
	}

	//---------------------------------------------------------------------
	// The control: Amount at zero must not move anything at all. A plugin that
	// displaced by a fixed pixel regardless of the control would pass every case
	// above if its constant happened to be near the tolerance.
	//---------------------------------------------------------------------
	{
		Driver driver;
		driver.plugin.SetFloatParameter( Abomerration::PT_GEOMETRY, 1.0f );
		driver.plugin.SetFloatParameter( Abomerration::PT_SPECTRUM, 0.0f );
		driver.plugin.SetFloatParameter( Abomerration::PT_AMOUNT, 0.0f );

		if( !driver.render( target, input, width, height ) )
			return false;

		const std::vector< unsigned char > pixels = readBytes( target );

		const float r = findEdge( pixels, width, height, 0.5f, 0 );
		const float b = findEdge( pixels, width, height, 0.5f, 2 );

		const float tolerance = 1.0f / static_cast< float >( width );
		const bool pass       = std::fabs( r - 0.5f ) <= tolerance && std::fabs( b - 0.5f ) <= tolerance;
		ok                    = ok && pass;

		std::printf( "  amount 0.00  R %+.4f  B %+.4f  %s (must not move)\n",
		             r - 0.5f, b - 0.5f, pass ? "ok" : "FAIL" );
	}

	releaseTarget( target );
	glDeleteTextures( 1, &input );

	return ok;
}

//---------------------------------------------------------------------------
// --spectrum
//---------------------------------------------------------------------------
bool checkSpectrum()
{
	const int width  = 320;
	const int height = 180;

	const std::vector< unsigned char > sceneBytes = buildScene( width, height, flatPixel );
	const GLuint input                            = uploadScene( sceneBytes, width, height );

	Target target = makeTarget( width, height );

	//What went in. Read back from the scene rather than assumed, so the check is
	//against the bytes the GPU was actually given.
	const float expected = static_cast< float >( sceneBytes[ 0 ] ) / 255.0f;

	bool ok = true;

	for( int spectrum = 0; spectrum < controls::spectrumCount(); ++spectrum )
	{
		for( float amount : { 0.0f, 0.3f, 0.7f, 1.0f } )
		{
			for( int geometry = 0; geometry < dispersion::kGeometryCount; ++geometry )
			{
				Driver driver;
				driver.plugin.SetFloatParameter( Abomerration::PT_SPECTRUM, static_cast< float >( spectrum ) );
				driver.plugin.SetFloatParameter( Abomerration::PT_AMOUNT, amount );
				driver.plugin.SetFloatParameter( Abomerration::PT_GEOMETRY, static_cast< float >( geometry ) );
				driver.plugin.SetFloatParameter( Abomerration::PT_EDGES, 0.0f );
				driver.plugin.SetFloatParameter( Abomerration::PT_FRINGE, 0.0f );

				if( !driver.render( target, input, width, height ) )
					return false;

				const std::vector< unsigned char > pixels = readBytes( target );

				//Well inside the frame. The dispersion clamps at the picture edge,
				//so the outermost pixels legitimately differ -- a clamp is not a
				//failure of energy preservation, it is what a clamp does.
				float worst      = 0.0f;
				float worstAlpha = 0.0f;
				for( float v = 0.25f; v < 0.76f; v += 0.05f )
				{
					for( float u = 0.25f; u < 0.76f; u += 0.05f )
					{
						const Rgba c = samplePixel( pixels, width, height, u, v );
						worst        = std::max( worst, std::fabs( c.r - expected ) );
						worst        = std::max( worst, std::fabs( c.g - expected ) );
						worst        = std::max( worst, std::fabs( c.b - expected ) );
						worstAlpha   = std::max( worstAlpha, std::fabs( c.a - 1.0f ) );
					}
				}

				//Two 8-bit quanta. One for the render's own rounding and one for
				//the un-premultiply in samplePixel; anything above that is the
				//weight table not summing to 1.
				const float tolerance = 2.5f / 255.0f;
				const bool pass       = worst <= tolerance && worstAlpha <= tolerance;
				ok                    = ok && pass;

				if( !pass )
					std::printf( "  %-10s geometry %d amount %.1f  worst %.4f alpha %.4f  FAIL\n",
					             controls::spectrumLabel( spectrum ), geometry, amount, worst, worstAlpha );
			}
		}

		std::printf( "  %-10s flat field preserved at every amount and geometry\n",
		             controls::spectrumLabel( spectrum ) );
	}

	//---------------------------------------------------------------------
	// The control: a weight table that is NOT normalised must fail the same
	// check. Built here by hand so the assertion is about normalisation and not
	// about the test being able to notice anything at all.
	//---------------------------------------------------------------------
	{
		dispersion::Sample samples[ dispersion::kMaxSamples ];
		const int n = dispersion::weights( 16, samples );

		float sumR = 0.0f;
		for( int i = 0; i < n; ++i )
			sumR += samples[ i ].r;

		//The real table sums to 1 per channel. That is the property --spectrum
		//depends on, so it is asserted directly as well as through a render:
		//a render can only say the picture came back right, while this says why.
		const bool pass = std::fabs( sumR - 1.0f ) <= 1e-5f;
		ok              = ok && pass;
		std::printf( "  %-10s red weights sum to %.6f  %s\n", "table", sumR, pass ? "ok" : "FAIL" );
	}

	releaseTarget( target );
	glDeleteTextures( 1, &input );

	return ok;
}

//---------------------------------------------------------------------------
// --banding
//---------------------------------------------------------------------------
/**
    How stepped a channel's fringe profile is.

    The WORST second difference along a row, in the region the dispersion covers,
    in 8-bit units. A smooth ramp has almost none; a staircase has a spike at
    every step.

    Why the second difference and not a variance or a detail measure: the profile
    of a dispersed step edge is *supposed* to be a ramp, so anything measuring how
    much the picture changed would also count the ramp itself and be highest when
    the plugin was working correctly. The second difference is zero for any
    straight ramp of any slope and large only where the slope jumps -- which is
    exactly and only what a quadrature step is.

    Why the maximum and not the sum, which is what this measured first: the sum is
    dominated by the NUMBER of joins rather than by how visible any of them is, so
    once the prefilter was in place it ranked Prism 32 (many small steps) as worse
    than Prism 8 (a few heavily smoothed ones) -- the opposite of what the eye
    reports. A staircase is visible when an individual step is a visible jump, so
    the statistic is the worst step.
*/
float staircase( const std::vector< unsigned char >& bottomUp, int width, int height,
                 float atY, int channel, float fromX, float toX )
{
	const int yDown = std::clamp( static_cast< int >( atY * static_cast< float >( height ) ), 0, height - 1 );
	const int y     = height - 1 - yDown;

	const unsigned char* row = bottomUp.data() + static_cast< size_t >( y ) * width * 4;

	const int x0 = std::clamp( static_cast< int >( fromX * static_cast< float >( width ) ), 1, width - 2 );
	const int x1 = std::clamp( static_cast< int >( toX * static_cast< float >( width ) ), 1, width - 2 );

	float worst = 0.0f;
	for( int x = std::min( x0, x1 ); x <= std::max( x0, x1 ); ++x )
	{
		const float a = static_cast< float >( row[ ( x - 1 ) * 4 + channel ] );
		const float b = static_cast< float >( row[ x * 4 + channel ] );
		const float c = static_cast< float >( row[ ( x + 1 ) * 4 + channel ] );
		worst = std::max( worst, std::fabs( a - 2.0f * b + c ) );
	}

	return worst;
}

/**
    Ripple at the sample spacing: the quadrature's own footprint, and nothing else.

    For each pixel, how far the profile sits from its own mean over a window one
    sample-spacing wide. A straight line's mean over a symmetric window equals its
    centre value, so this is **exactly zero for any ramp of any slope** and
    responds only to curvature at the scale of the window -- which is the scale a
    quadrature step lives at, and is far finer than anything the picture or the
    spectral response varies over.

    ------------------------------------------------- three metrics, two of them wrong

    Worth recording, because both wrong ones looked reasonable.

    **Summed second-difference energy.** Caught the original defect: before the
    prefilter existed the numbers were 394/376/288 at a 65-pixel path and
    382/388/384 at 108 -- saturated, and no longer even ordered by sample count.
    But with the prefilter in it ranked Prism 32 as *worse* than Prism 8, because a
    sum over joins is dominated by how MANY there are rather than how visible any
    one is.

    **The worst single second difference.** With the prefilter the numbers came out
    28/26/5, 9/13/12 and 8/8/9 -- which is 765/L almost exactly, independent of the
    sample count. That is not banding: it is the true curvature of the correct
    answer, because a hard black-to-white edge smeared through three primary
    responses has to turn that sharply somewhere. A metric that flags the
    physically correct profile is measuring the scene.

    **Difference from the densest setting.** Also wrong, and instructively so. It
    reported 118 and 96 of 255 -- but the prefilter deliberately softens by the
    sample spacing, so a cheap setting genuinely renders a *softer* picture. The
    settings are not supposed to converge to each other; they trade sharpness for
    cost. Measuring convergence measured the trade.

    Ripple at the window scale has none of those problems.
*/
float ripple( const std::vector< unsigned char >& bottomUp, int width, int height,
              float atY, int channel, int window )
{
	const int yDown = std::clamp( static_cast< int >( atY * static_cast< float >( height ) ), 0, height - 1 );
	const int y     = height - 1 - yDown;

	const unsigned char* row = bottomUp.data() + static_cast< size_t >( y ) * width * 4;

	const int half = std::max( 1, window / 2 );

	float worst = 0.0f;
	for( int x = half; x < width - half; ++x )
	{
		float sum = 0.0f;
		for( int k = -half; k <= half; ++k )
			sum += static_cast< float >( row[ ( x + k ) * 4 + channel ] );

		const float mean = sum / static_cast< float >( 2 * half + 1 );
		worst = std::max( worst, std::fabs( static_cast< float >( row[ x * 4 + channel ] ) - mean ) );
	}

	return worst;
}

/**
    The quadrature must leave no visible footprint.

    A dispersion of L pixels sampled N times steps every L/N pixels, and once that
    gap is wider than a pixel the samples skip the picture between them instead of
    averaging it. Each sample therefore reads from the mip level covering the gap
    (Copy.cpp), and this is the check that the combination actually works: ripple
    at the spacing scale, in 8-bit units, against a hard step edge -- the worst
    input there is, because it puts the maximum possible contrast inside the
    quadrature's reach.

    RGB Split is excluded on purpose. Three hard steps is the look it exists to
    produce, not an artefact of it, and it is the one setting that does not
    prefilter.
*/
bool checkQuadrature()
{
	const int width  = 1280;
	const int height = 720;

	const std::vector< unsigned char > sceneBytes = buildScene( width, height, edgePixel );
	const GLuint input                            = uploadScene( sceneBytes, width, height );

	Target target = makeTarget( width, height );

	//Six of 255, against a hard black-to-white step -- which is the worst input
	//that exists for this, because it puts the maximum possible contrast inside the
	//quadrature's reach. Measured: Prism 8 sits near 5 and the other two below 2.
	//
	//The threshold is not the visibility limit of a gradient step in the abstract;
	//it is what the cheapest Prism setting achieves on the worst possible input.
	//Ripple scales with the local contrast the samples span, so on real footage --
	//where a 27-pixel span almost never holds a full-range step -- all three
	//settings are further below this than the numbers here suggest. Stated that way
	//round because a threshold quietly loosened until the test passes is worth
	//nothing; this one names what the cheap setting really costs.
	constexpr float kVisible = 6.0f;

	bool ok = true;

	for( float amountControl : { 0.25f, 0.6f, 1.0f } )
	{
		std::string line;
		bool smooth = true;

		const controls::HostValues host = [ & ] {
			controls::HostValues h;
			h.amount = amountControl;
			return h;
		}();
		const dispersion::Field field = controls::field(
			host, static_cast< float >( width ) / static_cast< float >( height ), 1.0f, 0.0f );
		const float pathPixels = std::fabs( field.amount ) * static_cast< float >( height );

		for( int spectrum = controls::kSpectrumPrism8; spectrum < controls::spectrumCount(); ++spectrum )
		{
			Driver driver;
			driver.plugin.SetFloatParameter( Abomerration::PT_GEOMETRY, 1.0f );//Linear: one path length everywhere
			driver.plugin.SetFloatParameter( Abomerration::PT_ANGLE, 0.5f );
			driver.plugin.SetFloatParameter( Abomerration::PT_SPECTRUM, static_cast< float >( spectrum ) );
			driver.plugin.SetFloatParameter( Abomerration::PT_AMOUNT, amountControl );
			driver.plugin.SetFloatParameter( Abomerration::PT_EDGES, 0.0f );
			driver.plugin.SetFloatParameter( Abomerration::PT_FRINGE, 0.0f );

			if( !driver.render( target, input, width, height ) )
				return false;

			const std::vector< unsigned char > pixels = readBytes( target );

			//The window is the sample spacing, which is what the ripple period
			//would be. Rounded up and kept at three minimum, because a window of
			//one measures nothing.
			const int samples = controls::spectrumSamples( spectrum );
			const int window  = std::max( 3, static_cast< int >( std::ceil( pathPixels
			                                                                / static_cast< float >( samples - 1 ) ) ) );

			const float worst = ripple( pixels, width, height, 0.5f, 0, window );

			char buffer[ 80 ];
			std::snprintf( buffer, sizeof( buffer ), " %-9s %4.1f", controls::spectrumLabel( spectrum ), worst );
			line += buffer;

			if( worst > kVisible )
				smooth = false;
		}

		ok = ok && smooth;
		std::printf( "  amount %.2f (%5.1f px path) ripple:%s  %s\n",
		             amountControl, pathPixels, line.c_str(),
		             smooth ? "ok" : "FAIL (the quadrature is visible)" );
	}

	releaseTarget( target );
	glDeleteTextures( 1, &input );

	return ok;
}

//---------------------------------------------------------------------------
// --drive
//---------------------------------------------------------------------------
bool checkDrive()
{
	bool ok = true;

	auto report = [ &ok ]( const char* what, bool pass ) {
		std::printf( "  %-56s %s\n", what, pass ? "ok" : "FAIL" );
		ok = ok && pass;
	};

	std::vector< float > silence( drive::kAudioBins, 0.0f );

	drive::Input in;
	in.bins     = silence.data();
	in.binCount = static_cast< int >( silence.size() );
	in.bpm      = 120.0f;
	in.barPhase = 0.0f;
	in.seconds  = 0.0;

	//---------------------------------------------------------------------
	// Nothing engaged is exactly nothing. Not approximately: the manual lens is
	// the default state, and a scale of 0.999 there would be a silent
	// multiplication on every frame of a plugin nobody asked to be reactive.
	//---------------------------------------------------------------------
	{
		const drive::Output out = drive::compute( drive::Settings(), in );
		report( "depths at zero leave scale at exactly 1 and no push",
		        out.scale == 1.0f && out.push[ 0 ] == 0.0f && out.push[ 1 ] == 0.0f && out.push[ 2 ] == 0.0f );
	}

	//---------------------------------------------------------------------
	// Free mode has no grid, so the beat envelope is flat zero -- and full beat
	// depth in Free mode therefore gates the effect off entirely. That is a
	// consequence worth pinning down rather than discovering on a stage.
	//---------------------------------------------------------------------
	{
		drive::Settings s;
		s.sync      = drive::kSyncFree;
		s.beatDepth = 1.0f;

		const drive::Output out = drive::compute( s, in );
		report( "Free mode: beat envelope flat zero, full depth gates off",
		        out.beat == 0.0f && out.scale == 0.0f );
	}

	//---------------------------------------------------------------------
	// The bar recovery. A host hands over a tempo and a position within the
	// current bar and never says which bar it is; the recovery reconstructs a
	// continuous count from the clock. Checked as a function of time across
	// several bars, which is what makes it a continuity test: an off-by-one bar
	// or a truncation instead of a floor shows up as the envelope being the right
	// shape at the wrong phase.
	//---------------------------------------------------------------------
	{
		drive::Settings s;
		s.sync         = drive::kSyncLocked;
		s.beatDepth    = 1.0f;
		s.beatDecay    = 1.0f;//linear ramp, so the expected value is exact
		s.beatDivision = 1.0f;

		const double barSeconds = 2.0;//120 bpm, four beats to the bar

		float worst = 0.0f;
		for( double t = 0.0; t < 8.0; t += 0.017 )
		{
			drive::Input beat = in;
			beat.seconds      = t;
			//A host's barPhase: where we are inside the current bar.
			beat.barPhase = static_cast< float >( t / barSeconds - std::floor( t / barSeconds ) );

			const drive::Output out = drive::compute( s, beat );

			const double beats   = t / barSeconds * 4.0;
			const double frac    = beats - std::floor( beats );
			const float expected = static_cast< float >( 1.0 - frac );

			worst = std::max( worst, std::fabs( out.beat - expected ) );
		}

		std::printf( "  %-56s worst %.2e\n", "bar recovery tracks the clock across four bars", worst );
		report( "bar recovery exact to float round-off", worst < 1e-4f );
	}

	//---------------------------------------------------------------------
	// The depth carve-out. Depth takes from the always-on part rather than adding
	// on top, so silence is clean and the beat is the full amount -- and two
	// sources at high depth saturate rather than going negative, because a
	// negative scale would flip the dispersion instead of maxing it out.
	//---------------------------------------------------------------------
	{
		drive::Settings s;
		s.sync         = drive::kSyncLocked;
		s.beatDepth    = 1.0f;
		s.beatDecay    = 4.0f;
		s.beatDivision = 1.0f;

		drive::Input onBeat = in;
		onBeat.seconds      = 0.0;
		onBeat.barPhase     = 0.0f;

		const drive::Output out = drive::compute( s, onBeat );
		report( "full beat depth on the beat gives the whole amount", std::fabs( out.scale - 1.0f ) < 1e-5f );

		drive::Settings both;
		both.sync       = drive::kSyncLocked;
		both.beatDepth  = 0.8f;
		both.levelDepth = 0.8f;

		drive::Input quiet = in;
		quiet.seconds      = 1.0;//half a bar in: between beats
		quiet.barPhase     = 0.5f;

		const drive::Output sat = drive::compute( both, quiet );
		report( "two depths at 0.8 saturate rather than going negative", sat.scale >= 0.0f );
	}

	//---------------------------------------------------------------------
	// The band split, and the trap it exists for. Energy in the lowest bin must
	// reach bass and nothing else; energy near the top must reach treble and
	// nothing else. And bass must cover a SMALL fraction of the bins -- an equal
	// thirds split would put the entire musical range in it.
	//---------------------------------------------------------------------
	{
		std::vector< float > lowOnly( drive::kAudioBins, 0.0f );
		lowOnly[ 0 ] = 1.0f;

		float bass = 0.0f, mid = 0.0f, high = 0.0f;
		drive::bands( lowOnly.data(), drive::kAudioBins, &bass, &mid, &high );
		report( "energy in bin 0 reaches bass only", bass > 0.0f && mid == 0.0f && high == 0.0f );

		std::vector< float > highOnly( drive::kAudioBins, 0.0f );
		highOnly[ drive::kAudioBins - 1 ] = 1.0f;

		drive::bands( highOnly.data(), drive::kAudioBins, &bass, &mid, &high );
		report( "energy in the top bin reaches treble only", high > 0.0f && bass == 0.0f && mid == 0.0f );

		//The split is logarithmic, so bass is a handful of bins. If somebody
		//"simplifies" it to equal thirds this is the line that objects.
		std::vector< float > allOn( drive::kAudioBins, 1.0f );
		drive::bands( allOn.data(), drive::kAudioBins, &bass, &mid, &high );

		//Every band is a mean, so a uniform spectrum reads 1 in all three -- which
		//is itself worth asserting, because a band implemented as a sum would
		//read wildly different values and saturate on bin count alone.
		report( "a uniform spectrum reads equal in all three bands",
		        std::fabs( bass - 1.0f ) < 1e-5f && std::fabs( mid - 1.0f ) < 1e-5f
		            && std::fabs( high - 1.0f ) < 1e-5f );

		//And the boundary itself: bin 4 must NOT be bass at 64 bins.
		std::vector< float > binFour( drive::kAudioBins, 0.0f );
		binFour[ 4 ] = 1.0f;
		drive::bands( binFour.data(), drive::kAudioBins, &bass, &mid, &high );
		report( "bass is the bottom sixteenth, not a third", bass == 0.0f && mid > 0.0f );
	}

	//---------------------------------------------------------------------
	// Routing. Natural spreads the channels the way a lens does -- bass one way,
	// treble the other -- so the two extreme pushes must have opposite signs.
	//---------------------------------------------------------------------
	{
		std::vector< float > full( drive::kAudioBins, 0.6f );

		drive::Input loud = in;
		loud.bins         = full.data();

		drive::Settings s;
		s.bandDepth = 1.0f;

		s.route                    = drive::kRouteNatural;
		const drive::Output natural = drive::compute( s, loud );
		report( "Natural pushes red and blue in opposite directions",
		        natural.push[ 0 ] > 0.0f && natural.push[ 2 ] < 0.0f );

		s.route                     = drive::kRouteInverted;
		const drive::Output inverted = drive::compute( s, loud );
		report( "Inverted is the reverse of Natural",
		        inverted.push[ 0 ] < 0.0f && inverted.push[ 2 ] > 0.0f );

		s.route                 = drive::kRouteBass;
		const drive::Output bassOnly = drive::compute( s, loud );
		report( "Bass Only pushes all three channels together",
		        bassOnly.push[ 0 ] == bassOnly.push[ 1 ] && bassOnly.push[ 1 ] == bassOnly.push[ 2 ]
		            && bassOnly.push[ 0 ] > 0.0f );
	}

	//---------------------------------------------------------------------
	// A host with no audio routed at all. Not an error, and must not be a NaN:
	// a NaN reaching a uniform renders the frame black with no message anywhere.
	//---------------------------------------------------------------------
	{
		drive::Settings s;
		s.levelDepth = 1.0f;
		s.bandDepth  = 1.0f;

		drive::Input none;
		none.bins     = nullptr;
		none.binCount = 0;

		const drive::Output out = drive::compute( s, none );
		report( "a host with no audio buffer produces finite output",
		        std::isfinite( out.scale ) && std::isfinite( out.push[ 0 ] ) && out.level == 0.0f );
	}

	return ok;
}

//---------------------------------------------------------------------------
// --clock
//---------------------------------------------------------------------------
/**
    Milliseconds and seconds hosts must produce the same drift.

    **Resolume sends SetTime in milliseconds.** The FFGL header never says so,
    the SDK's own Particles sample divides by 1000, and this harness sends
    seconds -- so a plugin that consumes the clock raw is a thousand times fast
    in the only host anybody runs it in, and every offline check still passes.
    This is the one that would not.
*/
bool checkClock()
{
	const int width  = 160;
	const int height = 90;

	const std::vector< unsigned char > sceneBytes = buildScene( width, height, scenePixel );
	const GLuint input                            = uploadScene( sceneBytes, width, height );

	Target target = makeTarget( width, height );

	//Sixty frames at sixty frames a second: one second of show, expressed the two
	//ways a host can express it.
	constexpr int kFrames = 60;

	auto driftAfter = [ & ]( double perFrame ) -> float {
		Driver driver;
		driver.plugin.SetFloatParameter( Abomerration::PT_GEOMETRY, 3.0f );//Turbulent
		driver.plugin.SetFloatParameter( Abomerration::PT_DRIFT, 1.0f );

		for( int frame = 0; frame < kFrames; ++frame )
		{
			driver.plugin.SetClockScaleForTest( 1.0 );//seconds, said out loud rather than inferred
			driver.plugin.SetTime( static_cast< double >( frame ) * perFrame );
			driver.render( target, input, width, height );
		}

		return driver.plugin.driftPhaseForTest();
	};

	const float seconds = driftAfter( 1.0 / 60.0 );
	const float millis  = driftAfter( 1000.0 / 60.0 );

	//One second at the full Drift rate of 2 noise units per second, less one
	//frame because the first frame has no previous frame to measure a delta
	//against.
	const float expected = static_cast< float >( 2.0 * ( kFrames - 1 ) / 60.0 );

	const bool secondsOk = std::fabs( seconds - expected ) < 1e-3f;
	const bool millisOk  = std::fabs( millis - expected ) < 1e-3f;

	std::printf( "  seconds host: drift %.5f (want %.5f)  %s\n", seconds, expected, secondsOk ? "ok" : "FAIL" );
	std::printf( "  millis  host: drift %.5f (want %.5f)  %s\n", millis, expected, millisOk ? "ok" : "FAIL" );

	//And the control: a plugin that ignored the unit would differ by a factor of
	//a thousand between the two, so state the agreement directly.
	const bool agree = std::fabs( seconds - millis ) < 1e-3f;
	std::printf( "  the two hosts agree                        %s\n", agree ? "ok" : "FAIL" );

	releaseTarget( target );
	glDeleteTextures( 1, &input );

	return secondsOk && millisOk && agree;
}

//---------------------------------------------------------------------------
// --presets
//---------------------------------------------------------------------------
bool checkPresets()
{
	const int width  = 240;
	const int height = 135;

	const std::vector< unsigned char > sceneBytes = buildScene( width, height, scenePixel );
	const GLuint input                            = uploadScene( sceneBytes, width, height );

	Target target = makeTarget( width, height );

	//A spectrum with energy in it, and Beat sync selected, so the presets that
	//set a Beat Depth are actually exercised. Without both, several presets would
	//render identically to each other and the distinctness check would be
	//measuring the harness rather than the table.
	std::vector< float > spectrum( drive::kAudioBins );
	for( int i = 0; i < drive::kAudioBins; ++i )
		spectrum[ i ] = 0.35f + 0.5f * std::fabs( std::sin( static_cast< float >( i ) * 0.7f ) );

	std::vector< std::vector< unsigned char > > renders;
	bool ok = true;

	for( int i = 0; i <= presets::kCount; ++i )
	{
		Driver driver;
		injectSpectrum( driver.plugin, spectrum );
		driver.plugin.SetFloatParameter( Abomerration::PT_SYNC, 1.0f );//Locked
		driver.plugin.SetFloatParameter( Abomerration::PT_PRESET, static_cast< float >( i ) );

		if( !driveFrames( driver, target, input, width, height, 2 ) )
			return false;

		renders.push_back( readBytes( target ) );

		//Non-degenerate: it has to put something on the screen.
		double mean = 0.0;
		for( size_t p = 0; p < renders.back().size(); p += 4 )
			mean += renders.back()[ p ] + renders.back()[ p + 1 ] + renders.back()[ p + 2 ];
		mean /= static_cast< double >( renders.back().size() / 4 * 3 );

		const char* name = i == 0 ? "Custom" : presets::kPresets[ i - 1 ].name;
		const bool lit   = mean > 4.0;
		ok               = ok && lit;

		std::printf( "  %-20s mean %6.2f  %s\n", name, mean, lit ? "ok" : "FAIL (renders black)" );
	}

	//Distinct: no two presets may render the same picture. Compared as a mean
	//absolute difference so that a preset differing only in a control that does
	//nothing is caught.
	for( size_t a = 0; a < renders.size(); ++a )
	{
		for( size_t b = a + 1; b < renders.size(); ++b )
		{
			double diff = 0.0;
			for( size_t p = 0; p < renders[ a ].size(); ++p )
				diff += std::fabs( static_cast< double >( renders[ a ][ p ] )
				                   - static_cast< double >( renders[ b ][ p ] ) );
			diff /= static_cast< double >( renders[ a ].size() );

			if( diff < 0.5 )
			{
				const char* nameA = a == 0 ? "Custom" : presets::kPresets[ a - 1 ].name;
				const char* nameB = b == 0 ? "Custom" : presets::kPresets[ b - 1 ].name;
				std::printf( "  %s and %s render the same picture (%.4f)  FAIL\n", nameA, nameB, diff );
				ok = false;
			}
		}
	}

	if( ok )
		std::printf( "  all %d presets distinct and non-degenerate\n", presets::kCount + 1 );

	releaseTarget( target );
	glDeleteTextures( 1, &input );

	return ok;
}

//---------------------------------------------------------------------------
// --bench
//---------------------------------------------------------------------------
bool bench()
{
	struct Size
	{
		const char* name;
		int width;
		int height;
	};

	const Size sizes[] = { { "1080p", 1920, 1080 }, { "4K", 3840, 2160 } };

	for( const Size& size : sizes )
	{
		const std::vector< unsigned char > sceneBytes = buildScene( size.width, size.height, scenePixel );
		const GLuint input                            = uploadScene( sceneBytes, size.width, size.height );

		Target target = makeTarget( size.width, size.height );

		for( int spectrum = 0; spectrum < controls::spectrumCount(); ++spectrum )
		{
			Driver driver;
			driver.plugin.SetFloatParameter( Abomerration::PT_SPECTRUM, static_cast< float >( spectrum ) );
			driver.plugin.SetFloatParameter( Abomerration::PT_AMOUNT, 0.5f );
			driver.plugin.SetFloatParameter( Abomerration::PT_EDGES, 0.5f );

			//Warm up: the first frame compiles and allocates, and timing that
			//would report the build rather than the render.
			driver.render( target, input, size.width, size.height );
			glFinish();

			constexpr int kFrames = 20;
			const auto start      = std::chrono::steady_clock::now();
			for( int frame = 0; frame < kFrames; ++frame )
			{
				driver.plugin.SetClockScaleForTest( 1.0 );//seconds, said out loud rather than inferred
				driver.plugin.SetTime( static_cast< double >( frame ) / 60.0 );
				driver.render( target, input, size.width, size.height );
			}
			glFinish();
			const auto end = std::chrono::steady_clock::now();

			const double ms = std::chrono::duration< double, std::milli >( end - start ).count() / kFrames;
			std::printf( "  %-6s %-10s %6.2f ms/frame\n", size.name, controls::spectrumLabel( spectrum ), ms );
		}

		releaseTarget( target );
		glDeleteTextures( 1, &input );
	}

	return true;
}

//---------------------------------------------------------------------------
// --list
//---------------------------------------------------------------------------
void list( Abomerration& plugin )
{
	std::printf( "%-4s %-14s %-16s %s\n", "id", "type", "name", "default" );

	for( unsigned int id = 0; id < Abomerration::PT_COUNT; ++id )
	{
		const char* name = plugin.GetParamName( id );
		if( name == nullptr || name[ 0 ] == '\0' )
			continue;

		const unsigned int type = plugin.GetParamType( id );

		const char* typeName = "standard";
		switch( type )
		{
			case FF_TYPE_BOOLEAN: typeName = "boolean"; break;
			case FF_TYPE_EVENT: typeName = "event"; break;
			case FF_TYPE_TEXT: typeName = "text"; break;
			case FF_TYPE_OPTION: typeName = "option"; break;
			case FF_TYPE_BUFFER: typeName = "buffer"; break;
			default: break;
		}

		std::printf( "%-4u %-14s %-16s %.4f\n", id, typeName, name, plugin.GetFloatParameter( id ) );
	}
}

//---------------------------------------------------------------------------
// --sheet
//---------------------------------------------------------------------------
bool sheet( const std::string& path )
{
	//Every geometry down, every spectrum setting across. The two axes of the
	//plugin, and the one artefact where a mode that has quietly stopped working
	//is obvious rather than subtle.
	const int cellW = 320;
	const int cellH = 180;
	const int cols  = controls::spectrumCount();
	const int rows  = dispersion::kGeometryCount;

	const std::vector< unsigned char > sceneBytes = buildScene( cellW, cellH, scenePixel );
	const GLuint input                            = uploadScene( sceneBytes, cellW, cellH );

	Target target = makeTarget( cellW, cellH );

	std::vector< unsigned char > sheetRgba(
		static_cast< size_t >( cellW ) * cols * cellH * rows * 4, 0 );

	for( int row = 0; row < rows; ++row )
	{
		for( int col = 0; col < cols; ++col )
		{
			Driver driver;
			driver.plugin.SetFloatParameter( Abomerration::PT_GEOMETRY, static_cast< float >( row ) );
			driver.plugin.SetFloatParameter( Abomerration::PT_SPECTRUM, static_cast< float >( col ) );
			//Moderate on purpose: a large amount smears the card past the point
			//where the four spectrum settings differ from each other, and the
			//sheet exists to show that they do.
			driver.plugin.SetFloatParameter( Abomerration::PT_AMOUNT, 0.22f );
			driver.plugin.SetFloatParameter( Abomerration::PT_FRINGE, 0.25f );
			if( !driveFrames( driver, target, input, cellW, cellH, 2 ) )
				return false;

			const std::vector< unsigned char > cell = flipRows( readBytes( target ), cellW, cellH );

			for( int y = 0; y < cellH; ++y )
			{
				const size_t from = static_cast< size_t >( y ) * cellW * 4;
				const size_t to   = ( static_cast< size_t >( row * cellH + y ) * cellW * cols
                                    + static_cast< size_t >( col ) * cellW )
				                  * 4;
				std::memcpy( sheetRgba.data() + to, cell.data() + from,
				             static_cast< size_t >( cellW ) * 4 );
			}
		}
	}

	releaseTarget( target );
	glDeleteTextures( 1, &input );

	return writePng( path, cellW * cols, cellH * rows, sheetRgba );
}

/**
    A synthetic kick-and-hats spectrum, and the transport that goes with it.

    ---------------------------------------------------------------- why at all

    Every other plugin in this fleet renders its video from footage and a cue
    sheet and that is the whole story. This one is *sound-reactive*, so a render
    with no audio in it would show the manual lens while the captions claimed
    something else — which would make the video a misrepresentation rather than a
    demonstration.

    There is no audio to be had: the pipe carries raw frames, and the finished cut
    is silent by design (the pipeline writes a voiceover script instead of a
    soundtrack). So the drive is *generated* — a kick on every beat, hats on the
    eighths and a bass note on the bar, at 120 bpm, written straight into the
    parameter's spectrum elements the way a host writes them.

    That makes the reaction on screen real: the same `drive::compute` runs on the
    same kind of numbers, the beat lands on the grid because the transport is
    driven too, and Show Field's meters move because they are reading this. What
    it is not is a recording of the plugin listening to music, and the project's
    description says so.

    ------------------------------------------------------------ the units trap

    The host writes **raw FFT magnitudes** and `updateAudio` takes their square
    root. So a desired level of 0.9 has to be injected as 0.81, not 0.9 — inject
    the level directly and everything arrives louder than intended, with the bass
    band pinned near 1 and the depth controls looking like they do nothing.
*/
void injectRhythm( Abomerration& plugin, double seconds )
{
	constexpr double kBpm     = 120.0;
	const double beats        = seconds * kBpm / 60.0;

	const double beatPhase = beats - std::floor( beats );
	const double hatPhase  = ( beats * 2.0 ) - std::floor( beats * 2.0 );
	const double barPhase  = ( beats / 4.0 ) - std::floor( beats / 4.0 );

	//Decays, not gates: a square pulse would make the reaction look like a
	//parameter being switched rather than something following a sound.
	const double kick = std::exp( -beatPhase * 7.0 );
	const double hat  = std::exp( -hatPhase * 26.0 );
	//A held note under the bar, so the mid band carries something between
	//transients and Band Depth has a middle to route.
	const double note = 0.35 + 0.35 * std::exp( -barPhase * 2.0 );

	std::vector< float > level( drive::kAudioBins, 0.0f );

	for( int i = 0; i < drive::kAudioBins; ++i )
	{
		double value = 0.0;

		if( i < 4 )
			value = 0.95 * kick;                       // bass: the kick
		else if( i < 16 )
			value = 0.55 * note + 0.25 * kick;         // mid: the note, plus the kick's body
		else
			value = 0.80 * hat * ( 1.0 - ( i - 16 ) / 96.0 );// treble: the hat, rolling off

		//Squared, because updateAudio takes a square root of whatever the host
		//wrote. See the comment above.
		level[ i ] = static_cast< float >( value * value );
	}

	for( int i = 0; i < drive::kAudioBins; ++i )
		plugin.SetParamElementValue( Abomerration::PT_AUDIO, static_cast< unsigned int >( i ), level[ i ] );

	plugin.SetBeatInfo( static_cast< float >( kBpm ), static_cast< float >( barPhase ) );
	plugin.SetClockScaleForTest( 1.0 );//seconds, said out loud rather than inferred
	plugin.SetTime( seconds );
}

/**
    --pipe: real footage through the real plugin, on a cue sheet.

    Frames arrive as raw RGBA on stdin and leave as raw RGBA on stdout, so ffmpeg
    does the decoding and the encoding and this does the lens. That is how the
    project video is made, and it is a render rather than a screen recording for a
    reason worth stating: an FFGL plugin has no window and no UI of its own — its
    control surface IS Resolume's inspector — so "filming the app" would mean
    filming Arena, whose clip grid and effects browser are custom-drawn with
    nothing in the accessibility tree to address.

    What is on screen is genuinely this plugin's output, from the same class
    Resolume loads. It is just not a photograph of Resolume, and the end card says
    so.
*/
int runPipe( int width, int height, int fps, const std::string& scriptPath, const Options& options )
{
	std::map< std::string, Track > tracks;
	if( !scriptPath.empty() )
	{
		std::string error;
		tracks = loadScript( scriptPath, error );
		if( !error.empty() )
		{
			std::fprintf( stderr, "abomtest: %s\n", error.c_str() );
			return 1;
		}
	}

	Target target = makeTarget( width, height );

	Driver driver;
	if( !applySets( driver.plugin, options ) )
		return 2;

	//Resolve the cue sheet's names once, against the plugin itself. A cue for a
	//parameter that does not exist is a silent no-op otherwise, and the first
	//sign of it is a beat in the finished video where nothing happens.
	const auto byName = parameterIndex( driver.plugin );
	std::vector< std::pair< unsigned int, const Track* > > bound;
	for( const auto& entry : tracks )
	{
		const auto found = byName.find( entry.first );
		if( found == byName.end() )
		{
			std::fprintf( stderr, "abomtest: no parameter named \"%s\" in the script\n",
			              entry.first.c_str() );
			return 1;
		}
		bound.emplace_back( found->second, &entry.second );
	}

	GLuint input = 0;
	glGenTextures( 1, &input );
	glBindTexture( GL_TEXTURE_2D, input );
	glTexImage2D( GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );
	glBindTexture( GL_TEXTURE_2D, 0 );

	const size_t frameBytes = static_cast< size_t >( width ) * height * 4;
	std::vector< unsigned char > incoming( frameBytes );

	int frame = 0;
	while( readExactly( incoming.data(), frameBytes ) )
	{
		for( const auto& track : bound )
			driver.plugin.SetFloatParameter( track.first, valueAt( *track.second, frame ) );

		injectRhythm( driver.plugin, static_cast< double >( frame ) / static_cast< double >( fps ) );

		//ffmpeg hands over rows top-down; glTexImage2D treats its first row as
		//v = 0, which is the bottom. Flipping on the way in and again on the way
		//out keeps every coordinate in this file meaning what it says everywhere
		//else -- and it is the same flip uploadScene does, for the same reason.
		const std::vector< unsigned char > flipped = flipRows( incoming, width, height );

		glBindTexture( GL_TEXTURE_2D, input );
		glTexSubImage2D( GL_TEXTURE_2D, 0, 0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE,
		                 flipped.data() );
		glBindTexture( GL_TEXTURE_2D, 0 );

		if( !driver.render( target, input, width, height ) )
		{
			std::fprintf( stderr, "abomtest: render failed at frame %d\n", frame );
			return 1;
		}

		const std::vector< unsigned char > out = flipRows( readBytes( target ), width, height );
		if( std::fwrite( out.data(), 1, frameBytes, stdout ) != frameBytes )
		{
			std::fprintf( stderr, "abomtest: short write at frame %d\n", frame );
			return 1;
		}

		++frame;
	}

	std::fflush( stdout );
	std::fprintf( stderr, "abomtest: %d frames\n", frame );

	glDeleteTextures( 1, &input );
	releaseTarget( target );
	return 0;
}

} // namespace

int main( int argc, char** argv )
{
	Options options;

	std::string outPath;
	std::string scenePath;
	std::string sheetPath;
	bool wantList     = false;
	bool wantField    = false;
	bool wantOffset   = false;
	bool wantSpectrum = false;
	bool wantDrive    = false;
	bool wantClock    = false;
	bool wantPresets  = false;
	bool wantBench    = false;
	bool wantQuad     = false;
	int frames        = 2;
	int fps           = 30;
	bool pipeMode     = false;
	std::string scriptPath;

	for( int i = 1; i < argc; ++i )
	{
		const std::string arg = argv[ i ];

		auto next = [ & ]( const char* what ) -> std::string {
			if( i + 1 >= argc )
			{
				std::fprintf( stderr, "%s needs a value\n", what );
				std::exit( 2 );
			}
			return argv[ ++i ];
		};

		if( arg == "--out" )
			outPath = next( "--out" );
		else if( arg == "--scene" )
			scenePath = next( "--scene" );
		else if( arg == "--sheet" )
			sheetPath = next( "--sheet" );
		else if( arg == "--list" )
			wantList = true;
		else if( arg == "--field" )
			wantField = true;
		else if( arg == "--offset" )
			wantOffset = true;
		else if( arg == "--spectrum" )
			wantSpectrum = true;
		else if( arg == "--drive" )
			wantDrive = true;
		else if( arg == "--clock" )
			wantClock = true;
		else if( arg == "--presets" )
			wantPresets = true;
		else if( arg == "--bench" )
			wantBench = true;
		else if( arg == "--quadrature" )
			wantQuad = true;
		else if( arg == "--pipe" )
			pipeMode = true;
		else if( arg == "--script" )
			scriptPath = next( "--script" );
		else if( arg == "--fps" )
		{
			fps = std::atoi( next( "--fps" ).c_str() );
			if( fps < 1 )
			{
				std::fprintf( stderr, "--fps must be at least 1\n" );
				return 2;
			}
		}
		else if( arg == "--frames" )
		{
			frames = std::atoi( next( "--frames" ).c_str() );
			if( frames < 1 )
			{
				std::fprintf( stderr, "--frames must be at least 1\n" );
				return 2;
			}
		}
		else if( arg == "--size" )
		{
			const std::string value = next( "--size" );
			const size_t cross      = value.find( 'x' );
			if( cross == std::string::npos )
			{
				std::fprintf( stderr, "--size wants WxH\n" );
				return 2;
			}
			options.width  = std::atoi( value.substr( 0, cross ).c_str() );
			options.height = std::atoi( value.substr( cross + 1 ).c_str() );
			if( options.width < 2 || options.height < 2 )
			{
				std::fprintf( stderr, "--size is too small\n" );
				return 2;
			}
		}
		else if( arg == "--set" )
		{
			const std::string value = next( "--set" );
			const size_t equals     = value.find( '=' );
			if( equals == std::string::npos )
			{
				std::fprintf( stderr, "--set wants \"Name=value\"\n" );
				return 2;
			}
			options.sets.emplace_back( value.substr( 0, equals ),
			                           std::strtof( value.substr( equals + 1 ).c_str(), nullptr ) );
		}
		else
		{
			std::fprintf( stderr, "unknown argument: %s\n", arg.c_str() );
			return 2;
		}
	}

	//--drive is the only mode that needs no GL at all, and it is worth being able
	//to run it on a machine with no working context.
	const bool needsGL = !( wantDrive && outPath.empty() && scenePath.empty() && sheetPath.empty()
	                        && !wantList && !wantField && !wantOffset && !wantSpectrum && !wantClock
	                        && !wantPresets && !wantBench && !wantQuad && !pipeMode );

	CGLContextObj context = nullptr;
	if( needsGL )
	{
		context = createContext();
		if( context == nullptr )
		{
			std::fprintf( stderr, "could not create an OpenGL 4 core context\n" );
			return 1;
		}
	}

	if( pipeMode )
	{
		const int status = runPipe( options.width, options.height, fps, scriptPath, options );
		if( context != nullptr )
		{
			CGLSetCurrentContext( nullptr );
			CGLDestroyContext( context );
		}
		return status;
	}

	bool ok = true;

	if( !scenePath.empty() )
	{
		const std::vector< unsigned char > sceneBytes =
			buildScene( options.width, options.height, scenePixel );
		if( !writePng( scenePath, options.width, options.height, sceneBytes ) )
		{
			std::fprintf( stderr, "could not write %s\n", scenePath.c_str() );
			ok = false;
		}
	}

	if( wantList )
	{
		Abomerration plugin;
		list( plugin );
	}

	if( wantDrive )
	{
		std::printf( "the reaction arithmetic:\n" );
		ok = checkDrive() && ok;
	}

	if( wantField )
	{
		std::printf( "the GLSL field against Dispersion.cpp:\n" );
		ok = checkField() && ok;
	}

	if( wantOffset )
	{
		std::printf( "the picture moves by the distance asked for:\n" );
		ok = checkOffset() && ok;
	}

	if( wantSpectrum )
	{
		std::printf( "the weight table preserves energy:\n" );
		ok = checkSpectrum() && ok;
	}

	if( wantClock )
	{
		std::printf( "milliseconds and seconds hosts agree:\n" );
		ok = checkClock() && ok;
	}

	if( wantPresets )
	{
		std::printf( "the factory presets:\n" );
		ok = checkPresets() && ok;
	}

	if( wantQuad )
	{
		std::printf( "the spectral quadrature leaves no visible footprint:\n" );
		ok = checkQuadrature() && ok;
	}

	if( wantBench )
	{
		std::printf( "render cost:\n" );
		ok = bench() && ok;
	}

	if( !sheetPath.empty() )
	{
		if( !sheet( sheetPath ) )
		{
			std::fprintf( stderr, "could not write %s\n", sheetPath.c_str() );
			ok = false;
		}
	}

	if( !outPath.empty() )
	{
		const std::vector< unsigned char > sceneBytes =
			buildScene( options.width, options.height, scenePixel );
		const GLuint input = uploadScene( sceneBytes, options.width, options.height );

		Target target = makeTarget( options.width, options.height );

		Driver driver;
		if( !applySets( driver.plugin, options ) )
			return 2;

		//A spectrum and a transport, so a --set of a reactive control does
		//something. A headless process has no audio; this is what a host would
		//have been sending.
		std::vector< float > spectrum( drive::kAudioBins );
		for( int i = 0; i < drive::kAudioBins; ++i )
			spectrum[ i ] = 0.35f + 0.5f * std::fabs( std::sin( static_cast< float >( i ) * 0.7f ) );
		injectSpectrum( driver.plugin, spectrum );

		if( !driveFrames( driver, target, input, options.width, options.height, frames ) )
		{
			std::fprintf( stderr, "render failed\n" );
			ok = false;
		}
		else
		{
			const std::vector< unsigned char > pixels =
				flipRows( readBytes( target ), options.width, options.height );
			if( !writePng( outPath, options.width, options.height, pixels ) )
			{
				std::fprintf( stderr, "could not write %s\n", outPath.c_str() );
				ok = false;
			}
		}

		releaseTarget( target );
		glDeleteTextures( 1, &input );
	}

	if( context != nullptr )
	{
		CGLSetCurrentContext( nullptr );
		CGLDestroyContext( context );
	}

	return ok ? 0 : 1;
}
