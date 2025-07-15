/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

WT_PROCESS __wt_process;             /* Per-process structure */
static int __wt_pthread_once_failed; /* If initialization failed */

/*
 * This is the list of the timing stress configuration names and flags. It is a global structure
 * instead of declared in the config function so that other functions can use the name/flag
 * association.
 */
const WT_NAME_FLAG __wt_stress_types[] = {
  /*
   * Each split race delay is controlled using a different flag to allow more effective race
   * condition detection, since enabling all delays at once can lead to an overall slowdown to the
   * point where race conditions aren't encountered.
   *
   * Fail points are also defined in this list and will occur randomly when enabled.
   */
  {"aggressive_stash_free", WT_TIMING_STRESS_AGGRESSIVE_STASH_FREE},
  {"aggressive_sweep", WT_TIMING_STRESS_AGGRESSIVE_SWEEP},
  {"backup_rename", WT_TIMING_STRESS_BACKUP_RENAME},
  {"checkpoint_evict_page", WT_TIMING_STRESS_CHECKPOINT_EVICT_PAGE},
  {"checkpoint_handle", WT_TIMING_STRESS_CHECKPOINT_HANDLE},
  {"checkpoint_slow", WT_TIMING_STRESS_CHECKPOINT_SLOW},
  {"checkpoint_stop", WT_TIMING_STRESS_CHECKPOINT_STOP},
  {"commit_transaction_slow", WT_TIMING_STRESS_COMMIT_TRANSACTION_SLOW},
  {"compact_slow", WT_TIMING_STRESS_COMPACT_SLOW},
  {"conn_close_stress_log_printf", WT_TIMING_STRESS_CLOSE_STRESS_LOG},
  {"evict_reposition", WT_TIMING_STRESS_EVICT_REPOSITION},
  {"failpoint_eviction_split", WT_TIMING_STRESS_FAILPOINT_EVICTION_SPLIT},
  {"failpoint_history_delete_key_from_ts",
    WT_TIMING_STRESS_FAILPOINT_HISTORY_STORE_DELETE_KEY_FROM_TS},
  {"history_store_checkpoint_delay", WT_TIMING_STRESS_HS_CHECKPOINT_DELAY},
  {"history_store_search", WT_TIMING_STRESS_HS_SEARCH},
  {"history_store_sweep_race", WT_TIMING_STRESS_HS_SWEEP},
  {"live_restore_clean_up", WT_TIMING_STRESS_LIVE_RESTORE_CLEAN_UP},
  {"open_index_slow", WT_TIMING_STRESS_OPEN_INDEX_SLOW},
  {"prefetch_1", WT_TIMING_STRESS_PREFETCH_1}, {"prefetch_2", WT_TIMING_STRESS_PREFETCH_2},
  {"prefetch_3", WT_TIMING_STRESS_PREFETCH_3}, {"prefix_compare", WT_TIMING_STRESS_PREFIX_COMPARE},
  {"prepare_checkpoint_delay", WT_TIMING_STRESS_PREPARE_CHECKPOINT_DELAY},
  {"prepare_resolution_1", WT_TIMING_STRESS_PREPARE_RESOLUTION_1},
  {"prepare_resolution_2", WT_TIMING_STRESS_PREPARE_RESOLUTION_2},
  {"session_alter_slow", WT_TIMING_STRESS_SESSION_ALTER_SLOW},
  {"sleep_before_read_overflow_onpage", WT_TIMING_STRESS_SLEEP_BEFORE_READ_OVERFLOW_ONPAGE},
  {"split_1", WT_TIMING_STRESS_SPLIT_1}, {"split_2", WT_TIMING_STRESS_SPLIT_2},
  {"split_3", WT_TIMING_STRESS_SPLIT_3}, {"split_4", WT_TIMING_STRESS_SPLIT_4},
  {"split_5", WT_TIMING_STRESS_SPLIT_5}, {"split_6", WT_TIMING_STRESS_SPLIT_6},
  {"split_7", WT_TIMING_STRESS_SPLIT_7}, {"split_8", WT_TIMING_STRESS_SPLIT_8},
  {"tiered_flush_finish", WT_TIMING_STRESS_TIERED_FLUSH_FINISH}, {NULL, 0}};

