# abomerration

Sound-reactive chromatic aberration — the whole spectrum smeared along a path and
integrated, rather than three channels offset — as an FFGL effect for Resolume
Arena/Avenue, plus an OpenFX build for Resolve/Nuke/Natron/Vegas. C++/GLSL, CMake
MODULE → universal `.bundle` (macOS) + Windows `.dll`. Public MIT repo.

Read `AGENTS.md` before changing the field, the weight table, the prefilter or the
clock.

## Commands (CMake)
- Configure: `cmake -B build -DCMAKE_BUILD_TYPE=Release`
- Fast dev build: add `-DCMAKE_OSX_ARCHITECTURES=arm64`
- Build: `cmake --build build`
- Install to Resolume: `cmake --install build`
- Render a frame offline: `./build/abomtest --out /tmp/frame.png`
- Contact sheet of every geometry × spectrum: `./build/abomtest --sheet /tmp/sheet.png`
- The test card on its own: `./build/abomtest --scene /tmp/card.png`
- List parameters, with types and defaults: `./build/abomtest --list`
- Set anything by its host-facing name:
  `--set "Geometry=3" --set "Amount=0.6" --set "Spectrum=2"`
- Render size: `--size 1920x1080`
- Advance the synthetic transport: `--frames 60` (60 fps, 120 bpm)

## OpenFX build
- `source/ofx/AbomerrationOFX.cpp` → `build/Abomerration.ofx.bundle` (target
  `AbomerrationOFX`, `-DBUILD_OFX=OFF` to skip).
- `Dispersion.cpp`, `Drive.cpp` and `Controls.cpp` link straight from source, so
  the field, the weight table and every parameter curve have ONE home and a preset
  cannot mean different things in Resolume and Resolve. Only the per-pixel pass
  structure is mirrored there.
- **The reactive controls are absent, not inert.** OFX has no beat info and no FFT.
  `Drive.cpp` is still called with a zeroed input so the manual lens falls out of
  the same arithmetic.
- Smoke test:
  `../resolume-ofx-bridge/build/ofxprobe --dir build --render com.stoatworks.abomerration --size 640x360 --out /tmp/a.bmp`
- Install for Resolve: copy the bundle into `/Library/OFX/Plugins`.

## Verify
- Everything (22 checks, clean universal build): `tools/verify.sh`
- The GLSL field against `Dispersion.cpp`: `./build/abomtest --field`
- The picture moves the distance asked for: `./build/abomtest --offset`
- Spectrum preserves energy: `./build/abomtest --spectrum`
- The quadrature leaves no footprint: `./build/abomtest --quadrature`
- The reaction arithmetic (no GL): `./build/abomtest --drive`
- ms vs seconds hosts: `./build/abomtest --clock`
- Presets distinct and non-degenerate: `./build/abomtest --presets`
- No dead controls: `python3 tools/sweep.py`
- Render cost: `./build/abomtest --bench`

## Notes
- **The field and the weight table are independent.** A geometry says which way
  and how far; the table says what each wavelength counts as. Four geometries ×
  four spectrum settings is eight pieces of code, not sixteen.
- **`Field.cpp` mirrors `Dispersion.cpp`** and is the only mirror in the repo.
  Blocks are marked `//= mirrored` in both. Change one → run `--field`. The probe
  shader is assembled from the *same string* the plugin uses, so the test checks
  the real shader rather than a lookalike.
- **The weight table is NOT mirrored.** It is built in C++ and uploaded as a
  uniform. Keep it that way.
- **Weights are normalised per channel to sum to 1.** That is what makes Spectrum
  a quality control instead of a tint control. `--spectrum` fails without it.
- **RGB Split is exactly 3 samples with an identity table, and never prefilters.**
  It is a specific look, not a low-quality version of the others.
- **Every wavelength sample reads from a mip level covering TWICE the gap to its
  neighbour.** One gap is an octave above Nyquist and leaks ripple. See
  `Copy.cpp` for the before/after numbers.
- **The copy pass exists so the picture can be mipmapped** and so MaxUV is
  resolved once. Mip levels of a padded host texture average in undrawn padding.
- **Drift integrates; the beat envelope is absolute.** `time * rate` makes the
  noise field jump whenever the control is touched. A beat has to land on the grid.
- **Resolume sends `SetTime` in MILLISECONDS.** The unit is detected from the first
  plausible frame delta. `--clock` is the only check that would catch getting this
  wrong.
- **`Centre Y` = 0 is the top.** GL's v runs the other way; the flip happens once,
  in each shader's `main()`, and the field works in picture space throughout.
- **Option parameters hold the element value**, not 0..1, and are read through
  `controls::option()`. Standard parameters are all 0..1 and converted in
  `Controls.cpp` — `SetParamInfo` clamps a ranged default.
- **`SetTextParameter` must be overridden** or no real host can instantiate the
  plugin at all. `ffgltest` in verify.sh is what catches it.
- **Sync is Free/Locked, deliberately two options.** Division already spans a
  quarter beat to two bars; a separate Bar mode was byte-identical to Beat.
- macOS build must be universal (arm64 + x86_64). Verify with `lipo`, never the
  build log — and note `cmake -B build` reuses a warm cache, which is why
  `verify.sh` builds into `build-verify/` with the architectures stated explicitly.
- `layout`, `flat`, `active`, `filter`, `input`, `output`, `sample`, `common` are
  GLSL reserved words. Shader errors surface only at runtime, in the diagnostics
  log — and the disperse shader is assembled from three strings, so any line
  number the driver reports refers to a file that does not exist.
- Public repo. "Commit" = commit **and** push.

## Diagnostics

`source/Diag.{h,cpp}` — log file only, no crash handler (this runs inside
Resolume), no bundle command. It covers the failures that actually happen: a
shader that will not compile, which otherwise looks like "the effect does nothing"
with no message anywhere, and the host clock's units at frame 60.

    ~/Library/Logs/abomerration/abomerration.YYYY-MM-DD.log
