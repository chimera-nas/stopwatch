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
    stopwatch_start(&ctx, &sw);
#ifdef _WIN32
    Sleep(20);
#else
    struct timespec delay = {0, 20000000};
    nanosleep(&delay, NULL);
#endif
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
    return 0;
}
