# MOON DS800 — saved source snapshot

Saved 2026-10-25. **This is build 28's behaviour with build 29's changes reverted.**

Build 29 was three speculative changes (`claims_edit_ccs` in module.json, `name`
on hierarchy levels, `short_name` on params) and it made things WORSE: the knob
still pinned, and automation stopped moving the knob at all. Reverted here.

## Files

| File | What it is |
|---|---|
| `moons800.c` | the engine — CS-80 voice, ARP 2600 modulation, Buchla timbre, 4012 ladder |
| `moons800.h` | engine types, the ladder coeff struct, the voice and instance structs |
| `ds800_mod.h` | the modulation matrix |
| `schwung_moons800.c` | **the plugin wrapper** — includes the base-value table fix |
| `moons800_meta.h` | chain_params + ui_hierarchy; **the wrapper includes THIS file** |
| `moons800_all_meta.h` | the merged pages (kept in sync with the above) |
| `moons800_mod_meta.h` | the matrix declarations |
| `module.json` | the manifest, restored to the shape that loads |
| `test_params.c` | the parameter tests |
| `MOON-DS800-HANDOFF-original.md` | the handoff as received, kept for comparison |

## State verified at save time

```
pages                      : 10
chain entries              : 60
inline params              : 60
max knobs per page         : 8 (the host grid; 9 spills a second page)
knobs on page, undeclared  : none
declared, on no page       : none
unreachable setters        : none
both header files agree    : True
cutoff step                : 0.001
wrapper base table         : True
```

Ten pages: Root · Osc 1 · Osc 2 · Filter · Amp Env · LFO · S&H · Mod 1-2 · Mod 3-4 · Mod 5-6

## The four rules that cost the most to learn

**1. The wrapper reads `moons800_meta.h`.**
It `#include`s that file and serves `DS800_UI_HIERARCHY` and `DS800_CHAIN_PARAMS`
from it. `moons800_all_meta.h` is the merged copy and must stay identical in
pages and params. Editing only one changes nothing on the device.

**2. Every control needs a page AND a reachable setter.**
Both failures look identical on the panel: a knob that renders and snaps to
zero. Declared-but-unpaged gives a generic `LVL:` widget; a setter spliced after
a sibling's `return 0;` is unreachable and never stores (`noise_lvl`, builds
20-22). Check both before changing anything else.

**3. A page holds at most EIGHT knobs.**
Nine spills the ninth onto a `- 2` overflow page. That is what made
`MOD 1-3 2` / `MOD 4-6 - 2` with one knob each.

**4. Parameters are declared inline on their hierarchy level.**
Schwung merges `chain_params` entries the hierarchy does not declare, and
`step 0.001` on every float gives the encoders finer travel.

## KNOWN OPEN ITEM — the knob graphic after automation

**The symptom.** Record automation on filter cutoff. Stop the sequence. Turn the
knob: **the value readout follows the turn, but the knob graphic stays at zero.**
You can hear it change. The knob only moves when the sequence plays.

**What this is.** Two indicators disagreeing. The readout comes from the plugin,
so the plugin is reporting correctly (the base-value fix below). The **knob
graphic is drawn from the automation lane**, which overlays the parameter.

Schwung's own docs describe the bug and its fix for their modules:

> "the label bands become inverted strips carrying the locked value. (The dials
> *jumped* to it at the time; see the next bullet for why they no longer do.)"

The framework fixed this; Machinemodule has the fix. **It is a framework-side
handshake for drawing a module-owned knob, not a header or engine field.**

**What was tried and did NOT fix it** (all reverted or harmless):
- engine curves / `step` size (0.001 vs 0.01) — build 26, no change
- inline parameter metadata — build 27
- wrapper `get_param` returning the BASE instead of the engine value — build 28
- `claims_edit_ccs` + level `name` + param `short_name` — build 29, **made it worse**

**What would settle it:** the `ui_hierarchy` JSON that Machinemodule actually
serves (not its C source), compared against DS800's. Or a developer who knows
`lane_edit.c` / the knob-grid drawing path in charlesvestal/schwung.

**Best next step:** post the symptom — "after recording automation, the knob
stays at zero while the value readout follows the turn" — to the Schwung
community. That phrasing is precise enough to be recognised.

## The base-value fix in the wrapper (build 28)

`get_param` used to answer with the ENGINE's value:

```c
const float v = ds800_get_param(&inst->engine, key);
```

While a lane drives a parameter the engine holds the lane's value, so the host
believed the knob had moved there. Schwung's rule is that a plain read
"answers the BASE, never the plugin's value". The wrapper now keeps a base table
(`struct { char key[24]; float val; uint8_t used; } base[96]`): `set_param`
stores into it, `get_param` reads from it and falls back to the engine only when
a key has no stored base. This fixed the numeric readout; the knob graphic is
still drawn from the lane.

## Other known open items

- **Polyphonic pressure is not implemented.** `ds800_set_pressure` and
  `ds800_set_note_pressure` are stubs discarding their argument. The pads send
  0xA0. This is the CS-80 half of the instrument.
- **`stolen_from` handles one level of voice stealing.** A two-deep chain still
  orphans a voice.
- **`ds800_sh.h` and `ds800_lfo.h` have never been seen.** The pre-filter noise
  source is therefore implemented beside the voices in `moons800.c` rather than
  routed through the S&H's own generator (which already has proper colour
  filtering and gain compensation).
- **The wrapper's `ds800_is_int_key` list is stale.** It still names `div1a`,
  `div1b`, `div2a`, `div2b` (removed with the Subharmonicon dividers) and omits
  `lfo1_rate`, `sh_rate`, `sh_amt`, `noise_lvl` and the mod slots, so those print
  as `0.5000` instead of `1`.
- **The knob test misreports ~14 knobs** (PW measured on a saw rather than a
  pulse; matrix src/dst needing a two-slot baseline).
- **`test_moons800.c` will not compile** — it exercises the removed divider keys.
- **Nothing has been compiler-checked locally.** Every build was verified by
  structure against the device's own successful ARM compiles.

## Build / install

```bash
cp savestate/*.h savestate/*.c  ~/Desktop/moonds800-ship/src/
cp savestate/module.json        ~/Desktop/moonds800-ship/
cd ~/Desktop/moonds800-ship && bash scripts/build.sh && bash scripts/install.sh
```

Package exactly `dsp.so` + `module.json`, flat, extracted INTO the module
folder. Do not nest them in a `moonds800/` directory.

## Engine constants worth knowing

```c
DS800_4012_FB         0.42    /* resonance feedback width, textbook 0.15 */
DS800_4012_RES_GAIN   2.6     /* resonance ceiling; ladder oscillates ~4  */
DS800_4012_LOSS       0.22    /* passband loss as resonance rises         */
DS800_4012_PASS_FLOOR 0.70    /* the floor = the STABLE variant, option B */
DS800_LADDER_CLAMP    8.0     /* headroom for the ring to bloom           */
DS800_VOICE_GAIN      (1.0/7.0)
```

Envelope and rate curves are exponential (`lo * pow(hi/lo, t)`), which is what
made the attack and cutoff usable. Rates interpolate through their Hz tables;
the S&H table is the ARP 2600's own .2–100 Hz clock range and the LFOs .03–30 Hz.
