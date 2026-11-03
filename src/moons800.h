/* moons800.h -- MOON DS800, a CS-80 / ARP 2600 inspired voice for
 * Ableton Move / Schwung.
 *
 * WHAT THIS IS
 *   A polyphonic analogue-style voice. Every voice owns everything:
 *
 *     - two oscillators, each with two subharmonic taps
 *     - a resonant filter PAIR, highpass then lowpass, in series
 *     - its own amplitude and filter envelopes
 *     - its own note pressure
 *
 *   That is the CS-80 half. A MONO/POLY switch collapses it to one voice
 *   with last-note priority, which is the 2600 half.
 *
 * WHY EVERYTHING IS PER VOICE
 *   DubStarMooniCon, which this grew from, has polyphonic oscillators into
 *   ONE shared filter, and holds pressure as a single instance float. That
 *   is fine for a subharmonic organ. It is not a CS-80: on a CS-80, pressure
 *   on one key opens that key's filter and leaves the others alone, and each
 *   note's contour runs independently.
 *
 *   So the voice struct is where all of that lives, and the instance holds
 *   only the knobs and the precomputed tables.
 *
 * THE VOICE LIFETIME -- carried over from DSM, and it matters
 *   note_off does NOT kill the voice. It drops the gate, starts the release,
 *   and marks the voice `releasing`. The voice keeps rendering until its
 *   envelope reaches IDLE. The version before that set active = false the
 *   instant a key came up, which silenced the oscillator before the release
 *   could be heard -- every note ended dead.
 *
 * PERFORMANCE
 *   No pow, sqrt, sin, exp, floor, fmod or divide inside the render loop.
 *   Everything that changes only on a PARAMETER CHANGE is precomputed:
 *   octave ratios, divisor scales, envelope coefficients, filter coefficients.
 *   The stage-0 probe put four voices with dual filters at roughly 2.2x the
 *   operation count of DSM's single shared filter -- affordable, but it means
 *   the loop stays clean.
 */

#ifndef MOONS800_H
#define MOONS800_H

#include <stdint.h>
#include <stdbool.h>

#include "ds800_lfo.h"
#include "ds800_sh.h"
#include "ds800_mod.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DS800_VOICES      4
#define DS800_MAX_BLOCK   128

/* ---------------------------------------------------------------------------
 * Sub oscillator -- the CS-80's single sub, one octave down.
 *
 * DSM's subharmonic divider chain (eight tap ratios, two taps per oscillator,
 * fifths and three-octave stacks) is GONE. The CS-80 has one sub, an octave
 * below the core, and that is what this is. No divider state, no ratio table,
 * no per-tap scaling: the sub reads the oscillator's phase at half rate, so it
 * stays phase-locked to the core for free.
 * ------------------------------------------------------------------------ */

#define DS800_SUB_DIV 2

/* The Buchla Easel wavefolder's symmetry limit. Above this the fold offset
 * pushes the signal past the rails on every cycle and the top of the knob's
 * travel squeals instead of thickening. Capped in the parameter layer AND in
 * the render path -- the render path is the one that matters, because the
 * matrix can reach Symmetry as a destination. */



/* ---------------------------------------------------------------------------
 * Waveforms
 *
 * PULSE and a separate SQUARE would be the same shape, so the second pulse
 * position is a second DUTY instead: same family, different harmonic balance.
 * ------------------------------------------------------------------------ */

typedef enum {
    DS800_WAVE_PULSE = 0,
    DS800_WAVE_PULSE25,
    DS800_WAVE_SAW,
    DS800_WAVE_TRI,
    DS800_WAVE_SINE,      /* added back: the pure tone the Easel folder wants */
    DS800_WAVE_COUNT
} ds800_wave_t;

extern const char *const ds800_wave_names[DS800_WAVE_COUNT];

/* ---------------------------------------------------------------------------
 * Octave -- named steps, so the display says "+1 Octave" not "3"
 * ------------------------------------------------------------------------ */

typedef enum {
    DS800_OCT_MINUS2 = 0,
    DS800_OCT_MINUS1,
    DS800_OCT_UNISON,
    DS800_OCT_PLUS1,
    DS800_OCT_PLUS2,
    DS800_OCT_COUNT
} ds800_octave_t;

extern const int         ds800_octave_semitones[DS800_OCT_COUNT];
extern const char *const ds800_octave_names[DS800_OCT_COUNT];

/* ---------------------------------------------------------------------------
 * Envelope times -- named, so the knob reads as a musical time
 * ------------------------------------------------------------------------ */

typedef enum {
    DS800_ENV_A_1MS = 0, DS800_ENV_A_5MS, DS800_ENV_A_20MS, DS800_ENV_A_80MS,
    DS800_ENV_A_300MS,   DS800_ENV_A_1S,  DS800_ENV_A_3S,
    DS800_ENV_A_COUNT
} ds800_attack_t;

