#include <math.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
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

_Atomic bool boot_complete = false;
static pthread_mutex_t timer_state_lock = PTHREAD_MUTEX_INITIALIZER;
static uint64_t ticks_increment_fp;
static _Atomic uint64_t boot_elapsed_fp;
static _Atomic uint64_t boot_elapsed_ticks;
static uint64_t boot_real_start_ns;

#define SEMU_TIMER_BOOT_COEFF 1.744e8
#define SEMU_TIMER_FP_SHIFT 32
#define SEMU_TIMER_FP_ONE (1ULL << SEMU_TIMER_FP_SHIFT)

/* Timer calibration statistics */
static _Atomic uint64_t timer_call_count = 0;
static int timer_n_harts = 1;
static _Atomic bool timer_switched_to_real_time = false;
static _Atomic int64_t timer_offset = 0;

/* Calculate "x * n / d" without unnecessary overflow or loss of precision.
 *
 * Reference:
 * https://elixir.bootlin.com/linux/v6.10.7/source/include/linux/math.h#L121
 */
static inline uint64_t mult_frac(uint64_t x, uint64_t n, uint64_t d)
{
    const uint64_t q = x / d;
    const uint64_t r = x % d;

    return q * n + r * n / d;
}

static uint64_t atomic_fetch_max_u64(_Atomic uint64_t *target, uint64_t value)
{
    uint64_t old = atomic_load_explicit(target, memory_order_relaxed);

    while (old < value &&
           !atomic_compare_exchange_weak_explicit(
               target, &old, value, memory_order_relaxed, memory_order_relaxed))
        ;

    return old < value ? value : old;
}

/* High-precision time measurement:
 * - POSIX systems: clock_gettime() for nanosecond precision
 * - macOS: mach_absolute_time() with timebase conversion
 * - Other platforms: time(0) with conversion to nanoseconds as fallback
 *
 * The platform-specific timing logic is now clearly separated: POSIX and macOS
 * implementations provide high-precision measurements, while the fallback path
 * uses time(0) for a coarser but portable approach.
 */
static inline uint64_t host_time_ns()
{
#if defined(HAVE_POSIX_TIMER)
    struct timespec ts;
    clock_gettime(CLOCKID, &ts);
    return (uint64_t) ts.tv_sec * 1e9 + (uint64_t) ts.tv_nsec;

#elif defined(HAVE_MACH_TIMER)
    static mach_timebase_info_data_t ts = {0};
    if (ts.denom == 0)
        (void) mach_timebase_info(&ts);

    uint64_t now = mach_absolute_time();
    /* convert to nanoseconds: (now * t.numer / t.denom) */
    return mult_frac(now, ts.numer, (uint64_t) ts.denom);

#else
    /* Fallback to non-HRT calls time(0) in seconds => convert to ns. */
    time_t now_sec = time(0);
    return (uint64_t) now_sec * 1e9;
#endif
}

/* The function that returns the "emulator time" in ticks.
 *
 * Before the boot process is completed, the emulator manually manages the
 * growth of ticks to suppress RCU CPU stall warnings. After the boot process is
 * completed, the emulator switches back to the real-time timer, using an offset
 * bridging to ensure that the ticks of both timers remain consistent.
 */
