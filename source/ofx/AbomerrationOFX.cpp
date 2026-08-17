/// The OpenFX build of Abomerration, for DaVinci Resolve, Nuke, Natron, Vegas
/// and other OFX hosts.
///
/// Same lens as the FFGL build. The dispersion field lives once, in
/// Dispersion.cpp, the spectral weight table lives once in the same file, and
/// every parameter curve lives once in Controls.cpp -- this file LINKS all three
/// rather than copying them, which is what stops a preset meaning one thing in
/// Resolume and another in Resolve.
///
/// So unlike most of the fleet's OFX builds, the *interesting* arithmetic here is
/// not mirrored at all. What IS mirrored is the per-pixel machinery the GPU did
/// per fragment: the Sobel edge weight, the mip prefilter, the spectral sum, the
/// fringe boost and the mix. When editing Edge.cpp or Disperse.cpp, edit this too.
///
/// ------------------------------------------------------- what OFX cannot do
///
/// **There is no reaction here, and the reactive controls are not present.** OFX
/// has no beat information and no FFT buffer -- neither is in the API -- so Beat
/// Depth, Level Depth, Band Depth, Band Route, Division, Decay and Sync are all
/// omitted from the OFX inspector rather than shown doing nothing. A dead control
/// that looks live is worse than an absent one, and a host cannot supply what the
/// API has no way to express.
///
/// `Drive.cpp` is still linked and still called, with a zeroed input, because
/// that is what makes this the same arithmetic rather than a reduced copy of it:
/// it returns scale 1 and no push, and the manual lens falls out of the general
/// case. Presets that set a reactive control quietly skip it here -- see
/// `kPresetParamNames`.
///
/// ------------------------------------------------------------ the one departure
///
/// **Drift is `time * rate`, not integrated.** The FFGL build integrates the rate
/// so that moving the control changes what happens next and nothing else; OFX
/// hosts render frames in any order and may render the same frame twice, so there
/// is no previous frame to integrate from. The consequence is the one the FFGL
/// build avoids: moving Drift rewrites the whole history and the noise field
/// jumps. Unavoidable in this API, and the same departure tinsel documents.

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <vector>

#include "ofxsImageEffect.h"
#include "ofxsProcessing.h"

#include "../Controls.h"
#include "../Dispersion.h"
#include "../Drive.h"
#include "../Presets.h"

namespace
{
constexpr const char* kPluginIdentifier = "com.stoatworks.abomerration";
constexpr const char* kPluginName       = "Abomerration";
constexpr const char* kPluginGrouping   = "Stoatworks";
constexpr const char* kPluginDescription =
	"Chromatic aberration, taken well past what glass can do.\n\n"
	"A lens does not split a picture into three channels -- it smears the whole "
	"spectrum along a path and the sensor adds it up. This displaces the picture "
	"once per wavelength and integrates through spectral weights, so one control "
	"spans a cheap hard channel offset and a real prismatic fringe. Four "
	"geometries: radial like a real uncorrected lens, linear like a prism, "
	"tangential like nothing at all, and a drifting turbulent field.\n\n"
	"The sound-reactive controls are FFGL only -- OFX has no beat information and "
	"no audio -- so this build is the manual lens. The Resolume version reacts.\n\n"
	"https://stoatworks-labs.com";

constexpr const char* kParamPreset     = "preset";
constexpr const char* kParamGeometry   = "geometry";
constexpr const char* kParamAmount     = "amount";
constexpr const char* kParamCentreX    = "centreX";
constexpr const char* kParamCentreY    = "centreY";
constexpr const char* kParamAngle      = "angle";
constexpr const char* kParamFalloff    = "falloff";
constexpr const char* kParamSpectrum   = "spectrum";
constexpr const char* kParamTurbulence = "turbulence";
constexpr const char* kParamDrift      = "drift";
constexpr const char* kParamRedPush    = "redPush";
constexpr const char* kParamGreenPush  = "greenPush";
constexpr const char* kParamBluePush   = "bluePush";
constexpr const char* kParamEdges      = "edges";
constexpr const char* kParamFringe     = "fringe";
constexpr const char* kParamShowField  = "showField";
constexpr const char* kParamMix        = "mix";

/// The preset table is host-agnostic; this is the OFX binding of it, in
/// presets::Param order. Same job as the FFGL build's kPresetParamIDs.
///
/// **The nulls are deliberate.** Six of the eighteen parameters a preset sets are
/// reactive, and this build has no reactive parameters to set -- see the header.
/// A null means "this build has nowhere to put it", and applying a preset skips
/// it. The alternative was defining the controls anyway so the array could be
/// dense, which would put seven permanently dead sliders in somebody's inspector
/// to keep a table tidy.
const char* const kPresetParamNames[ abomerration::presets::kParamCount ] = {
	kParamGeometry, kParamAmount, kParamAngle, kParamFalloff, kParamSpectrum,
	kParamTurbulence, kParamDrift, kParamRedPush, kParamGreenPush, kParamBluePush,
	nullptr /*beat depth*/, nullptr /*beat decay*/, nullptr /*division*/,
	nullptr /*level depth*/, nullptr /*band depth*/, nullptr /*route*/,
	kParamEdges, kParamFringe
};

struct Rgba
{
	float r = 0.0f, g = 0.0f, b = 0.0f, a = 0.0f;
};

float clamp01f( float x )
{
	return x < 0.0f ? 0.0f : ( x > 1.0f ? 1.0f : x );
}

float lumaOf( float r, float g, float b )
{
	return 0.2126f * r + 0.7152f * g + 0.0722f * b;
}

/**
    The source picture, un-premultiplied, plus its mip pyramid.

    ------------------------------------------------------------------- why mips

    Because the GPU has them, and the two builds have to agree. The dispersion
    takes a handful of point samples along a path; once they are further apart than
    a pixel they skip the picture between them, so each sample reads from the level
    covering twice the gap to its neighbour -- see Copy.cpp for the measurements
    and the factor of two.

    A CPU build could have approximated that with a box blur per sample, and it
    would have been both slower and *different*. Building the same pyramid and
    interpolating between levels the same way is what makes the OFX render match
    the FFGL one rather than merely resemble it.

    Levels halve until one axis reaches a pixel. Each is a plain 2x2 box average of
    the one above, which is what `glGenerateMipmap` specifies.
*/
struct Source
{
	struct Level
	{
		int width  = 0;
		int height = 0;
		std::vector< Rgba > pixels;
	};