typedef enum {
    DS800_ENV_D_5MS = 0, DS800_ENV_D_20MS, DS800_ENV_D_80MS, DS800_ENV_D_300MS,
    DS800_ENV_D_1S,      DS800_ENV_D_2S,   DS800_ENV_D_4S,
    DS800_ENV_D_COUNT
} ds800_decay_t;

extern const double      ds800_attack_seconds[DS800_ENV_A_COUNT];
extern const char *const ds800_attack_names[DS800_ENV_A_COUNT];
extern const double      ds800_decay_seconds[DS800_ENV_D_COUNT];
extern const char *const ds800_decay_names[DS800_ENV_D_COUNT];

/* ---------------------------------------------------------------------------
 * Envelope stages
 * ------------------------------------------------------------------------ */

typedef enum {
    DS800_ENV_IDLE = 0,
    DS800_ENV_ATTACK,
    DS800_ENV_DECAY,
    DS800_ENV_SUSTAIN,
    DS800_ENV_RELEASE
} ds800_env_stage_t;


/* One envelope's running state. A voice owns two of these. */
typedef struct {
    double  level;
    double  sustain;
    uint8_t stage;
} ds800_env_t;

/* The Moog ladder's state and coefficients. Declared here because the voice
 * struct embeds one, and the render path takes the other. */
typedef struct {
    double b0, b1, b2, b3, b4;
} ds800_ladder_t;

typedef struct {
    double f;        /* the ladder's frequency term, already scaled */
    double fb;       /* resonance feedback */
    double gain;     /* 0.35013 * f^4 */
    double res;      /* 0..1, kept for the passband term */
    double pass;     /* input scale: the 4012's loss as resonance rises */
} ds800_ladder_coeff_t;

/* ---------------------------------------------------------------------------
 * Coefficients -- precomputed on parameter change, never in the loop
 * ------------------------------------------------------------------------ */

typedef struct {
    /* envelope per-sample increments, shared by every voice: they come from
     * knobs, so every voice moves at the same rate */
    double att, dec, rel;

    /* No filter coefficients here any more: the highpass is gone and the
     * Moog ladder carries its own (ds800_ladder_coeff_t). */
} ds800_coeff_t;

/* ---------------------------------------------------------------------------
 * Play mode
 * ------------------------------------------------------------------------ */

typedef enum {
    DS800_POLY = 0,
    DS800_MONO
} ds800_play_t;

/* ONE FILTER: THE MOOG LADDER, and nothing else.
 *
 * There is no highpass and no filter switch. The CS-80's HP+LP pair is gone:
 * sharing a modulation term made them cancel, and at the midpoint the
 * multiplier passed through zero and killed the audio outright. A ladder can
 * do everything the pair was there for, and it rings. */
#define DS800_LP_LADDER 1

extern const char *const ds800_play_names[2];

/* ---------------------------------------------------------------------------
 * Voice
 *
 * Everything a note needs. A voice is a complete instrument: two oscillators,
 * a filter pair, two envelopes, a pressure value.
 * ------------------------------------------------------------------------ */

typedef struct {
    /* --- oscillator: two per voice, core + one CS-80 sub each -------- */
    double  phase[2];          /* core accumulators, 0..1                */
    double  inc[2];            /* per-sample rate at this pitch          */
    double  freq;              /* resolved Hz for the note               */
    double  oct_ratio[2];      /* precomputed from the octave selector   */
    uint8_t wave[2];
    double  pw[2];             /* pulse width offset, -0.25..+0.25       */
    double  lvl[2][2];         /* core, sub -- one sub, not two taps      */
    double  fold[2];           /* wavefolder depth, 0..1                 */
    /* The note this voice was playing when another note STOLE it. Stealing
     * overwrites `note`, so without this the original note's note_off finds
     * nothing, the gate is never dropped, and the voice never reaches IDLE. */
    int     stolen_from;
    uint32_t mix_rng;          /* pre-filter noise source, per voice     */
    double  mix_nz;            /* its one-pole colour state             */

    /* --- the filter pair, this voice's own --------------------------- *
     * HP is the CS-80's 12 dB/oct SVF (unchanged). LP is the MOOG LADDER,
     * 4-pole, borrowed from charlesvestal/schwung-moog. */
    ds800_ladder_t lp;

    /* --- envelopes: one per voice, not one per instrument ------------ */
    ds800_env_t env_amp;
    ds800_env_t env_filt;


    /* --- Stage 2: this voice's own modulation ------------------------- */
    ds800_lfo_t  lfo1;         /* own phase, so a chord is not in lockstep */
    ds800_lfo_t  lfo2;
    ds800_sh_t   sh;           /* own noise state and held value */
    ds800_mod_t  mod;          /* own matrix: pressure and keytrack are per note */

    /* --- lifetime ---------------------------------------------------- */
    bool    active;
    bool    releasing;
    int     note;
    uint32_t age;
    unsigned dirty;
} ds800_voice_t;

