// SPDX-FileCopyrightText: 2026 Ben Jarvis
// SPDX-License-Identifier: MIT
#include <assert.h>
#include <stdint.h>
#include <time.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <stddef.h>

static uint64_t test_ticks(void);
static int test_gettime(clockid_t id, struct timespec *ts);
#define STOPWATCH_WALL_TICKS() test_ticks()
#define clock_gettime test_gettime
#include "stopwatch.h"
#undef clock_gettime

#define SECOND UINT64_C(1000000000)
#define LIMIT ((int64_t)STOPWATCH_WALL_MAX_SLEW_PPM * 1000)
#define EPOCH UINT64_C(1700000000000000000)
static uint64_t ticks, increment, delay;
static int64_t offset;
static unsigned int samples;

static uint64_t test_ticks(void)
{
    return __atomic_fetch_add(&ticks, increment, __ATOMIC_RELAXED);
}

static int test_gettime(clockid_t id, struct timespec *ts)
{
    if (id != CLOCK_REALTIME) return clock_gettime(id, ts);
    __atomic_add_fetch(&samples, 1, __ATOMIC_RELAXED);
    uint64_t now = __atomic_add_fetch(&ticks, delay, __ATOMIC_RELAXED);
    uint64_t ns = EPOCH + now + __atomic_load_n(&offset, __ATOMIC_RELAXED);
    ts->tv_sec = (time_t)(ns / SECOND);
    ts->tv_nsec = (long)(ns % SECOND);
    return 0;
}

/* One fake cycle is one nanosecond. Exercise the actual public wall reader,
 * including sampling and publication, on every Unix architecture. */
static void init(struct stopwatch_context *ctx)
{
    memset(ctx, 0, sizeof(*ctx));
    ctx->use_tsc = 1;
    ctx->tsc_mult = 1;
    ctx->tsc_hz = SECOND;
    ctx->refine_done = 1;
    ctx->wall.mult = 1;
    ctx->wall.base_ns = EPOCH;
    ctx->wall.last_ns = EPOCH;
    ticks = increment = delay = 0;
    offset = 0;
    samples = 0;
}

static void test_slew(int direction)
{
    struct stopwatch_context ctx;
    init(&ctx);
    struct stopwatch_context original = ctx;
    offset = direction * LIMIT / 2; /* Within one refresh interval's slew budget. */
    ticks = SECOND;
    assert(stopwatch_realtime_ns(&ctx) == EPOCH + SECOND); /* No step. */
    assert(samples == 3);
    ticks += SECOND / 2;
    uint64_t half = EPOCH + ticks + direction * LIMIT / 4;
    assert(stopwatch_realtime_ns(&ctx) == half);
    assert(samples == 3); /* No clock sampling on the hot path. */
    ticks = 2 * SECOND;
    assert(stopwatch_realtime_ns(&ctx) == EPOCH + ticks + offset);
    ticks = 3 * SECOND;
    assert(stopwatch_realtime_ns(&ctx) == EPOCH + ticks + offset);
    assert(memcmp(&ctx.use_tsc, &original.use_tsc,
                  sizeof(ctx) - offsetof(struct stopwatch_context, use_tsc)) == 0);
}

static void test_large_steps_and_idle(void)
{
    for (int sign = -1; sign <= 1; sign += 2) {
        struct stopwatch_context ctx;
        init(&ctx);
        offset = sign * 50 * LIMIT;
        uint64_t previous = EPOCH;
        for (int i = 1; i <= 60; i++) {
            ticks = (uint64_t)i * SECOND;
            uint64_t now = stopwatch_realtime_ns(&ctx);
            assert(now >= previous);
            assert(now - previous >= SECOND - LIMIT);
            assert(now - previous <= SECOND + LIMIT);
            previous = now;
        }
        assert(previous == EPOCH + ticks + offset);
    }
    struct stopwatch_context ctx;
    init(&ctx);
    offset = -LIMIT / 2;
    ticks = SECOND;
    stopwatch_realtime_ns(&ctx);
    ticks = 100 * SECOND; /* Correction stops when the debt is paid. */
    assert(stopwatch_realtime_ns(&ctx) == EPOCH + ticks + offset);
}

static void test_preemption_and_skew(void)
{
    struct stopwatch_context ctx;
    init(&ctx);
    ticks = SECOND;
    delay = SECOND / 10; /* Every sample gets descheduled for 100 ms. */
    uint64_t now = stopwatch_realtime_ns(&ctx);
    assert(ctx.wall.adjustment_ns == 0); /* Reject all the wide pairs. */
    assert(now == EPOCH + ticks);
    assert(samples == 3);
    delay = 0;
    ticks += SECOND;
    now = stopwatch_realtime_ns(&ctx);
    assert(now == EPOCH + ticks);
    ticks -= 100; /* Slightly skewed core; no backwards result. */
    assert(stopwatch_realtime_ns(&ctx) == now);
    ticks += 200;
    assert(stopwatch_realtime_ns(&ctx) == EPOCH + ticks);
}

static void test_fallback(void)
{
    struct stopwatch_context ctx;
    init(&ctx);
    ctx.use_tsc = 0;
    ticks = SECOND;
    assert(stopwatch_realtime_ns(&ctx) == EPOCH + ticks);
    offset = -(int64_t)(SECOND / 2);
    struct timespec ts;
    stopwatch_realtime(&ctx, &ts);
    assert((uint64_t)ts.tv_sec * SECOND + ts.tv_nsec == EPOCH + ticks + offset);
    assert(samples == 2); /* Exactly one CLOCK_REALTIME read per call. */
}

static void *reader(void *arg)
{
    struct stopwatch_context *ctx = arg;
    uint64_t previous = 0;
    for (int i = 0; i < 50000; i++) {
        uint64_t now = stopwatch_realtime_ns(ctx);
        assert(now >= previous);
        previous = now;
        /* Force both clock-step directions while other readers refresh. */
        if (i % 1000 == 0) {
            __atomic_store_n(&offset, (i % 2000 ? 1 : -1) * (int64_t)SECOND,
                             __ATOMIC_RELAXED);
        }
    }
    return NULL;
}

static void test_concurrency(void)
{
    struct stopwatch_context ctx;
    pthread_t threads[4];
    init(&ctx);
    increment = 10000; /* Run through refresh intervals without sleeping. */
    for (int i = 0; i < 4; i++) assert(pthread_create(&threads[i], NULL, reader, &ctx) == 0);
    for (int i = 0; i < 4; i++) assert(pthread_join(threads[i], NULL) == 0);
    assert(samples >= 3);
    assert(ctx.tsc_mult == 1 && ctx.tsc_hz == SECOND && ctx.refine_done == 1);
}

int main(void)
{
    test_slew(1);
    test_slew(-1);
    test_large_steps_and_idle();
    test_preemption_and_skew();
    test_fallback();
    test_concurrency();
    puts("wall clock: slew, convergence, preemption, fallback, and concurrency passed");
    return 0;
}
