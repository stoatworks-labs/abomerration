"""Every parameter must actually change the picture.

A GLSL uniform name that does not match the C++ is silently ignored:
glGetUniformLocation returns -1, glUniform on -1 is a documented no-op, and
nothing in the build says a word. A control can therefore be completely dead
while everything compiles, links, loads and renders. Nothing else in this repo
catches that.

So: render each parameter at both ends of its range against a baseline where
every stage is switched on, and report any that made no difference.

    python3 tools/sweep.py

Run it after adding a parameter, renaming a uniform, or moving anything between
the C++ and the GLSL. Exit code 1 means something is dead.

------------------------------------------------------------------- the traps

Most of this plugin's controls are SUPPOSED to do nothing in the default
configuration, and a sweep that ignores that reports half of them dead and is
right to.

  * **The FFT buffer is not a control and must be skipped.** `Audio` is an
    FF_TYPE_BUFFER whose 64 elements the host writes; its own float value is
    meaningless, so setting it to 0 and then 1 renders the same frame twice and
    reports a working audio input as dead. It is excluded by type, along with the
    About block's text line and buttons.

  * **Most geometry controls belong to one geometry.** Angle does nothing at all
    in Radial, because a radial field takes its direction from the radius.
    Falloff does nothing in Linear or Turbulent, which have no r term. Turbulence
    and Drift only exist in Turbulent. Centre X and Centre Y do nothing in
    Linear, which has no optical centre. That is what CONTEXT is for.

  * **Every reactive control needs the reaction switched on.** Beat Depth needs
    Sync on a grid; Beat Decay and Division need a Beat Depth to shape, *and* a
    moment that is not exactly on the beat -- at the instant of a beat the
    envelope is 1.0 whatever its decay, so the harness drives a transport whose
    barPhase tracks its clock rather than sitting at zero. Band Route needs a
    Band Depth to route.

  * **Drift is integrated, so one frame barely moves it.** At the top of its
    range it advances the noise field by two units per second, which over a
    single 60th of a second is 0.03 -- a change far below the noise floor of two
    renders. It is swept over a full second instead. FRAMES is that.

If a control ever reads dead, work out what is masking it before assuming the
test is wrong.
"""
import os
import struct
import subprocess
import sys
import tempfile
import zlib

# verify.sh builds into its own directory with the shipping architectures, and
# runs this from there -- so honour ABOMTEST rather than hardcoding a path that
# would silently sweep a different binary from the one being verified.
ABOMTEST = os.environ.get("ABOMTEST", "./build/abomtest")
SIZE = "640x360"
SCRATCH = tempfile.mkdtemp(prefix="abomsweep")

# A baseline with every stage active, so nothing reads dead merely because the
# thing it modifies is switched off.
#
# Radial with an off-centre optical centre is deliberate: it is the configuration
# in which both centre coordinates and the falloff all reach the picture at once.
# A centred radial field is symmetric and would hide a centre that never got to
# the shader.
BASE = {
    "Geometry": 0,
    "Amount": 0.50,
    "Centre X": 0.42,
    "Centre Y": 0.56,
    "Angle": 0.50,
    "Falloff": 0.50,
    "Spectrum": 2,
    "Turbulence": 0.40,
    "Drift": 0.50,
    "Red Push": 0.50,
    "Green Push": 0.50,
    "Blue Push": 0.50,
    "Sync": 0,
    "Beat Depth": 0.0,
    "Beat Decay": 0.45,
    "Division": 2,
    "Level Depth": 0.0,
    "Band Depth": 0.0,
    "Band Route": 0,
    "Edges": 0.40,
    "Fringe": 0.30,
    "Show Field": 0,
    "Mix": 1.0,
}

# Parameters that only do anything in one configuration, and the baseline change
# that switches it on.
CONTEXT = {
    # A radial field takes its direction from the radius, so Angle is only a
    # control in the two geometries that have a direction of their own.
    "Angle": {"Geometry": 1},
    # No r term in Linear or Turbulent, so no exponent on it either.
    "Falloff": {"Geometry": 0},
    "Turbulence": {"Geometry": 3},
    "Drift": {"Geometry": 3},
    # The reaction. Sync off the grid means a flat zero envelope by design.
    # Sync chooses whether the envelope follows the grid; with no depth handed to
    # the beat it has nothing to choose about.
    "Sync": {"Beat Depth": 0.90},
    "Beat Depth": {"Sync": 1},
    "Beat Decay": {"Sync": 1, "Beat Depth": 0.90},
    "Division": {"Sync": 1, "Beat Depth": 0.90},
    "Band Route": {"Band Depth": 0.90},
}

