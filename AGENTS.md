# abomerration — for whoever picks this up next

Sound-reactive chromatic aberration, as an FFGL 2.1 effect for Resolume
(`Abomerration`, ID **`AB01`**) plus an OpenFX build. C++17 + GLSL 4.1, CMake.
`CLAUDE.md` is the command reference and is not duplicated here; this file is the
*why*.

## The one idea, and why the code is shaped like this

**A lens does not split a picture into three channels. It focuses every wavelength
at a slightly different magnification, and the fringe is the whole spectrum
smeared along that path and integrated by the sensor.**

So the plugin displaces the picture once per *wavelength sample* and sums through
spectral weights. The hard red/blue split everybody knows is not a mode — it is
what the same loop does at three samples, because the three-sample weight table is
the identity. One control therefore spans "cheap channel offset" and "real
prismatic fringe" with nothing else changing.

Everything follows from splitting that into two halves that never touch:

- **The field** (`Dispersion.cpp`, mirrored in `shaders/Field.cpp`) — which way
  and how far, per pixel. Four geometries.
- **The weight table** (`Dispersion.cpp`, *not* mirrored) — what each sample
  counts as. Four settings.

Four × four is eight pieces of code, not sixteen. A new geometry is one branch in
one function and its twin. A new spectrum setting is one row in a table.

## Load-bearing invariants

**The weight table is normalised per channel to sum to exactly 1.** This is the
difference between Spectrum being a *quality* control and a *tint* control. Leave
the weights raw and moving from 8 samples to 32 changes how much energy each
channel collects, the colour balance shifts, and an operator reasonably concludes
the knob is a colour effect. `abomtest --spectrum` renders a flat field at every
setting and every amount and demands it come back flat; it fails immediately
without the normalisation.

**RGB Split is exactly three samples, an identity table, and no prefilter.** It
exists to reproduce something specific and well known. An approximation of it
would be a worse version of a thing that is not hard to get exactly right, and
prefiltering it would soften the hard edges that are the entire point.

**Only `offsetAt`, the hash and the noise are mirrored.** They have to be: the
field is a function of every pixel, so the GPU evaluates it, while the OFX build
and the harness need the C++. Marked `//= mirrored` in both files. The weight
table is deliberately *not* mirrored — it is uploaded as a uniform, because a
table has no reason to exist twice.

**Distances are in frame-height units**, so a given Amount displaces by the same
visible distance at 16:9 and 4:3. Invisible on a square render, which is why
`--field` uses 320×180.

**`Centre Y` = 0 is the top.** GL's v runs the other way. The flip happens once
per shader, in `main()`, and the field works in picture space throughout.

**Depth is carved out of the amount, not added to it.** `scale = (1 - depthSum) +
beatDepth*beat + levelDepth*level`. Full depth therefore means silence renders
clean and the beat renders the whole effect, which is what anybody means by a
control called Depth. The depths are clamped as a *sum*: two sources at 0.8 would
otherwise leave the always-on part at −0.6, and a negative scale **inverts** the
dispersion instead of saturating it.

## Traps that have already cost time here

**The spectral quadrature aliases its source, badly, and no sample count fixes
it.** A dispersion of L pixels sampled N times steps every L/N pixels. Measured on
a step edge before any prefilter existed, summed second-difference energy for
Prism 8 / 16 / 32:

    27 px path     394   202    46
    65 px path     394   376   288
    108 px path    382   388   384

The last row is the tell — at 108 pixels all three are equally bad and no longer
even ordered. One sample per pixel of a 108-pixel path is 108 fetches, tripled
when the channel pushes differ, which at 4K nobody can run. The fix is the
prefilter the quadrature was missing: each sample reads from the mip level covering
the gap. That is what `Copy.cpp` and the mipmapped copy buffer are for. Same lesson
as tilter's blur running on a box-downsampled copy.

