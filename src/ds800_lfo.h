/* ds800_lfo.h -- MOON DS800, Stage 2: the two LFOs.
 *
 * The ARP 2600's modulation sources, as plain arithmetic. No host types, no
 * allocation, nothing that needs the device to be tested.
 *
 * WHY THE PHASE LIVES ON THE VOICE
 *   Each voice gets its own LFO phase. A chord is then modulating slightly out
 *   of step note to note rather than in lockstep, which is most of what makes
 *   the CS-80 side of this instrument sound alive rather than programmed.
 *
 *   Key sync (a switch) resets the phase on note-on: repeatable sweeps for
 *   when you want the modulation to land the same way every time.
 *
 * SHAPES
 *   SINE, TRI, SAW, SQUARE are phase functions. RANDOM is the odd one: it
 *   holds a value per cycle and only changes when the phase wraps. It is a
 *   slow stepped source rather than a smooth one -- the 2600 had no such
 *   thing, and it earns its place by being the shape you cannot get from the
 *   others.
 *
 * COST
 *   One multiply-add per sample per LFO for the phase, one switch for the
 *   shape. No sin() in the loop: the sine is a polynomial in the phase, which
 *   the test checks against libm to within 1e-4.
 */

#ifndef DS800_LFO_H
#define DS800_LFO_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    DS800_LFO_SINE = 0,
    DS800_LFO_TRI,
    DS800_LFO_SAW,
    DS800_LFO_SQUARE,
    DS800_LFO_RANDOM,
    DS800_LFO_SHAPE_COUNT
} ds800_lfo_shape_t;

extern const char *const ds800_lfo_shape_names[DS800_LFO_SHAPE_COUNT];

/* Rates, named, so the knob reads as a musical speed. The 2600's LFO ran
 * roughly 0.03 Hz to 30 Hz; these sit inside that. */
typedef enum {
    DS800_LFO_R_003 = 0, DS800_LFO_R_01,  DS800_LFO_R_03,  DS800_LFO_R_1,
    DS800_LFO_R_3,       DS800_LFO_R_10,  DS800_LFO_R_30,
    DS800_LFO_RATE_COUNT
} ds800_lfo_rate_t;

extern const double      ds800_lfo_rates_hz[DS800_LFO_RATE_COUNT];
extern const char *const ds800_lfo_rate_names[DS800_LFO_RATE_COUNT];

/* One LFO's running state. A voice owns its own copy of these. */
typedef struct {
    double  phase;        /* 0..1 */
    double  inc;          /* per-sample rate, precomputed */
    double  random_val;   /* held value for RANDOM, stepped on the wrap */
    uint32_t rng;         /* xorshift state, per voice so voices differ */
} ds800_lfo_t;

/* A sine table, built once. del_sin_tab holds DS800_SIN_N + 1 entries so the
 * interpolation between the last sample and the first needs no wrap test. */
#define DS800_SIN_N 256
extern double ds800_sin_tab[DS800_SIN_N + 1];
void ds800_sin_init(void);

/* Set up a voice's LFO. `seed` should differ per voice -- pass the voice
 * index so two voices never produce the same random sequence. */
void   ds800_lfo_init(ds800_lfo_t *l, double sample_rate,
                      double rate_hz, uint32_t seed);

/* Change the rate without disturbing the phase. */
void   ds800_lfo_set_rate(ds800_lfo_t *l, double sample_rate, double rate_hz);

/* Key sync: restart the cycle. Called on note-on when sync is on. */
void   ds800_lfo_reset(ds800_lfo_t *l);

/* Advance one sample and return the current value, -1..1.
 * Returns through *wrapped as true on the sample the phase crosses 1.0, which
 * is what clocks sample & hold. */
double ds800_lfo_tick(ds800_lfo_t *l, ds800_lfo_shape_t shape, bool *wrapped);

/* The shape as a pure function of phase, for testing and for the S&H path.
 * `l` is only needed by RANDOM, which has state. */
double ds800_lfo_shape_at(ds800_lfo_t *l, ds800_lfo_shape_t shape, double phase);

#ifdef __cplusplus
}
#endif

#endif /* DS800_LFO_H */