static uint64_t semu_timer_clocksource(semu_timer_t *timer)
{
    uint64_t now_ns = host_time_ns();

    if (!semu_boot_complete_load()) {
        atomic_fetch_add_explicit(&timer_call_count, 1, memory_order_relaxed);
        uint64_t elapsed_fp =
            atomic_fetch_add_explicit(&boot_elapsed_fp, ticks_increment_fp,
                                      memory_order_relaxed) +
            ticks_increment_fp;
        uint64_t call_driven_ticks = elapsed_fp >> SEMU_TIMER_FP_SHIFT;
        uint64_t real_elapsed_ticks = 0;

        if (now_ns > boot_real_start_ns)
            real_elapsed_ticks =
                mult_frac(now_ns - boot_real_start_ns, timer->freq, 1e9);

        uint64_t elapsed_ticks = call_driven_ticks > real_elapsed_ticks
                                     ? call_driven_ticks
                                     : real_elapsed_ticks;

        return timer->begin +
               atomic_fetch_max_u64(&boot_elapsed_ticks, elapsed_ticks);
    }

    uint64_t real_ticks = mult_frac(now_ns, timer->freq, 1e9);
    if (!atomic_load_explicit(&timer_switched_to_real_time,
                              memory_order_acquire)) {
        pthread_mutex_lock(&timer_state_lock);
        if (!atomic_load_explicit(&timer_switched_to_real_time,
                                  memory_order_relaxed)) {
            uint64_t boot_ticks =
                timer->begin +
                atomic_load_explicit(&boot_elapsed_ticks, memory_order_relaxed);
            int64_t offset = (int64_t) (real_ticks - boot_ticks);
            atomic_store_explicit(&timer_offset, offset, memory_order_release);
            atomic_store_explicit(&timer_switched_to_real_time, true,
                                  memory_order_release);

#ifdef SEMU_TIMER_STATS
            /* Output timer calibration statistics only when SEMU_TIMER_STATS is
             * defined.
             */
            uint64_t call_count =
                atomic_load_explicit(&timer_call_count, memory_order_relaxed);
            double actual_coefficient = (double) call_count / timer_n_harts;
            double current_coefficient = SEMU_TIMER_BOOT_COEFF;
            double recommended_coefficient = actual_coefficient;

            fprintf(stderr, "\n[Timer Calibration Statistics]\n");
            fprintf(stderr, "  Boot completed after %llu timer calls\n",
                    (unsigned long long) call_count);
            fprintf(stderr, "  Number of harts: %d\n", timer_n_harts);
            fprintf(stderr,
                    "  Actual coefficient: %.3e (%.2f calls per hart)\n",
                    actual_coefficient, actual_coefficient);
            fprintf(stderr, "  Current coefficient: %.3e\n",
                    current_coefficient);
            fprintf(stderr, "  Difference: %.2f%% %s\n",
                    fabs(actual_coefficient - current_coefficient) /
                        current_coefficient * 100.0,
                    actual_coefficient > current_coefficient ? "(more calls)"
                                                             : "(fewer calls)");
            fprintf(stderr, "\n[Recommendation]\n");
            fprintf(stderr, "  Update utils.c line 121 to:\n");
            fprintf(
                stderr,
                "  ticks_increment = (SEMU_BOOT_TARGET_TIME * CLOCK_FREQ) / "
                "(%.3e * n_harts);\n",
                recommended_coefficient);
            fprintf(stderr, "\n");
#endif
        }
        pthread_mutex_unlock(&timer_state_lock);
    }

    int64_t offset = atomic_load_explicit(&timer_offset, memory_order_acquire);
    return (uint64_t) ((int64_t) real_ticks - offset);
}

void semu_timer_init(semu_timer_t *timer, uint64_t freq, int n_harts)
{
    pthread_mutex_lock(&timer_state_lock);

    semu_boot_complete_store(false);
    atomic_store_explicit(&timer_switched_to_real_time, false,
                          memory_order_release);
    atomic_store_explicit(&timer_offset, 0, memory_order_release);

    timer->freq = freq;
    boot_real_start_ns = host_time_ns();
    timer->begin = mult_frac(boot_real_start_ns, timer->freq, 1e9);
    atomic_store_explicit(&boot_elapsed_fp, 0, memory_order_release);
    atomic_store_explicit(&boot_elapsed_ticks, 0, memory_order_release);
    atomic_store_explicit(&timer_call_count, 0, memory_order_release);

    /* Store n_harts for calibration statistics */
    timer_n_harts = n_harts;

    /* According to statistics, the number of times semu_timer_clocksource
     * called is approximately SMP count * 1.744 * 1e8. By the time the boot
     * process is completed, the emulator will have a total of boot seconds *
     * frequency ticks. Therefore, each time, boot seconds * frequency /
     * (1.744 * 1e8 * SMP count) ticks need to be added.
     *
     * Note: This coefficient was recalibrated after MMU cache optimization
     * (8x2 set-associative with 99%+ hit rate). The original coefficient
     * (2.15 * 1e8) was based on measurements before the optimization. With
     * faster CPU execution, fewer timer calls are needed to complete boot.
     *
     * Calibration history:
     * - Original (pre-MMU cache): 2.15 x 10^8
     * - After MMU cache (measured): 1.696 x 10^8 (-21.1%)
     * - Verification measurement: 1.744 x 10^8 (error: 2.85%)
     * - Final coefficient: 1.744 x 10^8 (based on verification)
     */
    ticks_increment_fp =
        (uint64_t) (((double) SEMU_BOOT_TARGET_TIME * timer->freq *
                     (double) SEMU_TIMER_FP_ONE) /
                    (SEMU_TIMER_BOOT_COEFF * n_harts));

    pthread_mutex_unlock(&timer_state_lock);
}

uint64_t semu_timer_get(semu_timer_t *timer)
{
    return semu_timer_clocksource(timer) - timer->begin;
}

void semu_timer_rebase(semu_timer_t *timer, uint64_t time)
{
    timer->begin = semu_timer_clocksource(timer) - time;
}
