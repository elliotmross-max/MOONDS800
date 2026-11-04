# MOON DS800

A four-voice analogue polysynth for Ableton Move, as a
[Schwung](https://github.com/charlesvestal/schwung) sound generator.

**Status: working.** Builds for the Move, installs from the module tarball, and
plays on hardware. Ten pages, 60 parameters.

---

## What it is

Four voices, each with five sound sources and its own filter:

```
  ┌── osc 1 ──► wavefolder ──┐
  │                          ├──► mixer ──► ladder filter ──► VCA ──► out
  └── osc 2 ──► wavefolder ──┘        ▲
          ring modulator ─────────────┤
          sub oscillator ─────────────┤
          noise ──────────────────────┘
```

**Two oscillators**, each with a **triangle wavefolder** and a **symmetry**
control. Fold adds harmonics; a filter can only take them away. That is the
whole reason the folder is a sound source rather than another filter setting.

**A ring modulator**, a **sub oscillator** an octave below osc 1, and
**noise** with Low, Pink and White settings.

**One resonant lowpass ladder filter** per voice, with its own contour and key
tracking. There is no highpass and no filter mode switch.

**Each key responds to pressure on its own.** Leaning into one pad opens that
note's filter and leaves the others untouched — the filter movement follows
your fingers, note by note.

**Six modulation slots**, any source to any destination: two free-running LFOs,
a stepped sample and hold on its own 0.2–100 Hz clock, both envelopes, osc 2 at
audio rate, the noise source, key position, and per-key pressure. Destinations
include cutoff, resonance, pitch, pulse width, level, pan, fold, symmetry and
ring depth.

## Installing on Move

Install from the Module Store in Schwung, or from the release tarball:

```bash
scp moonds800-module.tar.gz ableton@move.local:/data/UserData/
ssh ableton@move.local 'tar -xzf /data/UserData/moonds800-module.tar.gz \
  -C /data/UserData/schwung/modules/sound_generators/'
```

The tarball's top level is `moonds800/`. Extract it into the folder **above**
the module folder, not into the module folder itself.

Reboot the Move, then add MOON DS800 to a Signal Chain slot.

## First five minutes

1. Play a few pads. Four notes at once is normal.
2. Turn up **Pressure to Cutoff**, then hold two pads at different pressures.
   Each note should brighten independently. This is the instrument's centre.
3. Turn **Fold** up on either oscillator. At 0 the wave is clean; past halfway
   it becomes a ragged harmonic cluster.
4. Set a matrix slot's source to **S&H** and its destination to **Fold**. The
   tone steps in rhythm with the sample and hold clock.
5. **Mono/Poly** on the root page: Mono is last-note priority, and releasing the
   top note falls back to a held one.

## Pages

| Page | Knobs |
|---|---|
| **Root** | LP Cutoff · Resonance · Osc 1 · Osc 2 · Noise · Sub · Volume · Mono/Poly |
| **Osc 1** | Wave · PW · Fold · Octave |
| **Osc 2** | Wave · PW · Fold · Octave |
| **Filter** | Env Amount · Keytrack · Filter Attack · Decay · Sustain · Release |
| **Amp Env** | Attack · Decay · Sustain · Release · Velocity |
| **LFO** | LFO1 Rate · LFO1 Wave · LFO2 Rate · LFO2 Wave · Sync |
| **S&H** | Rate · Source · Slew · Noise · Clock Env · Amount |
| **Mod 1–2** | six knobs: Source, Destination, Depth for two slots |
| **Mod 3–4** | the same, for slots three and four |
| **Mod 5–6** | the same, for slots five and six |

A page holds at most eight knobs. That is the host grid, not a limitation of
the instrument.

## Parameters — 60

Every control is reachable: each has a page to live on and a setter the host can
drive. Continuous controls use `step: 0.001` for fine encoder travel. Discrete
controls — waveform, octave, LFO shape, noise colour, modulation source and
destination — are enums, so the display names what you picked rather than
showing a number.

## Files

```
src/moons800.c                  the engine: oscillators, folders, ring, filter
src/moons800.h                  engine types, ladder coefficients, voice structs
src/ds800_lfo.c / .h            the two LFOs
src/ds800_sh.c / .h             sample and hold and its clock
src/ds800_mod.c / .h            the modulation matrix
src/moons800_meta.h             chain_params and ui_hierarchy
src/moons800_all_meta.h         the merged pages; must stay identical in content
src/moons800_mod_meta.h         the matrix declarations
src/schwung_moons800.c          the plugin wrapper
src/test_params.c               the parameter tests
module.json                     the manifest
scripts/{build,install,test}.sh build, deploy, verify
vendor/schwung/plugin_api_v1.h  Schwung's header, vendored
```

## Build and test

Needs only a C compiler for the tests; no Schwung checkout, no device:

```bash
bash scripts/test.sh
```

To cross-compile for the Move and deploy:

```bash
bash scripts/build.sh                        # cross-compile dsp.so in Docker
DEVICE=ableton@move.local bash scripts/install.sh
```

## Design notes

### The module only loads in one exact shape

Learned the hard way, over many builds. All three of these must hold or the
module appears in the menu and the slot stays empty:

- `module.json` declares `"dsp": "dsp.so"`, `"api_version": 2`, and a
  `capabilities` block containing `"chainable": true`.
- The binary is named `dsp.so` inside the package.
- The package is **flat**: `dsp.so` and `module.json` at the tarball root,
  extracted straight into the module folder. Wrapping them in a
  `moonds800/` directory and extracting into the parent nests them one level
  too deep and the module never loads.

### The wrapper reads `moons800_meta.h`

The wrapper `#include`s that file and serves `DS800_UI_HIERARCHY` and
`DS800_CHAIN_PARAMS` from it. `moons800_all_meta.h` is the merged copy and must
stay identical in pages and params. Editing only one changes nothing on the
device.

### Every control needs a page and a reachable setter

Both failures look the same on the panel: a knob that renders and snaps back to
zero. A parameter declared but on no page gets a generic widget with nothing to
store it in. A setter spliced after a sibling's `return 0;` is unreachable and
never stores.

### `plugin_api_v2_t` is closed

The wrapper includes the host's header rather than restating the struct, uses
designated initializers so an omitted field stays NULL instead of shifting
every pointer after it, and carries a `_Static_assert` on field order.

### Voices retire on their envelope, not on note-off

Note-off sets a voice releasing. It renders until its envelope reaches idle, so
a long release is never cut short by the next note.

### The engine renders float; the wrapper converts

The engine works in float stereo and knows nothing about the host. The wrapper
clips and converts to interleaved int16 once, at the boundary.

## Known open items

- **The cutoff knob graphic can disagree with its readout.** The readout comes
  from the plugin and is correct; the knob graphic is drawn from the automation
  lane, which overlays the parameter. A base-value table in the wrapper keeps
  the readout right.
- **A page holds at most eight knobs.** Nine spills the ninth onto an overflow
  page.

Earlier speculative changes — `claims_edit_ccs` in `module.json`, `name` on
hierarchy levels, `short_name` on params — made things worse rather than better
(a knob that pinned, and automation that stopped moving it) and are not to be
retried.

## Credits and licensing

Original code, MIT. No third-party source is copied. Schwung's plugin API header
is MIT, © Charles Vestal and contributors, and is vendored verbatim in
`vendor/schwung/`.

Thanks to [Schwung](https://github.com/charlesvestal/schwung) for the module
platform and its documentation, and to
[Model-72](https://github.com/sd88me/schwung-model72) and
[setBfree for Schwung](https://github.com/pdubucq/schwung-setbfree) for the
module layout conventions followed here.