	std::vector< Level > levels;

	int width() const
	{
		return levels.empty() ? 0 : levels[ 0 ].width;
	}

	int height() const
	{
		return levels.empty() ? 0 : levels[ 0 ].height;
	}

	/// Bilinear, in picture space with **v down**, clamped half a texel inside.
	/// Matches `texture()`: texel centres sit at (i + 0.5) / size, so the sample
	/// position in texel units is uv * size - 0.5, and dropping that half texel
	/// shifts everything by half a pixel.
	Rgba bilinear( int level, float u, float v ) const
	{
		const Level& l = levels[ std::clamp( level, 0, static_cast< int >( levels.size() ) - 1 ) ];

		const float halfU = 0.5f / static_cast< float >( l.width );
		const float halfV = 0.5f / static_cast< float >( l.height );

		const float cu = std::clamp( u, halfU, 1.0f - halfU );
		const float cv = std::clamp( v, halfV, 1.0f - halfV );

		const float fx = cu * static_cast< float >( l.width ) - 0.5f;
		const float fy = cv * static_cast< float >( l.height ) - 0.5f;

		const int x0   = static_cast< int >( std::floor( fx ) );
		const int y0   = static_cast< int >( std::floor( fy ) );
		const float tx = fx - static_cast< float >( x0 );
		const float ty = fy - static_cast< float >( y0 );

		auto at = [ & ]( int x, int y ) -> const Rgba& {
			return l.pixels[ static_cast< size_t >( std::clamp( y, 0, l.height - 1 ) ) * l.width
			                 + std::clamp( x, 0, l.width - 1 ) ];
		};

		const Rgba& a = at( x0, y0 );
		const Rgba& b = at( x0 + 1, y0 );
		const Rgba& c = at( x0, y0 + 1 );
		const Rgba& d = at( x0 + 1, y0 + 1 );

		Rgba out;
		out.r = ( a.r * ( 1.0f - tx ) + b.r * tx ) * ( 1.0f - ty ) + ( c.r * ( 1.0f - tx ) + d.r * tx ) * ty;
		out.g = ( a.g * ( 1.0f - tx ) + b.g * tx ) * ( 1.0f - ty ) + ( c.g * ( 1.0f - tx ) + d.g * tx ) * ty;
		out.b = ( a.b * ( 1.0f - tx ) + b.b * tx ) * ( 1.0f - ty ) + ( c.b * ( 1.0f - tx ) + d.b * tx ) * ty;
		out.a = ( a.a * ( 1.0f - tx ) + b.a * tx ) * ( 1.0f - ty ) + ( c.a * ( 1.0f - tx ) + d.a * tx ) * ty;
		return out;
	}

	/// Trilinear: bilinear within two levels and linear between them, which is
	/// what GL_LINEAR_MIPMAP_LINEAR does and therefore what `textureLod` in the
	/// disperse pass is doing.
	Rgba trilinear( float lod, float u, float v ) const
	{
		const float top   = std::clamp( lod, 0.0f, static_cast< float >( levels.size() ) - 1.0f );
		const int lower   = static_cast< int >( std::floor( top ) );
		const float blend = top - static_cast< float >( lower );

		if( blend <= 0.0f )
			return bilinear( lower, u, v );

		const Rgba a = bilinear( lower, u, v );
		const Rgba b = bilinear( lower + 1, u, v );

		Rgba out;
		out.r = a.r + ( b.r - a.r ) * blend;
		out.g = a.g + ( b.g - a.g ) * blend;
		out.b = a.b + ( b.b - a.b ) * blend;
		out.a = a.a + ( b.a - a.a ) * blend;
		return out;
	}

