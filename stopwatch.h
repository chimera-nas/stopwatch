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
 * Frequency / conversion factors are obtained, in order of preference:
 *   1. The Linux perf_event mmap page (cap_user_time): the kernel's own
 *      TSC->ns mult/shift. Exact, requires no calibration, and its presence
 *      is itself the kernel's verdict that the TSC is a safe timekeeping
 *      source on this machine.
 *   2. CPUID leaf 0x15 (Intel nominal TSC frequency).
 *   3. A brief (~50ms) calibration against clock_gettime, refined once after
 *      ~1 second of runtime.
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

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#define STOPWATCH_INLINE __forceinline
#else
#define STOPWATCH_INLINE __attribute__((always_inline)) inline
#endif

#if !defined(_WIN32) && (defined(__i386__) || defined(__x86_64__))
#  include <cpuid.h>
#  include <x86intrin.h>  /* __rdtsc */
#endif

#if defined(__linux__)
#  include <string.h>
#  include <unistd.h>
#  include <sys/mman.h>
#  include <sys/syscall.h>
#  include <linux/perf_event.h>
#endif

/* ---------- Configurable constants ---------- */

/* Fixed-point shift for the CPUID / calibration paths: ns = (cycles * mult) >> shift.
 * The perf-page path supplies its own shift (stored in ctx->tsc_shift).
 */
#define STOPWATCH_TSC_SHIFT            32u

/* Quick calibration interval (ns). Keep this short for init. */
#define STOPWATCH_QUICK_CALIBRATION_NS (50ULL * 1000ULL * 1000ULL)  /* 50 ms */

/* Refinement threshold: refine once after ~1 second worth of cycles. */
#define STOPWATCH_REFINE_AFTER_SECONDS 1ULL

#ifndef unlikely
#ifdef _WIN32
#define unlikely(x) (!!(x))
#else
#define unlikely(x) __builtin_expect(!!(x), 0)
#endif
#endif

#ifndef likely
#ifdef _WIN32
#define likely(x) (!!(x))
#else
#define likely(x) __builtin_expect(!!(x), 1)
#endif
#endif

/* ---------- Data structures ---------- */

/* Wall time has its own fixed conversion and correction state. It never
 * changes the monotonic stopwatch's calibration or conversion factors. */
#ifndef _WIN32
struct stopwatch_wall_clock {
    uint64_t mult;
    uint32_t shift;
    unsigned int writer;
    unsigned int sequence;
    uint64_t base_ticks;
    uint64_t base_ns;
    int64_t adjustment_ns;
    uint64_t refresh_ticks;
    uint64_t last_ns;
};
#endif

struct stopwatch_context {
#ifdef _WIN32
    uint64_t qpc_hz;
#endif
#ifndef _WIN32
    struct stopwatch_wall_clock wall;
#endif
    int      use_tsc;         /* 0 = use clock_gettime, 1 = use TSC */

    /* In TSC mode, these are meaningful. Otherwise, ignored. */
    uint64_t tsc_hz;          /* TSC frequency in Hz */
    uint64_t tsc_mult;        /* Fixed-point cycles->ns multiplier */
    uint32_t tsc_shift;       /* Fixed-point shift for tsc_mult */

    int      tsc_from_cpuid;  /* 1 if tsc_hz came from CPUID 0x15, 0 if calibrated */

    /* Refinement state (only used if refine_done == 0). */
    uint64_t ref_tsc;         /* TSC value at reference time */
    struct timespec ref_time; /* timespec at reference time */
    uint64_t refine_threshold_cycles; /* cycles to wait before refining (~1s worth) */
    int      refine_done;     /* 0 until we refine once, then 1 */
    uint64_t refine_ns;       /* ns used for refinement interval (0 until refined) */
};

