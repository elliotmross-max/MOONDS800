/* moons800.c -- MOON DS800 engine.
 *
 * Plain C, no host types, no file I/O, no allocation after init.
 *
 * Read moons800.h first -- it explains why everything is per voice.
 *
 * THE SIGNAL PATH, per voice, per sample:
 *
 *     VCO 1 (core + 2 taps) ─┐
 *                            ├─ sum ─ HP (resonant) ─ LP (resonant) ─ VCA ─ out
 *     VCO 2 (core + 2 taps) ─┘
 *
 *   The filter pair is the CS-80: a highpass then a lowpass, both resonant,
 *   always in series. There is no blend between them on purpose.
 *
 * WHY THE LOOP LOOKS LIKE THIS
 *   Stage 0 measured the cost: four voices with a filter pair each is about
 *   2.2x the operation count of one shared filter, because the three taps
 *   per oscillator already dominate. Affordable, but it means no pow, sqrt,
 *   sin, exp, floor, fmod or divide may appear inside ds800_render. Every
 *   value that changes on a PARAMETER CHANGE is precomputed in
 *   refresh_coeffs or refresh_voice.
 */

#include "moons800.h"
#include "moons800_all_meta.h"

#include <math.h>
#include <string.h>
#include <stdio.h>   /* snprintf, for the matrix key names */

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* ---------------------------------------------------------------------------
 * Tables
 * ------------------------------------------------------------------------ */

const char *const ds800_wave_names[DS800_WAVE_COUNT] = {
    "Pulse", "Pulse 25", "Saw", "Triangle", "Sine"
};

const int ds800_octave_semitones[DS800_OCT_COUNT] = { -24, -12, 0, 12, 24 };

const char *const ds800_octave_names[DS800_OCT_COUNT] = {
    "-2 Octave", "-1 Octave", "Unison", "+1 Octave", "+2 Octave"
};

/* ENVELOPE TIMES ARE CONTINUOUS, not a menu.
 *
 * They used to be seven named steps ("20 ms", "300 ms", ...), which reads well
 * on a panel but is useless to play: you cannot dial a sound in, and the steps
 * do not relate to each other. The CS-80 itself uses continuous time sliders.
 *
 * So each segment is now a 0..1 knob mapped to seconds. The mapping is CUBIC,
 * on purpose: a linear 0..1 would spend most of its travel above 2 seconds and
 * nothing below 100 ms, where percussion and plucks live. Cubing gives fine
 * control at the short end and still reaches the long end.
 *
 *   0.0  -> 1 ms      0.5  -> ~90 ms     0.8  -> ~2 s      1.0 -> 10 s
 */

/* Attack/decay hand over once the envelope is close enough that the ear reads
 * it as arrival. 0.999 on an exponential curve with a 10 s maximum left the
 * top of the attack knob still audibly rising when the stage flipped. */
#define DS800_ENV_ARRIVE  0.9995

#define DS800_ATT_MIN_S   0.001
#define DS800_ATT_MAX_S  10.0
#define DS800_DEC_MIN_S   0.005
#define DS800_DEC_MAX_S  10.0
#define DS800_REL_MIN_S   0.005
#define DS800_REL_MAX_S  15.0

static double time_from_knob(double t, double lo, double hi)
{
    /* EXPONENTIAL, not cubic. An envelope spans three decades (a few ms to
     * ten seconds) and only a RATIO mapping gives equal travel to equal
     * changes: every quarter of the knob is about x10. A cubic spent half the
     * knob between 1.25 s and 10 s, which is why the attack area felt stuck --
     * the useful range was crammed into the bottom third of the travel. */
    if (t < 0.0) t = 0.0;
    if (t > 1.0) t = 1.0;
    return lo * pow(hi / lo, t);
}

const char *const ds800_play_names[2] = { "Poly", "Mono" };


/* ---------------------------------------------------------------------------
 * Small helpers
 * ------------------------------------------------------------------------ */

static int    clampi(int v, int lo, int hi)   { return v < lo ? lo : (v > hi ? hi : v); }
static float  clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

static double note_to_hz(double base_hz, int note)
{
    return base_hz * pow(2.0, ((double)note - 69.0) / 12.0);
}

/* Every shape here has zero mean, which matters because three are summed at
 * once -- an offset shape would push DC into the filter and out to the
 * output.
 *
 * The 25% pulse is the one to be careful with. High for a quarter cycle at
 * +1 and low for three quarters at -1 gives a mean of -0.5, which is DC, not
 * a thinner pulse. Holding the low level at -1/3 centres it exactly:
 * 0.25*1 + 0.75*(-1/3) = 0. */
/* `pw` is a duty offset in -0.25..+0.25, applied to the two pulse shapes and
 * ignored by the other three. It is 0 unless the matrix drives PULSEWIDTH, so
 * the untouched instrument behaves exactly as it did in Stage 1.
 *
 * Both pulses stay zero-mean: the low level is chosen so the average is 0 at
 * any duty, which is why PULSE25 uses -1/3 rather than -1. */
static double wave_sample(uint8_t shape, double phase, double pw)
{
    switch (shape) {
    case DS800_WAVE_PULSE25: {
        double d = 0.25 + pw;
        if (d < 0.02) d = 0.02;
        if (d > 0.48) d = 0.48;
        /* zero-mean: high for d, low for 1-d, levels chosen so d*hi = (1-d)*lo */
        double lo = -d / (1.0 - d);
        return (phase < d) ? 1.0 : lo;
    }
    case DS800_WAVE_SAW:     return 2.0 * phase - 1.0;
    case DS800_WAVE_TRI:     return 4.0 * fabs(phase - 0.5) - 1.0;
    case DS800_WAVE_SINE:    return sin(2.0 * M_PI * phase);
    case DS800_WAVE_PULSE:
    default: {
        double d = 0.50 + pw;
        if (d < 0.02) d = 0.02;
        if (d > 0.98) d = 0.98;
        double lo = -d / (1.0 - d);
        return (phase < d) ? 1.0 : lo;
    }
    }
}

/* The Buchla Easel wavefolder.
 *
 * A TRIANGLE folder, as the Easel's is: the input drives a triangle shape and
 * the excess folds back at the rails. Folding ADDS harmonics -- it is a sound
 * source, not a filter setting, and the distinction matters because a filter
 * can only take harmonics away.
 *
 * `depth` 0..1 sets how much of the folded signal replaces the dry one, so at
 * 0 this is transparent and the instrument sounds exactly as it did before the
 * folder existed.
 *
 * `sym` offsets the fold point, which is what makes the two halves of the wave
 * fold by different amounts -- the asymmetry is the point, and it is why the
 * control is called Symmetry rather than Bias. CAPPED here as well as in the
 * parameter layer: the matrix can reach Symmetry as a destination, so a limit
 * enforced only at the knob would not hold.
 */
