#pragma once

/**
    Factory presets: named looks an operator can reach in one gesture.

    Each entry is *a fault with a cause* -- a cheap lens that was never
    corrected, a prism held in front of one, a three-strip camera slipping out of
    registration, a kick drum wired directly to the optics -- rather than a
    random set of slider positions. The controls here are the parts of an optical
    system plus what the music is allowed to do to them, so a coherent look is a
    coherent story about what went wrong and what is driving it.

    The values live in the same 0..1 host-facing space both builds expose, so ONE
    table drives the FFGL and the OFX plugin and a preset cannot look different in
    Resolume and Resolve. Plain data only; the machinery that applies it lives
    with each host's glue.

    Element 0 of the dropdown is "Custom" and is not in this table: it means "the
    sliders are the truth".

    ------------------------------------------------ what a preset must not set

    **Not the optical centre.** Centre X and Centre Y are where the operator's
    subject happens to be in their own footage. A preset that reached into those
    would take a correctly placed centre and move it off the subject, which is
    not a look, it is breakage.

    **Not Sync.** It does not exist in both builds -- FFGL hosts send beat
    information and OFX hosts have no way to -- so a shared table cannot name an
    element that means the same thing in both. The same reason orrery's Sync stays
    out of its presets.

    A consequence worth stating plainly, because it looks like a bug otherwise:
    the presets below that set a Beat Depth do nothing audible until the operator
    also sets Sync to Locked. That is the correct trade. The alternative is a
    preset that silently switches the transport mode underneath somebody's running
    show.

    **Not Mix, and not Show Field.** One is the wet/dry every effect has and the
    other is a diagnostic.
*/

namespace abomerration
{
namespace presets
{
/// The parameters a preset sets, in one fixed order. The FFGL build binds this
/// order to its ParamIDs and the OFX build to its param handles; both
/// static_assert against kParamCount so the three lists cannot drift silently.
enum Param
{
	kGeometry,
	kAmount,
	kAngle,
	kFalloff,
	kSpectrum,
	kTurbulence,
	kDrift,
	kRedPush,
	kGreenPush,
	kBluePush,
	kBeatDepth,
	kBeatDecay,
	kBeatDivision,
	kLevelDepth,
	kBandDepth,
	kRoute,
	kEdges,
	kFringe,
	kParamCount
};

struct Preset
{
	const char* name;
	float v[ kParamCount ];
};

// Option values are element indices, NOT fractions:
//   Geometry  0 Radial / 1 Linear / 2 Tangential / 3 Turbulent
//   Spectrum  0 RGB Split / 1 Prism 8 / 2 Prism 16 / 3 Prism 32
//   Division  0 quarter beat / 1 half / 2 beat / 3 two beats / 4 bar / 5 two bars
//   Route     0 Natural / 1 Inverted / 2 Bass Only / 3 Treble Only
//
// The three channel pushes are CENTRED controls: 0.5 is no extra push. A preset
// that wants no manual trim must say 0.5 and not 0.0, and every one below that
// leaves them alone says 0.5 for exactly that reason.
inline constexpr Preset kPresets[] = {
	//                     geo   amt   ang   fall  spec  turb  drft  pushR pushG pushB beatD decay div   lvl   band  route edge  fringe
	//A lens nobody corrected. Small, radial, concentrated in the corners, and
	//weighted to the edges -- which together is what the real fault looks like
	//and is the one preset here that could pass for an accident.
	{ "Uncorrected Lens", { 0.0f, 0.12f, 0.50f, 0.68f, 2.0f, 0.35f, 0.00f, 0.5f, 0.5f, 0.5f, 0.00f, 0.45f, 2.0f, 0.00f, 0.00f, 0.0f, 0.85f, 0.20f } },

	//The 1990s channel offset, straight. Linear, hard-edged, no spectrum and no
	//edge weighting, because both of those would soften the thing this is for.
	{ "Misregistered", { 1.0f, 0.20f, 0.50f, 0.50f, 0.0f, 0.35f, 0.00f, 0.5f, 0.5f, 0.5f, 0.00f, 0.45f, 2.0f, 0.00f, 0.00f, 0.0f, 0.00f, 0.00f } },

	//A prism across the lens, and enough samples that the fringe is a spectrum
	//rather than three copies. Fringe up because a real prism is brighter at the
	//edges than the arithmetic suggests.
	{ "Prism", { 1.0f, 0.34f, 0.30f, 0.50f, 3.0f, 0.35f, 0.00f, 0.5f, 0.5f, 0.5f, 0.00f, 0.45f, 2.0f, 0.00f, 0.00f, 0.0f, 0.55f, 0.45f } },

	//Kick drum straight into the optics. Full beat depth means silence renders
	//clean, so this is the one that demonstrates what Depth is carved out of.
	{ "Kick Punch", { 0.0f, 0.55f, 0.50f, 0.55f, 1.0f, 0.35f, 0.00f, 0.5f, 0.5f, 0.5f, 1.00f, 0.72f, 2.0f, 0.00f, 0.00f, 0.0f, 0.30f, 0.25f } },

	//Bass pumps the whole dispersion with no colour routing, which is the route
	//that stays legible on a dense mix. Level depth alongside it so quiet
	//passages breathe instead of sitting still.
	{ "Bass Bloom", { 0.0f, 0.45f, 0.50f, 0.50f, 2.0f, 0.35f, 0.00f, 0.5f, 0.5f, 0.5f, 0.00f, 0.45f, 2.0f, 0.35f, 0.75f, 2.0f, 0.40f, 0.30f } },

	//Hi-hats and cymbals only. Falloff below unity so it works in the middle of
	//the frame as well as the corners -- treble content is sparse, and an effect
	//that only shows in the corners on sparse content mostly shows nothing.
	{ "Cymbal Sizzle", { 0.0f, 0.30f, 0.50f, 0.34f, 1.0f, 0.35f, 0.00f, 0.5f, 0.5f, 0.5f, 0.00f, 0.45f, 0.0f, 0.00f, 0.90f, 3.0f, 0.70f, 0.55f } },

	//Tangential, which no lens does: the picture is wrung out around the centre
	//rather than pushed away from it.
	{ "Wrung Out", { 2.0f, 0.42f, 0.50f, 0.60f, 2.0f, 0.35f, 0.00f, 0.5f, 0.5f, 0.5f, 0.30f, 0.55f, 4.0f, 0.00f, 0.00f, 0.0f, 0.25f, 0.30f } },

	//The one the plugin is named for. Turbulent, drifting, every source driving
	//at once, and the channel trims pulled apart so the bands have somewhere to
	//push from. Not subtle and not meant to be.
	{ "Abomination", { 3.0f, 0.80f, 0.50f, 0.50f, 2.0f, 0.55f, 0.45f, 0.68f, 0.5f, 0.32f, 0.55f, 0.60f, 2.0f, 0.45f, 0.85f, 0.0f, 0.00f, 0.70f } },
};

constexpr int kCount = int( sizeof( kPresets ) / sizeof( kPresets[ 0 ] ) );

} // namespace presets
} // namespace abomerration
