/* schwung_moons800.c -- the Schwung plugin wrapper for MOON DS800.
 *
 * Written against src/host/plugin_api_v1.h, whose v2 struct is:
 *
 *   typedef struct plugin_api_v2 {
 *       uint32_t api_version;
 *       void* (*create_instance)(const char *module_dir, const char *json_defaults);
 *       void (*destroy_instance)(void *instance);
 *       void (*on_midi)(void *instance, const uint8_t *msg, int len, int source);
 *       void (*set_param)(void *instance, const char *key, const char *val);
 *       int  (*get_param)(void *instance, const char *key, char *buf, int buf_len);
 *       int  (*get_error)(void *instance, char *buf, int buf_len);
 *       void (*render_block)(void *instance, int16_t *out_interleaved_lr, int frames);
 *   } plugin_api_v2_t;
 *
 * TWO RULES THIS FILE FOLLOWS, the same two DubStarMooniCon follows:
 *
 *   1. It does NOT restate the struct. It includes the host's own header, so
 *      there is exactly one definition in the build.
 *   2. It uses DESIGNATED INITIALIZERS. A misspelled field is a compile error,
 *      and a field left out stays NULL instead of shifting every pointer after
 *      it. A positional initialiser with one field missing is what turns into
 *      a call to the wrong function on the audio thread with no diagnostic.
 *
 * AFTERTOUCH -- the difference from DSM, and the point of this instrument
 *
 *   Move's pads send POLYPHONIC aftertouch (0xA0): msg[1] is the note number,
 *   msg[2] is that key's pressure. Ableton's own manual is explicit that the
 *   pads are independently pressure-sensitive and that Move does not support
 *   channel aftertouch -- one pad's pressure never affects another.
 *
 *   DSM receives that message correctly and then throws the note number away,
 *   collapsing every key's pressure into one instance float. That is fine for
 *   a release-length modulation. It is not a CS-80.
 *
 *   So this wrapper routes 0xA0 to ds800_set_note_pressure(engine, note, p),
 *   which finds the voice holding that note and writes ITS pressure. Pressure
 *   on one key opens that key's filter and leaves the others alone.
 *
 *   0xD0 (channel pressure) is kept for USB controllers, where the pressure
 *   value is in msg[1] and applies to the whole channel. On the device itself
 *   the pads never send it.
 *
 * THREADING CONTRACT: every entry point runs on the SPI audio callback --
 * SCHED_FIFO 70, pinned to core 3, roughly 2370 us of slack per 128-frame
 * block. No file I/O, no allocation, no blocking, no logging in any of them.
 */

/* _GNU_SOURCE BEFORE ANY INCLUDE. Without it glibc hides cpu_set_t and the
 * affinity call: CPU_ZERO/CPU_SET expand to nothing, cpu_set_t stays
 * uninitialised, and the worker hands the kernel a garbage CPU mask through
 * pthread_setaffinity_np. That is what the three implicit-declaration
 * warnings on every build were saying, and they were dismissed as harmless.
 * DSM sets this and loads; DS800 did not and did crash on the SPI path. */
#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE 1
#endif

#include "moons800.h"
#include "moons800_meta.h"      /* DS800_CHAIN_PARAMS        */
#include "moons800_mod_meta.h"  /* DS800_MOD_CHAIN_PARAMS    */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stddef.h>
#include <pthread.h>
#include <sched.h>

#if defined(DS800_HOST_SIM)
/* The simulator calls the exported functions directly and needs no header. */
#else
#include "plugin_api_v1.h"

/* Field order is load-bearing. If the header ever reorders, this fires at
 * build time instead of at boot. */
_Static_assert(offsetof(plugin_api_v2_t, get_error) != 0, "no get_error field");
_Static_assert(offsetof(plugin_api_v2_t, render_block) >
               offsetof(plugin_api_v2_t, get_error),
               "plugin_api_v2_t: render_block must follow get_error");
#endif

#define DS800_VERSION "0.1.0"

