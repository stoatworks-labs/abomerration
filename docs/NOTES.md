# Notes

Working notes for this repo: status, decisions, and the traps that have actually bitten.
Migrated out of Claude Code's memory on 2026-08-24, so they are written in the first
person and dated by when each thing was learned — that date is usually the useful part.

Cross-cutting notes that are not specific to this repo live in
[fleet-notes](https://github.com/stoatworks-labs/fleet-notes).

*abomerration — sound-reactive chromatic aberration FFGL+OFX plugin; PUBLIC MIT v0.1.0, demo + video + Reel all LIVE 2026-08-17, CONFIRMED WORKING IN ARENA; wants a v0.1.1 for the Division default*

**abomerration** (built and released 2026-08-17) — chromatic aberration as an
FFGL 2.1 effect for Resolume (`Abomerration`, ID **`AB01`**) plus an OpenFX
build. C++17 + GLSL 4.1, CMake, `~/Projects/resolume/abomerration`, **PUBLIC
MIT** at `stoatworks-labs/abomerration`, **v0.1.0 released with 6 assets**.

Name is a deliberate portmanteau of *aberration* and *abomination*. It was
typed "abomerition" in the first prompt and corrected mid-session to
**abomerration** — two r's, and the repo was renamed before anything was
pushed. Do not "fix" the spelling.

**The one idea:** a lens does not split a picture into three channels, it
focuses every wavelength at a different magnification and the sensor integrates
the smear. So it displaces the picture **once per wavelength sample** and sums
through spectral weights — and the hard RGB split is the *identity case* of the
same loop at three samples, not a separate mode. One control therefore spans
"cheap channel offset" and "real prismatic fringe".

Two halves that never touch: the **field** (`Dispersion.cpp`, mirrored in
`shaders/Field.cpp` — the only mirror) and the **weight table**
(`Dispersion.cpp`, NOT mirrored, uploaded as a uniform). Four geometries ×
four spectrum settings is eight pieces of code, not sixteen.

**Weights are normalised per channel to sum to 1.** That is what makes Spectrum
a quality control and not a tint control; `--spectrum` fails without it.

## Four real defects, all found by measurement

- **The spectral quadrature aliased its source.** Summed second-difference
  energy at a 108px path was 382/388/384 for Prism 8/16/32 — saturated and not
  even ordered. No sample count fixes it (one sample per pixel of a 108px path
  is 108 fetches). Fixed with a **mipmapped copy buffer**: each sample reads the
  mip level covering the gap. Same lesson as tilter's box-downsampled blur.
- **The prefilter width must be TWICE the sample spacing, not the spacing.** A
  box of width d has its first zero an octave above Nyquist and leaks ripple.
  Ripple of 255 went 16.6/8.7/1.7 → **4.8/2.0/1.2**.
- **Sync had a Bar mode byte-identical to Beat** (`bars * 4.0` is the beat
  count). Two dropdown entries rendering the same picture. Found by
  `tools/sweep.py`, which called Sync dead and was right. Sync is now
  Free/Locked; **Division** (quarter beat → two bars) carries what Bar meant.
- **The harness rendered every frame upside down and three checks passed.**
  `buildScene` is top-down, `glTexImage2D` treats row 0 as the BOTTOM, and a
  `flipRows` on readback cancelled it for anything symmetric. `--field`,
  `--offset` and `--spectrum` all passed because each measures something a
  vertical flip does not change. One flip now, in `uploadScene`.

## Three metrics for banding, two of them wrong

Recorded in `checkQuadrature` because both looked reasonable. **Summed**
second-difference energy is dominated by how *many* joins there are, so it
ranked Prism 32 worse than Prism 8. **Worst single** second difference came out
at 765/L independent of sample count — that is the true curvature of a hard edge
through three primaries, not banding. **Difference from the densest setting**
measured the prefilter's deliberate softening. **Ripple at the sample spacing**
is blind to any linear ramp and is the one that works.

## Traps specific to this plugin

- **`barPhase` pinned at 0 makes Beat Decay and Division read as dead** — the
  bar recovery then recovers bar 0 exactly, the envelope sits at 1.0 forever,
  and at the instant of a beat neither control changes anything. The harness
  drives a transport whose barPhase tracks its clock.
- **`Angle` at 0 and 1 is the same picture** (full turn, both ends ±π). Sweep a
  quarter turn either side.
- **`cmake -B build` reuses a warm cache**, so a tree once configured arm64 stays
  arm64 and "fresh configure" verifies the wrong binary. `verify.sh` builds into
  `build-verify/` with architectures stated explicitly. This is a gap in
  tilter's verify.sh too.
- Band split is **logarithmic in bin index** (bass = bottom sixteenth). Equal
  thirds puts the entire musical range in "bass" on a linear-frequency buffer.

**The ms-vs-seconds check was itself broken from 2026-08-22 to 2026-08-25, and
the shape of the failure is the lesson.** `abomtest --clock` exists for one trap:
Resolume sends `SetTime` in MILLISECONDS, the FFGL header never says so, and a
plugin consuming the clock raw is 1000x fast in the only host anyone runs it in
while every other offline check still passes. The About-surface commit
(`dfeb602`) replaced the plugin's magnitude guess at the unit with a vote against
the wall clock — a better rule, but one **an offline harness cannot exercise**:
60 frames render far faster than real time, every wall delta lands under the
0.5 ms floor the vote needs, and nothing is ever decided. So it added
`SetClockScaleForTest` to declare the unit instead, and pasted the same line into
four places — one of them the lambda checkClock uses for BOTH hosts, saying
`1.0` (seconds) for each.

**The number is the tell.** It failed at 11.80000 against a wanted 1.96667 — six
times out, not a thousand. That is `kMaxFrameDelta * 59 * 2` = `0.1 * 59 * 2`:
the millisecond host was told its ms were seconds, so every frame stepped 16.67
"seconds" and every delta clamped. The reported figure was the clamp, not a
measurement, which is exactly why it did not look like the thousand-fold bug it
was filed as. **The plugin was correct throughout.** Fixed by making the scale a
parameter; proven by putting `raw` back in place of `raw * clockScale`, which
restores 11.80000 precisely.

Still uncovered, and honestly so: the vote itself. Deciding the unit from the
wall clock is run-time behaviour no offline harness reproduces.

**CI never saw any of it** — `ci.yml` does not run the full `verify.sh`, so this
was green in CI and red on the desk for three days, and blocked the v0.1.3
release until someone ran the harness by hand. Same shape as burin's sweep and
flipbook's verify.sh, found in the same week: **a harness that is not in CI is a
harness that is red until someone looks.**

**Verified 23/23 by `tools/verify.sh`** from a clean universal build: field
mirror to 7e-07 with a control case that must disagree (0.14); rendered channel
offsets to better than a pixel; flat-field energy preservation; ripple below
5/255; 14 GL-free reaction assertions incl. bar recovery to 1.2e-07; ms-vs-
seconds hosts agreeing; all 24 controls alive; `ffgltest` instantiation through
`plugMain`; OFX plist vs binary + ad-hoc signing. Cost 0.42–0.52 ms at 1080p,
0.89–1.34 ms at 4K on an M4 Max.

**CONFIRMED WORKING IN RESOLUME ARENA** (user, 2026-08-17) — the one claim none of
the 23 checks could establish, since every check drives the plugin class directly
or through `plugMain` and none can say whether 24 controls present sensibly in a
real inspector. Scoped to Arena only: the **OFX build has never been opened in
Resolve** (only `ofxprobe`), and **Windows is CI-built and never run**.

**The YouTube description still says "never been loaded into Resolume or Resolve"**
and needs a Studio edit — there is no write API, so it is Claude-in-Chrome plus the
Made-for-Kids trap. See [youtube studio edits](https://github.com/stoatworks-labs/fleet-notes/blob/main/notes/reference_youtube_studio_edits.md).

## Release state at 2026-08-17

Done: repo public, v0.1.0 tagged, CI published 6 assets, CI+Dependabot+funding
config, website `projects.json` entry + `shots.json` + generated thumbnail
(`/software/abomerration/` builds).

Also done: autosign (3/3 notarised, Developer ID, stapled), check-notarised,
gen-downloads (README block + downloads.json), and the **browser demo, LIVE at
`abomerration-demo.stoatworks-labs.com`** and linked from both the README and
projects.json. `verify.sh` is now **23 checks**.

**All seven homes are done.** YouTube **`l49OH5N5KUI`**, Instagram Reel
**`DcIo2njDtcn`**, both embeds live, site deployed and verified by content.

`abomtest` grew **`--pipe` / `--script` / `--fps`**, and because this is the only
sound-reactive plugin in the fleet and the cuts are silent, `--pipe` also
**generates the drive**: kick/hats/bass at 120 bpm written into the FFT parameter
with the transport driven to match. The units trap: the host writes **raw FFT
magnitudes** and `updateAudio` sqrt's them, so a level of 0.9 is injected as 0.81.

**`--pipe` immediately found a real defect**: the **default Division was element 0
(a quarter beat)** where every preset says 2.0 (Beat), so the pulse fired four
times per beat out of the box. Measured as a reaction repeating every 3.75 frames
at 30fps where 120 bpm demands 15. Fixed on main — **this is a behaviour change
after v0.1.0 and wants a v0.1.1** (no param ids move, so saved comps are safe).

Video notes: **Enter5_12 is the beat that states the whole idea** even though
tilter's notes call that clip unusable — a blur needs mass, a prism wants a thin
white line on black. The **sentinel BEATS entry at 42.0** is mandatory or the last
six seconds are silently discarded. Instagram needs the cover committed to the repo
and registered **SHA-pinned** in `social-covers.json`, and `make_social.py`'s
`CUTS` list is hand-maintained (an unknown name is a silent no-op).

## The demo

The shared kit's `gl.js` already had `PassBuffer({mip:true})` and
`generateMipmap()`, so no fleet-wide change was needed — but it has **no audio at
all**. So the page **synthesises** a kick-and-hats pattern at 120 bpm from
oscillators and shaped noise through an `AnalyserNode`. A bundled track is a
licensing question, a mic needs a permission prompt, and an `<audio>` element
needs a user gesture anyway — so the **Audio toggle is both the gesture and an
honest stand-in for Resolume's audio-source picker**.

Kit API notes that cost time: `Params` has **`get(id)`**, not a bulk accessor;
`PassBuffer`'s clear is **`clearTo(r,g,b,a)`**; `bind()` chains but `clear()` does
not exist; and `sources.render()` returns **`{texture,width,height}`**, whose size
can differ from the render size (hence two different half-texels, as in the
plugin). The kit does **not** support a `?preset=` query key.

**The bug worth remembering:** the audio `start()` assigned `this.ctx` before
`await ctx.resume()`, so the next frame saw a live context whose gain node did not
exist and threw out of `connect()`, taking the whole demo down. Publish nothing to
the instance until the graph is built — an await inside a constructor-like method
is a window, and that one was exactly one frame wide.

**Backticks were removed from the GLSL comments** so the shader text can sit in a
JS template literal unescaped, which is what keeps `check_shaders.py` a
character-for-character comparison. Do not put them back.

Also: **neither tilter nor abomerration is a member of the `video-plugins`
suite** in `suites.json` — a pre-existing editorial gap affecting both, needing
hand-written role/where/blurb/points.

Traps are in the repo's `AGENTS.md`. See [tilter](https://github.com/stoatworks-labs/tilter/blob/main/docs/NOTES.md) (`tilter`) (the donor),
[ffgl sdk bugs](https://github.com/stoatworks-labs/fleet-notes/blob/main/notes/reference_ffgl_sdk_bugs.md), [ffgl audio bpm patterns](https://github.com/stoatworks-labs/fleet-notes/blob/main/notes/reference_ffgl_audio_bpm_patterns.md),
[new plugin repo copy traps](https://github.com/stoatworks-labs/fleet-notes/blob/main/notes/reference_new_plugin_repo_copy_traps.md), [plugin factory presets](https://github.com/stoatworks-labs/fleet-notes/blob/main/notes/reference_plugin_factory_presets.md),
[resolume demo kit](https://github.com/stoatworks-labs/fleet-notes/blob/main/notes/reference_resolume_demo_kit.md), **release workflow** (working-practice note, kept in Claude memory).