	void buildMips()
	{
		while( levels.back().width > 1 || levels.back().height > 1 )
		{
			const Level& from = levels.back();

			Level to;
			to.width  = std::max( 1, from.width / 2 );
			to.height = std::max( 1, from.height / 2 );
			to.pixels.resize( static_cast< size_t >( to.width ) * to.height );

			for( int y = 0; y < to.height; ++y )
			{
				for( int x = 0; x < to.width; ++x )
				{
					//A 2x2 box of the level above, clamped so an odd size does not
					//read off the end. glGenerateMipmap on an odd dimension is
					//implementation defined; matching it exactly is not possible
					//and not worth chasing, because the disagreement is confined
					//to the last row or column of a reduced level.
					const int sx = std::min( x * 2, from.width - 1 );
					const int sy = std::min( y * 2, from.height - 1 );
					const int nx = std::min( sx + 1, from.width - 1 );
					const int ny = std::min( sy + 1, from.height - 1 );

					auto at = [ & ]( int px, int py ) -> const Rgba& {
						return from.pixels[ static_cast< size_t >( py ) * from.width + px ];
					};

					const Rgba& a = at( sx, sy );
					const Rgba& b = at( nx, sy );
					const Rgba& c = at( sx, ny );
					const Rgba& d = at( nx, ny );

					Rgba& out = to.pixels[ static_cast< size_t >( y ) * to.width + x ];
					out.r     = ( a.r + b.r + c.r + d.r ) * 0.25f;
					out.g     = ( a.g + b.g + c.g + d.g ) * 0.25f;
					out.b     = ( a.b + b.b + c.b + d.b ) * 0.25f;
					out.a     = ( a.a + b.a + c.a + d.a ) * 0.25f;
				}
			}

			levels.push_back( std::move( to ) );
		}
	}
};

/// Everything one render needs, in the physical units Dispersion.h and Controls.h
/// speak. Filled through the shared conversion, never here.
struct Settings
{
	abomerration::dispersion::Field field;
	abomerration::controls::Look look;
	float frameHeight = 1.0f;
};

//= mirrored: Edge.cpp
//
//Sobel rather than two central differences, and the reason is smoothness rather
//than accuracy: a two-tap gradient responds to a single pixel, so the weight
//field carries per-pixel detail and the picture shimmers whenever the source
//moves. The 0.25 is because Sobel's raw output for a black-to-white step is 4.
std::vector< float > buildEdges( const Source& source )
{
	const int width  = source.width();
	const int height = source.height();

	std::vector< float > out( static_cast< size_t >( width ) * height, 0.0f );

	const Source::Level& level = source.levels[ 0 ];

	auto luma = [ & ]( int x, int y ) {
		const Rgba& p = level.pixels[ static_cast< size_t >( std::clamp( y, 0, height - 1 ) ) * width
		                             + std::clamp( x, 0, width - 1 ) ];
		return lumaOf( p.r, p.g, p.b );
	};

	for( int y = 0; y < height; ++y )
	{
		for( int x = 0; x < width; ++x )
		{
			const float tl = luma( x - 1, y - 1 );
			const float tc = luma( x, y - 1 );
			const float tr = luma( x + 1, y - 1 );
			const float ml = luma( x - 1, y );
			const float mr = luma( x + 1, y );
			const float bl = luma( x - 1, y + 1 );
			const float bc = luma( x, y + 1 );
			const float br = luma( x + 1, y + 1 );

			const float gx = ( tr + 2.0f * mr + br ) - ( tl + 2.0f * ml + bl );
			const float gy = ( bl + 2.0f * bc + br ) - ( tl + 2.0f * tc + tr );

			out[ static_cast< size_t >( y ) * width + x ] =
				clamp01f( std::sqrt( gx * gx + gy * gy ) * 0.25f );
		}
	}

	return out;
}

class AbomerrationProcessorBase : public OFX::ImageProcessor
{
public:
	explicit AbomerrationProcessorBase( OFX::ImageEffect& instance ) :
		OFX::ImageProcessor( instance )
	{
	}

	void setup( const OFX::Image* source, const Settings& settingsIn, bool premultiplied )
	{
		settings = settingsIn;

		const OfxRectI bounds = source->getBounds();
		const int width       = bounds.x2 - bounds.x1;
		const int height      = bounds.y2 - bounds.y1;

		Source::Level base;
		base.width  = width;
		base.height = height;
		base.pixels.resize( static_cast< size_t >( width ) * height );

		readInto( base, source, premultiplied );

		src.levels.clear();
		src.levels.push_back( std::move( base ) );
		src.buildMips();

		edges = settings.look.edges > 0.0f ? buildEdges( src ) : std::vector< float >();
	}

protected:
	/// Read the host's image into linear float RGBA, un-premultiplied, with **row
	/// 0 as the TOP of the picture** -- because the field works in picture space
	/// with v down and OFX images are bottom-up.
	virtual void readInto( Source::Level& into, const OFX::Image* source, bool premultiplied ) = 0;

	Source src;
	std::vector< float > edges;
	Settings settings;
};

template< class Pixel, int Components, int Max >
class AbomerrationProcessor : public AbomerrationProcessorBase
{
public:
	explicit AbomerrationProcessor( OFX::ImageEffect& instance ) :
		AbomerrationProcessorBase( instance )
	{
	}