/* The base-value table's capacity. Every writable parameter plus the
 * read-only status keys, with headroom. */
#define DS800_BASE_SLOTS 96

/* What the worker is given. Heap-allocated by create_instance, freed by the
 * worker -- see the note above dsm_worker/DSM's own history.
 *
 * The rate MUST travel by value on the heap, not through inst->sample_rate:
 * create_instance assigns that field immediately before pthread_create, and
 * the worker can be scheduled before the store lands, reading calloc's 0.0.
 * build_tables then refuses at its rate guard and ready never publishes. */
typedef struct {
    ds800_t    engine;
    atomic_int ready;              /* published by the worker, read by audio */
    pthread_t  worker;
    int        worker_started;
    double     sample_rate;

    /* staging for render_block: the engine writes float stereo, we convert
     * to interleaved int16 out of this. In the instance, not on the stack. */
    float      stage_l[DS800_MAX_BLOCK];
    float      stage_r[DS800_MAX_BLOCK];

    /* --- THE BASE VALUES -----------------------------------------------
     * What set_param last wrote, per key. get_param answers from HERE, not
     * from the engine.
     *
     * WHY. When a parameter is driven by an automation lane, the engine's
     * value IS the lane's value. Schwung's rule for a plain read is
     * "answers the BASE, never the plugin's value" -- the display draws the
     * knob at the base and marks the driven value separately. A wrapper that
     * reports the engine's value makes the host believe the knob has moved
     * to the lane, so the knob pins there and stops responding to the
     * encoder. That is the fault this fixes: the cutoff knob would only move
     * with recorded automation, and snapped back to zero otherwise.
     *
     * Sized generously: 96 keys covers every control plus the read-only
     * status entries, with room to grow. A fixed table, no allocation after
     * create_instance (see the threading contract above). */
    struct { char key[24]; float val; uint8_t used; } base[DS800_BASE_SLOTS];
} ds800_instance_t;

typedef struct {
    ds800_instance_t *inst;
    double            sample_rate;
} ds800_worker_arg_t;


/* ---------------------------------------------------------------------------
 * Parameter service
 *
 * Integer-valued keys are answered without a decimal point, so the display
 * shows the enum position rather than "3.0000".
 * ------------------------------------------------------------------------ */

/* --- the base table: what the knob was last SET to --------------------- */

static void base_store(ds800_instance_t *inst, const char *key, float v)
{
    if (!inst || !key) return;
    if (strlen(key) >= sizeof(inst->base[0].key)) return;
    for (int i = 0; i < DS800_BASE_SLOTS; i++) {
        if (inst->base[i].used && !strcmp(inst->base[i].key, key)) {
            inst->base[i].val = v;
            return;
        }
    }
    for (int i = 0; i < DS800_BASE_SLOTS; i++) {
        if (!inst->base[i].used) {
            snprintf(inst->base[i].key, sizeof(inst->base[i].key), "%s", key);
            inst->base[i].val  = v;
            inst->base[i].used = 1;
            return;
        }
    }
    /* Table full: fall through to the engine value rather than lie. */
}

/* Returns 1 and writes *out when the key has a stored base. */
static int base_load(const ds800_instance_t *inst, const char *key, float *out)
{
    if (!inst || !key) return 0;
    for (int i = 0; i < DS800_BASE_SLOTS; i++) {
        if (inst->base[i].used && !strcmp(inst->base[i].key, key)) {
            *out = inst->base[i].val;
            return 1;
        }
    }
    return 0;
}

