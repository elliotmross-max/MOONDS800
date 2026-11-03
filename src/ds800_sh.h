/* ds800_sh.h -- MOON DS800, Stage 2: sample & hold, and its own clock.
 *
 * MATCHED TO THE ARP 2600, not invented.
 *
 * From the ARP 2600 Owner's Manual and service manual:
 *
 *   "The SAMPLE & HOLD section has its own square-wave LFO, with its speed
 *    controlled by the RATE slider."
 *
 *   "The frequency of the INTERNAL CLOCK is governed by the RATE control and
 *    covers a range of approximately 0.2 Hz to 100 Hz."
 *
 *   "This controls the frequency content of the noise. It's a continuous
 *    slider, but three positions are named: WHITE, PINK, and LOW FREQUENCY."
 *
 *   The S/H input is normalled to the noise generator, and the same internal
 *   clock also triggers the envelope generators.
 *
 * TWO THINGS THAT FOLLOW FROM THAT, and why they matter:
 *
 *   1. THE CLOCK IS ITS OWN OSCILLATOR, not one of the modulation LFOs. An
 *      earlier draft of this file borrowed LFO 1's phase wrap as the clock,
 *      which ties two unrelated controls together and caps the rate at 30 Hz.
 *      The 2600's clock runs to 100 Hz, and that is the difference between a
 *      slow wander and a rhythmic texture.
 *
 *   2. NOISE IS A COLOUR, not a single source. White, pink and low-frequency
 *      are three points on one control, and the tilt between them is what
 *      gives the classic stepped-random quality rather than hiss.
 *
 * The clock output is exposed separately too, because on the 2600 it is a
 * panel jack -- usable to trigger envelopes and anything else.
 */

#ifndef DS800_SH_H
#define DS800_SH_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------------
 * The internal clock -- a square-wave LFO, 0.2..100 Hz
 *
 * This is NOT one of the modulation LFOs. The 2600 gives sample & hold its
 * own, and it drives the envelopes as well.
 * ------------------------------------------------------------------------ */

typedef struct {
    double phase;      /* 0..1 */
    double inc;        /* per-sample rate, precomputed */
} ds800_sh_clock_t;

/* The RATE slider's named positions, from 0.2 Hz to the 100 Hz ceiling the
 * service manual gives. Eight steps, matching the meta header's option list. */
#define DS800_SH_RATE_COUNT 8
extern const double      ds800_sh_rate_hz[DS800_SH_RATE_COUNT];
extern const char *const ds800_sh_rate_names[DS800_SH_RATE_COUNT];

void   ds800_sh_clock_init(ds800_sh_clock_t *c, double sample_rate, double rate_hz);
void   ds800_sh_clock_set_rate(ds800_sh_clock_t *c, double sample_rate, double rate_hz);

/* Advance one sample. Returns the square wave (-1 or +1); *fired is true on
 * the sample the clock goes high, which is the sample command. */
double ds800_sh_clock_tick(ds800_sh_clock_t *c, bool *fired);

/* ---------------------------------------------------------------------------
 * Noise colour
 * ------------------------------------------------------------------------ */

typedef enum {
    DS800_NOISE_WHITE = 0,   /* flat: all frequencies equally */
    DS800_NOISE_PINK,        /* -3 dB/octave */
    DS800_NOISE_LOW,         /* low frequency noise, mostly below audio */
    DS800_NOISE_COLOUR_COUNT
} ds800_noise_colour_t;

extern const char *const ds800_noise_colour_names[DS800_NOISE_COLOUR_COUNT];

/* ---------------------------------------------------------------------------
 * The sample & hold itself
 * ------------------------------------------------------------------------ */

typedef enum {
    DS800_SH_IN_NOISE = 0,   /* the 2600's normal: noise into S/H */
    DS800_SH_IN_LFO2,        /* anything else, passed in by the caller */
    DS800_SH_IN_COUNT
} ds800_sh_input_t;

extern const char *const ds800_sh_input_names[DS800_SH_IN_COUNT];

typedef struct {
    double  value;       /* the held value, -1..1 */
    double  target;      /* where the slew is heading */
    double  slew;        /* per-sample approach, 0 = instant */

    /* Noise, with a colour tilt. Two one-poles is enough to move between
     * white and pink; the low setting pushes both corners far down. */
    double  noise_a, noise_b;
    double  noise_col_a, noise_col_b;
    double  noise_gain;      /* per-colour normalisation, set once */

    uint32_t rng;        /* xorshift state, per voice */
} ds800_sh_t;

void ds800_sh_init(ds800_sh_t *sh, double sample_rate, uint32_t seed);

/* Slew in seconds. 0 = instant steps, which is the 2600's behaviour with no
 * external processing. */
void ds800_sh_set_slew(ds800_sh_t *sh, double sample_rate, double seconds);

/* Noise colour, 0..1 across the slider: 0 = low frequency, 0.5 = pink,
 * 1 = white. Matches the panel's LOW ... PINK ... WHITE ordering. */
void ds800_sh_set_noise_colour(ds800_sh_t *sh, double sample_rate, double pos);

/* Call once per sample.
 *   fired       the clock's sample-command flag
 *   source      what is being sampled
 *   lfo2_value  the value to latch when source is LFO2
 * Returns the held (and slewed) value. */
double ds800_sh_tick(ds800_sh_t *sh, bool fired, ds800_sh_input_t source,
                     double lfo2_value);

/* The current held value without advancing. */
double ds800_sh_value(const ds800_sh_t *sh);

#ifdef __cplusplus
}
#endif

#endif /* DS800_SH_H */
