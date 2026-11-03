/* ds800_sh.c -- MOON DS800, Stage 2: sample & hold, and its own clock.
 *
 * Plain C. No host types, no allocation, no library calls in the hot path.
 *
 * Read ds800_sh.h for why the clock is separate and why noise has a colour
 * rather than being a single source.
 */

#include "ds800_sh.h"

#include <math.h>

const char *const ds800_noise_colour_names[DS800_NOISE_COLOUR_COUNT] = {
    "White", "Pink", "Low"
};

const char *const ds800_sh_input_names[DS800_SH_IN_COUNT] = {
    "Noise", "LFO 2"
};

/* The clock's rate range, from the 2600's service manual: the INTERNAL CLOCK
 * "covers a range of approximately 0.2 Hz to 100 Hz". */
#define DS800_SH_CLOCK_MIN_HZ   0.2
#define DS800_SH_CLOCK_MAX_HZ 100.0

/* The RATE slider positions. The 2600's range is 0.2 Hz to 100 Hz; these are
 * eight musical points inside it. */
const double ds800_sh_rate_hz[DS800_SH_RATE_COUNT] = {
    0.2, 0.5, 1.0, 2.0, 4.0, 10.0, 30.0, 100.0
};
const char *const ds800_sh_rate_names[DS800_SH_RATE_COUNT] = {
    "0.2 Hz", "0.5 Hz", "1 Hz", "2 Hz", "4 Hz", "10 Hz", "30 Hz", "100 Hz"
};

/* ---------------------------------------------------------------------------
 * The internal clock
 * ------------------------------------------------------------------------ */

void ds800_sh_clock_init(ds800_sh_clock_t *c, double sample_rate, double rate_hz)
{
    if (!c) return;
    c->phase = 0.0;
    c->inc   = (sample_rate > 0.0) ? (rate_hz / sample_rate) : 0.0;
}

void ds800_sh_clock_set_rate(ds800_sh_clock_t *c, double sample_rate, double rate_hz)
{
    if (!c || sample_rate <= 0.0) return;
    if (rate_hz < DS800_SH_CLOCK_MIN_HZ) rate_hz = DS800_SH_CLOCK_MIN_HZ;
    if (rate_hz > DS800_SH_CLOCK_MAX_HZ) rate_hz = DS800_SH_CLOCK_MAX_HZ;
    c->inc = rate_hz / sample_rate;
    /* Phase deliberately kept: turning the RATE slider should not restart the
     * pattern, which is what makes it playable while it runs. */
}

double ds800_sh_clock_tick(ds800_sh_clock_t *c, bool *fired)
{
    if (!c) { if (fired) *fired = false; return 0.0; }

    c->phase += c->inc;

    bool f = false;
    if (c->phase >= 1.0) {
        c->phase -= 1.0;
        f = true;
    }

    if (fired) *fired = f;

    /* A square wave: high for the first half of the cycle. The sample command
     * is the rising edge, which is the wrap flag above. */
    return (c->phase < 0.5) ? 1.0 : -1.0;
}

/* ---------------------------------------------------------------------------
 * Noise, with a colour
 * ------------------------------------------------------------------------ */

/* xorshift32 -- cheap, deterministic, per-voice seedable. */
static uint32_t xorshift(uint32_t *s)
{
    uint32_t x = *s;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *s = x ? x : 0x1234567u;      /* never let the state reach zero */
    return *s;
}

static double noise_raw(uint32_t *s)
{
    uint32_t r = xorshift(s);
    return ((double)(r >> 8) / 8388607.5) - 1.0;   /* 24 bits -> -1..1 */
}