# Endpoints to sweep between, where 0 and 1 are the wrong pair.
ENDS = {
    # Discrete option parameters: sweep the real element range.
    "Geometry": (0, 3),
    "Spectrum": (0, 3),
    # A full turn, so 0 and 1 are both +/- pi -- the same direction, and the same
    # picture. A quarter turn either side of zero is the pair that differs.
    "Angle": (0.25, 0.75),
    "Sync": (0, 1),
    "Division": (0, 5),
    "Band Route": (0, 3),
    "Show Field": (0, 1),
    # At the extremes the optical centre leaves the frame and the field is nearly
    # uniform at both ends, which renders two similar frames and reports a
    # working control as dead.
    "Centre X": (0.30, 0.70),
    "Centre Y": (0.30, 0.70),
    # Amount at 0 is a genuine bypass, so sweep from a small real dispersion --
    # otherwise this measures "the effect does something", which every other
    # control's sweep already establishes.
    "Amount": (0.15, 0.85),
}

# How many frames of the synthetic 60 fps transport to advance before measuring.
# Two is enough for anything that reacts within a frame; Drift integrates and
# needs a real interval. See the module docstring.
FRAMES = {
    "Drift": 60,
}

# Not controls: the About block is a text line and four buttons that open a
# browser, and Audio is a host-written FFT buffer whose own value means nothing.
SKIP_TYPES = {"text", "event", "buffer"}


def render(path, overrides, frames):
    args = [ABOMTEST, "--out", path, "--size", SIZE, "--frames", str(frames)]
    merged = dict(BASE)
    merged.update(overrides)
    for key, value in merged.items():
        args += ["--set", f"{key}={value}"]
    result = subprocess.run(args, capture_output=True, text=True)
    if result.returncode != 0:
        print("render failed:", result.stdout, result.stderr)
        sys.exit(1)
    with open(path, "rb") as handle:
        return handle.read()


def pixels(png):
    i = 8
    idat = b""
    width = height = 0
    while i < len(png):
        length = struct.unpack(">I", png[i:i + 4])[0]
        kind = png[i + 4:i + 8]
        data = png[i + 8:i + 8 + length]
        if kind == b"IHDR":
            width, height = struct.unpack(">II", data[:8])
        if kind == b"IDAT":
            idat += data
        i += 12 + length
    raw = zlib.decompress(idat)
    stride = width * 4
    return b"".join(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)] for y in range(height))


def difference(a, b):
    pa, pb = pixels(a), pixels(b)
    changed = 0
    total = 0
    count = len(pa) // 4
    for i in range(0, len(pa), 4):
        d = max(abs(pa[i] - pb[i]), abs(pa[i + 1] - pb[i + 1]), abs(pa[i + 2] - pb[i + 2]))
        if d > 2:
            changed += 1
        total += d
    return changed / count * 100.0, total / count


def parameters():
    listing = subprocess.run([ABOMTEST, "--list"], capture_output=True, text=True)
    if listing.returncode != 0:
        print("could not list parameters:", listing.stderr)
        sys.exit(1)

    out = []
    for line in listing.stdout.strip().splitlines()[1:]:
        fields = line.split()
        if len(fields) < 4:
            continue
        # `--list` prints: id, type, name (which may contain spaces), default.
        kind = fields[1]
        name = " ".join(fields[2:-1])
        if kind in SKIP_TYPES:
            continue
        out.append(name)
    return out


def main():
    if not os.path.exists(ABOMTEST):
        print(f"{ABOMTEST} not found -- build first")
        return 1

    names = parameters()
    print(f"{'parameter':<14} {'pixels changed':>15} {'mean delta':>11}   verdict")

    dead = []
    for name in names:
        low, high = ENDS.get(name, (0.0, 1.0))
        context = CONTEXT.get(name, {})
        frames = FRAMES.get(name, 2)

        a = render(os.path.join(SCRATCH, "a.png"), {**context, name: low}, frames)
        b = render(os.path.join(SCRATCH, "b.png"), {**context, name: high}, frames)

        percent, mean = difference(a, b)
        # A tenth of a per cent of the frame is a real change; anything below is
        # dithering and rounding between two renders of the same picture.
        alive = percent > 0.1
        if not alive:
            dead.append(name)

        notes = []
        if context:
            notes.append(", ".join(f"{k}={v}" for k, v in context.items()))
        if frames != 2:
            notes.append(f"{frames} frames")
        note = "  (" + "; ".join(notes) + ")" if notes else ""

        print(f"{name:<14} {percent:>14.2f}% {mean:>11.3f}   {'ok' if alive else 'DEAD'}{note}")

    print()
    if dead:
        print("DEAD CONTROLS:", ", ".join(dead))
        print("Check the uniform name matches the GLSL, and that nothing in BASE masks it.")
        return 1

    print(f"all {len(names)} controls reach the picture")
    return 0


if __name__ == "__main__":
    sys.exit(main())
