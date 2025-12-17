/*
 * Copyright (c) 2025 Ben Jarvis
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

/*
 * Simple stopwatch / timing framework.
 *
 * - On x86 with invariant TSC, uses RDTSC as the time base.
 * - Otherwise, falls back to clock_gettime(CLOCK_MONOTONIC, ...).
 *
 * Ticks:
 *   - TSC mode: ticks == TSC cycles
 *   - Non-TSC mode: ticks == nanoseconds
 *
 * API:
 *   struct stopwatch_context ctx;
 *   stopwatch_context_init(&ctx);
 *
 *   struct stopwatch sw;
 *   stopwatch_start(&ctx, &sw);
 *
 *   uint64_t ticks = stopwatch_read_ticks(&ctx, &sw);
 *   uint64_t ns    = stopwatch_ticks_to_ns(&ctx, ticks);
 *   uint64_t ticks2 = stopwatch_ns_to_ticks(&ctx, ns);
 *
 *   uint64_t elapsed_ns = stopwatch_elapsed_ns(&ctx, &sw);
 */

#ifndef STOPWATCH_H
#define STOPWATCH_H

#include <stdint.h>
#include <time.h>

#if defined(__i386__) || defined(__x86_64__)
#  include <cpuid.h>
#  include <x86intrin.h>  /* __rdtsc */
#endif

/* ---------- Configurable constants ---------- */

/* Fixed-point shift for cycles -> ns conversion: ns = (cycles * mult) >> SHIFT */
#define STOPWATCH_TSC_SHIFT            32u

/* Quick calibration interval (ns). Keep this short for init. */
#define STOPWATCH_QUICK_CALIBRATION_NS (50ULL * 1000ULL * 1000ULL)  /* 50 ms */

/* Refinement threshold: refine once after ~1 second worth of cycles. */
#define STOPWATCH_REFINE_AFTER_SECONDS 1ULL

#ifndef unlikely
#define unlikely(x)                     __builtin_expect(!!(x), 0)
#endif 

#ifndef likely
#define likely(x)                       __builtin_expect(!!(x), 1)
#endif

/* ---------- Data structures ---------- */

struct stopwatch_context {
    int      use_tsc;         /* 0 = use clock_gettime, 1 = use TSC */

    /* In TSC mode, these are meaningful. Otherwise, ignored. */
    uint64_t tsc_hz;          /* TSC frequency in Hz */
    uint64_t tsc_mult;        /* Fixed-point cycles->ns multiplier */

    int      tsc_from_cpuid;  /* 1 if tsc_hz came from CPUID 0x15, 0 if calibrated */

    /* Refinement state (only used if tsc_from_cpuid == 0). */
    uint64_t ref_tsc;         /* TSC value at reference time */
    struct timespec ref_time; /* timespec at reference time */
    uint64_t refine_threshold_cycles; /* cycles to wait before refining (~1s worth) */
    int      refine_done;     /* 0 until we refine once, then 1 */
    uint64_t refine_ns;       /* ns used for refinement interval (0 until refined) */
};

struct stopwatch {
    struct timespec start; /* In TSC mode: start.tv_nsec holds TSC start value */
};

/* ---------- Internal helpers ---------- */

static inline uint64_t
stopwatch_timespec_diff_ns(const struct timespec *start,
                           const struct timespec *end)
{
    time_t sec  = end->tv_sec - start->tv_sec;
    long   nsec = end->tv_nsec - start->tv_nsec;

    if (nsec < 0) {
        --sec;
        nsec += 1000000000L;
    }

    return (uint64_t)sec * 1000000000ULL + (uint64_t)nsec;
}

static inline clockid_t
stopwatch_monotonic_clock_id(void)
{
#ifdef CLOCK_MONOTONIC_RAW
    return CLOCK_MONOTONIC_RAW;
#else
    return CLOCK_MONOTONIC;
#endif
}

#if defined(__i386__) || defined(__x86_64__)

/* Check for invariant TSC via CPUID 0x80000007 EDX bit 8. */
static int
stopwatch_has_invariant_tsc(void)
{
    unsigned int max_leaf = __get_cpuid_max(0x80000000u, NULL);
    if (max_leaf < 0x80000007u) {
        return 0;
    }

    unsigned int eax, ebx, ecx, edx;
    __cpuid(0x80000007u, eax, ebx, ecx, edx);

    /* Bit 8: Invariant TSC */
    return (edx & (1u << 8)) != 0;
}

/* Try to get TSC frequency via CPUID leaf 0x15 (Intel only). */
static uint64_t
stopwatch_get_tsc_hz_via_cpuid(void)
{
    unsigned int max_leaf = __get_cpuid_max(0, NULL);
    if (max_leaf < 0x15u) {
        return 0;
    }

    unsigned int eax, ebx, ecx, edx;
    __cpuid(0x15u, eax, ebx, ecx, edx);

    if (eax == 0 || ebx == 0) {
        return 0;
    }

    /*
     * Intel spec:
     *   TSC frequency = (ECX * EBX) / EAX  (Hz)
     *   If ECX == 0, core crystal clock is not enumerated.
     */
    if (ecx == 0) {
        return 0;
    }

    uint64_t num = (uint64_t)ecx * (uint64_t)ebx;
    uint64_t den = (uint64_t)eax;
    return num / den;
}