void ds800_sh_set_noise_colour(ds800_sh_t *sh, double sample_rate, double pos)
{
    if (!sh || sample_rate <= 0.0) return;

    /* Panel order is LOW ... PINK ... WHITE, so 0 = low, 0.5 = pink, 1 = white.
     * Two one-pole lowpasses in series give a -3 dB/octave tilt at pink when
     * their corners are spread; pushing both far down gives low-frequency
     * noise. Coefficients are one-pole form, computed here and never in the
     * tick. */
    if (pos < 0.0) pos = 0.0;
    if (pos > 1.0) pos = 1.0;

    double f_a, f_b;
    if (pos <= 0.5) {
        /* LOW .. PINK: corners far down, moving up to the pink spread */
        double t = pos / 0.5;
        f_a = 8.0   + t * (200.0  - 8.0);
        f_b = 2.0   + t * (900.0  - 2.0);
    } else {
        /* PINK .. WHITE: corners move up until both are out of the way */
        double t = (pos - 0.5) / 0.5;
        f_a = 200.0   + t * (8000.0  - 200.0);
        f_b = 900.0   + t * (18000.0 - 900.0);
    }

    sh->noise_col_a = 1.0 - exp(-2.0 * M_PI * f_a / sample_rate);
    sh->noise_col_b = 1.0 - exp(-2.0 * M_PI * f_b / sample_rate);

    if (sh->noise_col_a > 1.0) sh->noise_col_a = 1.0;
    if (sh->noise_col_b > 1.0) sh->noise_col_b = 1.0;

    /* Normalise for the loss the filters introduce.
     *
     * A fixed multiplier was wrong here: at the white end both corners are
     * effectively open, so the filtered signal already swings close to full
     * scale and any fixed gain pushes it past 1.0 -- the test caught the
     * output reaching 1.889 on a range the matrix promises is -1..1.
     *
     * The gain each pole applies to broadband noise is sqrt((1-a)/(1+a)),
     * so the compensation is 1 over the product of the two. Computed here,
     * on a parameter change, never in the tick. */
    double g_a = sqrt((1.0 - sh->noise_col_a) / (1.0 + sh->noise_col_a));
    double g_b = sqrt((1.0 - sh->noise_col_b) / (1.0 + sh->noise_col_b));
    double loss = g_a * g_b;

    sh->noise_gain = (loss > 1e-6) ? (1.0 / loss) : 1.0;

    /* Never let the compensation exceed what keeps the output in range: the
     * filtered noise peaks around 0.96 of full scale, so the ceiling is about
     * 1.05. */
    if (sh->noise_gain > 1.05) sh->noise_gain = 1.05;
}

/* One sample of coloured noise. Two one-poles, no library calls. */
static double noise_coloured(ds800_sh_t *sh)
{
    double n = noise_raw(&sh->rng);

    sh->noise_a += sh->noise_col_a * (n - sh->noise_a);
    sh->noise_b += sh->noise_col_b * (sh->noise_a - sh->noise_b);

    /* Bring the filtered level back up for the loss the poles introduced.
     * The gain is computed per colour in set_noise_colour, so it is 1.0
     * (well, ~1.05) at the white end where there is nothing to compensate. */
    double v = sh->noise_b * sh->noise_gain;
    if (v >  1.0) v =  1.0;
    if (v < -1.0) v = -1.0;
    return v;
}

/* ---------------------------------------------------------------------------
 * Sample & hold
 * ------------------------------------------------------------------------ */

void ds800_sh_init(ds800_sh_t *sh, double sample_rate, uint32_t seed)
{
    if (!sh) return;

    sh->rng    = seed ? seed : 0x9E3779B9u;
    sh->value  = noise_raw(&sh->rng);
    sh->target = sh->value;
    sh->slew   = 0.0;              /* instant by default, as on the 2600 */

    sh->noise_a = 0.0;
    sh->noise_b = 0.0;
    ds800_sh_set_noise_colour(sh, sample_rate, 1.0);   /* default white */
}

void ds800_sh_set_slew(ds800_sh_t *sh, double sample_rate, double seconds)
{
    if (!sh || sample_rate <= 0.0) return;

    if (seconds <= 0.0) {
        sh->slew = 0.0;            /* instant steps */
        return;
    }
    sh->slew = 1.0 - exp(-1.0 / (sample_rate * seconds));
}

double ds800_sh_tick(ds800_sh_t *sh, bool fired, ds800_sh_input_t source,
                     double lfo2_value)
{
    if (!sh) return 0.0;

    /* The noise filter runs every sample, so its state is continuous whether
     * or not the clock fires. */
    double n = noise_coloured(sh);

    if (fired) {
        switch (source) {
        case DS800_SH_IN_LFO2:
            sh->target = lfo2_value;   /* latched as-is */
            break;

        case DS800_SH_IN_NOISE:
        default:
            /* The 2600's normal: noise into S/H. */
            sh->target = n;
            break;
        }
    }

    if (sh->slew <= 0.0) {
        sh->value = sh->target;        /* instant: the held value IS the target */
    } else {
        sh->value += sh->slew * (sh->target - sh->value);
    }

    return sh->value;
}

double ds800_sh_value(const ds800_sh_t *sh)
{
    return sh ? sh->value : 0.0;
}
