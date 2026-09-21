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

#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdint.h>
#include <inttypes.h>
#include <time.h>
#include <stdlib.h>
#include <string.h>

#include "stopwatch.h"

static volatile uint64_t g_sink_u64 = 0;
static volatile struct timespec g_sink_ts;

/* ---------- Utility: rate formatting, timespec diff ---------- */

static void
format_rate(double calls_per_sec, char *buf, size_t size)
{
    if (calls_per_sec >= 1e9) {
        snprintf(buf, size, "%.3f G", calls_per_sec / 1e9);
    } else if (calls_per_sec >= 1e6) {
        snprintf(buf, size, "%.3f M", calls_per_sec / 1e6);
    } else if (calls_per_sec >= 1e3) {
        snprintf(buf, size, "%.3f K", calls_per_sec / 1e3);
    } else {
        snprintf(buf, size, "%.3f", calls_per_sec);
    }
}

static inline uint64_t
timespec_diff_ns(const struct timespec *start,
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

/* ========================================================================
 * PART 1: Raw timing overhead benchmark (as before)
 * ======================================================================== */

static struct stopwatch_context *g_ctx = NULL;
static struct stopwatch         *g_sw_subject = NULL;

static void
body_stopwatch_read_ticks(void)
{
    g_sink_u64 ^= stopwatch_read_ticks(g_ctx, g_sw_subject);
}

static void
body_stopwatch_elapsed_ns(void)
{
    g_sink_u64 ^= stopwatch_elapsed_ns(g_ctx, g_sw_subject);
}

static void
body_clock_gettime(void)
{
    clock_gettime(CLOCK_MONOTONIC, (struct timespec *)&g_sink_ts);
}

static void
body_stopwatch_realtime(void)
{
    g_sink_u64 ^= stopwatch_realtime_ns(g_ctx);
}

static void
body_clock_realtime(void)
{
    clock_gettime(CLOCK_REALTIME, (struct timespec *)&g_sink_ts);
}

/* Measure elapsed TSC ticks for `iterations` calls of the given body. */
static uint64_t
measure_ticks(struct stopwatch_context *ctx,
              void (*body)(void),
              uint64_t iterations)
{
    struct stopwatch timer;
    stopwatch_start(ctx, &timer);

    for (uint64_t i = 0; i < iterations; ++i) {
        body();
    }

    return stopwatch_read_ticks(ctx, &timer);
}

/* Run a micro-benchmark for one function body. */
static void
run_micro_benchmark(struct stopwatch_context *ctx,
                    const char *label,
                    void (*body)(void))
{
    const uint64_t base_iters      = 1000000ULL;   /* 1M */
    const double   target_total_ns = 1000e6;        /* ~1000 ms */

    printf("== %s ==\n", label);

    /* Warmup. */
    (void)measure_ticks(ctx, body, base_iters);

    /* Estimate time per call. */
    uint64_t ticks = measure_ticks(ctx, body, base_iters);
    uint64_t ns    = stopwatch_ticks_to_ns(ctx, ticks);

    double ns_per_call_est = (double)ns / (double)base_iters;
    if (ns_per_call_est <= 0.0)
        ns_per_call_est = 1.0;

    /* Choose iterations so total time ≈ target_total_ns. */
    double   desired_iters_d = target_total_ns / ns_per_call_est;
    uint64_t iterations      = (uint64_t)(desired_iters_d + 0.5);

    if (iterations < base_iters)
        iterations = base_iters;

    /* Final measurement. */
    ticks = measure_ticks(ctx, body, iterations);
    ns    = stopwatch_ticks_to_ns(ctx, ticks);

    double total_ns    = (double)ns;
    double ns_per_call = total_ns / (double)iterations;
    double calls_per_s = 1e9 / ns_per_call;

    char rate_buf[32];
    format_rate(calls_per_s, rate_buf, sizeof(rate_buf));

    printf("  iterations: %" PRIu64 "\n", iterations);
    printf("  total time: %.3f ms\n", total_ns / 1e6);
    printf("  avg time : %.3f ns/call\n", ns_per_call);
    printf("  rate     : %s calls/s\n\n", rate_buf);
}


/* ========================================================================
 * main()
 * ======================================================================== */

int
main(void)
{
    struct stopwatch_context ctx;
    stopwatch_context_init(&ctx);

    if (!ctx.use_tsc) {
        printf("TSC not available; this benchmark is intended for TSC mode.\n");
        return 1;
    }

    printf("Benchmarking stopwatch vs clock_gettime\n");
    printf("  tsc_hz   = %" PRIu64 " Hz\n", ctx.tsc_hz);
    printf("  tsc_mult = %" PRIu64 " (shift=%u)\n\n",
           ctx.tsc_mult, STOPWATCH_TSC_SHIFT);

    /* Subject stopwatch for the micro overhead test. */
    struct stopwatch sw_subject;
    stopwatch_start(&ctx, &sw_subject);

    g_ctx        = &ctx;
    g_sw_subject = &sw_subject;

    /* Part 1: pure timing overhead microbench. */
    run_micro_benchmark(&ctx,
        "stopwatch_read_ticks(&ctx, &sw_subject)",
        body_stopwatch_read_ticks);

    run_micro_benchmark(&ctx,
        "stopwatch_elapsed_ns(&ctx, &sw_subject)",
        body_stopwatch_elapsed_ns);

    run_micro_benchmark(&ctx,
        "clock_gettime(CLOCK_MONOTONIC, &ts)",
        body_clock_gettime);

    if (g_sink_u64 == 0xdeadbeefULL) {
        printf("sink: %" PRIu64 "\n", g_sink_u64);
    }

    run_micro_benchmark(&ctx, "stopwatch_realtime_ns(&ctx)", body_stopwatch_realtime);
    run_micro_benchmark(&ctx, "clock_gettime(CLOCK_REALTIME, &ts)", body_clock_realtime);

    return 0;
}
