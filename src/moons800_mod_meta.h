/* moons800_mod_meta.h -- MOON DS800, Stage 2: the Mod, LFO and S&H pages.
 *
 * WRITTEN WHOLE, and GENERATED. Rebuild with scratch/gen_all_meta.py.
 *
 * THE DESTINATION LIST IS ELEVEN. The engine gained Fold, Symmetry and Ring as
 * destinations so the sample & hold can step the fold -- the Buchla Easel
 * wavefolder is a sound source, and a source you cannot modulate is half an
 * instrument. DS800_DST_COUNT is 11 in ds800_mod.h; these six lists must say
 * the same eleven or the panel shows a confidently wrong label.
 *
 * NOISE COLOUR is Low / Pink / White, default 2 -- the one the device has
 * always shown. moons800_meta.h must say the same.
 *
 * sh_env is here: the 2600's clock fires the envelopes, which is what makes it
 * the instrument's rhythmic heart rather than just a sequencer pulse.
 */

#ifndef MOONS800_MOD_META_H
#define MOONS800_MOD_META_H

static const char *DS800_MOD_CHAIN_PARAMS =
  "[{\"key\":\"lfo1_rate\",\"name\":\"LFO1 Rate\",\"type\":\"enum\",\"options\":[\"0.03 Hz\",\"0.1 Hz\""
  ",\"0.3 Hz\",\"1 Hz\",\"3 Hz\",\"10 Hz\",\"30 Hz\"],\"default\":3},{\"key\":\"lfo1_shape\",\"name\":"
  "\"LFO1 Wave\",\"type\":\"enum\",\"options\":[\"Sine\",\"Tri\",\"Saw\",\"Square\",\"Random\"],\"defau"
  "lt\":0},{\"key\":\"lfo2_rate\",\"name\":\"LFO2 Rate\",\"type\":\"enum\",\"options\":[\"0.03 Hz\",\"0"
  ".1 Hz\",\"0.3 Hz\",\"1 Hz\",\"3 Hz\",\"10 Hz\",\"30 Hz\"],\"default\":4},{\"key\":\"lfo2_shape\",\"n"
  "ame\":\"LFO2 Wave\",\"type\":\"enum\",\"options\":[\"Sine\",\"Tri\",\"Saw\",\"Square\",\"Random\"],"
  "\"default\":0},{\"key\":\"lfo_sync\",\"name\":\"Sync\",\"type\":\"enum\",\"options\":[\"Off\",\"On\""
  "],\"default\":0},{\"key\":\"sh_rate\",\"name\":\"S&H Rate\",\"type\":\"enum\",\"options\":[\"0.2 Hz"
  "\",\"0.5 Hz\",\"1 Hz\",\"2 Hz\",\"4 Hz\",\"10 Hz\",\"30 Hz\",\"100 Hz\"],\"default\":5},{\"key\":\"s"
  "h_input\",\"name\":\"S&H Src\",\"type\":\"enum\",\"options\":[\"Noise\",\"LFO 2\"],\"default\":0},{"
  "\"key\":\"sh_slew\",\"name\":\"Slew\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01,\"default"
  "\":0},{\"key\":\"sh_env\",\"name\":\"Clock Env\",\"type\":\"enum\",\"options\":[\"Off\",\"On\"],\"de"
  "fault\":0},{\"key\":\"noise_colour\",\"name\":\"Noise\",\"type\":\"enum\",\"options\":[\"Low\",\"Pin"
  "k\",\"White\"],\"default\":2},{\"key\":\"mod1_src\",\"name\":\"1 Src\",\"type\":\"enum\",\"options\""
  ":[\"None\",\"LFO 1\",\"LFO 2\",\"S&H\",\"Clock\",\"Amp Env\",\"Filter Env\",\"Osc 2\",\"Pressure\","
  "\"Keytrack\"],\"default\":0},{\"key\":\"mod1_dst\",\"name\":\"1 Dst\",\"type\":\"enum\",\"options\":"
  "[\"None\",\"LP Cutoff\",\"Resonance\",\"Pitch\",\"Pulse Width\",\"VCA\",\"Pan\",\"Fold\"],\"default"
  "\":0},{\"key\":\"mod1_depth\",\"name\":\"1 Dep\",\"type\":\"float\",\"min\":-1,\"max\":1,\"step\":0."
  "01,\"default\":0},{\"key\":\"mod2_src\",\"name\":\"2 Src\",\"type\":\"enum\",\"options\":[\"None\","
  "\"LFO 1\",\"LFO 2\",\"S&H\",\"Clock\",\"Amp Env\",\"Filter Env\",\"Osc 2\",\"Pressure\",\"Keytrack\""
  "],\"default\":0},{\"key\":\"mod2_dst\",\"name\":\"2 Dst\",\"type\":\"enum\",\"options\":[\"None\",\""
  "LP Cutoff\",\"Resonance\",\"Pitch\",\"Pulse Width\",\"VCA\",\"Pan\",\"Fold\"],\"default\":0},{\"key"
  "\":\"mod2_depth\",\"name\":\"2 Dep\",\"type\":\"float\",\"min\":-1,\"max\":1,\"step\":0.01,\"default"
  "\":0},{\"key\":\"mod3_src\",\"name\":\"3 Src\",\"type\":\"enum\",\"options\":[\"None\",\"LFO 1\",\"L"
  "FO 2\",\"S&H\",\"Clock\",\"Amp Env\",\"Filter Env\",\"Osc 2\",\"Pressure\",\"Keytrack\"],\"default\""
  ":0},{\"key\":\"mod3_dst\",\"name\":\"3 Dst\",\"type\":\"enum\",\"options\":[\"None\",\"LP Cutoff\","
  "\"Resonance\",\"Pitch\",\"Pulse Width\",\"VCA\",\"Pan\",\"Fold\"],\"default\":0},{\"key\":\"mod3_dep"
  "th\",\"name\":\"3 Dep\",\"type\":\"float\",\"min\":-1,\"max\":1,\"step\":0.01,\"default\":0},{\"key"
  "\":\"mod4_src\",\"name\":\"4 Src\",\"type\":\"enum\",\"options\":[\"None\",\"LFO 1\",\"LFO 2\",\"S&H"
  "\",\"Clock\",\"Amp Env\",\"Filter Env\",\"Osc 2\",\"Pressure\",\"Keytrack\"],\"default\":0},{\"key\""
  ":\"mod4_dst\",\"name\":\"4 Dst\",\"type\":\"enum\",\"options\":[\"None\",\"LP Cutoff\",\"Resonance\""
  ",\"Pitch\",\"Pulse Width\",\"VCA\",\"Pan\",\"Fold\"],\"default\":0},{\"key\":\"mod4_depth\",\"name\""
  ":\"4 Dep\",\"type\":\"float\",\"min\":-1,\"max\":1,\"step\":0.01,\"default\":0},{\"key\":\"mod5_src"
  "\",\"name\":\"5 Src\",\"type\":\"enum\",\"options\":[\"None\",\"LFO 1\",\"LFO 2\",\"S&H\",\"Clock\","
  "\"Amp Env\",\"Filter Env\",\"Osc 2\",\"Pressure\",\"Keytrack\"],\"default\":0},{\"key\":\"mod5_dst\""
  ",\"name\":\"5 Dst\",\"type\":\"enum\",\"options\":[\"None\",\"LP Cutoff\",\"Resonance\",\"Pitch\",\""
  "Pulse Width\",\"VCA\",\"Pan\",\"Fold\"],\"default\":0},{\"key\":\"mod5_depth\",\"name\":\"5 Dep\",\""
  "type\":\"float\",\"min\":-1,\"max\":1,\"step\":0.01,\"default\":0},{\"key\":\"mod6_src\",\"name\":\""
  "6 Src\",\"type\":\"enum\",\"options\":[\"None\",\"LFO 1\",\"LFO 2\",\"S&H\",\"Clock\",\"Amp Env\",\""
  "Filter Env\",\"Osc 2\",\"Pressure\",\"Keytrack\"],\"default\":0},{\"key\":\"mod6_dst\",\"name\":\"6 "
  "Dst\",\"type\":\"enum\",\"options\":[\"None\",\"LP Cutoff\",\"Resonance\",\"Pitch\",\"Pulse Width\","
  "\"VCA\",\"Pan\",\"Fold\"],\"default\":0},{\"key\":\"mod6_depth\",\"name\":\"6 Dep\",\"type\":\"float"
  "\",\"min\":-1,\"max\":1,\"step\":0.01,\"default\":0},{\"key\":\"sh_value\",\"name\":\"S&H Out\",\"ty"
  "pe\":\"float\",\"readonly\":true,\"min\":-1,\"max\":1},{\"key\":\"clock_state\",\"name\":\"Clock\","
  "\"type\":\"int\",\"readonly\":true,\"min\":0,\"max\":1}]";

#endif /* MOONS800_MOD_META_H */
