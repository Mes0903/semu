#include <time.h>

#include "utils.h"

#if defined(__APPLE__)
#define HAVE_MACH_TIMER
#include <mach/mach_time.h>
#elif !defined(_WIN32) && !defined(_WIN64)
#define HAVE_POSIX_TIMER

/*
 * Use a faster but less precise clock source because we need quick
 * timestamps rather than fine-grained precision.
 */
#ifdef CLOCK_MONOTONIC_COARSE
#define CLOCKID CLOCK_MONOTONIC_COARSE
#else
#define CLOCKID CLOCK_REALTIME_COARSE
#endif
#endif

#ifndef SEMU_SMP
#define SEMU_SMP 1
#endif

#ifndef SEMU_BOOT_TARGET_TIME
#define SEMU_BOOT_TARGET_TIME 10
#endif

bool boot_complete = false;
static double scale_factor;

/* Calculate "x * n / d" without unnecessary overflow or loss of precision.
 *
 * Reference:
 * https://elixir.bootlin.com/linux/v6.10.7/source/include/linux/math.h#L121
 */
static inline uint64_t mult_frac(uint64_t x, double n, uint64_t d)
{
    const uint64_t q = x / d;
    const uint64_t r = x % d;

    return q * n + r * n / d;
}

/* Use timespec and frequency to calculate how many ticks to increment. For
 * example, if the frequency is set to 65,000,000, then there are 65,000,000
 * ticks per second. Respectively, if the time is set to 1 second, then there
 * are 65,000,000 ticks.
 *
 * Thus, by seconds * frequency + nanoseconds * frequency / 1,000,000,000, we
 * can get the number of ticks.
 */
static inline uint64_t get_ticks(struct timespec *ts, double freq)
{
    return ts->tv_sec * freq + mult_frac(ts->tv_nsec, freq, 1000000000ULL);
}

/* Measure how long it takes for the high resolution timer to update once, to
 * scale real time in order to set the emulator time.
 */
static void measure_bogomips_ns(uint64_t target_loop)
{
    struct timespec start, end;
    clock_gettime(CLOCKID, &start);

    for (uint64_t loops = 0; loops < target_loop; loops++)
        clock_gettime(CLOCKID, &end);

    int64_t sec_diff = end.tv_sec - start.tv_sec;
    int64_t nsec_diff = end.tv_nsec - start.tv_nsec;
    double ns_per_call = (sec_diff * 1e9 + nsec_diff) / target_loop;

    /* Based on simple statistics, 'semu_timer_clocksource' accounts for
     * approximately 10% of the boot process execution time. Since the logic
     * inside 'semu_timer_clocksource' is relatively simple, it can be assumed
     * that its execution time is roughly equivalent to that of a
     * 'clock_gettime' call.
     *
     * Similarly, based on statistics, 'semu_timer_clocksource' is called
     * approximately 2*1e8 times. Therefore, we can roughly estimate that the
     * boot process will take '(ns_per_call/1e9) * SEMU_SMP * 2 * 1e8 *
     * (100%/10%)' seconds.
     */
    double predict_sec = ns_per_call * SEMU_SMP * 2;
    scale_factor = SEMU_BOOT_TARGET_TIME / predict_sec;
}

void semu_timer_init(semu_timer_t *timer, uint64_t freq)
{
    measure_bogomips_ns(freq); /* Measure the time taken by 'clock_gettime' */

    timer->freq = freq;
    semu_timer_rebase(timer, 0);
}

static uint64_t semu_timer_clocksource(semu_timer_t *timer)
{
    /* After boot process complete, the timer will switch to real time. Thus,
     * there is an offset between the real time and the emulator time.
     *
     * After switching to real time, the correct way to update time is to
     * calculate the increment of time. Then add it to the emulator time.
     */
    static int64_t offset = 0;
    static bool first_switch = true;

#if defined(HAVE_POSIX_TIMER)
    struct timespec emulator_time;
    clock_gettime(CLOCKID, &emulator_time);

    if (!boot_complete) {
        return get_ticks(&emulator_time, timer->freq * scale_factor);
    } else {
        if (first_switch) {
            first_switch = false;
            uint64_t real_ticks = get_ticks(&emulator_time, timer->freq);
            uint64_t scaled_ticks =
                get_ticks(&emulator_time, timer->freq * scale_factor);

            offset = (int64_t) (real_ticks - scaled_ticks);
        }

        uint64_t real_freq_ticks = get_ticks(&emulator_time, timer->freq);
        return real_freq_ticks - offset;
    }
#elif defined(HAVE_MACH_TIMER)
    static mach_timebase_info_data_t emulator_time;
    if (emulator_time.denom == 0)
        (void) mach_timebase_info(&emulator_time);

    uint64_t now = mach_absolute_time();
    uint64_t ns = mult_frac(now, emulator_time.numer, emulator_time.denom);
    if (!boot_complete) {
        return mult_frac(ns, (uint64_t) (timer->freq * scale_factor),
                         1000000000ULL);
    } else {
        if (first_switch) {
            first_switch = false;
            uint64_t real_ticks = mult_frac(ns, timer->freq, 1000000000ULL);
            uint64_t scaled_ticks = mult_frac(
                ns, (uint64_t) (timer->freq * scale_factor), 1000000000ULL);
            offset = (int64_t) (real_ticks - scaled_ticks);
        }

        uint64_t real_freq_ticks = mult_frac(ns, timer->freq, 1000000000ULL);
        return real_freq_ticks - offset;
    }
#else
    time_t now_sec = time(0);

    if (!boot_complete) {
        return ((uint64_t) now_sec) * (uint64_t) (timer->freq * scale_factor);
    } else {
        if (first_switch) {
            first_switch = false;
            uint64_t real_val = ((uint64_t) now_sec) * (uint64_t) (timer->freq);
            uint64_t scaled_val =
                ((uint64_t) now_sec) * (uint64_t) (timer->freq * scale_factor);
            offset = (int64_t) real_val - (int64_t) scaled_val;
        }

        uint64_t real_freq_val =
            ((uint64_t) now_sec) * (uint64_t) (timer->freq);
        return real_freq_val - offset;
    }
#endif
}

uint64_t semu_timer_get(semu_timer_t *timer)
{
    return semu_timer_clocksource(timer) - timer->begin;
}

void semu_timer_rebase(semu_timer_t *timer, uint64_t time)
{
    timer->begin = semu_timer_clocksource(timer) - time;
}