static inline double fold_sample(double x, double depth)
{
    if (depth <= 0.0) return x;

    /* THE DRIVE STAGE, and it is the whole reason the knob works.
     *
     * A folder does nothing to a signal that stays inside +/-1.0 -- there is
     * no excess to fold. The oscillator arrives at about +/-0.7, so without
     * gain the depth knob was inert at every setting, which is exactly what
     * it sounded like.
     *
     * Real designs drive the input well past the rails first (the Buchla 259
     * and the Dannysound Timbre both do), because the harmonics come from the
     * folding, and folding only happens past the rails.
     *
     *   depth 0    -> x1    transparent
     *   depth 0.5  -> x4.5  a 0.7 oscillator peaks at 3.15, folds twice
     *   depth 1    -> x8    heavily folded
     */
    double driven = x * (1.0 + depth * 7.0);

    double y = driven;

    /* Triangle fold: reflect into -1..1. The input is now well past the rails,
     * so more reflections are needed than the old +/-1 range required. */
    for (int i = 0; i < 8; i++) {
        if (y >  1.0) y =  2.0 - y;
        if (y < -1.0) y = -2.0 - y;
    }
    if (y >  1.0) y =  1.0;
    if (y < -1.0) y = -1.0;

    /* depth crossfades dry -> folded, so the knob is continuous from off, and
     * the folded signal is scaled back so the level does not jump. */
    return x + depth * ((y * 0.6) - x);
}

/* ---------------------------------------------------------------------------
 * Coefficients -- computed here, never in the audio loop.
 * ------------------------------------------------------------------------ */

/* Filter coefficient from a 0..1 cutoff knob.
 *
 * The ceiling is on the COEFFICIENT, not the frequency. A trapezoidal SVF is
 * stable only while g < 1; above that the integrator states grow instead of
 * decaying and the output becomes full-scale noise. Clamping the frequency
 * does not protect anything, because the knob can reach a frequency whose g
 * is already past the limit.
 */

static double env_inc(double seconds, double sr)
{
    if (seconds <= 0.0) return 1.0;
    return 1.0 - exp(-1.0 / (sr * seconds));
}

/* A RATE knob is 0..1 and the table holds the real frequencies. Interpolate
 * between neighbouring entries so EVERY position is reachable -- the table's
 * points become landmarks on a continuous control, not a menu. Interpolation
 * is linear IN HZ, which keeps the tables' own deliberately uneven spacing
 * (the ARP 2600's clock is .2 to 100 Hz, its LFOs .03 to 30 Hz). */
static double rate_from_knob(double t, const double *table, int n)
{
    double pos, frac;
    int    i0, i1;
    if (n <= 1) return table[0];
    if (t < 0.0) t = 0.0;
    if (t > 1.0) t = 1.0;
    pos = t * (double)(n - 1);
    i0  = (int)pos;
    i1  = (i0 + 1 < n) ? i0 + 1 : i0;
    frac = pos - (double)i0;
    return table[i0] + frac * (table[i1] - table[i0]);
}

/* Forward declaration: ladder_coeff() is called from refresh_coeffs below but
 * defined further down with the rest of the Moog ladder. */
static void ladder_coeff(double cutoff01, double resonance01, double sr,
                         ds800_ladder_coeff_t *c);

static void refresh_coeffs(ds800_t *s)
{
    const double sr = s->sample_rate;

    /* --- amplitude envelope ----------------------------------------- */
    s->coeff.att = env_inc(time_from_knob(s->env_attack,  DS800_ATT_MIN_S, DS800_ATT_MAX_S), sr);
    s->coeff.dec = env_inc(time_from_knob(s->env_decay,   DS800_DEC_MIN_S, DS800_DEC_MAX_S), sr);
    s->coeff.rel = env_inc(time_from_knob(s->env_release, DS800_REL_MIN_S, DS800_REL_MAX_S), sr);

    /* --- filter envelope: its OWN times, on request ------------------ */
    s->fcoeff.att = env_inc(time_from_knob(s->fenv_attack,  DS800_ATT_MIN_S, DS800_ATT_MAX_S), sr);
    s->fcoeff.dec = env_inc(time_from_knob(s->fenv_decay,   DS800_DEC_MIN_S, DS800_DEC_MAX_S), sr);
    s->fcoeff.rel = env_inc(time_from_knob(s->fenv_release, DS800_REL_MIN_S, DS800_REL_MAX_S), sr);

    /* --- filter pair ------------------------------------------------ */
    /* The HP's knob POSITION is reversed so it turns the same way as the LP:
     * right = open. The curve it feeds is the original one, unchanged. */
    /* ONE filter at a time, so ONE set of coefficients for the selected mode.
     *
     * The contour no longer moves anything here: it is applied PER SAMPLE as
     * an offset in cutoff units, which is what the Moog does and what stops
     * the offset from running the coefficient past its stability ceiling. */
    /* ONE FILTER: the Moog ladder. Nothing to branch on. */
    ladder_coeff(s->filter_cutoff, s->filter_resonance, sr, &s->lpco);

    /* noise_colour -> the S&H noise spectrum, 0..1:
     *   0 = Low   (heavily lowpassed, a slow random wobble)
     *   1 = White (flat, full-band hiss)
     * Pink sits at 0.5 between them. */
    {
        double pos = (s->noise_colour == 0) ? 0.0
                   : (s->noise_colour == 1) ? 0.5 : 1.0;
        for (int i = 0; i < DS800_VOICES; i++)
            ds800_sh_set_noise_colour(&s->voice[i].sh, s->sample_rate, pos);
    }
}

/* Recompute one voice's precomputed values from the current parameters.
 * Called on a parameter change and on note-on, never per sample. */
static void refresh_voice(ds800_voice_t *v, const ds800_t *s)
{
    for (int o = 0; o < 2; o++) {
        v->oct_ratio[o] = pow(2.0, (double)ds800_octave_semitones[s->oct_sel[o]] / 12.0);

        v->wave[o]    = s->wave_sel[o];
        v->pw[o]      = (double)s->pw_sel[o];
        v->lvl[o][0]  = s->lvl[o][0];
        v->lvl[o][1]  = s->sub_lvl[o];        /* the CS-80's single sub */

        /* The wavefolder. Symmetry is CAPPED HERE, in the precomputed value,
         * as well as inside fold_sample -- the matrix can drive Symmetry, so a
         * limit enforced only where the knob is read would not hold. */
        v->fold[o] = (double)s->fold_sel[o];
    }

    /* The ring lives PER VOICE because the render loop runs per voice, but the
     * knobs are on the instance. Copy them here, on a parameter change. */
}

/* The internal ring carrier's per-sample increment. Recomputing this is a
 * divide, so it is done here on a parameter change, never in the loop. */

static void mark_all_dirty(ds800_t *s)
{
    for (int i = 0; i < DS800_VOICES; i++) s->voice[i].dirty = 1;
}

/* ---------------------------------------------------------------------------
 * Build
 * ------------------------------------------------------------------------ */