	void multiThreadProcessImages( OfxRectI window ) override
	{
		const OfxRectI bounds = _dstImg->getBounds();
		const int width       = bounds.x2 - bounds.x1;
		const int height      = bounds.y2 - bounds.y1;

		const abomerration::controls::Look& look = settings.look;

		for( int y = window.y1; y < window.y2; ++y )
		{
			if( _effect.abort() )
				return;

			Pixel* row = static_cast< Pixel* >( _dstImg->getPixelAddress( window.x1, y ) );
			if( row == nullptr )
				continue;

			//OFX rows run bottom-up; the field works in picture space with v down.
			//One flip, here.
			const float v = ( static_cast< float >( height - 1 - ( y - bounds.y1 ) ) + 0.5f )
			                / static_cast< float >( height );

			for( int x = window.x1; x < window.x2; ++x, row += Components )
			{
				const float u = ( static_cast< float >( x - bounds.x1 ) + 0.5f )
				                / static_cast< float >( width );

				//= mirrored: Disperse.cpp main()
				float offU = 0.0f;
				float offV = 0.0f;
				abomerration::dispersion::offsetAt( settings.field, u, v, &offU, &offV );

				if( !edges.empty() )
				{
					const int ex   = std::clamp( x - bounds.x1, 0, width - 1 );
					const int ey   = std::clamp( static_cast< int >( v * static_cast< float >( height ) ),
					                             0, height - 1 );
					const float e  = edges[ static_cast< size_t >( ey ) * width + ex ];
					const float w  = 1.0f + ( e - 1.0f ) * look.edges;
					offU *= w;
					offV *= w;
				}

				const float length = std::sqrt( offU * offU + offV * offV );

				//Twice the gap to the next sample. See Disperse.cpp: a box of the
				//gap itself has its first zero an octave above Nyquist and leaks
				//ripple into the sum.
				const float spacing = look.sampleCount > 1
				                          ? length * settings.frameHeight
				                                / static_cast< float >( look.sampleCount - 1 )
				                          : 0.0f;
				const float lod = look.prefilter ? std::log2( std::max( spacing * 2.0f, 1.0f ) ) : 0.0f;

				const Rgba original = src.bilinear( 0, u, v );

				float r = 0.0f, g = 0.0f, b = 0.0f, a = 0.0f;

				if( look.uniformPush )
				{
					const float p = look.push[ 0 ];
					for( int i = 0; i < look.sampleCount; ++i )
					{
						const abomerration::dispersion::Sample& s = look.samples[ i ];
						const float t                             = ( s.s + p ) * 0.5f;
						const Rgba c = src.trilinear( lod, u + offU * t, v + offV * t );

						r += c.r * s.r;
						g += c.g * s.g;
						b += c.b * s.b;
						//Alpha is achromatic, so it takes the mean of the three
						//responses -- which sums to exactly 1 over the table.
						a += c.a * ( s.r + s.g + s.b ) * ( 1.0f / 3.0f );
					}
				}
				else
				{
					for( int i = 0; i < look.sampleCount; ++i )
					{
						const abomerration::dispersion::Sample& s = look.samples[ i ];

						const float tr = ( s.s + look.push[ 0 ] ) * 0.5f;
						const float tg = ( s.s + look.push[ 1 ] ) * 0.5f;
						const float tb = ( s.s + look.push[ 2 ] ) * 0.5f;

						const Rgba cr = src.trilinear( lod, u + offU * tr, v + offV * tr );
						const Rgba cg = src.trilinear( lod, u + offU * tg, v + offV * tg );
						const Rgba cb = src.trilinear( lod, u + offU * tb, v + offV * tb );

						r += cr.r * s.r;
						g += cg.g * s.g;
						b += cb.b * s.b;
						a += ( cr.a * s.r + cg.a * s.g + cb.a * s.b ) * ( 1.0f / 3.0f );
					}
				}

				if( look.fringe > 0.0f )
				{
					r += ( r - original.r ) * look.fringe;
					g += ( g - original.g ) * look.fringe;
					b += ( b - original.b ) * look.fringe;
				}

				if( look.showField )
				{
					//No meters here: they read the audio drive, and this build has
					//no audio. The magnitude ramp is the half that still means
					//something.
					const float grey = lumaOf( original.r, original.g, original.b ) * 0.25f;
					const float norm = clamp01f( length / std::max( std::fabs( settings.field.amount ), 1e-6f ) );

					r = grey + clamp01f( norm * 2.0f - 0.6f ) * 0.85f;
					g = grey + clamp01f( 1.0f - std::fabs( norm - 0.5f ) * 2.2f ) * 0.85f;
					b = grey + clamp01f( 1.0f - norm * 2.2f ) * 0.85f;
					a = 1.0f;
				}
				else
				{
					r = original.r + ( r - original.r ) * look.mix;
					g = original.g + ( g - original.g ) * look.mix;
					b = original.b + ( b - original.b ) * look.mix;
					a = original.a + ( a - original.a ) * look.mix;
				}

				writePixel( row, r, g, b, a );
			}
		}
	}

protected:
	void readInto( Source::Level& into, const OFX::Image* source, bool premultiplied ) override
	{
		const OfxRectI bounds = source->getBounds();
		const int width       = bounds.x2 - bounds.x1;
		const int height      = bounds.y2 - bounds.y1;

		for( int y = 0; y < height; ++y )
		{
			//Flip: OFX is bottom-up, picture space is top-down.
			const int sourceY = bounds.y2 - 1 - y;

			const Pixel* row = static_cast< const Pixel* >(
				source->getPixelAddress( bounds.x1, sourceY ) );

			for( int x = 0; x < width; ++x )
			{
				Rgba& out = into.pixels[ static_cast< size_t >( y ) * width + x ];

				if( row == nullptr )
				{
					out = Rgba();
					continue;
				}

				const Pixel* p = row + static_cast< size_t >( x ) * Components;

				out.r = static_cast< float >( p[ 0 ] ) / static_cast< float >( Max );
				out.g = static_cast< float >( p[ 1 ] ) / static_cast< float >( Max );
				out.b = static_cast< float >( p[ 2 ] ) / static_cast< float >( Max );
				out.a = Components == 4 ? static_cast< float >( p[ 3 ] ) / static_cast< float >( Max ) : 1.0f;

				//Un-premultiply, because every weight below is a weighted mean of
				//colours and a premultiplied colour is not a colour. Re-applied on
				//the way out.
				if( premultiplied && Components == 4 && out.a > 1e-6f )
				{
					out.r /= out.a;
					out.g /= out.a;
					out.b /= out.a;
				}
			}
		}

		wasPremultiplied = premultiplied;
	}

private:
	void writePixel( Pixel* into, float r, float g, float b, float a )
	{
		const float alpha = Components == 4 ? clamp01f( a ) : 1.0f;
		const float scale = ( wasPremultiplied && Components == 4 ) ? alpha : 1.0f;

		const float channels[ 3 ] = { r * scale, g * scale, b * scale };

		for( int c = 0; c < 3; ++c )
		{
			//Float hosts get their values unclamped at the top end: a scene-linear
			//pipeline is entitled to values above 1 and clamping them here would
			//quietly crush a highlight the rest of the chain expected to keep.
			const float value = Max == 1 ? std::max( 0.0f, channels[ c ] ) : clamp01f( channels[ c ] );
			into[ c ]         = static_cast< Pixel >( value * static_cast< float >( Max ) + ( Max == 1 ? 0.0f : 0.5f ) );
		}

		if( Components == 4 )
			into[ 3 ] = static_cast< Pixel >( alpha * static_cast< float >( Max ) + ( Max == 1 ? 0.0f : 0.5f ) );
	}