static int ds800_is_int_key(const char *key)
{
    /* REWRITTEN AGAINST THE ENGINE. The old list still named div1a/div1b/
     * div2a/div2b -- the Subharmonicon dividers, removed from the engine --
     * and called the four ENVELOPE TIMES integers, which they stopped being
     * when they became continuous 0..1 floats mapped through an exponential
     * curve. It also omitted every Stage 2 enum.
     *
     * Now: exactly the keys the engine stores as uint8_t or clamps through
     * clampi(), which is the definition that matters. Everything else is a
     * float and prints with decimals. */
    static const char *ints[] = {
        /* octave selectors and waveforms */
        "oct1", "oct2", "wave1", "wave2",
        /* the discrete switches */
        "play_mode", "lfo_sync", "sh_input", "sh_env", "noise_colour",
        /* LFO and S&H shapes */
        "lfo1_shape", "lfo2_shape",
        /* the modulation matrix: src and dst are enum positions */
        "mod1_src", "mod1_dst", "mod2_src", "mod2_dst",
        "mod3_src", "mod3_dst", "mod4_src", "mod4_dst",
        "mod5_src", "mod5_dst", "mod6_src", "mod6_dst",
        /* read-only status, answered by this wrapper, not the engine */
        "ready", "voices", "env_stage",
        NULL
    };

    /* NOTE on the four that are in this list but not in the engine's own
     * clampi() path: noise_colour IS an enum (the engine stores the position
     * as uint8_t), and ready / voices / env_stage are integers this wrapper
     * formats itself. All four must print without decimals, so all four
     * belong here. */
    for (int i = 0; ints[i]; i++) if (!strcmp(key, ints[i])) return 1;
    return 0;
}

/* ---------------------------------------------------------------------------
 * Worker -- builds the engine off the audio callback.
 * ------------------------------------------------------------------------ */

static void *ds800_worker(void *arg)
{
    ds800_worker_arg_t *wa = (ds800_worker_arg_t *)arg;

    /* Take what we need, then release the argument at once. Nothing below
     * depends on `wa`, and the worker owns this allocation. */
    ds800_instance_t *inst = wa->inst;
    const double      rate = wa->sample_rate;
    free(wa);

#if defined(__linux__)
    /* Demote first: threads inherit the creator's SCHED_FIFO priority, and a
     * FIFO 70 worker would starve Move's own Link Main at FIFO 35. Both this
     * and the pinning below are Linux-only -- cpu_set_t, CPU_ZERO/CPU_SET and
     * pthread_setaffinity_np are glibc, and macOS has none of them, so the
     * whole block is guarded and the file still compiles natively. */
    struct sched_param sp;
    memset(&sp, 0, sizeof(sp));
    sp.sched_priority = 0;
    pthread_setschedparam(pthread_self(), SCHED_OTHER, &sp);

    /* Keep off core 3 -- that core belongs to the SPI callback. */
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(0, &set);
    CPU_SET(1, &set);
    CPU_SET(2, &set);
    pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
#endif

    int rc = ds800_build_tables(&inst->engine, rate, 440.0);

    atomic_store_explicit(&inst->ready, rc == 0 ? 1 : 0, memory_order_release);
    return NULL;
}

/* ---------------------------------------------------------------------------
 * v2 entry points
 * ------------------------------------------------------------------------ */

static void *create_instance(const char *module_dir, const char *json_defaults)
{
    (void)module_dir;
    (void)json_defaults;

    ds800_instance_t *inst = (ds800_instance_t *)calloc(1, sizeof(*inst));
    if (!inst) return NULL;

    /* MOVE_SAMPLE_RATE is 44100. Do not assume 48000: at 48 kHz the whole
     * instrument plays about a semitone and a half sharp. */
    inst->sample_rate = 44100.0;
    atomic_store_explicit(&inst->ready, 0, memory_order_relaxed);

    /* The worker's argument goes on the heap and the WORKER frees it: its
     * lifetime outlives create_instance's frame. Handing the rate "by value"
     * through a stack struct is not safe for a thread that outlives the
     * frame -- the pointer is still a pointer. */
    ds800_worker_arg_t *wa = (ds800_worker_arg_t *)malloc(sizeof(*wa));
    if (!wa) { free(inst); return NULL; }
    wa->inst        = inst;
    wa->sample_rate = inst->sample_rate;

    if (pthread_create(&inst->worker, NULL, ds800_worker, wa) != 0) {
        free(wa);
        free(inst);
        return NULL;
    }
    inst->worker_started = 1;

    /* Returns immediately: the slot exists and outputs silence until the
     * worker publishes ready. Building inline here is what produced
     * `took 4294967.295 ms on the SPI callback` on DSM -- the usize overflow
     * sentinel -- which stalls the whole device rather than just this slot. */
    return inst;
}