/* Quick TSC calibration vs. clock_gettime over target_ns (nanoseconds). */
static uint64_t
stopwatch_calibrate_tsc_interval(uint64_t target_ns)
{
    struct timespec t0, t1;
    uint64_t c0, c1, elapsed_ns;

    clockid_t clk = stopwatch_monotonic_clock_id();

    clock_gettime(clk, &t0);
    c0 = __rdtsc();

    do {
        clock_gettime(clk, &t1);
        elapsed_ns = stopwatch_timespec_diff_ns(&t0, &t1);
    } while (elapsed_ns < target_ns);

    c1 = __rdtsc();

    uint64_t cycles = c1 - c0;

    /* hz ≈ cycles * 1e9 / elapsed_ns */
    if (elapsed_ns == 0) {
        return 0;
    }

    uint64_t hz = (cycles * 1000000000ULL + elapsed_ns / 2) / elapsed_ns;
    return hz;
}

/* Compute fixed-point multiplier for cycles->ns: ns = (cycles * mult) >> SHIFT */
static uint64_t
stopwatch_compute_tsc_mult(uint64_t tsc_hz)
{
    if (tsc_hz == 0) {
        return 0;
    }

    /* mult = (1e9 << SHIFT) / tsc_hz, rounded */
    uint64_t numerator = (1000000000ULL << STOPWATCH_TSC_SHIFT);
    uint64_t mult      = (numerator + tsc_hz / 2) / tsc_hz;
    return mult;
}

/* Possibly refine TSC frequency using the time since ref_tsc/ref_time.
 * Only runs once, after enough cycles have elapsed (~1 second worth).
 */
static  __attribute__((always_inline)) inline void 
stopwatch_maybe_refine(struct stopwatch_context *ctx, uint64_t now_tsc)
{
    /* Only refine if:
     *  - using TSC
     *  - initial frequency was calibrated (not CPUID)
     *  - we haven't refined yet
     */


    if (likely(!ctx->use_tsc || ctx->tsc_from_cpuid || ctx->refine_done)) return;

    uint64_t delta_cycles = now_tsc - ctx->ref_tsc;

    /* Wait until we've seen at least ~1 second worth of cycles
     * based on the initial estimate.
     */
    if (likely(delta_cycles < ctx->refine_threshold_cycles)) {
        return;
    }

    /* Now do a one-time refinement using the full interval since ref_time. */

    ctx->refine_done = 1;

    struct timespec now_ts;
    clock_gettime(stopwatch_monotonic_clock_id(), &now_ts);
    uint64_t ns = stopwatch_timespec_diff_ns(&ctx->ref_time, &now_ts);
    if (ns == 0) {
        return;
    }

    /* new_hz ≈ delta_cycles * 1e9 / ns (with rounding) */
    __int128 num = (__int128)delta_cycles * 1000000000LL + (ns / 2);
    uint64_t new_hz = (uint64_t)(num / ns);
    if (new_hz == 0) {
        return;
    }

    uint64_t new_mult = stopwatch_compute_tsc_mult(new_hz);
    if (new_mult == 0) {
        return;
    }

    ctx->tsc_hz   = new_hz;
    ctx->tsc_mult = new_mult;
    ctx->refine_ns   = ns;
}

#endif /* x86 */

/* ---------- Public API ---------- */

/*
 * Initialize a stopwatch_context.
 *
 * - Detects if invariant TSC is available (x86 only).
 * - If yes:
 *     - Tries CPUID 0x15 for a direct TSC frequency (Intel).
 *     - Otherwise, performs a quick calibration (~50 ms).
 *     - Stores a reference (TSC, timespec) for possible one-time refinement.
 * - If no:
 *     - Falls back to clock_gettime(CLOCK_MONOTONIC, ...).
 */
static inline void
stopwatch_context_init(struct stopwatch_context *ctx)
{
    ctx->use_tsc           = 0;
    ctx->tsc_hz            = 0;
    ctx->tsc_mult          = 0;
    ctx->tsc_from_cpuid    = 0;
    ctx->ref_tsc           = 0;
    ctx->ref_time.tv_sec   = 0;
    ctx->ref_time.tv_nsec  = 0;
    ctx->refine_threshold_cycles = 0;
    ctx->refine_done       = 1;  /* default: nothing to refine */
    ctx->refine_ns         = 0;

#if defined(__i386__) || defined(__x86_64__)
    if (!stopwatch_has_invariant_tsc()) {
        return; /* TSC not safe; use clock_gettime. */
    }

    uint64_t tsc_hz = stopwatch_get_tsc_hz_via_cpuid();
    if (tsc_hz != 0) {
        ctx->tsc_from_cpuid = 1;
        ctx->refine_done    = 1;  /* don't bother refining CPUID-based Hz */
    } else {
        /* Fallback: quick calibration; should be sub-100ms. */
        tsc_hz = stopwatch_calibrate_tsc_interval(STOPWATCH_QUICK_CALIBRATION_NS);
        if (tsc_hz == 0) {
            return; /* Could not determine a frequency; fall back. */
        }
        ctx->tsc_from_cpuid = 0;
        ctx->refine_done    = 0;  /* allow one-time refinement later */
    }

    uint64_t mult = stopwatch_compute_tsc_mult(tsc_hz);
    if (mult == 0) {
        return;
    }

    ctx->tsc_hz   = tsc_hz;
    ctx->tsc_mult = mult;
    ctx->use_tsc  = 1;

    if (!ctx->tsc_from_cpuid) {
        /* Set up refinement reference point (TSC + time) and threshold cycles. */
        clock_gettime(stopwatch_monotonic_clock_id(), &ctx->ref_time);
        ctx->ref_tsc = __rdtsc();

        /* Approximate 1 second worth of cycles using the initial estimate. */
        ctx->refine_threshold_cycles =
            (uint64_t)(tsc_hz * STOPWATCH_REFINE_AFTER_SECONDS);
    }

#else
    (void)ctx; /* Non-x86: always use clock_gettime. */
#endif
}