int ds800_build_tables(ds800_t *s, double sample_rate, double base_hz)
{
    if (!s || sample_rate < 8000.0) return -1;

    memset(s, 0, sizeof(*s));
    s->sample_rate  = sample_rate;
    s->base_note    = base_hz;
    s->mono_note    = -1;

    for (int o = 0; o < 2; o++) {
        s->oct_sel[o]     = DS800_OCT_UNISON;
        s->wave_sel[o]    = DS800_WAVE_PULSE;
        s->pw_sel[o]      = 0.0f;
        s->lvl[o][0]      = 0.7f;
        s->sub_lvl[o]     = 0.0f;
        s->fold_sel[o]    = 0.0f;   /* folder off: transparent, as before */
    }

    /* --- ring modulator --------------------------------------------- */

    s->filter_cutoff    = 0.75f;
    s->filter_resonance = 0.25f;
    s->filter_contour   = 0.0f;   /* envelope off until asked */
    /* The CS-80 filter contour: IL and AL, in semitone-ish units 0..5. IL is
     * NEGATIVE (the note starts below the cutoff), AL POSITIVE (it attacks
     * above). Both default to 0, so the filter sits still until asked. */
    s->key_track        = 0.0f;

    /* 0..1 knobs. ~5 ms attack, ~400 ms decay and release: a quick attack
     * and a musical decay, which is where a polysynth wants to sit. */
    s->env_attack       = 0.17f;
    s->env_decay        = 0.34f;
    s->env_release      = 0.34f;
    s->env_sustain      = 0.70f;
    s->vel_env          = 0.0f;

    s->fenv_attack      = 0.12f;   /* a touch faster than the amp */
    s->fenv_decay       = 0.34f;
    s->fenv_release     = 0.34f;
    s->fenv_sustain     = 0.30f;

    s->volume           = 0.8f;
    s->noise_lvl        = 0.0f;
    s->play_mode        = DS800_POLY;

    for (int i = 0; i < DS800_VOICES; i++) {
        ds800_voice_t *v = &s->voice[i];
        v->note        = -1;
        v->stolen_from = -1;
        v->mix_rng     = 0x9E3779B9u ^ (uint32_t)(i * 2654435761u + 1u);
        v->mix_nz      = 0.0;
        v->phase[0]  = 0.0;
        v->phase[1]  = 0.0;
            v->active    = false;
        v->releasing = false;
        v->age       = 0;
        v->dirty     = 1;
        v->env_amp.stage  = DS800_ENV_IDLE;
        v->env_filt.stage = DS800_ENV_IDLE;
        v->env_amp.level  = 0.0;
        v->env_filt.level = 0.0;
        refresh_voice(v, s);
    }

    /* --- Stage 2 defaults -------------------------------------------- */
    s->lfo1_rate  = 0.5f;                  /* 3 Hz  -- knob 0..1, exponential */
    s->lfo1_shape = DS800_LFO_SINE;
    s->lfo2_rate  = 0.667f;               /* 10 Hz */  s->lfo2_shape = DS800_LFO_SINE;
    s->lfo_sync   = 0;
    s->sh_rate    = 0.714f;               /* 10 Hz */                     /* 10 Hz -- the 2600 range where
                                           * a S&H through a resonant filter
                                           * sounds stepped rather than slow */
    s->sh_input   = DS800_SH_IN_NOISE;
    s->sh_env     = 0;                      /* off until asked for */
    s->sh_amt     = 0.0f;   /* no normalled connection until asked */
    s->sh_slew    = 0.0f;
    s->noise_colour = DS800_NOISE_WHITE;

    ds800_sh_clock_init(&s->sh_clock, sample_rate,
                        rate_from_knob(s->sh_rate, ds800_sh_rate_hz, DS800_SH_RATE_COUNT));
    s->sh_clock_fired = false;

    for (int i = 0; i < DS800_VOICES; i++) {
        ds800_voice_t *v = &s->voice[i];
        ds800_lfo_init(&v->lfo1, sample_rate,
                       rate_from_knob(s->lfo1_rate, ds800_lfo_rates_hz, DS800_LFO_RATE_COUNT), (uint32_t)(i + 1));
        ds800_lfo_init(&v->lfo2, sample_rate,
                       rate_from_knob(s->lfo2_rate, ds800_lfo_rates_hz, DS800_LFO_RATE_COUNT), (uint32_t)(i + 11));
        ds800_sh_init(&v->sh, sample_rate, (uint32_t)(i + 101));
        ds800_sh_set_noise_colour(&v->sh, sample_rate, 1.0);   /* white */
        ds800_mod_init(&v->mod);
    }

    refresh_coeffs(s);
    s->ready = 1;
    return 0;
}

/* ---------------------------------------------------------------------------
 * Envelope -- the ADSR state machine, one sample at a time.
 *
 *   IDLE    -> ATTACK   when the gate goes high
 *   ATTACK  -> DECAY    at level > 0.999
 *   DECAY   -> SUSTAIN  at level <= sustain
 *   SUSTAIN -> RELEASE  when the gate goes low
 *   RELEASE -> IDLE     at level < 0.0005
 * ------------------------------------------------------------------------ */

static double env_step(ds800_env_t *e, const ds800_coeff_t *c, bool gate)
{
    /* A dropped gate starts the release from ANY sounding stage, not only
     * from SUSTAIN. Getting this wrong is a voice that never dies: a note
     * released while still in DECAY would decay toward sustain, arrive
     * there, and sit forever, because DECAY and SUSTAIN were the only
     * stages the gate was consulted from and DECAY ignores it. The render
     * loop retires a voice when its envelope reaches IDLE, so an envelope
     * that never reaches IDLE is a voice that is never retired.
     *
     * IDLE and RELEASE are exempt: an idle envelope has nothing to release,
     * and re-flagging one already releasing would fight any per-voice
     * release-rate difference. */
    if (!gate && e->stage != DS800_ENV_IDLE && e->stage != DS800_ENV_RELEASE)
        e->stage = DS800_ENV_RELEASE;

    switch (e->stage) {
    case DS800_ENV_IDLE:
        if (gate) e->stage = DS800_ENV_ATTACK;
        break;

    case DS800_ENV_ATTACK:
        e->level += c->att * (1.0 - e->level);
        if (e->level > DS800_ENV_ARRIVE) { e->level = 1.0; e->stage = DS800_ENV_DECAY; }
        break;

    case DS800_ENV_DECAY:
        e->level -= c->dec * (e->level - e->sustain);
        if (e->level <= e->sustain) { e->level = e->sustain; e->stage = DS800_ENV_SUSTAIN; }
        break;

    case DS800_ENV_SUSTAIN:
        break;                      /* the gate is handled above */

    case DS800_ENV_RELEASE:
        e->level -= c->rel * e->level;
        if (e->level < 0.0005) { e->level = 0.0; e->stage = DS800_ENV_IDLE; }
        break;
    }
    return e->level;
}

/* ---------------------------------------------------------------------------
 * Notes
 * ------------------------------------------------------------------------ */

/* Which oscillator slot a voice renders. Both oscillators are per voice now,
 * so this is not an index into a global pair -- it exists only so the two
 * oscillators can be given different octave/divisor settings inside one
 * voice, which the parameter list does. */
static void voice_set_note(ds800_voice_t *v, const ds800_t *s,
                           int note, float vel, uint32_t age)
{
    /* If this voice is being TAKEN from a note that is still held, remember
     * which note that was. The old note's note_off must still find this voice,
     * or the voice is orphaned: it keeps `releasing` false, its envelope never
     * reaches IDLE, and it is never retired. (DSM's comment on the same point:
     * "note is deliberately NOT cleared: the voice still needs to know which
     * note it is finishing.") */
    v->stolen_from = (v->active && !v->releasing && v->note != note)
                     ? v->note : -1;

    v->note      = note;
    v->freq      = note_to_hz(s->base_note, note);
    v->active    = true;
    v->releasing = false;
    v->age       = age;

    /* A new note is a new onset, not a continuation of the last one. */
    v->phase[0] = 0.0;
    v->phase[1] = 0.0;

    /* Velocity scales the envelope depth, not the raw level, so the filter
     * contour follows the same gesture. */
    double depth = 1.0 - (double)s->vel_env * (1.0 - (double)vel);

    v->env_amp.sustain  = (double)s->env_sustain  * depth;
    v->env_filt.sustain = (double)s->fenv_sustain * depth;

    v->env_amp.stage  = DS800_ENV_ATTACK;
    v->env_filt.stage = DS800_ENV_ATTACK;

    refresh_voice(v, s);
}