/*
 * __endian_check --
 *     Check the build matches the machine.
 */
static int
__endian_check(void)
{
    uint64_t v;
    const char *e;
    bool big;

    v = 1;
    big = *((uint8_t *)&v) == 0;

#ifdef WORDS_BIGENDIAN
    if (big)
        return (0);
    e = "big-endian";
#else
    if (!big)
        return (0);
    e = "little-endian";
#endif
    fprintf(stderr,
      "This is a %s build of the WiredTiger data engine, incompatible with this system\n", e);
    return (EINVAL);
}

#if defined(__amd64) || defined(__aarch64__)

/*
 * __reset_thread_tick --
 *     Reset the OS thread timeslice to raise the probability of uninterrupted run afterwards.
 */
static void
__reset_thread_tick(void)
{
    /*!!!
     * The "OS scheduler timeslice" refers to the fixed amount of CPU time allocated to a process
     * or thread before the operating system's scheduler may preempt it to allow another process
     * to run.
     *
     * A timeslice typically ranges from a few milliseconds to tens of milliseconds, depending on
     * the operating system and its configuration.
     *
     * When the timeslice for a process expires, the scheduler performs a context switch, switching
     * to another ready-to-run process. A context switch can also occur for other reasons like
     * CPU interrupts before the timeslice expires.
     *
     * When a thread starts execution, it receives a fresh timeslice, increasing the likelihood of
     * running uninterrupted for its duration.
     *
     * Potential ways to reset the timeslice are:
     *
     * - yield()
     *   - What it does:
     *     - yield() explicitly tells the operating system's scheduler that the current thread
     *       is willing to relinquish the CPU before its timeslice has expired.
     *     - The thread is moved from the "running" state to the "ready" state, allowing other
     *       threads of equal or higher priority to execute.
     *     - If no other threads are ready to run, the scheduler may reschedule the same thread,
     *       allowing it to continue with its remaining timeslice.
     *   - Impact on timeslice:
     *     - The remaining portion of the thread's current timeslice is usually preserved.
     *       When the thread is rescheduled, it continues with the leftover time.
     *     - In some schedulers (e.g., Linux's Completely Fair Scheduler), yielding might
     *       slightly adjust the thread's virtual runtime, but it doesn't fully reset or
     *       replenish the timeslice.
     * - sleep()
     *   - What it does:
     *     - sleep makes a thread pause execution for a specified duration and transitions it into
     *       a "waiting state".
     *   - Impact on timeslice:
     *     - The thread's timeslice is typically discarded when it enters the sleep state.
     *       Upon waking up, the thread competes with other threads for CPU time as if it were
     *       newly scheduled, with a fresh timeslice being assigned according to the
     *       scheduling algorithm.
     *
     * This behavior is quite consistent across operating systems like Linux, MacOS, and Windows:
     *
     * - yield() does *not reset* the timeslice; it merely pauses execution voluntarily and
     *   retains the remaining time.
     * - sleep() discards the current timeslice, and the thread starts fresh after waking up.
     */
    __wt_sleep(0, 10);
}

/*
 * __get_epoch_and_tsc --
 *     Get the current time and TSC ticks before and after the call to __wt_epoch. Returns the
 *     duration of the call in TSC ticks.
 */
static uint64_t
__get_epoch_and_tsc(struct timespec *clock1, uint64_t *tsc1, uint64_t *tsc2)
{
    *tsc1 = __wt_rdtsc();
    __wt_epoch(NULL, clock1);
    *tsc2 = __wt_rdtsc();
    return (*tsc2 - *tsc1);
}