**The prefilter width is TWICE the sample spacing, not the spacing.** Samples
spaced *d* apart carry frequencies below Nyquist 1/(2*d*); a box of width *d* has
its first zero at 1/*d*, an octave too high, and passes everything between straight
into the sum. Ripple of 255, Prism 8 / 16 / 32:

    width d     16.6   8.7   1.7
    width 2d     4.8   2.0   1.2

**Three metrics for that, two of them wrong**, and the wrong ones are recorded in
`abomtest`'s `checkQuadrature` because both looked reasonable. Summed
second-difference energy is dominated by how *many* joins there are, so once the
prefilter existed it ranked Prism 32 worse than Prism 8. The worst single second
difference came out at 765/L independent of sample count — which is the true
curvature of the correct answer, not banding, because a hard edge smeared through
three primaries has to turn that sharply somewhere. Difference-from-the-densest-
setting measured the prefilter's deliberate softening. Ripple at the sample
spacing is blind to any linear ramp and is the one that works.

**The harness rendered every frame upside down, and three checks passed anyway.**
`buildScene` works top-down; `glTexImage2D` treats its first row as v = 0, the
bottom. So the upload flipped the picture and `flipRows` on readback flipped it
back — cancelling for anything symmetric. `--field`, `--offset` and `--spectrum`
all passed, because each measures something a vertical flip does not change. Only
putting a bypassed frame next to the test card showed it. There is now exactly one
flip, in `uploadScene`, where the convention actually changes.

**`barPhase` pinned at zero makes two controls read as dead.** The bar recovery
reconciles the clock against `barPhase`, so a `barPhase` of 0 near t=0 recovers bar
0 exactly and the beat envelope sits at its peak 1.0 forever — at which point Beat
Decay and Division both change nothing, because at the instant of a beat neither
does. The harness drives a transport whose `barPhase` tracks its clock. Nothing
about this looks like a harness bug from outside.

**Sync had a Bar mode that was byte-identical to Beat.** `bars * 4.0` is the beat
count. Two dropdown entries rendering the same picture. Found by `tools/sweep.py`,
which reported Sync dead and was right. Sync is now Free/Locked, and Division —
which already spans a quarter beat to two bars — carries what Bar would have said.

**`Angle` at 0 and 1 is the same picture.** A full turn, so both ends are ±π. The
sweep needs a quarter turn either side.

**`cmake -B build` reuses a warm cache**, so a tree configured once with
`-DCMAKE_OSX_ARCHITECTURES=arm64` stays arm64 and a "fresh configure" verifies the
wrong binary. `verify.sh` builds into `build-verify/` with the architectures stated
explicitly.

Everything in [`reference_ffgl_sdk_bugs`] applies too — the scoped bindings that
clear rather than restore, the FBO that leaks its colour texture, the missing
umbrella include, `Set()` having no bool or integer-vector overload,
`ScopedFBOBinding` not restoring the viewport, and `SetTextParameter` having to be
overridden or no host can instantiate the plugin at all.
**Resolve's Fusion page reports no frame rate on its clips, and a missing property
throws.** Found 2026-10-03 in a real DaVinci Resolve Studio 21.1, on the fleet's
sibling ports (Fusion page: MediaIn → OFX tool → MediaOut, then a render job): the
job failed with "The Fusion composition … could not be processed successfully".
Fusion gives `kOfxImageEffectPropFrameRate` on the effect but on no clip, and leaves
out the clips' unmapped rate and range. The Support library turns a missing property
into a C++ exception (`PropertyUnknownToHost`), and one thrown out of `render` is
`kOfxStatErrMissingHostFeature`: a failed frame. This port read the clip's rate
unguarded, so it failed every frame there.

`framesPerSecond()` in `source/ofx/AbomerrationOFX.cpp` asks the output clip, the
source clip and the effect, each in its own `try`, and takes the first positive,
finite rate; otherwise 24, Resolve's default timeline rate. In Fusion that is the
effect's rate, which follows the timeline: a raw-API probe plugin read 24 there in a
24 fps project and 25 in a 25 fps one (2026-10-04). So in Fusion Drift moves at the
composition's own rate, as the plugin description and the README say. A host that
reports zero now gets 24 as well, where this file used to assume 25. **Read no host
property in an action without a guard**, and never let a clip's frame range bound a
fetch. This port reads no frame range.

Checked against a test host stricter than Fusion, which withholds the effect's rate
too (`ofxprobe --quirks fusion`, a scratch build of resolume-ofx-bridge's probe):
the previous build fails under it with `kOfxStatErrMissingHostFeature`; this one
renders at the 24 fps fallback, byte-identical to a 24 fps host's render and unlike
a 25 or 30 fps one, in the plugin's own context and in General; and a host that
reports a rate gets exactly the output it got before.

**Declare the output frame-varying, or Fusion repeats a generator's first frame.**
`getClipPreferences` calls `setOutputFrameVarying( true )`. Drift moves the
turbulent field with the frame's time. Without that declaration a host may treat the
output as fixed while the inputs and parameters hold still. Measured 2026-10-04 in
Resolve Studio 21.1's Fusion page: every fleet generator rendered frames 20-22
byte-identical, none having declared it, and with the declaration they animate.

A tool fed by a MediaIn is re-rendered every frame either way, so in Fusion this
changes nothing visible; the declaration is still the correct one, and other hosts
(Nuke, Natron) cache by it.

The flag changes no pixels: `ofxprobe` renders byte-identical with and without it,
on a moving sequence, on a still and under `--quirks fusion`.


## What is genuinely verified, and what is assumed

**Verified in a real host:** the FFGL build has been **loaded into Resolume Arena
and confirmed working**. That matters more than its one line suggests, because it
is the only thing on this page that no check could have established — every check
drives the plugin class directly or through `plugMain`, and none of them can say
whether twenty-four controls present sensibly in somebody's inspector.

**Verified, by measurement, from a clean universal build — 23 checks, all
passing:**

- the GLSL field against the C++ across all four geometries, worst disagreement
  7e-07 of a frame width, **with a control case that must and does disagree**
  (0.14);
- the rendered per-channel offsets against arithmetic done outside the plugin, to
  better than one pixel at three amounts, plus a zero-amount case that must not
  move;
- flat-field energy preservation at every spectrum setting × every amount × every
  geometry, and the red weights summing to 1.000000;
- quadrature ripple below 5 of 255 against a hard step edge;
- 14 assertions on the reaction arithmetic with no GL involved, including the bar
  recovery tracking the clock across four bars to 1.2e-07, the logarithmic band
  split (bin 4 is *not* bass), and a no-audio host producing finite output;
- a milliseconds host and a seconds host producing identical drift;
- all 24 controls reaching the picture;
- the bundle instantiating through `plugMain` and rendering;
- the OFX bundle rendering and changing the picture;
- the OFX plist matching its binary, and both bundles ad-hoc signing.

**Assumed, or simply not done:**

- **The OpenFX build has only been run on Resolve's Fusion page**: from v0.1.8
  (the frame-rate guard) it renders there as a tool, MediaIn → Abomerration →
  MediaOut, in DaVinci Resolve Studio 21.1 on macOS (2026-10-04). Never on
  Resolve's other pages, in Nuke, Natron or Vegas. Everything measured above is
  offline, on one Apple M4 Max.
- **The Windows build is compiled in CI and has never been run.**
- **The FFT buffer's frequency mapping is undocumented.** The band split is
  logarithmic in bin index, which is right for any linear-in-frequency buffer and
  merely differently weighted otherwise. Band Route is how an operator compensates.
  Nothing here is a claim about Resolume's internals.
- **The spectral response is an approximation, not colorimetry.** Gaussian bumps
  near each primary's dominant wavelength, not the CIE 1931 colour matching
  functions. Fine for smearing a picture along a line; not a colorimetric claim.
- **Whether Resolume honours `FF_EVENT_FLAG_VALUE`** — which is what makes the
  preset dropdown update the sliders — is still unconfirmed across the fleet. A
  host that ignores it renders presets correctly and shows stale knobs.
- **The OFX and FFGL renders have not been compared pixel for pixel.** Both are
  smoke-tested and both share the field, the table and the curves by linking, but
  no test asserts they agree.
- **`glGenerateMipmap` on an odd dimension is implementation defined**, so the OFX
  pyramid cannot match the GPU's exactly in the last row or column of a reduced
  level.

## Siblings

`tilter` is the nearest relative and the donor for this repo's scaffolding — read
its `AGENTS.md` for the shared build and release traps. `tinsel`, `downpour` and
`orrery` are where the FFT and beat-sync patterns come from. `resolume-ofx-bridge`
provides `ofxprobe` and `ffgltest`, both of which `verify.sh` uses when present.

## Notes

`docs/NOTES.md` carries this repo's working notes — current status, decisions
already made, and the traps that have actually bitten. Read it before changing
anything non-obvious. Cross-cutting fleet knowledge lives in
[fleet-notes](https://github.com/stoatworks-labs/fleet-notes).
