/* ds800_mod.c -- MOON DS800, Stage 2: the modulation matrix.
 *
 * Plain C. No host types, no allocation, no library calls.
 *
 * Read ds800_mod.h for the design. The one rule that matters in here: both
 * the source and the destination are SWITCHES ON ENUMS, never function
 * pointers. Six slots x four voices x one sample is 24 reads a sample, and an
 * indirect call at each of those is how a matrix eats an audio callback.
 */

#include "ds800_mod.h"

#include <string.h>

const char *const ds800_mod_source_names[DS800_SRC_COUNT] = {
    "None",
    "LFO 1",
    "LFO 2",
    "S&H",
    "Clock",
    "Amp Env",
    "Filter Env",
    "Osc 2",
    "Pressure",
    "Key Track"
};

const char *const ds800_mod_dest_names[DS800_DST_COUNT] = {
    "None",
    "Cutoff LP",
    "Cutoff 2",
    "Resonance",
    "Pitch",
    "Pulse Width",
    "VCA",
    "Pan",
    "Fold",
    "Symmetry",
    "Ring"
};

/* ---------------------------------------------------------------------------
 * Setup
 * ------------------------------------------------------------------------ */

void ds800_mod_init(ds800_mod_t *m)
{
    if (!m) return;
    memset(m, 0, sizeof(*m));
    /* Every slot starts SRC_NONE -> DST_NONE at zero depth, so an untouched
     * matrix costs nothing and changes nothing. Nothing modulates until you
     * say so -- which is the opposite of the failure mode where a new
     * instrument arrives with modulation you did not ask for. */
}

int ds800_mod_set(ds800_mod_t *m, int slot, ds800_mod_source_t src,
                  ds800_mod_dest_t dst, float depth)
{
    if (!m) return -1;
    if (slot < 0 || slot >= DS800_MOD_SLOTS) return -1;
    if (src < 0 || src >= DS800_SRC_COUNT) return -1;
    if (dst < 0 || dst >= DS800_DST_COUNT) return -1;

    if (depth < -1.0f) depth = -1.0f;
    if (depth >  1.0f) depth =  1.0f;

    m->slot[slot].src   = (uint8_t)src;
    m->slot[slot].dst   = (uint8_t)dst;
    m->slot[slot].depth = depth;
    return 0;
}

/* ---------------------------------------------------------------------------
 * Reading a source
 *
 * A switch. Every case is one array member read; nothing here has state.
 * ------------------------------------------------------------------------ */

double ds800_mod_source_value(const ds800_mod_inputs_t *in, uint8_t src)
{
    if (!in) return 0.0;

    switch (src) {
    case DS800_SRC_LFO1:      return in->lfo1;
    case DS800_SRC_LFO2:      return in->lfo2;
    case DS800_SRC_SANDH:     return in->sandh;
    case DS800_SRC_CLOCK:     return in->clock;
    case DS800_SRC_ENV_AMP:   return in->env_amp;
    case DS800_SRC_ENV_FILT:  return in->env_filt;
    case DS800_SRC_OSC2:      return in->osc2;
    case DS800_SRC_PRESSURE:  return in->pressure;
    case DS800_SRC_KEYTRACK:  return in->keytrack;
    case DS800_SRC_NONE:
    default:                  return 0.0;
    }
}

/* ---------------------------------------------------------------------------
 * Summing one destination
 *
 * Six iterations at most, each a compare and a multiply-accumulate. A slot
 * whose destination does not match is skipped on the integer compare, before
 * any floating-point work.
 * ------------------------------------------------------------------------ */

double ds800_mod_sum(const ds800_mod_t *m, const ds800_mod_inputs_t *in,
                     uint8_t dst)
{
    if (!m || !in) return 0.0;
    if (dst == DS800_DST_NONE) return 0.0;

    double total = 0.0;

    for (int i = 0; i < DS800_MOD_SLOTS; i++) {
        const ds800_mod_slot_t *s = &m->slot[i];

        if (s->dst != dst)      continue;   /* integer compare first */
        if (s->src == DS800_SRC_NONE) continue;
        if (s->depth == 0.0f)   continue;

        total += (double)s->depth * ds800_mod_source_value(in, s->src);
    }

    return total;
}