/*
 * Start a stopwatch.
 *
 * In TSC mode:
 *   - Optionally performs a one-time refinement if enough time has elapsed.
 *   - Stores current TSC in start.tv_nsec (tv_sec unused).
 * In non-TSC mode:
 *   - Stores current CLOCK_MONOTONIC time in start.
 */
static __attribute__((always_inline)) inline void
stopwatch_start(struct stopwatch_context *ctx, struct stopwatch *sw)
{
    if (ctx->use_tsc) {
#if defined(__i386__) || defined(__x86_64__)
        uint64_t now = __rdtsc();

        /* Possibly refine TSC frequency once, using time since init. */
        stopwatch_maybe_refine(ctx, now);

        sw->start.tv_nsec = (long)now;
#else
        /* Should not happen, but fall back just in case. */
        clock_gettime(CLOCK_MONOTONIC, &sw->start);
#endif
    } else {
        clock_gettime(CLOCK_MONOTONIC, &sw->start);
    }
}

/*
 * Read ticks since stopwatch_start().
 *
 * Returns:
 *   - TSC mode:  current_tsc - start_tsc
 *   - Non-TSC:   elapsed nanoseconds
 */
static __attribute__((always_inline)) inline uint64_t
stopwatch_read_ticks(const struct stopwatch_context *ctx,
                     const struct stopwatch *sw)
{
    if (ctx->use_tsc) {
#if defined(__i386__) || defined(__x86_64__)
        uint64_t now = __rdtsc();
        uint64_t start = (uint64_t)sw->start.tv_nsec;
        return now - start;
#else
        /* Should not happen, but fall back. */
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        return stopwatch_timespec_diff_ns(&sw->start, &now);
#endif
    } else {
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        return stopwatch_timespec_diff_ns(&sw->start, &now);
    }
}

/*
 * Convert ticks -> nanoseconds.
 *
 * - TSC mode:
 *     ns = (ticks * tsc_mult) >> STOPWATCH_TSC_SHIFT
 * - Non-TSC mode:
 *     ns = ticks (ticks are already ns)
 */
static __attribute__((always_inline)) inline uint64_t
stopwatch_ticks_to_ns(const struct stopwatch_context *ctx, uint64_t ticks)
{
    if (!ctx->use_tsc) {
        return ticks;
    }

#if defined(__i386__) || defined(__x86_64__)
    uint64_t mult = ctx->tsc_mult;
    __int128 prod = (__int128)ticks * (__int128)mult;
    uint64_t ns = (uint64_t)(prod >> STOPWATCH_TSC_SHIFT);
    return ns;
#else
    /* Should not happen, but fallback. */
    return ticks;
#endif
}

/*
 * Convert nanoseconds -> ticks.
 *
 * - TSC mode:
 *     ticks ≈ ns * tsc_hz / 1e9
 *
 * - Non-TSC mode:
 *     ticks = ns
 */
static __attribute__((always_inline)) inline uint64_t
stopwatch_ns_to_ticks(const struct stopwatch_context *ctx, uint64_t ns)
{
    if (!ctx->use_tsc) {
        return ns;
    }

#if defined(__i386__) || defined(__x86_64__)
    uint64_t tsc_hz = ctx->tsc_hz;

    /* ticks ≈ ns * tsc_hz / 1e9 (rounded) */
    __int128 prod = (__int128)ns * (__int128)tsc_hz + 999999999LL;
    uint64_t ticks = (uint64_t)(prod / 1000000000LL);

    return ticks;
#else
    return ns;
#endif
}

/*
 * Convenience: directly get elapsed time in nanoseconds for a stopwatch.
 */
static __attribute__((always_inline)) inline uint64_t
stopwatch_elapsed_ns(const struct stopwatch_context *ctx,
                     const struct stopwatch *sw)
{
    uint64_t ticks = stopwatch_read_ticks(ctx, sw);
    return stopwatch_ticks_to_ns(ctx, ticks);
}

#endif /* STOPWATCH_H */