struct stopwatch {
    union {
        uint64_t        ticks; /* TSC mode: start TSC value */
        struct timespec ts;    /* Non-TSC mode: start CLOCK_MONOTONIC time */
    } start;
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

#ifndef _WIN32
static inline clockid_t
stopwatch_monotonic_clock_id(void)
{
#ifdef CLOCK_MONOTONIC_RAW
    return CLOCK_MONOTONIC_RAW;
#else
    return CLOCK_MONOTONIC;
#endif
}

#endif /* !_WIN32 */

#if !defined(_WIN32) && (defined(__i386__) || defined(__x86_64__))

/* Check for invariant TSC via CPUID 0x80000007 EDX bit 8. */
static inline int
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
static inline uint64_t
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
static inline uint64_t
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
static inline uint64_t
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

/*
 * Tier 1: obtain the kernel's TSC->ns conversion factors from the perf_event
 * mmap page. Returns 1 and populates tsc_mult / tsc_shift / tsc_hz on success.
 *
 * cap_user_time is only set when the kernel is actually driving timekeeping
 * from the TSC (i.e. it passed the boot-time inter-core sync test and did not
 * demote the clocksource), so success here doubles as the safety gate.
 */
static inline int
stopwatch_init_from_perf(struct stopwatch_context *ctx)
{
#if defined(__linux__)
    struct perf_event_attr attr;

    memset(&attr, 0, sizeof(attr));
    attr.type           = PERF_TYPE_HARDWARE;
    attr.config         = PERF_COUNT_HW_INSTRUCTIONS;
    attr.size           = sizeof(attr);
    attr.disabled       = 1;
    attr.exclude_kernel = 1;
    attr.exclude_hv     = 1;

    int fd = (int)syscall(SYS_perf_event_open, &attr, 0 /* this process */, -1, -1, 0);
    if (fd < 0) {
        return 0; /* paranoid policy or unsupported; fall back */
    }

    void *addr = mmap(NULL, 4096, PROT_READ, MAP_SHARED, fd, 0);
    if (addr == MAP_FAILED) {
        close(fd);
        return 0;
    }

    volatile struct perf_event_mmap_page *pc =
        (volatile struct perf_event_mmap_page *)addr;

    uint32_t seq, cap, time_mult, time_shift;

    /* Seqlock: the kernel updates these factors live, so retry on an odd or
     * changed sequence.
     */
    do {
        seq = pc->lock;
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        cap        = pc->cap_user_time;
        time_mult  = pc->time_mult;
        time_shift = pc->time_shift;
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
    } while (pc->lock != seq || (seq & 1));

    munmap(addr, 4096);
    close(fd);

    if (!cap || time_mult == 0) {
        return 0; /* kernel is not driving timekeeping from the TSC */
    }

    ctx->tsc_mult  = time_mult;
    ctx->tsc_shift = time_shift;

    /* Derive hz for ns->ticks: hz = (1e9 << shift) / mult. */
    __int128 num = ((__int128)1000000000ULL << time_shift);
    ctx->tsc_hz  = (uint64_t)(num / (__int128)time_mult);

    return 1;
#else  /* !__linux__ */
    (void)ctx;
    return 0;
#endif /* __linux__ */
}

/* Possibly refine TSC frequency using the time since ref_tsc/ref_time.
 * Only runs once, after enough cycles have elapsed (~1 second worth), and
 * only on the calibration path (perf / CPUID set refine_done == 1).
 */
static  STOPWATCH_INLINE void
stopwatch_maybe_refine(struct stopwatch_context *ctx, uint64_t now_tsc)
{
    /* Only refine if:
     *  - using TSC
     *  - initial frequency was calibrated (not perf / CPUID)
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
 * - x86: tries the perf page, then CPUID 0x15, then calibration (see the
 *   file header). Stores a reference (TSC, timespec) for a possible one-time
 *   refinement only on the calibration path.
 * - Otherwise: falls back to clock_gettime(CLOCK_MONOTONIC, ...).
 */
static inline void
stopwatch_monotonic_context_init(struct stopwatch_context *ctx)
{
    ctx->use_tsc           = 0;
#ifdef _WIN32
    LARGE_INTEGER frequency;
    QueryPerformanceFrequency(&frequency);
    ctx->qpc_hz = (uint64_t) frequency.QuadPart;
#endif
    ctx->tsc_hz            = 0;
    ctx->tsc_mult          = 0;
    ctx->tsc_shift         = STOPWATCH_TSC_SHIFT;
    ctx->tsc_from_cpuid    = 0;
    ctx->ref_tsc           = 0;
    ctx->ref_time.tv_sec   = 0;
    ctx->ref_time.tv_nsec  = 0;
    ctx->refine_threshold_cycles = 0;
    ctx->refine_done       = 1;  /* default: nothing to refine */
    ctx->refine_ns         = 0;

#if !defined(_WIN32) && (defined(__i386__) || defined(__x86_64__))
    /* Tier 1: kernel perf page. Exact factors, no calibration, and its
     * presence is the kernel's verdict that the TSC is safe. The context
     * stays immutable after this (no hot-path refinement).
     */
    if (stopwatch_init_from_perf(ctx)) {
        ctx->use_tsc     = 1;
        ctx->refine_done = 1;
        return;
    }

    /* Tiers 2 and 3 derive the frequency ourselves, so require invariant TSC. */
    if (!stopwatch_has_invariant_tsc()) {
        return; /* TSC not safe; use clock_gettime. */
    }

    ctx->tsc_shift = STOPWATCH_TSC_SHIFT;

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
 *   - Stores current TSC in start.ticks.
 * In non-TSC mode:
 *   - Stores current CLOCK_MONOTONIC time in start.ts.
 */
static STOPWATCH_INLINE void
stopwatch_start(struct stopwatch_context *ctx, struct stopwatch *sw)
{
#ifdef _WIN32
    LARGE_INTEGER counter;
    (void) ctx;
    QueryPerformanceCounter(&counter);
    sw->start.ticks = (uint64_t) counter.QuadPart;
#else
    if (ctx->use_tsc) {
#if !defined(_WIN32) && (defined(__i386__) || defined(__x86_64__))
        uint64_t now = __rdtsc();

        /* Possibly refine TSC frequency once, using time since init. */
        stopwatch_maybe_refine(ctx, now);

        sw->start.ticks = now;
#else
        /* Should not happen, but fall back just in case. */
        clock_gettime(CLOCK_MONOTONIC, &sw->start.ts);
#endif
    } else {
        clock_gettime(CLOCK_MONOTONIC, &sw->start.ts);
    }
#endif
}

/*
 * Read ticks since stopwatch_start().
 *
 * Returns:
 *   - TSC mode:  current_tsc - start_tsc
 *   - Non-TSC:   elapsed nanoseconds
 *
 * The TSC subtraction is clamped at zero: a start captured on one core and a
 * read on another can see a small negative delta if the per-core TSCs are not
 * perfectly aligned, and we would rather report 0 than a ~2^64 outlier.
 */
static STOPWATCH_INLINE uint64_t
stopwatch_read_ticks(const struct stopwatch_context *ctx,
                     const struct stopwatch *sw)
{
#ifdef _WIN32
    LARGE_INTEGER counter;
    (void) ctx;
    QueryPerformanceCounter(&counter);
    uint64_t now = (uint64_t) counter.QuadPart;
    return now >= sw->start.ticks ? now - sw->start.ticks : 0;
#else
    if (ctx->use_tsc) {
#if !defined(_WIN32) && (defined(__i386__) || defined(__x86_64__))
        uint64_t now   = __rdtsc();
        uint64_t start = sw->start.ticks;
        return likely(now >= start) ? now - start : 0;
#else
        /* Should not happen, but fall back. */
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        return stopwatch_timespec_diff_ns(&sw->start.ts, &now);
#endif
    } else {
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        return stopwatch_timespec_diff_ns(&sw->start.ts, &now);
    }
#endif
}

/*
 * Convert ticks -> nanoseconds.
 *
 * - TSC mode:
 *     ns = (ticks * tsc_mult) >> tsc_shift
 * - Non-TSC mode:
 *     ns = ticks (ticks are already ns)
 */
static STOPWATCH_INLINE uint64_t
stopwatch_ticks_to_ns(const struct stopwatch_context *ctx, uint64_t ticks)
{
#ifdef _WIN32
    /* Divide first so long-running timers do not overflow ticks * 1e9. */
    return (ticks / ctx->qpc_hz) * UINT64_C(1000000000) +
           (ticks % ctx->qpc_hz) * UINT64_C(1000000000) / ctx->qpc_hz;
#else
    if (!ctx->use_tsc) {
        return ticks;
    }

#if !defined(_WIN32) && (defined(__i386__) || defined(__x86_64__))
    __int128 prod = (__int128)ticks * (__int128)ctx->tsc_mult;
    uint64_t ns = (uint64_t)(prod >> ctx->tsc_shift);
    return ns;
#else
    /* Should not happen, but fallback. */
    return ticks;
#endif
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
static STOPWATCH_INLINE uint64_t
stopwatch_ns_to_ticks(const struct stopwatch_context *ctx, uint64_t ns)
{
#ifdef _WIN32
    return (ns / UINT64_C(1000000000)) * ctx->qpc_hz +
           ((ns % UINT64_C(1000000000)) * ctx->qpc_hz + UINT64_C(999999999)) / UINT64_C(1000000000);
#else
    if (!ctx->use_tsc) {
        return ns;
    }

#if !defined(_WIN32) && (defined(__i386__) || defined(__x86_64__))
    uint64_t tsc_hz = ctx->tsc_hz;

    /* ticks ≈ ns * tsc_hz / 1e9 (rounded) */
    __int128 prod = (__int128)ns * (__int128)tsc_hz + 999999999LL;
    uint64_t ticks = (uint64_t)(prod / 1000000000LL);

    return ticks;
#else
    return ns;
#endif
#endif
}

/*
 * Convenience: directly get elapsed time in nanoseconds for a stopwatch.
 */
static STOPWATCH_INLINE uint64_t
stopwatch_elapsed_ns(const struct stopwatch_context *ctx,
                     const struct stopwatch *sw)
{
    uint64_t ticks = stopwatch_read_ticks(ctx, sw);
    return stopwatch_ticks_to_ns(ctx, ticks);
}


/* ---------- Wall-clock API ---------- */

/* Maximum rate adjustment in parts per million. 100000 = +/-10%: a 100 ms
 * error takes at least one second to absorb. Set to 500 for traditional NTP
 * rates. This must be identical in all translation units using a context. */
#ifndef STOPWATCH_WALL_MAX_SLEW_PPM
#define STOPWATCH_WALL_MAX_SLEW_PPM 100000
#endif
#if STOPWATCH_WALL_MAX_SLEW_PPM <= 0 || STOPWATCH_WALL_MAX_SLEW_PPM >= 1000000
#error STOPWATCH_WALL_MAX_SLEW_PPM must be strictly between 0 and 1000000
#endif

#ifndef _WIN32
/* An override lets the deterministic tests drive the TSC path on non-x86
 * hosts as well. Applications should use the default hardware reader. */
#ifndef STOPWATCH_WALL_TICKS
static inline uint64_t
stopwatch_wall_ticks(void)
{
#if defined(__i386__) || defined(__x86_64__)
    return __rdtsc();
#else
    return 0; /* The native non-x86 context never enables TSC. */
#endif
}
#define STOPWATCH_WALL_TICKS() stopwatch_wall_ticks()
#endif

struct stopwatch_wall_snapshot {
    uint64_t now_ticks;
    uint64_t base_ticks;
    uint64_t base_ns;
    int64_t adjustment_ns;
    uint64_t refresh_ticks;
};

static inline uint64_t
stopwatch_wall_elapsed(const struct stopwatch_wall_clock *wall, uint64_t ticks)
{
    return (uint64_t)(((unsigned __int128)ticks * wall->mult) >> wall->shift);
}

static inline uint64_t
stopwatch_wall_estimate(const struct stopwatch_wall_clock *wall,
                        const struct stopwatch_wall_snapshot *s, uint64_t ticks)
{
    uint64_t elapsed = ticks > s->base_ticks ?
        stopwatch_wall_elapsed(wall, ticks - s->base_ticks) : 0;
    /* Apply the pending adjustment over one second, then resume the nominal
     * rate. Never keep applying a correction beyond the amount owed. */
    uint64_t progress = elapsed < 1000000000ULL ? elapsed : 1000000000ULL;
    int64_t correction = (int64_t)(((__int128)s->adjustment_ns * progress) /
                                   1000000000LL);
    return s->base_ns + elapsed + correction;
}

static inline void
stopwatch_wall_snapshot(const struct stopwatch_wall_clock *wall,
                        struct stopwatch_wall_snapshot *s)
{
    for (;;) {
        unsigned int seq = __atomic_load_n(&wall->sequence, __ATOMIC_ACQUIRE);
        if (seq & 1) continue;
        s->base_ticks = __atomic_load_n(&wall->base_ticks, __ATOMIC_RELAXED);
        s->base_ns = __atomic_load_n(&wall->base_ns, __ATOMIC_RELAXED);
        s->adjustment_ns = __atomic_load_n(&wall->adjustment_ns, __ATOMIC_RELAXED);
        s->refresh_ticks = __atomic_load_n(&wall->refresh_ticks, __ATOMIC_RELAXED);
        s->now_ticks = STOPWATCH_WALL_TICKS();
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        if (seq == __atomic_load_n(&wall->sequence, __ATOMIC_RELAXED)) return;
    }
}

/* Bracket CLOCK_REALTIME with ticks and keep the narrowest of three samples.
 * A preempted sample must not be mistaken for clock drift. The midpoint
 * bounds the pairing error to half the observed sampling interval. */
static inline int
stopwatch_wall_sample(const struct stopwatch_wall_clock *wall,
                      uint64_t *ticks, uint64_t *ns)
{
    uint64_t best = UINT64_MAX;
    for (int i = 0; i < 3; i++) {
        struct timespec ts;
        uint64_t before = STOPWATCH_WALL_TICKS();
        int rc = clock_gettime(CLOCK_REALTIME, &ts);
        uint64_t after = STOPWATCH_WALL_TICKS();
        if (rc != 0 || after < before) continue;
        uint64_t width = after - before;
        if (width < best) {
            best = width;
            *ticks = before + width / 2;
            *ns = (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
        }
    }
    /* Reject pairs spanning over 1 ms. Keep the old curve and try again at
     * the next refresh, rather than installing a scheduling-induced offset. */
    return best != UINT64_MAX && stopwatch_wall_elapsed(wall, best) <= 1000000ULL;
}

static inline void
stopwatch_wall_refresh(struct stopwatch_wall_clock *wall, uint64_t now)
{
    unsigned int expected = 0;
    if (!__atomic_compare_exchange_n(&wall->writer, &expected, 1, 0,
                                     __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) return;

    struct stopwatch_wall_snapshot s;
    stopwatch_wall_snapshot(wall, &s);
    if (now <= s.refresh_ticks ||
        stopwatch_wall_elapsed(wall, now - s.refresh_ticks) < 1000000000ULL) {
        __atomic_store_n(&wall->writer, 0, __ATOMIC_RELEASE);
        return;
    }

    uint64_t ticks = 0, actual = 0;
    if (stopwatch_wall_sample(wall, &ticks, &actual) && ticks >= s.base_ticks) {
        uint64_t estimate = stopwatch_wall_estimate(wall, &s, ticks);
        __int128 delta = (__int128)actual - estimate;
        int64_t limit = (int64_t)STOPWATCH_WALL_MAX_SLEW_PPM * 1000;
        s.base_ticks = ticks;
        s.base_ns = estimate; /* Continuous at the anchor, even on a clock step. */
        s.adjustment_ns = delta > limit ? limit : delta < -limit ? -limit : (int64_t)delta;
    }
    s.refresh_ticks = STOPWATCH_WALL_TICKS();

    /* Readers keep using the old curve during the slow sampling operation.
     * Only this short publication needs a seqlock; all shared fields are
     * atomic so speculative readers are also free of C data races. */
    __atomic_add_fetch(&wall->sequence, 1, __ATOMIC_ACQ_REL);
    __atomic_store_n(&wall->base_ticks, s.base_ticks, __ATOMIC_RELAXED);
    __atomic_store_n(&wall->base_ns, s.base_ns, __ATOMIC_RELAXED);
    __atomic_store_n(&wall->adjustment_ns, s.adjustment_ns, __ATOMIC_RELAXED);
    __atomic_store_n(&wall->refresh_ticks, s.refresh_ticks, __ATOMIC_RELAXED);
    __atomic_add_fetch(&wall->sequence, 1, __ATOMIC_RELEASE);
    __atomic_store_n(&wall->writer, 0, __ATOMIC_RELEASE);
}
#endif /* !_WIN32 */

/* Initialize before publishing the context to readers. Wall time uses the
 * initial TSC conversion independently of later monotonic refinement. */
static inline void
stopwatch_context_init(struct stopwatch_context *ctx)
{
    stopwatch_monotonic_context_init(ctx);
#ifndef _WIN32
    struct stopwatch_wall_clock *wall = &ctx->wall;
    wall->mult = ctx->tsc_mult;
    wall->shift = ctx->tsc_shift;
    wall->writer = 0;
    wall->sequence = 0;
    wall->base_ticks = 0;
    wall->base_ns = 0;
    wall->adjustment_ns = 0;
    wall->refresh_ticks = 0;
    wall->last_ns = 0;
    if (ctx->use_tsc) {
        /* If every initial sample was preempted, the narrowest valid pair
         * still supplies an initial estimate. Later refreshes slew it. */
        stopwatch_wall_sample(wall, &wall->base_ticks, &wall->base_ns);
        wall->refresh_ticks = wall->base_ticks;
        wall->last_ns = wall->base_ns;
    }
#endif
}

/* Unix-epoch wall time in nanoseconds. TSC mode is nondecreasing across
 * readers of this context and slews toward CLOCK_REALTIME, without stepping.
 * The fallback deliberately follows the system clock, including its steps.
 * This API is thread-safe, but is not async-signal-safe in TSC mode. */
static inline uint64_t
stopwatch_realtime_ns(struct stopwatch_context *ctx)
{
#ifdef _WIN32
    FILETIME ft;
    ULARGE_INTEGER value;
    (void)ctx;
    GetSystemTimePreciseAsFileTime(&ft);
    value.LowPart = ft.dwLowDateTime;
    value.HighPart = ft.dwHighDateTime;
    return (value.QuadPart - UINT64_C(116444736000000000)) * 100;
#else
    if (!ctx->use_tsc) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
    }
    struct stopwatch_wall_clock *wall = &ctx->wall;
    struct stopwatch_wall_snapshot s;
    stopwatch_wall_snapshot(wall, &s);
    uint64_t now = s.now_ticks;
    if (now > s.refresh_ticks &&
        stopwatch_wall_elapsed(wall, now - s.refresh_ticks) >= 1000000000ULL) {
        stopwatch_wall_refresh(wall, now);
        stopwatch_wall_snapshot(wall, &s);
        now = s.now_ticks;
    }
    uint64_t ns = stopwatch_wall_estimate(wall, &s, now);
    /* A reader may be delayed across a refresh or migrate between slightly
     * skewed TSCs. Publish a high-water mark to cover those cases as well. */
    uint64_t last = __atomic_load_n(&wall->last_ns, __ATOMIC_RELAXED);
    while (ns > last) {
        if (__atomic_compare_exchange_n(&wall->last_ns, &last, ns, 1,
                                         __ATOMIC_RELAXED, __ATOMIC_RELAXED)) return ns;
    }
    return last;
#endif
}

static inline void
stopwatch_realtime(struct stopwatch_context *ctx, struct timespec *ts)
{
    uint64_t ns = stopwatch_realtime_ns(ctx);
    ts->tv_sec = (time_t)(ns / 1000000000ULL);
    ts->tv_nsec = (long)(ns % 1000000000ULL);
}

#endif /* STOPWATCH_H */