	bool wasPremultiplied = false;
};

class AbomerrationPlugin : public OFX::ImageEffect
{
public:
	explicit AbomerrationPlugin( OfxImageEffectHandle handle ) :
		OFX::ImageEffect( handle )
	{
		dstClip = fetchClip( kOfxImageEffectOutputClipName );
		srcClip = fetchClip( kOfxImageEffectSimpleSourceClipName );

		preset     = fetchChoiceParam( kParamPreset );
		geometry   = fetchChoiceParam( kParamGeometry );
		spectrum   = fetchChoiceParam( kParamSpectrum );
		amount     = fetchDoubleParam( kParamAmount );
		centreX    = fetchDoubleParam( kParamCentreX );
		centreY    = fetchDoubleParam( kParamCentreY );
		angle      = fetchDoubleParam( kParamAngle );
		falloff    = fetchDoubleParam( kParamFalloff );
		turbulence = fetchDoubleParam( kParamTurbulence );
		drift      = fetchDoubleParam( kParamDrift );
		redPush    = fetchDoubleParam( kParamRedPush );
		greenPush  = fetchDoubleParam( kParamGreenPush );
		bluePush   = fetchDoubleParam( kParamBluePush );
		edges      = fetchDoubleParam( kParamEdges );
		fringe     = fetchDoubleParam( kParamFringe );
		showField  = fetchBooleanParam( kParamShowField );
		mix        = fetchDoubleParam( kParamMix );
	}

	void render( const OFX::RenderArguments& args ) override
	{
		std::unique_ptr< OFX::Image > dst( dstClip->fetchImage( args.time ) );
		std::unique_ptr< OFX::Image > src( srcClip->fetchImage( args.time ) );

		if( dst == nullptr || src == nullptr )
			OFX::throwSuiteStatusException( kOfxStatFailed );

		const OFX::BitDepthEnum depth       = dst->getPixelDepth();
		const OFX::PixelComponentEnum comps = dst->getPixelComponents();

		if( comps != OFX::ePixelComponentRGBA && comps != OFX::ePixelComponentRGB )
			OFX::throwSuiteStatusException( kOfxStatErrUnsupported );

		const OfxRectI bounds = src->getBounds();
		const double par      = src->getPixelAspectRatio() > 0.0 ? src->getPixelAspectRatio() : 1.0;
		const int height      = std::max( 1, bounds.y2 - bounds.y1 );
		const double aspect   = double( bounds.x2 - bounds.x1 ) * par / double( height );

		const Settings settings = settingsAtTime( args.time, static_cast< float >( aspect ),
		                                          static_cast< float >( height ) );
		const bool premultiplied = srcClip->getPreMultiplication() == OFX::eImagePreMultiplied;

		switch( depth )
		{
			case OFX::eBitDepthUByte:
				comps == OFX::ePixelComponentRGBA
					? run< AbomerrationProcessor< unsigned char, 4, 255 > >( args, dst.get(), src.get(), settings, premultiplied )
					: run< AbomerrationProcessor< unsigned char, 3, 255 > >( args, dst.get(), src.get(), settings, premultiplied );
				break;
			case OFX::eBitDepthUShort:
				comps == OFX::ePixelComponentRGBA
					? run< AbomerrationProcessor< unsigned short, 4, 65535 > >( args, dst.get(), src.get(), settings, premultiplied )
					: run< AbomerrationProcessor< unsigned short, 3, 65535 > >( args, dst.get(), src.get(), settings, premultiplied );
				break;
			case OFX::eBitDepthFloat:
				comps == OFX::ePixelComponentRGBA
					? run< AbomerrationProcessor< float, 4, 1 > >( args, dst.get(), src.get(), settings, premultiplied )
					: run< AbomerrationProcessor< float, 3, 1 > >( args, dst.get(), src.get(), settings, premultiplied );
				break;
			default:
				OFX::throwSuiteStatusException( kOfxStatErrUnsupported );
		}
	}

	void changedParam( const OFX::InstanceChangedArgs& args, const std::string& paramName ) override
	{
		using namespace abomerration::presets;

		if( paramName == kParamPreset )
		{
			int chosen = 0;
			preset->getValue( chosen );
			if( chosen <= 0 || chosen > kCount || applyingPreset )
				return;

			// The copy IS the preset -- same table as the FFGL build, same 0..1
			// space. One edit block so undo takes the whole preset back at once.
			const Preset& p = kPresets[ chosen - 1 ];
			applyingPreset  = true;
			beginEditBlock( "Preset" );
			for( int i = 0; i < kParamCount; ++i )
				if( kPresetParamNames[ i ] != nullptr )
					setParam( kPresetParamNames[ i ], p.v[ i ] );
			endEditBlock();
			applyingPreset = false;
			return;
		}

		// Editing a covered control while a preset is active hands control back to
		// the sliders. Judged by value, not by the change reason: hosts are not
		// consistent about reasons, but "no longer equal to the preset" is
		// unambiguous and absorbs the host echoing our own setValues.
		if( applyingPreset || args.reason == OFX::eChangeTime )
			return;

		int active = 0;
		preset->getValue( active );
		if( active <= 0 || active > kCount )
			return;

		const Preset& p = kPresets[ active - 1 ];
		for( int i = 0; i < kParamCount; ++i )
		{
			if( kPresetParamNames[ i ] != nullptr && paramName == kPresetParamNames[ i ]
			    && paramDiffers( kPresetParamNames[ i ], p.v[ i ] ) )
			{
				applyingPreset = true;
				preset->setValue( 0 );
				applyingPreset = false;
				return;
			}
		}
	}