static void held_push(ds800_t *s, int note)
{
    for (int i = 0; i < s->held_count; i++)
        if (s->held[i] == note) return;
    if (s->held_count < 8) s->held[s->held_count++] = note;
}

static void held_remove(ds800_t *s, int note)
{
    for (int i = 0; i < s->held_count; i++) {
        if (s->held[i] == note) {
            for (int k = i; k < s->held_count - 1; k++) s->held[k] = s->held[k + 1];
            s->held_count--;
            return;
        }
    }
}

void ds800_note_on(ds800_t *s, int note, float vel)
{
    if (!s) return;

    held_push(s, note);

    if (s->play_mode == DS800_MONO) {
        /* True 2600 behaviour: one voice, always stolen. */
        s->mono_note = note;
        voice_set_note(&s->voice[0], s, note, vel, ++s->note_age);
        return;
    }

    /* POLY: first free slot, then steal the oldest. */
    int idx = -1;
    for (int i = 0; i < DS800_VOICES; i++) {
        if (!s->voice[i].active) { idx = i; break; }
    }
    if (idx < 0) {
        uint32_t oldest = 0xFFFFFFFFu;
        idx = 0;
        for (int i = 0; i < DS800_VOICES; i++) {
            if (s->voice[i].age < oldest) { oldest = s->voice[i].age; idx = i; }
        }
    }
    voice_set_note(&s->voice[idx], s, note, vel, ++s->note_age);
}

void ds800_note_off(ds800_t *s, int note)
{
    if (!s) return;

    held_remove(s, note);

    /* Drop the gate on the voice holding this note. THE VOICE STAYS ACTIVE:
     * it is retired by the render loop when its envelope reaches IDLE, which
     * is the only place a voice should ever be switched off. */
    for (int i = 0; i < DS800_VOICES; i++) {
        ds800_voice_t *v = &s->voice[i];
        if (!v->active) continue;

        /* Match the note the voice is playing, OR the note it was playing
         * before it was stolen. The second case is what stops a stolen voice
         * from being orphaned. No `!releasing` guard: DSM does not have one,
         * and re-flagging an already-releasing voice is harmless. */
        if (v->note == note || v->stolen_from == note) {
            v->releasing = true;
            if (v->stolen_from == note) v->stolen_from = -1;
        }
    }

    if (s->play_mode == DS800_MONO && s->mono_note == note) {
        /* Fall back to the most recently played note still held. */
        if (s->held_count > 0) {
            s->mono_note = s->held[s->held_count - 1];
            voice_set_note(&s->voice[0], s, s->mono_note, 1.0f, ++s->note_age);
        } else {
            s->mono_note = -1;
        }
    }
}

/* PRESSURE IS NOT ROUTED ANY MORE.
 *
 * These two entry points are kept because the plugin wrapper calls
 * ds800_set_note_pressure() on every 0xA0 the Move's pads send, and removing
 * the symbols would break the build. They are deliberately inert: the fixed
 * pressure->cutoff and pressure->release routings were removed to simplify
 * the panel. Pressure remains available as a SOURCE in the matrix for anyone
 * who wants to patch it.
 */
void ds800_set_pressure(ds800_t *s, float pressure)
{
    (void)s; (void)pressure;
}

void ds800_set_note_pressure(ds800_t *s, int note, float pressure)
{
    (void)s; (void)note; (void)pressure;
}

/* ---------------------------------------------------------------------------
 * Render -- audio thread. No allocation, no locks, no I/O.
 * ------------------------------------------------------------------------ */

/* Headroom: a voice sums two oscillators of three sources each, so six
 * signals can overlap inside ONE voice before its filter. Scaling by 1/6 is
 * a bound, not a tuned value -- it holds at every setting rather than for one
 * patch. Without it the summed input reaches ~1.4, the SVF's integrator
 * states run past their intended range and clip inside the filter, and
 * because that happens before the volume multiply the instrument distorts at
 * any volume. */
/* SEVEN pre-filter signals now (two oscs + two subs + fold products + the
 * noise source), so the bound moves from 1/6. The bound exists because the
 * summed input must not clip BEFORE the volume multiply, which would distort
 * the instrument at any volume. The two subs are taps of the oscillators
 * rather than full independent voices, so 1/7 is a bound with margin, not a
 * precise figure. */
#define DS800_VOICE_GAIN (1.0 / 7.0)

/* --- 4012 resonance character -----------------------------------------
 * DS800_4012_FB widens the cutoff term in the feedback so resonance bites
 * earlier. 0.15 is the textbook ladder; wider is rougher. Kept MODEST
 * because option B tames the instability rather than reproducing it.
 * DS800_4012_LOSS is the passband loss as resonance rises, and
 * DS800_4012_PASS_FLOOR stops that loss from gutting the filter -- the
 * floor is what makes this the STABLE variant rather than a faithful one. */
#define DS800_4012_FB         0.42
/* The resonance ceiling, fixed. A four-pole ladder begins to self-oscillate
 * at a loop gain of about 4; a little past it makes the very top of the
 * Resonance knob sing rather than merely peak. Lower this toward 1.0 for the
 * polite ceiling. */
#define DS800_4012_RES_GAIN   2.6
#define DS800_LADDER_CLAMP    8.0
#define DS800_4012_LOSS       0.22
#define DS800_4012_PASS_FLOOR 0.70


static void ladder_coeff(double cutoff01, double resonance01, double sr,
                         ds800_ladder_coeff_t *c)
{
    if (cutoff01 < 0.0) cutoff01 = 0.0;
    if (cutoff01 > 1.0) cutoff01 = 1.0;

    /* their mapping, exactly: 20 Hz at 0, 20 kHz at 1 */
    double cutoff_hz = 20.0 * pow(1000.0, cutoff01);

    double fc = cutoff_hz / sr;
    if (fc > 0.49) fc = 0.49;
    if (fc < 0.001) fc = 0.001;

    double f = fc * 1.16;
    c->f    = f;

    /* --- THE 4012's RESONANCE PATH -------------------------------------
     * Topology is the same as a Moog ladder -- the 4012 WAS a Moog ladder
     * and that is what the patent suit was about. Its character lives in
     * two places the textbook version leaves out:
     *
     *   1. FEEDBACK. The resonant quality comes from the feedback path far
     *      more than from the pole spacing. A clean (1 - 0.15 f^2) tracks
     *      smoothly and rings politely; ARP's is rougher and bites earlier.
     *      DS800_4012_FB widens the cutoff-dependent term so resonance does
     *      real work across the range instead of only near the top.
     *
     *   2. PASSBAND LOSS. A real 4012 loses gain as resonance rises, and
     *      loses it again at high cutoff -- up to half the signal at high
     *      frequency settings. That lossiness is a large part of why it
     *      sounds aggressive rather than pretty. Option B: the losses are
     *      scaled back so the filter keeps its punch, but they are present,
     *      because removing them entirely is what made this sound like a
     *      generic Moog. */
    c->res  = resonance01;

    /* THE RESONANCE CEILING.
     *
     * A four-pole ladder self-oscillates once its loop gain reaches about 4,
     * because the four poles together give roughly 4x at the resonant peak.
     * With fb = resonance01 the knob topped out at 1.0 -- peaky, never
     * singing, which is why these old synths always had more.
     *
     * DS800_4012_RES_GAIN closes that gap as a FIXED choice: 1.0 is the
     * polite ceiling, past 4 the top of the Resonance knob rings and then
     * sings. The ladder's own saturation is what stops it running away,
     * exactly as on the hardware. */
    c->fb   = resonance01 * DS800_4012_RES_GAIN * (1.0 - DS800_4012_FB * f * f);
    c->gain = 0.35013 * f * f * f * f;
    c->pass = 1.0 - DS800_4012_LOSS * resonance01 * (0.5 + 0.5 * f);
    if (c->pass < DS800_4012_PASS_FLOOR) c->pass = DS800_4012_PASS_FLOOR;
}

