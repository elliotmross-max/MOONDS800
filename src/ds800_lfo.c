/* ds800_lfo.c -- MOON DS800, Stage 2: the two LFOs.
 *
 * Plain C. No host types, no allocation, no library calls in the tick.
 *
 * THE SINE IS A POLYNOMIAL
 *   sin() in a per-sample loop is exactly the mistake DubStarMooniCon's own
 *   header records: 176,000 library calls a second for a value that only
 *   changes when a knob turns. Here the phase advances and the shape is
 *   evaluated from it -- the sine is a Bhaskara-style approximation, good to
 *   about 1e-4, which the test checks against libm.
 *
 * RANDOM
 *   The 2600 had no random source. This one holds a value per cycle and steps
 *   it when the phase wraps, which makes it a musical stepped source rather
 *   than a per-sample noise generator. Each voice seeds its own xorshift, so
 *   a chord does not get the same sequence on every note.
 */

#include "ds800_lfo.h"

#include <math.h>

/* M_PI is not in strict C89/C99 math.h -- on the Mac it is hidden unless
 * _GNU_SOURCE is set, and an undefined M_PI does not fail the build: it is
 * taken as an implicit int 0, so every table entry becomes sin(0) = 0 and the
 * sine comes out as a flat line. That is exactly what happened here. Define it
 * explicitly, the same way the DS800 engine and DSM's engine both do. */
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

const char *const ds800_lfo_shape_names[DS800_LFO_SHAPE_COUNT] = {
    "Sine", "Tri", "Saw", "Square", "Random"
};

const double ds800_lfo_rates_hz[DS800_LFO_RATE_COUNT] = {
    0.03, 0.1, 0.3, 1.0, 3.0, 10.0, 30.0
};
const char *const ds800_lfo_rate_names[DS800_LFO_RATE_COUNT] = {
    "0.03 Hz", "0.1 Hz", "0.3 Hz", "1 Hz", "3 Hz", "10 Hz", "30 Hz"
};

/* ---------------------------------------------------------------------------
 * The shapes
 * ------------------------------------------------------------------------ */

/* Sine from a table, built once at init.
 *
 * This replaces a polynomial approximation that was wrong: the parabola form
 * needs its input in -pi..pi and I was feeding it -1..1, which produced an
 * error of 2.0 -- the test caught it. A table with interpolation is both
 * cheaper and exact enough that there is no error to argue about: 256 entries
 * measured against libm gives 7.5e-5, comfortably below audibility.
 *
 * DS800_SIN_N + 1 entries so the read between the last sample and the first
 * needs no wrap test. */
double ds800_sin_tab[DS800_SIN_N + 1];

void ds800_sin_init(void)
{
    static int done = 0;
    if (done) return;
    done = 1;
    for (int i = 0; i <= DS800_SIN_N; i++)
        ds800_sin_tab[i] = sin(2.0 * M_PI * (double)i / (double)DS800_SIN_N);
}

/* Table read with linear interpolation. No library call in the hot path. */
static double sine_at(double phase)
{
    /* phase is kept in 0..1 by the tick, but guard anyway: a rate change
     * could in principle land one sample outside. */
    if (phase < 0.0) phase = 0.0;
    if (phase >= 1.0) phase -= 1.0;

    double f = phase * (double)DS800_SIN_N;
    int    i = (int)f;
    double frac = f - (double)i;

    double a = ds800_sin_tab[i];
    double b = ds800_sin_tab[i + 1];
    return a + frac * (b - a);
}

static double tri_at(double phase)
{
    /* -1 at 0, +1 at 0.5, -1 at 1 */
    if (phase < 0.5) return 4.0 * phase - 1.0;
    return 3.0 - 4.0 * phase;
}

static double saw_at(double phase)
{
    /* rising ramp, -1..1 */
    return 2.0 * phase - 1.0;
}

static double square_at(double phase)
{
    return (phase < 0.5) ? 1.0 : -1.0;
}

/* xorshift32 -- cheap, deterministic, good enough for a stepped source. */
static uint32_t xorshift(uint32_t *s)
{
    uint32_t x = *s;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *s = x ? x : 0x1234567u;      /* never let the state reach zero */
    return *s;
}

/* One random value in -1..1 from the current rng state. */
static double random_step(uint32_t *s)
{
    uint32_t r = xorshift(s);
    return ((double)(r >> 8) / 8388607.5) - 1.0;   /* 24 bits -> -1..1 */
}

/* ---------------------------------------------------------------------------
 * Public
 * ------------------------------------------------------------------------ */

void ds800_lfo_init(ds800_lfo_t *l, double sample_rate,
                    double rate_hz, uint32_t seed)
{
    /* The sine table is built here rather than trusted to the caller: leaving
     * it to a separate init meant the test (and anything else that used an LFO
     * without calling it) read an all-zero table, so the sine came out flat.
     * A static guard makes this free after the first call. */
    ds800_sin_init();

    if (!l) return;

    l->phase       = 0.0;
    l->inc         = (sample_rate > 0.0) ? (rate_hz / sample_rate) : 0.0;
    l->rng        = seed ? seed : 0x9E3779B9u;
    l->random_val = random_step(&l->rng);
}

void ds800_lfo_set_rate(ds800_lfo_t *l, double sample_rate, double rate_hz)
{
    if (!l || sample_rate <= 0.0) return;
    l->inc = rate_hz / sample_rate;
    /* Phase is deliberately NOT reset: turning the rate knob should not
     * restart the cycle, or a slow sweep would jump every time you touch it. */
}

void ds800_lfo_reset(ds800_lfo_t *l)
{
    if (!l) return;
    l->phase = 0.0;
    /* Key sync restarts the phase but not the random sequence -- otherwise
     * every note would get the same stepped pattern, which defeats it. */
}

double ds800_lfo_shape_at(ds800_lfo_t *l, ds800_lfo_shape_t shape, double phase)
{
    switch (shape) {
    case DS800_LFO_TRI:    return tri_at(phase);
    case DS800_LFO_SAW:    return saw_at(phase);
    case DS800_LFO_SQUARE: return square_at(phase);
    case DS800_LFO_RANDOM: return l ? l->random_val : 0.0;
    case DS800_LFO_SINE:
    default:               return sine_at(phase);
    }
}

double ds800_lfo_tick(ds800_lfo_t *l, ds800_lfo_shape_t shape, bool *wrapped)
{
    if (!l) { if (wrapped) *wrapped = false; return 0.0; }

    l->phase += l->inc;

    bool w = false;
    if (l->phase >= 1.0) {
        l->phase -= 1.0;
        w = true;

        /* RANDOM steps on the wrap -- that is what makes it musical rather
         * than noise. */
        if (shape == DS800_LFO_RANDOM) {
            l->random_val  = random_step(&l->rng);
        }
    }

    if (wrapped) *wrapped = w;
    return ds800_lfo_shape_at(l, shape, l->phase);
}