	bool isIdentity( const OFX::IsIdentityArguments& args, OFX::Clip*& identityClip,
	                 double& identityTime ) override
	{
		// No dispersion and no channel trim is the picture that came in. Worth
		// declaring: a host that knows a frame is untouched can skip the render
		// entirely.
		//
		// The channel pushes have to be in the test. Amount at zero makes the
		// field zero, so a push has nothing to scale and the frame really is
		// untouched -- but a reader checking only Amount would be right by
		// accident, and the next person to give the pushes their own displacement
		// would inherit a wrong identity claim.
		const bool neutral =
			amount->getValueAtTime( args.time ) <= 0.0
			&& std::fabs( redPush->getValueAtTime( args.time ) - 0.5 ) < 1e-9
			&& std::fabs( greenPush->getValueAtTime( args.time ) - 0.5 ) < 1e-9
			&& std::fabs( bluePush->getValueAtTime( args.time ) - 0.5 ) < 1e-9
			&& !showField->getValueAtTime( args.time );

		if( neutral || mix->getValueAtTime( args.time ) <= 0.0 )
		{
			identityClip = srcClip;
			identityTime = args.time;
			return true;
		}
		return false;
	}

private:
	Settings settingsAtTime( double time, float aspect, float frameHeight ) const
	{
		abomerration::controls::HostValues host;

		int choice = 0;
		geometry->getValueAtTime( time, choice );
		host.geometry = static_cast< float >( choice );
		spectrum->getValueAtTime( time, choice );
		host.spectrum = static_cast< float >( choice );

		host.amount     = static_cast< float >( amount->getValueAtTime( time ) );
		host.centreX    = static_cast< float >( centreX->getValueAtTime( time ) );
		host.centreY    = static_cast< float >( centreY->getValueAtTime( time ) );
		host.angle      = static_cast< float >( angle->getValueAtTime( time ) );
		host.falloff    = static_cast< float >( falloff->getValueAtTime( time ) );
		host.turbulence = static_cast< float >( turbulence->getValueAtTime( time ) );
		host.drift      = static_cast< float >( drift->getValueAtTime( time ) );
		host.redPush    = static_cast< float >( redPush->getValueAtTime( time ) );
		host.greenPush  = static_cast< float >( greenPush->getValueAtTime( time ) );
		host.bluePush   = static_cast< float >( bluePush->getValueAtTime( time ) );
		host.edges      = static_cast< float >( edges->getValueAtTime( time ) );
		host.fringe     = static_cast< float >( fringe->getValueAtTime( time ) );
		host.mix        = static_cast< float >( mix->getValueAtTime( time ) );

		bool show = false;
		showField->getValueAtTime( time, show );
		host.showField = show ? 1.0f : 0.0f;

		// The reaction, through the same code the FFGL build uses, with nothing
		// to react to. Returns scale 1 and no push -- see the header for why this
		// is a call rather than a hardcoded 1.0f.
		const abomerration::drive::Output driven =
			abomerration::drive::compute( abomerration::controls::driveSettings( host ),
			                              abomerration::drive::Input() );

		// Drift is time * rate here, not integrated. See the header: OFX renders
		// frames in any order, so there is no previous frame to integrate from.
		// Seconds rather than frames, so a given Drift looks the same in a 24 fps
		// timeline and a 60 fps one.
		const double fps      = srcClip->getFrameRate() > 0.0 ? srcClip->getFrameRate() : 25.0;
		const float driftPhase = static_cast< float >( time / fps )
		                         * abomerration::controls::driftRate( host );

		Settings out;
		out.field       = abomerration::controls::field( host, aspect, driven.scale, driftPhase );
		out.look        = abomerration::controls::look( host, driven.push );
		out.frameHeight = frameHeight;
		return out;
	}

	template< class Processor >
	void run( const OFX::RenderArguments& args, OFX::Image* dst, OFX::Image* src,
	          const Settings& settings, bool premultiplied )
	{
		Processor processor( *this );
		processor.setDstImg( dst );
		processor.setRenderWindow( args.renderWindow );
		processor.setup( src, settings, premultiplied );
		processor.process();
	}

	OFX::ChoiceParam* tryChoice( const char* name ) const
	{
		if( std::strcmp( name, kParamGeometry ) == 0 )
			return geometry;
		if( std::strcmp( name, kParamSpectrum ) == 0 )
			return spectrum;
		return nullptr;
	}

	OFX::DoubleParam* tryDouble( const char* name ) const
	{
		if( std::strcmp( name, kParamAmount ) == 0 )
			return amount;
		if( std::strcmp( name, kParamAngle ) == 0 )
			return angle;
		if( std::strcmp( name, kParamFalloff ) == 0 )
			return falloff;
		if( std::strcmp( name, kParamTurbulence ) == 0 )
			return turbulence;
		if( std::strcmp( name, kParamDrift ) == 0 )
			return drift;
		if( std::strcmp( name, kParamRedPush ) == 0 )
			return redPush;
		if( std::strcmp( name, kParamGreenPush ) == 0 )
			return greenPush;
		if( std::strcmp( name, kParamBluePush ) == 0 )
			return bluePush;
		if( std::strcmp( name, kParamEdges ) == 0 )
			return edges;
		if( std::strcmp( name, kParamFringe ) == 0 )
			return fringe;
		return nullptr;
	}

	void setParam( const char* name, float value )
	{
		if( OFX::ChoiceParam* c = tryChoice( name ) )
		{
			const int v = static_cast< int >( std::lround( value ) );
			int current = 0;
			c->getValue( current );
			if( current != v )
				c->setValue( v );
			return;
		}
		if( OFX::DoubleParam* d = tryDouble( name ) )
		{
			const double v = static_cast< double >( value );
			double current = 0.0;
			d->getValue( current );
			if( std::fabs( current - v ) > 1e-9 )
				d->setValue( v );
		}
	}

	bool paramDiffers( const char* name, float value ) const
	{
		if( OFX::ChoiceParam* c = tryChoice( name ) )
		{
			int current = 0;
			c->getValue( current );
			return current != static_cast< int >( std::lround( value ) );
		}
		if( OFX::DoubleParam* d = tryDouble( name ) )
		{
			double current = 0.0;
			d->getValue( current );
			return std::fabs( current - static_cast< double >( value ) ) > 1e-6;
		}
		return false;
	}