static inline double ladder_step(ds800_ladder_t *s, double in,
                                 const ds800_ladder_coeff_t *k)
{
    double x = (in * k->pass - s->b4 * k->fb) * k->gain;

    s->b1 = x + 0.3 * s->b0 + (1.0 - k->f) * s->b1;
    s->b0 = x;
    s->b2 = s->b1 + 0.3 * s->b1 + (1.0 - k->f) * s->b2;
    s->b3 = s->b2 + 0.3 * s->b2 + (1.0 - k->f) * s->b3;
    s->b4 = s->b3 + 0.3 * s->b3 + (1.0 - k->f) * s->b4;

    /* HEADROOM FOR SELF-OSCILLATION. At +/-4 the clamp is reached
     * continuously once the filter starts ringing, which flattens the sine
     * into a squarer edge. +/-8 lets it bloom; the feedback path's own
     * compression above is what still bounds it. */
    if (s->b4 >  DS800_LADDER_CLAMP) s->b4 =  DS800_LADDER_CLAMP;
    if (s->b4 < -DS800_LADDER_CLAMP) s->b4 = -DS800_LADDER_CLAMP;
    return s->b4;
}

void ds800_render(ds800_t *s, float *out_l, float *out_r, int frames)
{
    if (!s || !out_l || !out_r) return;
    if (frames <= 0) return;
    if (frames > DS800_MAX_BLOCK) frames = DS800_MAX_BLOCK;

    /* Filter-envelope depth, resolved once per block rather than per sample. */
    /* IL and AL as octaves of cutoff movement. 5 units == 4 octaves, which is
     * the manual's full range and enough to go from inaudible to wide open. */
    const double ktrack = (double)s->key_track;

    for (int i = 0; i < frames; i++) {
        double mix = 0.0;

        /* The S&H clock is ONE, shared by every voice -- the 2600 puts a
         * single clock on the panel and it drives the module for all notes.
         * Ticked once per frame, here, not once per voice. */
        /* Clear the fire flag FIRST. ds800_sh_clock_tick sets it, but nothing
         * guarantees it clears it on a non-firing sample -- and if it does not,
         * the flag latches true and the S&H samples EVERY sample instead of
         * once per clock. That is a noise generator, not a stepped source,
         * which is what "the sample and hold does not sound right" describes.
         * Clearing here makes the flag mean "this sample, this clock" exactly. */
        s->sh_clock_fired = false;
        s->sh_clock_square = ds800_sh_clock_tick(&s->sh_clock, &s->sh_clock_fired);

        /* THE 2600'S CLOCK FIRES THE ENVELOPES.
         *
         * On the 2600 the clock is not only a sequencer pulse -- it is what
         * drives the instrument rhythmically, and the envelopes are one of
         * the things it drives. With sh_env on, each clock pulse restarts the
         * envelopes of every voice that is not already releasing, so a held
         * note pulses in time rather than sitting still.
         *
         * Releasing voices are SKIPPED: retriggering one would restart a note
         * the player has already let go of, and the voice would never retire.
         */
        const bool clock_env = s->sh_env && s->sh_clock_fired;

        for (int vi = 0; vi < DS800_VOICES; vi++) {
            ds800_voice_t *v = &s->voice[vi];
            if (!v->active) continue;

            const bool gate = !v->releasing;

            /* The clock retriggers a sounding voice's envelopes. Done before
             * the step so the pulse lands on this sample, not the next. */
            if (clock_env && gate) {
                v->env_amp.stage  = DS800_ENV_ATTACK;
                v->env_filt.stage = DS800_ENV_ATTACK;
            }

            /* --- envelopes: this voice's own, twice ------------------- */
            double ev = env_step(&v->env_amp,  &s->coeff,  gate);
            double ef = env_step(&v->env_filt, &s->fcoeff, gate);

            /* --- oscillators: both, inside this voice ---------------- */
            double acc = 0.0;
            double fold_out[2] = { 0.0, 0.0 };

            /* --- Stage 2: this voice's own modulation -----------------
             *
             * The LFOs advance per voice, so a chord is not in lockstep.
             * The S&H is clocked by the SHARED clock, and reads noise or
             * LFO 2 depending on the panel switch. Every value then goes
             * into one struct, and the matrix resolves it -- the engine
             * never walks the slots itself. */
            bool lfo1_wrap = false, lfo2_wrap = false;
            double lfo1_v = ds800_lfo_tick(&v->lfo1, (ds800_lfo_shape_t)s->lfo1_shape, &lfo1_wrap);
            double lfo2_v = ds800_lfo_tick(&v->lfo2, (ds800_lfo_shape_t)s->lfo2_shape, &lfo2_wrap);

            double sh_v = ds800_sh_tick(&v->sh, s->sh_clock_fired,
                                        (ds800_sh_input_t)s->sh_input, lfo2_v);

            ds800_mod_inputs_t min;
            min.lfo1      = lfo1_v;
            min.lfo2      = lfo2_v;
            min.sandh     = sh_v;
            min.clock     = s->sh_clock_square;
            min.env_amp   = ev;
            min.env_filt  = ef;
            /* Pulse-width modulation is read first: the min struct feeds the
             * matrix, and osc2's core is one of its inputs. */
            double pitch_mod = ds800_mod_sum(&v->mod, &min, DS800_DST_PITCH);
            double pitch_mul = 1.0 + pitch_mod * 0.25;
            if (pitch_mul < 0.01) pitch_mul = 0.01;
            min.osc2      = wave_sample(v->wave[1], v->phase[1], v->pw[1]);
            min.pressure  = 0.0;
            min.keytrack  = ((double)v->note - 60.0) / 36.0;

            /* Pulse-width modulation. Read once; the oscillator loop applies
             * it to the pulse shapes only, and wave_sample ignores it for the
             * other two waveforms. */
            double pw_mod = ds800_mod_sum(&v->mod, &min, DS800_DST_PULSEWIDTH);

            /* The Easel destinations. Read here with the others, because they
             * are CONSTANT for the whole sample across both oscillators --
             * except Fold and Symmetry, which are per oscillator and applied
             * inside the loop below. */
            double fold_mod = ds800_mod_sum(&v->mod, &min, DS800_DST_FOLD);

            for (int o = 0; o < 2; o++) {
                v->inc[o] = v->freq * v->oct_ratio[o] * pitch_mul / s->sample_rate;
                v->phase[o] += v->inc[o];
                if (v->phase[o] >= 1.0) v->phase[o] -= 1.0;

                /* core + the CS-80 sub (one octave down, half rate, so it stays
                 * phase-locked to the core for free) */
                /* The PW KNOB and the PW modulation sum. Before this the
                 * render passed pw_mod alone: the knob was stored, copied to
                 * the voice by refresh_voice, and never read. */
                double pw = v->pw[o] + pw_mod;
                if (pw >  0.25) pw =  0.25;
                if (pw < -0.25) pw = -0.25;

                double osc = v->lvl[o][0] * wave_sample(v->wave[o], v->phase[o], pw);

                if (v->lvl[o][1] > 0.0) {
                    double psub = v->phase[o] * 0.5;
                    if (psub >= 1.0) psub -= 1.0;
                    osc += v->lvl[o][1] * wave_sample(v->wave[o], psub, pw);
                }

                /* THE WAVEFOLDER sits here: after the core+sub mix, before the
                 * ring. Folding adds harmonics, so the ring then modulates
                 * folded material rather than the other way round.
                 *
                 * Fold and Symmetry take matrix modulation. Depth is clamped to
                 * 0..1 AFTER the sum, so a modulated fold cannot go negative or
                 * past full; symmetry is clamped inside fold_sample itself,
                 * which is the cap that has to hold because the matrix can
                 * reach it. */
                double fold_depth = v->fold[o] + fold_mod;
                if (fold_depth < 0.0) fold_depth = 0.0;
                if (fold_depth > 1.0) fold_depth = 1.0;

                osc = fold_sample(osc, fold_depth);

                fold_out[o] = osc;
            }

            /* THE RING MODULATOR IS REMOVED. Its three remaining controls (Level,
             * Source, Pan) went with it, and so did the crossfade machinery.
             * The dry oscillator sum is what this voice is now. */
            acc += fold_out[0] + fold_out[1];

            /* THE PRE-FILTER MIXER'S NOISE SOURCE.
             *
             * The ARP 2600 routes noise to the S&H input AND to the
             * pre-filter mixer as a separate signal, and that second path is
             * a large part of why S&H through a resonant filter sounds the
             * way it does: the stepped cutoff sweeps across a dense mix and
             * the noise emphasises the spitty, metallic edge.
             *
             * Its colour follows the same Noise knob the S&H samples, so one
             * control tints both. Generated per voice so a chord is not in
             * lockstep, exactly like the LFOs. */
            if (s->noise_lvl > 0.0001f) {
                uint32_t *rs = &v->mix_rng;
                *rs ^= *rs << 13; *rs ^= *rs >> 17; *rs ^= *rs << 5;
                double n = ((double)(*rs >> 8) / 8388607.5) - 1.0;

                /* one pole, corner set by the colour: Low / Pink / White */
                double nz_a = (s->noise_colour == DS800_NOISE_WHITE) ? 0.9
                            : (s->noise_colour == DS800_NOISE_PINK)  ? 0.12
                                                                     : 0.02;
                v->mix_nz += nz_a * (n - v->mix_nz);
                acc += v->mix_nz * (double)s->noise_lvl;
            }

            acc *= DS800_VOICE_GAIN;

            /* --- ONE FILTER, SELECTED ------------------------------------------
             *
             * THE MOOG'S SHAPE, exactly:
             *
             *     cutoff_norm = clamp(cutoff + contour * env, 0, 1)
             *     cutoff_hz   = 20 * pow(1000, cutoff_norm)
             *
             * The contour is an offset in CUTOFF UNITS, clamped, then mapped.
             * The old code added an octave offset to a gain MULTIPLIER, which
             * at Initial=5 reached -4.0 -- a negative coefficient, which
             * inverts an SVF's integrator instead of filtering. That was the
             * crash, and it is structurally impossible now: the value can
             * never leave 0..1 before the mapping.
             *
             * And only ONE filter runs. The HP and LP used to take the same
             * shared term, so they opened and closed together and passed
             * through zero at the midpoint -- cancelling each other. That
             * cannot happen with one filter in the path.
             */
            double cut = (double)s->filter_cutoff;
            cut += (double)s->filter_contour * ef;

            /* key tracking: higher notes open the filter */
            if (ktrack > 0.0) cut += ktrack * ((double)v->note - 60.0) / 36.0;

            /* the matrix, in cutoff units like everything else */
            /* THE NORMALLED S&H CONNECTION. On a 2600 the S&H has a fixed
             * path to the filter as well as the patch points, which is what
             * makes it usable on its own. sh_amt is that path. */
            cut += (double)s->sh_amt * sh_v * 0.5;

            cut += ds800_mod_sum(&v->mod, &min, DS800_DST_CUTOFF_LP) * 0.5;
            cut += ds800_mod_sum(&v->mod, &min, DS800_DST_CUTOFF_HP) * 0.5;

            if (cut < 0.0) cut = 0.0;
            if (cut > 1.0) cut = 1.0;

            double res_mod = ds800_mod_sum(&v->mod, &min, DS800_DST_RESONANCE);
            double res = (double)s->filter_resonance + res_mod;
            if (res < 0.0) res = 0.0;
            if (res > 1.0) res = 1.0;

            /* THE MOOG LADDER, 24 dB/oct, resonance on its own feedback path
             * so it rings the way a 2600 does. Coefficients recomputed for
             * this sample's cutoff because the contour moves it. */
            ds800_ladder_coeff_t lk;
            ladder_coeff(cut, res, s->sample_rate, &lk);
            double filtered = ladder_step(&v->lp, acc, &lk);

            /* VCA modulation scales the voice's output */
            double vca_mod = 1.0 + ds800_mod_sum(&v->mod, &min, DS800_DST_VCA);
            if (vca_mod < 0.0) vca_mod = 0.0;

            mix += filtered * ev * vca_mod;

            /* --- retire: only when the envelope is fully idle -------- */
            if (v->env_amp.stage == DS800_ENV_IDLE) {
                v->active      = false;
                v->releasing   = false;
                v->note        = -1;
                v->stolen_from = -1;
                /* the ladder's own state is cleared with the rest below */
                v->lp.b0 = v->lp.b1 = v->lp.b2 = v->lp.b3 = v->lp.b4 = 0.0;
            }
        }

        double out = mix * (double)s->volume;
        if (out >  1.0) out =  1.0;
        if (out < -1.0) out = -1.0;

        out_l[i] = (float)out;
        out_r[i] = (float)out;
    }

    /* Pick up ANY parameter change made since the last block.
     *
     * This used to be gated on v->dirty, which only some setters set -- so
     * most knobs (every level, fold, symmetry, the LFO settings, the S&H,
     * noise colour) were stored on the instance and never reached a voice
     * until a new note was played. The flag bought nothing worth that: this
     * is four calls of arithmetic per block, not per sample. */
    for (int vi = 0; vi < DS800_VOICES; vi++) {
        refresh_voice(&s->voice[vi], s);
        s->voice[vi].dirty = 0;
    }
}

