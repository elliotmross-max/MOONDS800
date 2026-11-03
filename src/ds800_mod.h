/* ds800_mod.h -- MOON DS800, Stage 2: the modulation matrix.
 *
 * This is the ARP 2600's patchability, as data rather than cables.
 *
 * THE 2600'S IDEA
 *   On a 2600 any source can drive any destination, at a depth you set with a
 *   slider, and you change it by moving a cable. A fixed-panel instrument
 *   gets the same thing with a matrix: N sources x M destinations, a depth
 *   for each connection. Six slots is enough to be genuinely patchable
 *   without turning the panel into a spreadsheet.
 *
 * WHERE IT IS RESOLVED
 *   Once per sample per voice, into a single summed value per destination --
 *   never per slot at the point of use. The render loop reads
 *
 *       double mod_cut = ds800_mod_sum(&v->mod, DS800_DST_CUTOFF_LP, ...);
 *
 *   and adds it. Six multiply-accumulates is nothing next to the filters.
 *
 * WHY THERE ARE NO FUNCTION POINTERS
 *   An indirect call per slot per voice per sample is 24 calls per sample at
 *   four voices. Both the source and destination are switches on enums for
 *   exactly that reason.
 *
 * DEFAULTS
 *   Every slot starts as NONE -> NONE at zero depth, so an untouched matrix
 *   costs nothing and changes nothing. Nothing modulates until you say so.
 */

#ifndef DS800_MOD_H
#define DS800_MOD_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DS800_MOD_SLOTS 6

/* ---------------------------------------------------------------------------
 * Sources -- what can drive a connection
 * ------------------------------------------------------------------------ */

typedef enum {
    DS800_SRC_NONE = 0,
    DS800_SRC_LFO1,
    DS800_SRC_LFO2,
    DS800_SRC_SANDH,        /* sample & hold output */
    DS800_SRC_CLOCK,        /* the S&H clock square, as the 2600 exposes it */
    DS800_SRC_ENV_AMP,
    DS800_SRC_ENV_FILT,
    DS800_SRC_OSC2,         /* audio-rate, for FM */
    DS800_SRC_PRESSURE,     /* this voice's aftertouch */
    DS800_SRC_KEYTRACK,     /* note position around middle C */
    DS800_SRC_COUNT
} ds800_mod_source_t;

extern const char *const ds800_mod_source_names[DS800_SRC_COUNT];

/* ---------------------------------------------------------------------------
 * Destinations -- what can be driven
 * ------------------------------------------------------------------------ */

typedef enum {
    DS800_DST_NONE = 0,
    DS800_DST_CUTOFF_LP,
    DS800_DST_CUTOFF_HP,
    DS800_DST_RESONANCE,
    DS800_DST_PITCH,
    DS800_DST_PULSEWIDTH,
    DS800_DST_VCA,
    DS800_DST_PAN,
    /* Buchla Easel timbre, reachable from the matrix: stepping the fold from
     * the sample & hold is the most useful thing a module with a folder can
     * do, and it is why these are destinations and not just knobs. */
    DS800_DST_FOLD,
    DS800_DST_SYMMETRY,
    DS800_DST_RING,
    DS800_DST_COUNT
} ds800_mod_dest_t;

extern const char *const ds800_mod_dest_names[DS800_DST_COUNT];

/* ---------------------------------------------------------------------------
 * The values a slot can read. Filled once per sample by the caller from
 * whatever it has to hand -- the engine passes its LFO and S&H values here,
 * so this file never needs to know how they are produced.
 * ------------------------------------------------------------------------ */

typedef struct {
    double lfo1;
    double lfo2;
    double sandh;
    double clock;        /* -1 or +1 */
    double env_amp;
    double env_filt;
    double osc2;
    double pressure;     /* 0..1 */
    double keytrack;     /* -1..1, centred on middle C */
} ds800_mod_inputs_t;

/* One connection. */
typedef struct {
    uint8_t src;
    uint8_t dst;
    float   depth;       /* -1..1 */
} ds800_mod_slot_t;

/* A voice's whole matrix. Per voice, because pressure and keytrack are per
 * note -- two notes can be patched to behave differently. */
typedef struct {
    ds800_mod_slot_t slot[DS800_MOD_SLOTS];
} ds800_mod_t;

void ds800_mod_init(ds800_mod_t *m);

/* Set one slot. `slot` is 0..DS800_MOD_SLOTS-1. Returns 0 on success,
 * -1 if the slot or the enum value is out of range. */
int  ds800_mod_set(ds800_mod_t *m, int slot, ds800_mod_source_t src,
                   ds800_mod_dest_t dst, float depth);

/* Read one source's current value. Pure switch, no state. */
double ds800_mod_source_value(const ds800_mod_inputs_t *in, uint8_t src);

/* Sum every slot aimed at `dst`. Returns the total, which the caller scales
 * by its own units -- this is a -1..1 signal space, not Hz or volts. */
double ds800_mod_sum(const ds800_mod_t *m, const ds800_mod_inputs_t *in,
                     uint8_t dst);

#ifdef __cplusplus
}
#endif

#endif /* DS800_MOD_H */