/*
 * __compare_uint64 --
 *     uint64_t comparison function.
 */
static int
__compare_uint64(const void *a, const void *b)
{
    return (int)(*(uint64_t *)a - *(uint64_t *)b);
}

/*
 * __get_epoch_call_ticks --
 *     Returns how many ticks it takes to call __wt_epoch at best and on average.
 */
static void
__get_epoch_call_ticks(uint64_t *epoch_ticks_min, uint64_t *epoch_ticks_avg)
{
#define EPOCH_CALL_CALIBRATE_SAMPLES 50
    uint64_t duration[EPOCH_CALL_CALIBRATE_SAMPLES];

    __reset_thread_tick();
    for (int i = 0; i < EPOCH_CALL_CALIBRATE_SAMPLES; ++i) {
        struct timespec clock1;
        uint64_t tsc1, tsc2;
        duration[i] = __get_epoch_and_tsc(&clock1, &tsc1, &tsc2);
    }
    __wt_qsort(duration, EPOCH_CALL_CALIBRATE_SAMPLES, sizeof(uint64_t), __compare_uint64);

    /*
     * Use 30% percentile (median) for "average". Also, on some platforms the clock rate is so slow
     * that TSC difference can be 0, so add a little bit for some lee-way.
     */
    *epoch_ticks_avg = duration[EPOCH_CALL_CALIBRATE_SAMPLES / 3] + 1;

    /* Throw away first few results as outliers for the "best". */
    *epoch_ticks_min = duration[2];
}

/*
 * __get_epoch_and_ticks --
 *     Gets the current time as wall clock and TSC ticks. Uses multiple attempts to make sure that
 *     there's a limited time between the two.
 *
 * Returns:
 *
 * - true if it managed to get a good result; false upon failure.
 *
 * - clock_time: the wall clock time.
 *
 * - tsc_time: CPU TSC time.
 */
static bool
__get_epoch_and_ticks(struct timespec *clock_time, uint64_t *tsc_time, uint64_t epoch_ticks_min,
  uint64_t epoch_ticks_avg)
{
    uint64_t ticks_best = epoch_ticks_avg + 1; /* Not interested in anything worse than average. */
#define GET_EPOCH_MAX_ATTEMPTS 200
    for (int i = 0; i < GET_EPOCH_MAX_ATTEMPTS; ++i) {
        struct timespec clock1;
        uint64_t tsc1, tsc2;
        uint64_t duration = __get_epoch_and_tsc(&clock1, &tsc1, &tsc2);

        /* If it took the minimum time, we're happy with the result - return it straight away. */
        if (duration <= epoch_ticks_min) {
            *clock_time = clock1;
            *tsc_time = tsc1;
            return (true);
        }

        if (duration < ticks_best) {
            /* Remember the best result. */
            *clock_time = clock1;
            *tsc_time = tsc1;
            ticks_best = duration;
        }
    }

    /* Return true if we have a good enough result. */
    return (ticks_best <= epoch_ticks_avg);
}

#define CLOCK_CALIBRATE_USEC 10000 /* Number of microseconds for clock calibration. */

/*
 * __global_calibrate_ticks --
 *     Calibrate a ratio from rdtsc ticks to nanoseconds.
 */