/* ---------------------------------------------------------------------------
 * Parameters
 *
 * Keys follow DSM's naming where the control is the same, so muscle memory
 * and any host-side mapping carry over.
 * ------------------------------------------------------------------------ */

int ds800_set_param(ds800_t *s, const char *key, float value)
{
    if (!s || !key) return -1;

    /* --- oscillators ------------------------------------------------- */
    if (!strcmp(key, "oct1"))   { s->oct_sel[0] = (uint8_t)clampi((int)value, 0, DS800_OCT_COUNT - 1); mark_all_dirty(s); return 0; }
    if (!strcmp(key, "oct2"))   { s->oct_sel[1] = (uint8_t)clampi((int)value, 0, DS800_OCT_COUNT - 1); mark_all_dirty(s); return 0; }
    if (!strcmp(key, "osc1_lvl")) { s->lvl[0][0]  = clampf(value, 0, 1); mark_all_dirty(s); return 0; }
    if (!strcmp(key, "osc2_lvl")) { s->lvl[1][0]  = clampf(value, 0, 1); mark_all_dirty(s); return 0; }
    if (!strcmp(key, "sub_lvl"))  { s->sub_lvl[0] = clampf(value, 0, 1); s->sub_lvl[1] = s->sub_lvl[0]; mark_all_dirty(s); return 0; }
    if (!strcmp(key, "pw1"))    { s->pw_sel[0]   = clampf(value, -0.25f, 0.25f); mark_all_dirty(s); return 0; }
    if (!strcmp(key, "pw2"))    { s->pw_sel[1]   = clampf(value, -0.25f, 0.25f); mark_all_dirty(s); return 0; }
    if (!strcmp(key, "fold1"))  { s->fold_sel[0] = clampf(value, 0, 1); mark_all_dirty(s); return 0; }
    if (!strcmp(key, "fold2"))  { s->fold_sel[1] = clampf(value, 0, 1); mark_all_dirty(s); return 0; }
    if (!strcmp(key, "wave1"))  { s->wave_sel[0] = (uint8_t)clampi((int)value, 0, DS800_WAVE_COUNT - 1); mark_all_dirty(s); return 0; }
    if (!strcmp(key, "wave2"))  { s->wave_sel[1] = (uint8_t)clampi((int)value, 0, DS800_WAVE_COUNT - 1); mark_all_dirty(s); return 0; }

    /* --- filter ------------------------------------------------------ */
    if (!strcmp(key, "filter_cutoff"))    { s->filter_cutoff    = clampf(value, 0, 1); refresh_coeffs(s); return 0; }
    if (!strcmp(key, "filter_resonance")) { s->filter_resonance = clampf(value, 0, 1); refresh_coeffs(s); return 0; }
    if (!strcmp(key, "filter_contour"))   { s->filter_contour   = clampf(value, 0, 1); return 0; }
    if (!strcmp(key, "key_track"))    { s->key_track    = clampf(value, 0, 1); return 0; }

    /* --- amplitude envelope ------------------------------------------ */
    if (!strcmp(key, "env_attack"))  { s->env_attack  = clampf(value, 0, 1); refresh_coeffs(s); return 0; }
    if (!strcmp(key, "env_decay"))   { s->env_decay   = clampf(value, 0, 1); refresh_coeffs(s); return 0; }
    if (!strcmp(key, "env_release")) { s->env_release = clampf(value, 0, 1); refresh_coeffs(s); return 0; }
    if (!strcmp(key, "env_sustain")) { s->env_sustain = clampf(value, 0, 1); return 0; }
    if (!strcmp(key, "vel_env"))     { s->vel_env     = clampf(value, 0, 1); return 0; }

    /* --- filter envelope: its own times ------------------------------ */
    if (!strcmp(key, "fenv_attack"))  { s->fenv_attack  = clampf(value, 0, 1); refresh_coeffs(s); return 0; }
    if (!strcmp(key, "fenv_decay"))   { s->fenv_decay   = clampf(value, 0, 1); refresh_coeffs(s); return 0; }
    if (!strcmp(key, "fenv_release")) { s->fenv_release = clampf(value, 0, 1); refresh_coeffs(s); return 0; }
    if (!strcmp(key, "fenv_sustain")) { s->fenv_sustain = clampf(value, 0, 1); return 0; }

    /* --- output ------------------------------------------------------ */
    if (!strcmp(key, "volume"))    { s->volume    = clampf(value, 0, 1); return 0; }
    if (!strcmp(key, "play_mode")) { s->play_mode = (uint8_t)clampi((int)value, 0, 1); return 0; }

    /* --- Stage 2: the LFOs ------------------------------------------- */
    if (!strcmp(key, "lfo1_rate"))  {
        s->lfo1_rate = (float)clampf(value, 0.0f, 1.0f);
        for (int i = 0; i < DS800_VOICES; i++)
            ds800_lfo_set_rate(&s->voice[i].lfo1, s->sample_rate,
                               rate_from_knob(s->lfo1_rate, ds800_lfo_rates_hz, DS800_LFO_RATE_COUNT));
        return 0;
    }
    if (!strcmp(key, "lfo1_shape")) { s->lfo1_shape = (uint8_t)clampi((int)value, 0, DS800_LFO_SHAPE_COUNT - 1); return 0; }
    if (!strcmp(key, "lfo2_rate"))  {
        s->lfo2_rate = (float)clampf(value, 0.0f, 1.0f);
        for (int i = 0; i < DS800_VOICES; i++)
            ds800_lfo_set_rate(&s->voice[i].lfo2, s->sample_rate,
                               rate_from_knob(s->lfo2_rate, ds800_lfo_rates_hz, DS800_LFO_RATE_COUNT));
        return 0;
    }
    if (!strcmp(key, "lfo2_shape")) { s->lfo2_shape = (uint8_t)clampi((int)value, 0, DS800_LFO_SHAPE_COUNT - 1); return 0; }
    if (!strcmp(key, "lfo_sync"))   { s->lfo_sync   = (uint8_t)clampi((int)value, 0, 1); return 0; }

    /* --- Stage 2: the S&H clock and its noise ------------------------ */
    if (!strcmp(key, "sh_rate"))    { s->sh_rate   = (float)clampf(value, 0.0f, 1.0f);
                                      ds800_sh_clock_set_rate(&s->sh_clock, s->sample_rate,
                                                              rate_from_knob(s->sh_rate, ds800_sh_rate_hz,
                                                                             DS800_SH_RATE_COUNT)); return 0; }
    if (!strcmp(key, "sh_input"))   { s->sh_input = (uint8_t)clampi((int)value, 0, DS800_SH_IN_COUNT - 1); return 0; }
    if (!strcmp(key, "sh_env"))      { s->sh_env = (uint8_t)clampi((int)value, 0, 1); return 0; }
    if (!strcmp(key, "sh_amt"))     { s->sh_amt = clampf(value, 0, 1); return 0; }
    if (!strcmp(key, "sh_slew"))    {
        s->sh_slew = clampf(value, 0, 1);
        /* map 0..1 onto 0..0.2 s of slew */
        for (int i = 0; i < DS800_VOICES; i++)
            ds800_sh_set_slew(&s->voice[i].sh, s->sample_rate,
                              (double)s->sh_slew * 0.2);
        return 0;
    }
    if (!strcmp(key, "noise_colour")) {
        s->noise_colour = (uint8_t)clampi((int)value, 0, DS800_NOISE_COLOUR_COUNT - 1);
        double pos = (s->noise_colour == DS800_NOISE_WHITE) ? 1.0
                   : (s->noise_colour == DS800_NOISE_PINK)  ? 0.5 : 0.0;
        for (int i = 0; i < DS800_VOICES; i++)
            ds800_sh_set_noise_colour(&s->voice[i].sh, s->sample_rate, pos);
        return 0;
    }

    /* THE PRE-FILTER MIXER'S NOISE LEVEL. The 2600's mixer takes noise, and
     * that is what gives S&H-through-a-resonant-filter its edge. 0 by
     * default so the instrument is unchanged until it is turned up.
     *
     * This MUST sit after noise_colour's closing brace. Spliced in above it,
     * it lands after that branch's `return 0;` and is unreachable -- the knob
     * is declared, positioned correctly, and never stores anything, which
     * reads on the panel as a value that snaps back to zero. */
    if (!strcmp(key, "noise_lvl"))   { s->noise_lvl = clampf(value, 0.0f, 1.0f); return 0; }


    /* --- Stage 2: the matrix ----------------------------------------- */
    {
        static const char *slot_pre[DS800_MOD_SLOTS] = { "mod1_", "mod2_", "mod3_", "mod4_", "mod5_", "mod6_" };
        for (int i = 0; i < DS800_MOD_SLOTS; i++) {
            char buf[16];
            snprintf(buf, sizeof(buf), "%s%s", slot_pre[i], "src");
            if (!strcmp(key, buf)) {
                uint8_t v = (uint8_t)clampi((int)value, 0, DS800_SRC_COUNT - 1);
                for (int k = 0; k < DS800_VOICES; k++) s->voice[k].mod.slot[i].src = v;
                return 0;
            }
            snprintf(buf, sizeof(buf), "%s%s", slot_pre[i], "dst");
            if (!strcmp(key, buf)) {
                uint8_t v = (uint8_t)clampi((int)value, 0, DS800_DST_COUNT - 1);
                for (int k = 0; k < DS800_VOICES; k++) s->voice[k].mod.slot[i].dst = v;
                return 0;
            }
            snprintf(buf, sizeof(buf), "%s%s", slot_pre[i], "depth");
            if (!strcmp(key, buf)) {
                float v = clampf(value, -1.0f, 1.0f);
                for (int k = 0; k < DS800_VOICES; k++) s->voice[k].mod.slot[i].depth = v;
                return 0;
            }
        }
    }

    return -1;
}