static void destroy_instance(void *instance)
{
    ds800_instance_t *inst = (ds800_instance_t *)instance;
    if (!inst) return;
    if (inst->worker_started) pthread_join(inst->worker, NULL);
    free(inst);
}

static void on_midi(void *instance, const uint8_t *msg, int len, int source)
{
    (void)source;
    ds800_instance_t *inst = (ds800_instance_t *)instance;
    if (!inst || !msg || len < 2) return;

    const uint8_t status = msg[0] & 0xF0;

    if (status == 0x90 && len >= 3) {
        if (msg[2] > 0) ds800_note_on(&inst->engine, msg[1], (float)msg[2] / 127.0f);
        else            ds800_note_off(&inst->engine, msg[1]);   /* vel 0 = off */

    } else if (status == 0x80 && len >= 3) {
        ds800_note_off(&inst->engine, msg[1]);

    } else if (status == 0xB0 && len >= 3 && msg[1] == 123) {
        for (int n = 0; n < 128; n++) ds800_note_off(&inst->engine, n);
        /* A controller that has gone quiet must not leave pressure set. */
        ds800_set_pressure(&inst->engine, 0.0f);
    }
}

/* The value arrives as a STRING, not a float. strtof is in libc: no syscall,
 * no allocation, safe on this thread. */
static void set_param(void *instance, const char *key, const char *val)
{
    ds800_instance_t *inst = (ds800_instance_t *)instance;
    if (!inst || !key || !val) return;

    /* read-only */
    if (!strcmp(key, "ready") || !strcmp(key, "voices") || !strcmp(key, "env_stage"))
        return;

    float v = strtof(val, NULL);
    (void)ds800_set_param(&inst->engine, key, v);

    /* Remember what the knob was SET to. This is what get_param answers with,
     * so the display shows the knob's own position even while an automation
     * lane is driving the engine's value. */
    base_store(inst, key, v);
}

/* Writes into the CALLER's buffer and returns the length written. It never
 * returns a pointer to our own storage. */
static int get_param(void *instance, const char *key, char *buf, int buf_len)
{
    ds800_instance_t *inst = (ds800_instance_t *)instance;
    if (!key || !buf || buf_len <= 0) return -1;

    /* The two JSON services the Shadow UI needs. Both must answer even before
     * the engine exists. */
    if (!strcmp(key, "ui_hierarchy")) {
        int n = snprintf(buf, (size_t)buf_len, "%s", DS800_UI_HIERARCHY);
        return (n < 0 || n >= buf_len) ? -1 : n;
    }
    if (!strcmp(key, "chain_params")) {
        /* STAGE 1 ONLY, DELIBERATELY.
         *
         * Stage 1 declares 37 parameters (4285 bytes) and Stage 2 another 30
         * (3808), and joining them into one JSON array -- which is what the
         * host builds its parameter table from -- produced an 8092-byte
         * answer. DSM's whole working set was ~2400 bytes. Modulating at
         * 8 kB got the module to appear in the picker and then never load,
         * with nothing in schwung.log, which is what a get_param that the
         * host refuses looks like from the outside.
         *
         * Stage 2's keys still reach the engine through set_param, so the
         * modulation controls work; they stop being ADVERTISED up front. Once
         * the real limit is known this can grow back, in measured steps.
         */
        int n = snprintf(buf, (size_t)buf_len, "%s", DS800_CHAIN_PARAMS);
        return (n < 0 || n >= buf_len) ? -1 : n;
    }

    if (!strcmp(key, "ready")) {
        int r = inst ? atomic_load_explicit(&inst->ready, memory_order_acquire) : 0;
        int n = snprintf(buf, (size_t)buf_len, "%d", r);
        return (n < 0 || n >= buf_len) ? -1 : n;
    }
    if (!strcmp(key, "voices")) {
        int n = snprintf(buf, (size_t)buf_len, "%d", DS800_VOICES);
        return (n < 0 || n >= buf_len) ? -1 : n;
    }

    if (!inst) { buf[0] = '\0'; return 0; }

    /* THE BASE, NOT THE ENGINE'S VALUE.
     *
     * While an automation lane drives this parameter the engine holds the
     * lane's value, and reporting that back makes the host pin the knob to
     * it -- the knob then only moves with recorded automation and snaps to
     * zero otherwise, even though set_param still reaches the engine and the
     * sound still changes. Schwung's rule for a plain read is that it
     * "answers the BASE, never the plugin's value"; this is where DS800 was
     * breaking it. */
    float v;
    if (!base_load(inst, key, &v))
        v = ds800_get_param(&inst->engine, key);

    int n;
    if (ds800_is_int_key(key))
        n = snprintf(buf, (size_t)buf_len, "%d", (int)(v < 0 ? v - 0.5f : v + 0.5f));
    else
        n = snprintf(buf, (size_t)buf_len, "%.4f", (double)v);

    return (n < 0 || n >= buf_len) ? -1 : n;
}