static void
__global_calibrate_ticks(void)
{
    /*
     * Here we aim to get two time measurements from __wt_epoch and rdtsc that are obtained nearly
     * simultaneously. To ensure these probes are taken almost at the same time, we first determine
     * minimum and median invocation time for __wt_epoch in __get_epoch_call_ticks. This is later
     * used in __get_epoch_and_ticks.
     *
     * We read the clock via __wt_epoch and rdtsc twice to obtain the start and end times, invoking
     * __wt_sleep in between. This measures the duration between invocations using each method and
     * calculates their ratio. This strategy is accurate only if __wt_epoch and rdtsc are measured
     * roughly simultaneously.
     *
     * To achieve "good" measurements, __get_epoch_and_ticks reads the time using rdtsc before and
     * after invoking __wt_epoch. If the difference between these rdtsc readings is larger than the
     * previously calibrated median, it suggests a scheduling event occurred, causing the probes not
     * to be simultaneous. We repeat this process until rdtsc and __wt_epoch are invoked roughly
     * simultaneously. Finally, we subtract the difference between the "good" measures for each
     * timing strategy and obtain their ratio.
     */
    uint64_t epoch_ticks_min, epoch_ticks_avg, tsc_start, tsc_stop;
    struct timespec clock_start, clock_stop;

    __get_epoch_call_ticks(&epoch_ticks_min, &epoch_ticks_avg);

    if (!__get_epoch_and_ticks(&clock_start, &tsc_start, epoch_ticks_min, epoch_ticks_avg))
        return;

    __wt_sleep(0, CLOCK_CALIBRATE_USEC);

    if (!__get_epoch_and_ticks(&clock_stop, &tsc_stop, epoch_ticks_min, epoch_ticks_avg))
        return;

    uint64_t diff_nsec = WT_TIMEDIFF_NS(clock_stop, clock_start);
    uint64_t diff_tsc = tsc_stop - tsc_start;

#define CLOCK_MIN_DIFF_NSEC 10
#define CLOCK_MIN_DIFF_TSC 10

    /*
     * Further improvement: check that diff_tsc is "much" (100-1000 times) bigger than
     * epoch_ticks_avg and run additional cycles if needed.
     */

    if (diff_nsec < CLOCK_MIN_DIFF_NSEC || diff_tsc < CLOCK_MIN_DIFF_TSC)
        /* Too short to be meaningful or not enough granularity. */
        return;

    double ratio = (double)diff_tsc / (double)diff_nsec;
    if (ratio <= DBL_EPSILON)
        /* Too small to be meaningful. */
        return;

    __wt_process.tsc_nsec_ratio = ratio;
    __wt_process.use_epochtime = false;
}

#endif

/*
 * __global_setup_clock --
 *     Set up variables for __wt_clock().
 */
static void
__global_setup_clock(void)
{
    /*
     * Default to using __wt_epoch until we have a good value for the ratio.
     */
    __wt_process.tsc_nsec_ratio = WT_TSC_DEFAULT_RATIO;
    __wt_process.use_epochtime = true;

#if defined(__amd64) || defined(__aarch64__)
    __global_calibrate_ticks();
#endif
}

/*
 * __global_once --
 *     Global initialization, run once.
 */
static void
__global_once(void)
{
    WT_DECL_RET;

    if ((ret = __wt_spin_init(NULL, &__wt_process.spinlock, "global")) != 0) {
        __wt_pthread_once_failed = ret;
        return;
    }

    TAILQ_INIT(&__wt_process.connqh);

    /*
     * Set up the checksum functions. If there's only one, set it as the alternate, that way code
     * doesn't have to check if it's set or not.
     */
    __wt_process.checksum = wiredtiger_crc32c_func();
    __wt_process.checksum_with_seed = wiredtiger_crc32c_with_seed_func();

    __global_setup_clock();

    /* Run-time configuration. */
#ifdef WT_STANDALONE_BUILD
    __wt_process.tiered_shared_2023 = true;
#endif
}

/*
 * __wt_library_init --
 *     Some things to do, before we do anything else.
 */
int
__wt_library_init(void)
{
    static bool first = true;
    WT_DECL_RET;

    /* Check the build matches the machine. */
    WT_RET(__endian_check());

    /*
     * Do per-process initialization once, before anything else, but only once. I don't know how
     * heavy-weight the function (pthread_once, in the POSIX world), might be, so I'm front-ending
     * it with a local static and only using that function to avoid a race.
     */
    if (first) {
        if ((ret = __wt_once(__global_once)) != 0)
            __wt_pthread_once_failed = ret;
        first = false;
    }
    return (__wt_pthread_once_failed);
}