float ds800_get_param(ds800_t *s, const char *key)
{
    if (!s || !key) return 0.0f;

    if (!strcmp(key, "oct1"))   return (float)s->oct_sel[0];
    if (!strcmp(key, "oct2"))   return (float)s->oct_sel[1];
    if (!strcmp(key, "osc1_lvl")) return s->lvl[0][0];
    if (!strcmp(key, "osc2_lvl")) return s->lvl[1][0];
    if (!strcmp(key, "sub_lvl"))  return s->sub_lvl[0];
    if (!strcmp(key, "pw1"))      return s->pw_sel[0];
    if (!strcmp(key, "pw2"))      return s->pw_sel[1];
    if (!strcmp(key, "fold1"))    return s->fold_sel[0];
    if (!strcmp(key, "fold2"))    return s->fold_sel[1];
    if (!strcmp(key, "wave1"))  return (float)s->wave_sel[0];
    if (!strcmp(key, "wave2"))  return (float)s->wave_sel[1];

    if (!strcmp(key, "filter_cutoff"))    return s->filter_cutoff;
    if (!strcmp(key, "filter_resonance")) return s->filter_resonance;
    if (!strcmp(key, "filter_contour"))   return s->filter_contour;
    if (!strcmp(key, "key_track"))    return s->key_track;

    if (!strcmp(key, "env_attack"))   return s->env_attack;
    if (!strcmp(key, "env_decay"))    return s->env_decay;
    if (!strcmp(key, "env_release"))  return s->env_release;
    if (!strcmp(key, "env_sustain"))  return s->env_sustain;
    if (!strcmp(key, "vel_env"))      return s->vel_env;

    if (!strcmp(key, "fenv_attack"))  return s->fenv_attack;
    if (!strcmp(key, "fenv_decay"))   return s->fenv_decay;
    if (!strcmp(key, "fenv_release")) return s->fenv_release;
    if (!strcmp(key, "fenv_sustain")) return s->fenv_sustain;

    if (!strcmp(key, "volume"))    return s->volume;
    if (!strcmp(key, "play_mode")) return (float)s->play_mode;

    /* --- Stage 2 status ---------------------------------------------- */
    if (!strcmp(key, "lfo1_rate"))  return (float)s->lfo1_rate;
    if (!strcmp(key, "lfo1_shape")) return (float)s->lfo1_shape;
    if (!strcmp(key, "lfo2_rate"))  return (float)s->lfo2_rate;
    if (!strcmp(key, "lfo2_shape")) return (float)s->lfo2_shape;
    if (!strcmp(key, "lfo_sync"))   return (float)s->lfo_sync;
    if (!strcmp(key, "sh_rate"))    return (float)s->sh_rate;
    if (!strcmp(key, "sh_input"))   return (float)s->sh_input;
    if (!strcmp(key, "sh_amt"))     return s->sh_amt;
    if (!strcmp(key, "sh_slew"))    return s->sh_slew;
    if (!strcmp(key, "sh_env"))     return (float)s->sh_env;
    if (!strcmp(key, "noise_colour")) return (float)s->noise_colour;
    if (!strcmp(key, "noise_lvl"))    return s->noise_lvl;

    {
        static const char *slot_pre[DS800_MOD_SLOTS] = { "mod1_", "mod2_", "mod3_", "mod4_", "mod5_", "mod6_" };
        for (int i = 0; i < DS800_MOD_SLOTS; i++) {
            char buf[16];
            snprintf(buf, sizeof(buf), "%s%s", slot_pre[i], "src");
            if (!strcmp(key, buf)) return (float)s->voice[0].mod.slot[i].src;
            snprintf(buf, sizeof(buf), "%s%s", slot_pre[i], "dst");
            if (!strcmp(key, buf)) return (float)s->voice[0].mod.slot[i].dst;
            snprintf(buf, sizeof(buf), "%s%s", slot_pre[i], "depth");
            if (!strcmp(key, buf)) return s->voice[0].mod.slot[i].depth;
        }
    }

    if (!strcmp(key, "sh_value"))    return (float)ds800_sh_value(&s->voice[0].sh);
    if (!strcmp(key, "clock_state")) return s->sh_clock_square > 0.0 ? 1.0f : 0.0f;

    /* --- read-only status -------------------------------------------- */
    if (!strcmp(key, "ready"))     return (float)s->ready;
    if (!strcmp(key, "voices")) {
        int n = 0;
        for (int i = 0; i < DS800_VOICES; i++) if (s->voice[i].active) n++;
        return (float)n;
    }

    return 0.0f;
}

/* ---------------------------------------------------------------------------
 * UI hierarchy
 *
 * All nine pages are served from ONE hierarchy, generated into
 * moons800_all_meta.h from the Stage 1 and Stage 2 page lists. Two separate
 * hierarchies would have shown only three of the nine pages: the host reads
 * ui_hierarchy from the PLUGIN only, and ignores one declared in module.json.
 * ------------------------------------------------------------------------ */

const char *ds800_ui_hierarchy(void)
{
    return DS800_ALL_UI_HIERARCHY;
}
