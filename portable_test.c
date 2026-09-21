// SPDX-FileCopyrightText: 2026 Ben Jarvis
// SPDX-License-Identifier: MIT
#include <assert.h>
#include "stopwatch.h"
#ifdef NDEBUG
#error Tests require assertions
#endif
int main(void)
{
    struct stopwatch_context ctx;
    struct stopwatch sw;
    stopwatch_context_init(&ctx);
    struct timespec wall;
    stopwatch_realtime(&ctx, &wall);
    assert(wall.tv_sec > 0 && wall.tv_nsec >= 0 && wall.tv_nsec < 1000000000);
    uint64_t wall_before = stopwatch_realtime_ns(&ctx);
    stopwatch_start(&ctx, &sw);
#ifdef _WIN32
    Sleep(20);
#else
    struct timespec delay = {0, 20000000};
    nanosleep(&delay, NULL);
#endif
    assert(stopwatch_realtime_ns(&ctx) > wall_before);
    uint64_t ticks = stopwatch_read_ticks(&ctx, &sw);
    uint64_t ns = stopwatch_ticks_to_ns(&ctx, ticks);
    assert(ns >= UINT64_C(10000000));
    assert(stopwatch_elapsed_ns(&ctx, &sw) >= ns);
    assert(stopwatch_ticks_to_ns(&ctx, 0) == 0);
    assert(stopwatch_ns_to_ticks(&ctx, 0) == 0);
#ifdef _WIN32
    /* Conversion stays exact across durations that overflow ticks * 1e9. */
    assert(stopwatch_ticks_to_ns(&ctx, ctx.qpc_hz) == UINT64_C(1000000000));
    assert(stopwatch_ticks_to_ns(&ctx, ctx.qpc_hz * 86400) == UINT64_C(86400000000000));
    assert(stopwatch_ns_to_ticks(&ctx, UINT64_C(86400000000000)) == ctx.qpc_hz * 86400);
    assert(stopwatch_ns_to_ticks(&ctx, 1) >= 1);
#endif
    /* Cross the lazy wall-clock refresh interval on the real selected
     * clocksource, in addition to the injected TSC tests. */
    for (int i = 0; i < 12; i++) {
#ifdef _WIN32
        Sleep(100);
#else
        struct timespec pause = {0, 100000000};
        nanosleep(&pause, NULL);
#endif
        uint64_t wall_now = stopwatch_realtime_ns(&ctx);
        assert(wall_now >= wall_before);
        wall_before = wall_now;
    }
    return 0;
}
