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

#include <stdio.h>
#include <stdint.h>
#include <inttypes.h>
#include <time.h>
#include <string.h>
#include <unistd.h>   // sleep()
#include "stopwatch.h"

/* Use the same MONOTONIC clock as the stopwatch calibration. */
static inline clockid_t
test_monotonic_clock_id(void)
{
#ifdef CLOCK_MONOTONIC_RAW
    return CLOCK_MONOTONIC_RAW;
#else
    return CLOCK_MONOTONIC;
#endif
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

/* One measurement: spin until 'target_ns' has elapsed (by clock_gettime),
 * and record both the "real" elapsed ns and the stopwatch elapsed ns.
 */
static void
run_single_measure(const struct stopwatch_context *ctx,
                   clockid_t clk,
                   uint64_t target_ns,
                   uint64_t *out_real_ns,
                   uint64_t *out_sw_ns,
                   int64_t  *out_err_ns)
{
    struct stopwatch sw;
    struct timespec t_start, t_end;

    clock_gettime(clk, &t_start);
    stopwatch_start((struct stopwatch_context *)ctx, &sw);

    for (;;) {
        clock_gettime(clk, &t_end);
        uint64_t real_ns = timespec_diff_ns(&t_start, &t_end);
        if (real_ns >= target_ns) {
            uint64_t sw_ns = stopwatch_elapsed_ns(ctx, &sw);
            *out_real_ns = real_ns;
            *out_sw_ns   = sw_ns;
            *out_err_ns  = (int64_t)sw_ns - (int64_t)real_ns;
            return;
        }
    }
}

/* Choose iteration count for a target interval.
 * Shorter intervals -> more iterations; longer -> fewer (to keep runtime sane).
 */
static size_t
iterations_for_target(uint64_t target_ns)
{
    if (target_ns <= 1000ULL) {                 // <= 1 us
        return 5000;                            // 5000 iterations
    } else if (target_ns <= 100000ULL) {        // <= 100 us
        return 2000;
    } else if (target_ns <= 1000000ULL) {       // <= 1 ms
        return 1000;
    } else if (target_ns <= 10000000ULL) {      // <= 10 ms
        return 500;
    } else if (target_ns <= 100000000ULL) {     // <= 100 ms
        return 100;
    } else {                                    // 1 s
        return 10;                              // ~10 seconds total
    }
}

int
main(int argc, char **argv)
{
    struct stopwatch_context ctx;
    stopwatch_context_init(&ctx);

    printf("Stopwatch test\n");
    printf("  use_tsc = %d\n", ctx.use_tsc);
    if (ctx.use_tsc) {
        printf("  tsc_hz  = %" PRIu64 " Hz\n", ctx.tsc_hz);
        printf("  tsc_mult= %" PRIu64 " (fixed-point, shift=%u)\n",
               ctx.tsc_mult, STOPWATCH_TSC_SHIFT);
    }
    printf("\n");

    /* Targets:
     * - Fine: 100 ns, 200 ns, ..., 1000 ns
     * - Coarse (same as before, except 100 ns & 1 us are already covered):
     *     10 us, 100 us, 1 ms, 10 ms, 100 ms, 1 s
     */
    const uint64_t targets_ns[] = {
        100ULL, 200ULL, 300ULL, 400ULL, 500ULL,
        600ULL, 700ULL, 800ULL, 900ULL, 1000ULL,
        10000ULL,          // 10 us
        100000ULL,         // 100 us
        1000000ULL,        // 1 ms
        10000000ULL,       // 10 ms
        100000000ULL,      // 100 ms
        1000000000ULL      // 1 s
    };
    const size_t num_targets = sizeof(targets_ns) / sizeof(targets_ns[0]);

    clockid_t clk = test_monotonic_clock_id();

    printf("Target(ns), N, Mean_Real(ns), Mean_Stopwatch(ns), Mean_Error(ns), Mean_Error(%%)\n");

    for (size_t i = 0; i < num_targets; ++i) {
        uint64_t target = targets_ns[i];
        size_t   N      = iterations_for_target(target);
        size_t   warmup = N / 5;
        if (warmup < 10) warmup = 10;

        /* Per-target warmup: throw away some initial runs to heat caches. */
        for (size_t w = 0; w < warmup; ++w) {
            uint64_t r, s;
            int64_t  e;
            run_single_measure(&ctx, clk, target, &r, &s, &e);
        }

        long double sum_real = 0.0L;
        long double sum_sw   = 0.0L;
        long double sum_err  = 0.0L;

        for (size_t k = 0; k < N; ++k) {
            uint64_t real_ns, sw_ns;
            int64_t  err_ns;
            run_single_measure(&ctx, clk, target, &real_ns, &sw_ns, &err_ns);

            sum_real += (long double)real_ns;
            sum_sw   += (long double)sw_ns;
            sum_err  += (long double)err_ns;
        }

        long double mean_real = sum_real / (long double)N;
        long double mean_sw   = sum_sw   / (long double)N;
        long double mean_err  = sum_err  / (long double)N;
        long double mean_err_pct = (mean_err * 100.0L) / mean_real;

        printf("%" PRIu64 ", %zu, %.1Lf, %.1Lf, %.3Lf, %.6Lf\n",
               target, N, mean_real, mean_sw, mean_err, mean_err_pct);
    }

    return 0;
}
