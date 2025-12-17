# stopwatch

C framework for efficient high frequency time measurement using TSC

Applications often need to measure elapsed time during processing.  For example, for performance measurements.  For the most part, on linux, applications can use clock_gettime(CLOCK_MONOTONIC[_RAW], ...) to capture timestamps, then calculate the difference between those timestamps to arrive at elapsed time.   This is generally quite efficient, since modern kernels use vDSO to avoid a system call.

However, in very high performance systems where many millions of requests per second are being completed, the cost of clock_gettime() can still be significant.  On modern CPUs, it is often possible to reliably calculate elapsed time using the time stamp counter (TSC) register directly instead.   This allows calculating elapsed time directly in terms of clock cycles, rather than nanoseconds.   Knowing the CPU clock frequency, we can then translate the clock cycles into nanoseconds at our leisure, possibly out of the hot path.   For example if producing a histogram of request processing times, we might maintain the histogram in units of clock cycles and only translate it to nanoseconds when we output the final results.

This is the functionality that stopwatch provides.   It automatically detects if TSC can be safely used, and if not it falls back to using clock_gettime() internally.

Consider carefully if using stopwatch is appropriate for your use case.  This is very much a micro-optimization which may or may not be beneficial depending on the situation.

## Building

The stopwatch implementation is self-contained in a single header, stopwatch.h.  Simply add it to your project and #include it.

## Initializing

Place a stopwatch_context somewhere accessible from all the places you want to measure time, and initialize it once before making any other calls:

```c

struct stopwatch_context StopwatchCtx;

stopwatch_context_init(&StopwatchCtx);.
```

The context may be safely used from multiple threads.    Note that on systems which do not provide the clock frequency via CPU ID, this function will block for a period of time to measure the clock frequency.

## Measuring time

Place a stopwatch somewhere that will exist for the duration of the time measurement, such as on the stack or as a member of a request structure:

```c
struct stopwatch sw;
```

To start the stopwatch:

```c
stopwatch_start(&StopwatchCtx, &sw);
```

To read the elapsed cycles since the stopwatch was started:

```c
uint64_t cycles = stopwatch_read_ticks(&StopwatchCtx, &sw);
```

A stopwatch may be read any number of times after being started and may be restarted by calling stopwatch_start() again.

To convert a cycle count into elapsed nanoseconds:

```c
uint64_t ns = stopwatch_ticks_to_ns(&StopwatchCtx, cycles);
```

To convert nanoseconds into cycles:

```c
uint64_t cycles = stopwatch_ns_to_ticks(&StopwatchCtx, ns);
```

Converting nanoseconds to cycles may be useful to implement functionality such as timers.   Calculate the cycle count at which the timer will have expired, then repeatedly check if that many cycles have passed without the need to convert between time and cycles repeatedly.

For convenience, there is a wrapper to read a stopwatch directly into nanoseconds:

```c
uint64_t ns = stopwatch_elapsed_ns(&StopwatchCtx, &sw);
```

## Correctness

There is an included correctness test that measures the accuracy of the stopwatch against clock_gettime():

```sh

# gcc -O3 -o test test.c
# ./test
Stopwatch test
  use_tsc = 1
  tsc_hz  = 2246621132 Hz
  tsc_mult= 1911745258 (fixed-point, shift=32)

Target(ns), N, Mean_Real(ns), Mean_Stopwatch(ns), Mean_Error(ns), Mean_Error(%)
100, 5000, 109.2, 109.1, -0.120, -0.109848
200, 5000, 214.0, 214.1, 0.113, 0.052898
300, 5000, 306.5, 306.0, -0.499, -0.162960
400, 5000, 416.4, 417.0, 0.587, 0.140929
500, 5000, 514.0, 513.2, -0.779, -0.151588
600, 5000, 605.8, 605.2, -0.664, -0.109632
700, 5000, 717.0, 724.3, 7.298, 1.017866
800, 5000, 813.0, 812.4, -0.635, -0.078156
900, 5000, 906.1, 905.8, -0.381, -0.042025
1000, 5000, 1014.3, 1015.0, 0.706, 0.069625
10000, 2000, 10019.7, 10031.6, 11.958, 0.119350
100000, 2000, 100008.1, 100019.8, 11.712, 0.011711
1000000, 1000, 1000008.1, 1000020.0, 11.902, 0.001190
10000000, 500, 10000010.2, 10000022.3, 12.092, 0.000121
100000000, 100, 100000011.5, 100000028.1, 16.640, 0.000017
1000000000, 10, 1000000006.5, 1000000057.8, 51.300, 0.000005
```

## Performance

There is also a performance test showing the difference in cost between the stopwatch and clock_gettime().

Your mileage may vary considerably depending on your CPU, compiler, kernel version, etc.

```sh
# gcc -O3 -o bench bench.c
# ./bench
Benchmarking stopwatch vs clock_gettime
  tsc_hz   = 2246617521 Hz
  tsc_mult = 1911748331 (shift=32)

== stopwatch_read_ticks(&ctx, &sw_subject) ==
  iterations: 105293560
  total time: 1000.943 ms
  avg time : 9.506 ns/call
  rate     : 105.194 M calls/s

== stopwatch_elapsed_ns(&ctx, &sw_subject) ==
  iterations: 104755864
  total time: 997.141 ms
  avg time : 9.519 ns/call
  rate     : 105.056 M calls/s

== clock_gettime(CLOCK_MONOTONIC, &ts) ==
  iterations: 43327821
  total time: 1000.615 ms
  avg time : 23.094 ns/call
  rate     : 43.301 M calls/s
```