	OFX::Clip* dstClip = nullptr;
	OFX::Clip* srcClip = nullptr;

	OFX::ChoiceParam* preset   = nullptr;
	OFX::ChoiceParam* geometry = nullptr;
	OFX::ChoiceParam* spectrum = nullptr;

	OFX::DoubleParam* amount     = nullptr;
	OFX::DoubleParam* centreX    = nullptr;
	OFX::DoubleParam* centreY    = nullptr;
	OFX::DoubleParam* angle      = nullptr;
	OFX::DoubleParam* falloff    = nullptr;
	OFX::DoubleParam* turbulence = nullptr;
	OFX::DoubleParam* drift      = nullptr;
	OFX::DoubleParam* redPush    = nullptr;
	OFX::DoubleParam* greenPush  = nullptr;
	OFX::DoubleParam* bluePush   = nullptr;
	OFX::DoubleParam* edges      = nullptr;
	OFX::DoubleParam* fringe     = nullptr;
	OFX::DoubleParam* mix        = nullptr;

	OFX::BooleanParam* showField = nullptr;

	/// Set while this plugin is writing its own parameters, so the value-comparison
	/// fallback below does not treat the preset's own copy as an operator edit.
	bool applyingPreset = false;
};

OFX::DoubleParamDescriptor* defineSlider( OFX::ImageEffectDescriptor& desc,
                                          OFX::PageParamDescriptor* page,
                                          const char* name, const char* label,
                                          const char* hint, double def )
{
	OFX::DoubleParamDescriptor* p = desc.defineDoubleParam( name );
	p->setLabels( label, label, label );
	p->setHint( hint );
	p->setRange( 0.0, 1.0 );
	p->setDisplayRange( 0.0, 1.0 );
	p->setDefault( def );
	page->addChild( *p );
	return p;
}

} // namespace

mDeclarePluginFactory( AbomerrationPluginFactory, {}, {} );

void AbomerrationPluginFactory::describe( OFX::ImageEffectDescriptor& desc )
{
	desc.setLabels( kPluginName, kPluginName, kPluginName );
	desc.setPluginGrouping( kPluginGrouping );
	desc.setPluginDescription( kPluginDescription );

	desc.addSupportedContext( OFX::eContextFilter );
	desc.addSupportedContext( OFX::eContextGeneral );

	desc.addSupportedBitDepth( OFX::eBitDepthUByte );
	desc.addSupportedBitDepth( OFX::eBitDepthUShort );
	desc.addSupportedBitDepth( OFX::eBitDepthFloat );

	// A dispersion gathers from anywhere along its path, and the mip pyramid is
	// built from the whole frame, so this cannot render from tiles. Frames stay
	// independent of each other and of render order -- there is no history here,
	// which is exactly why Drift has to be computed from the clock rather than
	// integrated.
	desc.setSupportsTiles( false );
	desc.setTemporalClipAccess( false );
	desc.setRenderThreadSafety( OFX::eRenderFullySafe );
	desc.setSupportsMultiResolution( true );
}