/* ---------------------------------------------------------------------------
 * Instance -- the knobs and the tables, nothing that belongs to a note
 * ------------------------------------------------------------------------ */

typedef struct {
    double  sample_rate;
    double  base_note;
    int32_t ready;

    /* --- oscillator parameters --------------------------------------- */
    uint8_t oct_sel[2];          /* ds800_octave_t, per oscillator      */
    uint8_t wave_sel[2];
    float   pw_sel[2];           /* pulse width offset, -0.25..+0.25    */
    float   sub_lvl[2];          /* the CS-80 sub, one octave down      */
    float   lvl[2][2];           /* core, sub                           */
    float   fold_sel[2];         /* Easel wavefolder depth, 0..1        */
    /* The knob side. The render loop reads the per-voice COPIES in
     * ds800_voice_t, refreshed from these in refresh_voice(). */

    /* --- filter ------------------------------------------------------ */
    float   filter_cutoff;       /* 0..1                                */
    float   filter_resonance;    /* 0..1                                */
    float   filter_contour;      /* 0..1, envelope amount to the cutoff */

    /* --- filter envelope --------------------------------------------- */
    /* The CS-80 filter contour. IL pulls the cutoff DOWN at note-on, AL
     * pushes it UP through the attack, and the cutoff knob itself is where
     * the filter SUSTAINS. The manual: "The Filter Cutoff value is the
     * Sustain Level... The Envelope Depth is the Difference between the IL
     * and the AL." Both 0..5; only IL and AL are knobs, there is no
     * separate envelope amount because the contour is the amount. */

    float   key_track;           /* 0..1                                */

    /* --- amplitude envelope ------------------------------------------ */
    /* Continuous 0..1 knobs, mapped to seconds in refresh_coeffs by a cubic
     * curve. They used to be enum indices into a seven-step table -- seven
     * named times, which is a menu rather than a control. */
    float   env_attack;
    float   env_decay;
    float   env_release;
    float   env_sustain;
    float   vel_env;             /* 0..1, velocity -> envelope depth    */

    /* --- filter envelope times (separate, on request) ---------------- */
    float   fenv_attack;
    float   fenv_decay;
    float   fenv_release;
    float   fenv_sustain;

    /* --- output ------------------------------------------------------ */
    float   volume;
    uint8_t play_mode;           /* ds800_play_t                        */

    /* --- aftertouch, channel-wide. On Move the pads send per note; this
     * is the USB-controller path, and it applies to every voice. ------- */
    float   pressure;

    /* --- Stage 2: modulation parameters ------------------------------- */
    float   lfo1_rate;           /* 0..1 -> the Hz table, interpolated   */
    uint8_t lfo1_shape;
    float   lfo2_rate;           /* 0..1 -> the Hz table, interpolated   */
    uint8_t lfo2_shape;
    uint8_t lfo_sync;
    float   sh_rate;             /* 0..1 -> the Hz table, interpolated   */
    uint8_t sh_input;            /* ds800_sh_input_t                     */
    float   sh_amt;              /* 0..1, S&H straight to the cutoff, so the
                                  * module is usable without patching a slot */
    uint8_t sh_env;              /* 1 = the clock retriggers the envelopes */
    float   sh_slew;
    uint8_t noise_colour;        /* ds800_noise_colour_t                 */
    float   noise_lvl;           /* pre-filter mixer noise, 0..1          */

    /* The sample & hold clock is ONE, shared: the 2600 gives the module a
     * single clock on the panel, and it drives the envelopes too. The LFOs
     * are per voice; this is not. */
    ds800_sh_clock_t sh_clock;
    bool             sh_clock_fired;
    double           sh_clock_square;   /* the clock's level, for the matrix */

    ds800_coeff_t coeff;
    ds800_coeff_t fcoeff;        /* filter envelope's own times         */
    ds800_ladder_coeff_t lpco;   /* Moog ladder coefficients            */

    /* --- state ------------------------------------------------------- */
    ds800_voice_t voice[DS800_VOICES];
    int      held[8];
    int      held_count;
    uint32_t note_age;
    int      mono_note;          /* last note in MONO, -1 when none     */
} ds800_t;

/* ---------------------------------------------------------------------------
 * API -- no host types, so the whole engine builds and tests off-device
 * ------------------------------------------------------------------------ */

int   ds800_build_tables(ds800_t *s, double sample_rate, double base_hz);
void  ds800_render(ds800_t *s, float *out_l, float *out_r, int frames);
void  ds800_note_on(ds800_t *s, int note, float vel);
void  ds800_note_off(ds800_t *s, int note);
void  ds800_set_pressure(ds800_t *s, float pressure);
void  ds800_set_note_pressure(ds800_t *s, int note, float pressure);

int   ds800_set_param(ds800_t *s, const char *key, float value);
float ds800_get_param(ds800_t *s, const char *key);

const char *ds800_ui_hierarchy(void);

#ifdef __cplusplus
}
#endif

#endif /* MOONS800_H */
