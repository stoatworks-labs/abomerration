# Abomerration user guide

Abomerration is **sound-reactive chromatic aberration** for [Resolume](https://resolume.com)
Arena and Avenue, as an FFGL plugin — and the same lens again as an OpenFX plugin for Resolve,
Nuke, Natron and Vegas. Photographers spent a century getting rid of chromatic aberration. This
puts it back, wires it to the music, and keeps going well past anything glass could do.

The idea it is built on: **a lens does not split a picture into three channels.** It focuses every
wavelength at a slightly different magnification, and the coloured fringe at the edge of a frame
is the whole visible spectrum smeared along that path and then integrated by the sensor. So this
plugin displaces the picture *once per wavelength sample* and adds the results up through spectral
weights.

The familiar hard red/blue split is not a separate mode or a different code path — it is what this
becomes when you ask for three samples, because the three-sample weight table is the identity.
That is why one control spans "cheap 1990s channel offset" and "real prismatic fringe" with
nothing else changing.

![Radial dispersion with a full spectral fringe, growing toward the corners](hero.png)

*Radial at Prism 32 with Edges up: warm fringes on the inner side of each edge, cool on the outer,
growing with distance from the optical centre — and the flat areas untouched, because that is what
a real lens does.*

> **Before you rely on this:** 22 automated checks pass from a clean universal build. The GLSL
> field agrees with an independent C++ implementation to 7e-07 of a frame width across all four
> geometries — with a control case that must disagree — the rendered channel offsets match the
> displacement the controls asked for to better than a pixel, and the reaction arithmetic is
> proved identical whether the host counts in milliseconds or seconds.
>
> **It has been loaded into Resolume Arena and confirmed working**, which is the one thing none of
> those checks could establish. Still open: the **OpenFX build has never been opened in Resolve**,
> Nuke, Natron or Vegas — only smoke-tested through a probe; the **Windows build is compiled in CI
> and has never been run**; no GPU other than an Apple M4 Max has rendered it; and none of it has
> been through a show.
>
> This codebase was created with AI assistance, directed and reviewed by a human author.

---

## Installing

```
macOS    ~/Documents/Resolume Arena/Extra Effects/     (Abomerration.bundle)
Windows  %USERPROFILE%\Documents\Resolume Arena\Extra Effects\   (Abomerration.dll)
```

Avenue uses the same layout under its own folder name. There is also a macOS disk image and a
Windows installer, which put it there for you.

The macOS builds are **Developer ID-signed and notarised**, so there is nothing to clear. The
Windows builds are unsigned, but plugin files are not gated the way `.exe` files are — only the
installer trips SmartScreen, once.

### OpenFX hosts

Put `Abomerration.ofx.bundle` in `/Library/OFX/Plugins` (macOS) or
`C:\Program Files\Common Files\OFX\Plugins` (Windows).

**The OFX build is the manual lens only.** OFX has no beat information and no audio input —
neither is in the API — so the reactive controls are *absent* there rather than present and doing
nothing.

---

## Start here: pick a geometry, then turn on Show Field

**Geometry** is the half of the plugin that decides which way and how far each pixel moves.

- **Radial** — what a real uncorrected lens does. Zero on the optical axis, worst in the corners.
  Start here if you want it to look like glass.
- **Linear** — a prism in front of the lens, or a three-strip camera out of registration. No
  optical centre at all, so the whole frame is displaced equally.
- **Tangential** — rotates each wavelength about the centre instead of pushing it outward. No lens
  does this.
- **Turbulent** — direction taken from a drifting noise field. This is the abomination the plugin
  is named for.

Then turn **Show Field** on. It paints the dispersion magnitude over a dimmed picture with four
meters along the bottom for bass, mid, treble and beat.

It is worth the trip because **a flat region with an enormous displacement looks exactly like one
with none** — displacing a region of constant colour returns the same region. Without the overlay,
"is it hearing anything at all?" is unanswerable inside Resolume. Set the reaction up with it on,
then turn it off.

---

## Spectrum: three samples, or thirty-two

**Prism** is the sample count, and it is a *quality* control rather than a colour control. At
**RGB Split** you get the hard three-channel offset; at **Prism 32** you get a continuous
prismatic fringe. Nothing else changes — the field is identical, and only how finely the spectrum
is integrated differs.

A measured limit, since it is visible: at the cheapest setting a hard black-to-white edge leaves
about 5 of 255 of ripple in the fringe. More samples buy a *sharper* picture at the same
smoothness rather than less ripple, because each sample already reads from a mip level covering
twice the gap to its neighbour.

**Edges** weights the dispersion by local contrast. Real lateral aberration is invisible in a flat
area, so this is what makes it read as a lens. Wind it to zero and you get the misregistered
camera instead — every flat region shifted bodily.

---

## Making it listen

**Every reaction control is off by default.** Dropped on a layer with nothing routed, this is an
ordinary manual aberration lens and behaves like one.

There are four ways in, and they stack:

| | What it does |
| --- | --- |
| **Beat pulse** | Locked to the host's transport, on any division from a quarter beat to two bars, with a decay from a linear ramp to a hard click. |
| **Level** | An overall pump from the routed audio. |
| **Band split** | Bass, mid and treble each push *their own channel*, so the picture comes apart along with the mix rather than merely pumping. |
| **Channel trims** | Manual offsets, on top of everything above. |

**Depth is carved out of the amount rather than added on top.** At full depth, silence renders a
clean picture and the beat renders the whole effect — so the loud moments are what you dialled in,
not more than it.

The band split is the one worth spending time on. A level pump moves everything together; the
split moves red with the kick and blue with the hats, which is a different thing entirely and is
what stops the effect reading as a tremolo.

---

## If it looks wrong

**Nothing is happening at all.** Turn on **Show Field**. If the meters are dead, no audio is
routed — every reactive control starts at zero on purpose. If the field is bright but the picture
is unchanged, you are looking at a flat region: displacement has nothing to displace.

**It pumps but the picture never comes apart.** You are on Level rather than the band split.

**The fringe is banded rather than smooth.** Raise **Prism**. Three samples is a hard split by
definition.

**It looks like a badly registered camera, not a lens.** **Edges** is at zero. Real aberration only
shows at contrast.

**The corners are fine and the middle is wrong.** Radial does the opposite of that — you are
probably on Linear or Tangential.

**It runs a thousand times too fast in one host.** It should not; hosts that count in milliseconds
and hosts that count in seconds are both handled and tested against each other. If you ever see
it, that is a bug worth reporting.

---

## Cost

Measured on an M4 Max: **0.42–0.52 ms/frame at 1080p, 0.89–1.34 ms at 4K.** The sample count is
the thing you pay for, so Prism 32 on a 4K layer is the expensive corner.