void AbomerrationPluginFactory::describeInContext( OFX::ImageEffectDescriptor& desc, OFX::ContextEnum )
{
	OFX::ClipDescriptor* srcClip = desc.defineClip( kOfxImageEffectSimpleSourceClipName );
	srcClip->addSupportedComponent( OFX::ePixelComponentRGBA );
	srcClip->addSupportedComponent( OFX::ePixelComponentRGB );
	srcClip->setSupportsTiles( false );

	OFX::ClipDescriptor* dstClip = desc.defineClip( kOfxImageEffectOutputClipName );
	dstClip->addSupportedComponent( OFX::ePixelComponentRGBA );
	dstClip->addSupportedComponent( OFX::ePixelComponentRGB );
	dstClip->setSupportsTiles( false );

	// Same parameters, same 0..1 ranges, same defaults as the FFGL build, so the
	// two inspectors read identically and one set of docs covers both -- minus the
	// reactive controls, which this API cannot feed.
	OFX::PageParamDescriptor* page = desc.definePageParam( "Controls" );
	const abomerration::controls::HostValues defaults;

	OFX::ChoiceParamDescriptor* presetParam = desc.defineChoiceParam( kParamPreset );
	presetParam->setLabels( "Preset", "Preset", "Preset" );
	presetParam->setHint( "Factory looks. Picking one sets the controls; editing any of them "
	                      "afterwards falls back to Custom. The presets that drive the "
	                      "aberration from audio are Resolume only, and set only their manual "
	                      "half here." );
	presetParam->appendOption( "Custom" );
	for( int i = 0; i < abomerration::presets::kCount; ++i )
		presetParam->appendOption( abomerration::presets::kPresets[ i ].name );
	presetParam->setDefault( 0 );
	presetParam->setIsPersistant( true );
	presetParam->setEvaluateOnChange( false );//the copied values re-render; the label does not
	presetParam->setAnimates( false );
	page->addChild( *presetParam );

	//------------------------------------------------------------- Aberration
	OFX::GroupParamDescriptor* aberration = desc.defineGroupParam( "Aberration" );
	aberration->setLabels( "Aberration", "Aberration", "Aberration" );

	OFX::ChoiceParamDescriptor* geometryParam = desc.defineChoiceParam( kParamGeometry );
	geometryParam->setLabels( "Geometry", "Geometry", "Geometry" );
	geometryParam->setHint( "Which way the spectrum is smeared. Radial is what a real "
	                        "uncorrected lens does; Linear is a prism in front of it; "
	                        "Tangential and Turbulent are not optics at all." );
	for( int i = 0; i < abomerration::controls::geometryCount(); ++i )
		geometryParam->appendOption( abomerration::controls::geometryLabel( i ) );
	geometryParam->setDefault( 0 );
	geometryParam->setParent( *aberration );
	page->addChild( *geometryParam );

	defineSlider( desc, page, kParamAmount, "Amount",
	              "How far apart the two ends of the spectrum land, as a fraction of the "
	              "frame height. A real lens is a small fraction of a per cent; the top "
	              "of this range is 15 per cent.", defaults.amount )
		->setParent( *aberration );
	defineSlider( desc, page, kParamCentreX, "Centre X",
	              "The optical centre across the frame. Unused by Linear, which has no centre.",
	              defaults.centreX )
		->setParent( *aberration );
	defineSlider( desc, page, kParamCentreY, "Centre Y",
	              "The optical centre up the frame; 0 is the top. Unused by Linear.",
	              defaults.centreY )
		->setParent( *aberration );
	defineSlider( desc, page, kParamAngle, "Angle",
	              "Linear: the direction. Turbulent: rotates the noise field. A full turn, "
	              "because the two ends of a dispersion are different colours and reversing "
	              "it is a different picture.", defaults.angle )
		->setParent( *aberration );
	defineSlider( desc, page, kParamFalloff, "Falloff",
	              "Radial and Tangential: how fast the dispersion grows with distance from "
	              "the centre. 0.5 is linear in radius, higher concentrates it in the "
	              "corners like a bad lens, lower spreads it into the middle like no lens "
	              "at all.", defaults.falloff )
		->setParent( *aberration );

	OFX::ChoiceParamDescriptor* spectrumParam = desc.defineChoiceParam( kParamSpectrum );
	spectrumParam->setLabels( "Spectrum", "Spectrum", "Spectrum" );
	spectrumParam->setHint( "How many wavelengths are sampled along the path. RGB Split is "
	                        "three, and is the hard-edged channel offset -- not a low-quality "
	                        "version of the others but a different look. The Prism settings "
	                        "trade cost for sharpness: each wavelength is prefiltered over "
	                        "the gap to the next, so more samples means a sharper picture at "
	                        "the same smoothness." );
	for( int i = 0; i < abomerration::controls::spectrumCount(); ++i )
		spectrumParam->appendOption( abomerration::controls::spectrumLabel( i ) );
	spectrumParam->setDefault( 1 );
	spectrumParam->setParent( *aberration );
	page->addChild( *spectrumParam );

	defineSlider( desc, page, kParamTurbulence, "Turbulence",
	              "Turbulent only: the noise frequency, in cycles across the frame height.",
	              defaults.turbulence )
		->setParent( *aberration );
	defineSlider( desc, page, kParamDrift, "Drift",
	              "Turbulent only: how fast the noise field moves. Zero freezes it, which "
	              "is a fixed lens fault and worth having.", defaults.drift )
		->setParent( *aberration );

	//--------------------------------------------------------------- Channels
	OFX::GroupParamDescriptor* channels = desc.defineGroupParam( "Channels" );
	channels->setLabels( "Channels", "Channels", "Channels" );

	defineSlider( desc, page, kParamRedPush, "Red Push",
	              "Extra displacement for red on top of the spectrum. 0.5 is none. These "
	              "exist to break the physical relationship, not to fine-tune it.",
	              defaults.redPush )
		->setParent( *channels );
	defineSlider( desc, page, kParamGreenPush, "Green Push",
	              "Extra displacement for green. 0.5 is none.", defaults.greenPush )
		->setParent( *channels );
	defineSlider( desc, page, kParamBluePush, "Blue Push",
	              "Extra displacement for blue. 0.5 is none.", defaults.bluePush )
		->setParent( *channels );

	//------------------------------------------------------------------- Look
	OFX::GroupParamDescriptor* look = desc.defineGroupParam( "Look" );
	look->setLabels( "Look", "Look", "Look" );

	defineSlider( desc, page, kParamEdges, "Edges",
	              "Weights the dispersion by local contrast. Real lateral aberration is "
	              "invisible in flat areas and obvious at edges, because displacing a "
	              "region of constant colour returns the same region. At 0 the whole "
	              "picture is displaced, which is the misregistered-camera look.",
	              defaults.edges )
		->setParent( *look );
	defineSlider( desc, page, kParamFringe, "Fringe",
	              "Amplifies the difference from the undispersed picture, so the fringe "
	              "reads harder without anything moving further. It cannot invent a fringe "
	              "where there is no dispersion.", defaults.fringe )
		->setParent( *look );

	//----------------------------------------------------------------- Output
	OFX::GroupParamDescriptor* output = desc.defineGroupParam( "Output" );
	output->setLabels( "Output", "Output", "Output" );

	OFX::BooleanParamDescriptor* showParam = desc.defineBooleanParam( kParamShowField );
	showParam->setLabels( "Show Field", "Show Field", "Show Field" );
	showParam->setHint( "Paints the dispersion magnitude over a dim picture: cold where "
	                    "nothing is moving, hot where it is moving most. For seeing what the "
	                    "geometry and Edges are actually doing, because a flat region with an "
	                    "enormous displacement looks exactly like one with none." );
	showParam->setDefault( false );
	showParam->setParent( *output );
	page->addChild( *showParam );

	defineSlider( desc, page, kParamMix, "Mix", "Wet/dry against the untouched input.", defaults.mix )
		->setParent( *output );
}

OFX::ImageEffect* AbomerrationPluginFactory::createInstance( OfxImageEffectHandle handle, OFX::ContextEnum )
{
	return new AbomerrationPlugin( handle );
}

void OFX::Plugin::getPluginIDs( OFX::PluginFactoryArray& ids )
{
	// Deliberately leaked: a by-value static would register an exit-time destructor
	// inside this module, and a host that dlclose()s the bundle before process exit
	// then jumps through a dangling pointer.
	static AbomerrationPluginFactory* factory =
		new AbomerrationPluginFactory( kPluginIdentifier, PLUGIN_VERSION_MAJOR, PLUGIN_VERSION_MINOR );
	ids.push_back( factory );
}
