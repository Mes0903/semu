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

/* For a given struct timespec 'ts' and freq, compute the total
 * emulated ticks = sec * freq + nsec * freq / 1e9.
 *
 * For example, if the frequency is set to 65,000,000, then there are 65,000,000
 * ticks per second. Respectively, if the time is set to 1 second, then there
 * are 65,000,000 ticks.
 */
static inline uint64_t get_ticks(struct timespec *ts, double freq)
{
    return ts->tv_sec * freq + mult_frac(ts->tv_nsec, freq, 1000000000ULL);
}

/* On POSIX => use clock_gettime().
 * On macOS => use mach_absolute_time().
 * Else => fallback to time(0) in seconds, convert to ns.
 *
 * Now, the POSIX/macOS logic can be clearly reused. Meanwhile, the fallback
 * path might just do a coarser approach with time(0).
 */
static inline uint64_t host_time_ns()
{
#if defined(HAVE_POSIX_TIMER)
    struct timespec ts;
    clock_gettime(CLOCKID, &ts);
    return (uint64_t) ts.tv_sec * 1000000000ULL + (uint64_t) ts.tv_nsec;

#elif defined(HAVE_MACH_TIMER)
    static mach_timebase_info_data_t ts = {0};
    if (ts.denom == 0)
        (void) mach_timebase_info(&ts);

    uint64_t now = mach_absolute_time();
    // convert to nanoseconds: (now * t.numer / t.denom)
    return mult_frac(now, (double) ts.numer, (uint64_t) ts.denom);

#else
    /* Minimal fallback: time(0) in seconds => convert to ns. */
    time_t now_sec = time(0);
    return (uint64_t) now_sec * 1000000000ULL;
#endif
}

/* Measure the overhead of a high-resolution timer call, typically
 * 'clock_gettime()' on POSIX or 'mach_absolute_time()' on macOS.
 *
 * 1) Times how long it takes to call 'host_time_ns()' repeatedly (target_loop).
 * 2) Derives an average overhead per call => ns_per_call.
 * 3) Because semu_timer_clocksource is ~10% of boot overhead, and called ~2e8
 *    times * SMP, we get predict_sec = ns_per_call * SMP * 2. Then set
 *    'scale_factor' so the entire boot completes in SEMU_BOOT_TARGET_TIME
 *    seconds.
 */
static void measure_bogomips_ns(uint64_t target_loop)
{
    /* Mark start time in ns */
    uint64_t start_ns = host_time_ns();

    /* Perform 'target_loop' times calling the host HRT. */
    for (uint64_t loops = 0; loops < target_loop; loops++)
        (void) host_time_ns();

    /* Mark end time in ns */
    uint64_t end_ns = host_time_ns();

    /* Calculate average overhead per call */
    double ns_per_call = (double) (end_ns - start_ns) / (double) target_loop;

    /* 'semu_timer_clocksource' is called ~2e8 times per SMP. Each call's
     * overhead ~ ns_per_call. The total overhead is ~ ns_per_call * SMP * 2e8.
     * That overhead is about 10% of the entire boot, so effectively:
     *   predict_sec = ns_per_call * SMP * 2
     * Then scale_factor = (desired_time) / (predict_sec).
     */
    double predict_sec = ns_per_call * SEMU_SMP * 2.0;
    scale_factor = SEMU_BOOT_TARGET_TIME / predict_sec;
}

/* The main function that returns the "emulated time" in ticks.
 *
 * Before the boot completes, we scale time by 'scale_factor' for a "fake
 * increments" approach. After boot completes, we switch to real time
 * with an offset bridging so that there's no big jump.
 */
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

#if defined(HAVE_POSIX_TIMER) || defined(HAVE_MACH_TIMER)
    uint64_t now_ns = host_time_ns();

    /* real_ticks => (now_ns * freq) / 1e9 */
    uint64_t real_ticks =
        mult_frac(now_ns, (double) timer->freq, 1000000000ULL);

    /* scaled_ticks => (now_ns * (freq*scale_factor)) / 1e9 */
    uint64_t scaled_ticks =
        mult_frac(now_ns, (double) (timer->freq * scale_factor), 1000000000ULL);

    if (!boot_complete)
        return scaled_ticks; /* Return scaled ticks in the boot phase. */

    /* The boot is done => switch to real freq with an offset bridging. */
    if (first_switch) {
        first_switch = false;
        offset = (int64_t) (real_ticks - scaled_ticks);
    }
    return (uint64_t) ((int64_t) real_ticks - offset);

#elif defined(HAVE_MACH_TIMER)
    /* Because we don't rely on sub-second calls to 'host_time_ns()' here,
     * we directly use time(0). This means the time resolution is coarse (1
     * second), but the logic is the same: we do a scaled approach pre-boot,
     * then real freq with an offset post-boot.
     */
    time_t now_sec = time(0);

    /* Before boot done, scale time. */
    if (!boot_complete)
        return (uint64_t) now_sec * (uint64_t) (timer->freq * scale_factor);

    if (first_switch) {
        first_switch = false;
        uint64_t real_val = (uint64_t) now_sec * (uint64_t) timer->freq;
        uint64_t scaled_val =
            (uint64_t) now_sec * (uint64_t) (timer->freq * scale_factor);
        offset = (int64_t) (real_val - scaled_val);
    }

    /* Return real freq minus offset. */
    uint64_t real_freq_val = (uint64_t) now_sec * (uint64_t) timer->freq;
    return real_freq_val - offset;
#endif
}

void semu_timer_init(semu_timer_t *timer, uint64_t freq)
{
    /* Measure how long each call to 'host_time_ns()' roughly takes,
     * then use that to pick 'scale_factor'. For example, pass freq
     * as the loop count or some large number to get a stable measure.
     */
    measure_bogomips_ns(freq);

    timer->freq = freq;
    semu_timer_rebase(timer, 0);
}

uint64_t semu_timer_get(semu_timer_t *timer)
{
    return semu_timer_clocksource(timer) - timer->begin;
}

void semu_timer_rebase(semu_timer_t *timer, uint64_t time)
{
    timer->begin = semu_timer_clocksource(timer) - time;
}