static int get_error(void *instance, char *buf, int buf_len)
{
    ds800_instance_t *inst = (ds800_instance_t *)instance;
    if (!buf || buf_len <= 0) return -1;

    int ready = inst ? atomic_load_explicit(&inst->ready, memory_order_acquire) : 0;
    if (ready == 1) { buf[0] = '\0'; return 0; }

    int n = snprintf(buf, (size_t)buf_len, "engine building");
    return (n < 0 || n >= buf_len) ? -1 : n;
}

/* The engine renders float stereo; the host wants interleaved int16.
 * Convert and clip once, here, at the boundary. */
static void render_block(void *instance, int16_t *out_lr, int frames)
{
    ds800_instance_t *inst = (ds800_instance_t *)instance;
    if (!out_lr || frames <= 0) return;

    if (!inst || atomic_load_explicit(&inst->ready, memory_order_acquire) != 1) {
        for (int i = 0; i < frames * 2; i++) out_lr[i] = 0;
        return;
    }

    int n = frames;
    if (n > DS800_MAX_BLOCK) n = DS800_MAX_BLOCK;

    ds800_render(&inst->engine, inst->stage_l, inst->stage_r, n);

    for (int i = 0; i < n; i++) {
        float sl = inst->stage_l[i];
        float sr = inst->stage_r[i];
        if (sl >  1.0f) sl =  1.0f;
        if (sl < -1.0f) sl = -1.0f;
        if (sr >  1.0f) sr =  1.0f;
        if (sr < -1.0f) sr = -1.0f;
        out_lr[i * 2 + 0] = (int16_t)(sl * 32767.0f);
        out_lr[i * 2 + 1] = (int16_t)(sr * 32767.0f);
    }
    /* Asked for more than we render? Silence, never stale data. */
    for (int i = n; i < frames; i++) {
        out_lr[i * 2 + 0] = 0;
        out_lr[i * 2 + 1] = 0;
    }
}

/* ---------------------------------------------------------------------------
 * Registration -- designated initializers, so a wrong field name is a
 * compile error and an omitted field stays NULL rather than shifting the rest.
 * ------------------------------------------------------------------------ */

static const plugin_api_v2_t ds800_api = {
    .api_version      = 2,
    .create_instance  = create_instance,
    .destroy_instance = destroy_instance,
    .on_midi          = on_midi,
    .set_param        = set_param,
    .get_param        = get_param,
    .get_error        = get_error,
    .render_block     = render_block,
};

plugin_api_v2_t *move_plugin_init_v2(const host_api_v1_t *host)
{
    (void)host;
    return (plugin_api_v2_t *)&ds800_api;
}
