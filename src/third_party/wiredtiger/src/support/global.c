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

/*
 * __global_calibrate_ticks --
 *     Calibrate a ratio from rdtsc ticks to nanoseconds.
 */
static void
__global_calibrate_ticks(void)
{
    /*
     * Default to using __wt_epoch until we have a good value for the ratio.
     */
    __wt_process.tsc_nsec_ratio = WT_TSC_DEFAULT_RATIO;
    __wt_process.use_epochtime = true;

#if defined(__i386) || defined(__amd64) || defined(__aarch64__)
    {
        struct timespec start, stop;
        double ratio;
        uint64_t diff_nsec, diff_tsc, min_nsec, min_tsc;
        uint64_t tries, tsc_start, tsc_stop;
        volatile uint64_t i;

        /*
         * Run this calibration loop a few times to make sure we get a reading that does not have a
         * potential scheduling shift in it. The inner loop is CPU intensive but a scheduling change
         * in the middle could throw off calculations. Take the minimum amount of time and compute
         * the ratio.
         */
        min_nsec = min_tsc = UINT64_MAX;
        for (tries = 0; tries < 3; ++tries) {
            /* This needs to be CPU intensive and large enough. */
            __wt_epoch(NULL, &start);
            tsc_start = __wt_rdtsc();
            for (i = 0; i < 100 * WT_MILLION; i++)
                ;
            tsc_stop = __wt_rdtsc();
            __wt_epoch(NULL, &stop);
            diff_nsec = WT_TIMEDIFF_NS(stop, start);
            diff_tsc = tsc_stop - tsc_start;

            /* If the clock didn't tick over, we don't have a sample. */
            if (diff_nsec == 0 || diff_tsc == 0)
                continue;
            min_nsec = WT_MIN(min_nsec, diff_nsec);
            min_tsc = WT_MIN(min_tsc, diff_tsc);
        }

        /*
         * Only use rdtsc if we got a good reading. One reason this might fail is that the system's
         * clock granularity is not fine-grained enough.
         */
        if (min_nsec != UINT64_MAX) {
            ratio = (double)min_tsc / (double)min_nsec;
            if (ratio > DBL_EPSILON) {
                __wt_process.tsc_nsec_ratio = ratio;
                __wt_process.use_epochtime = false;
            }
        }
    }
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

    __global_calibrate_ticks();

    /* Run-time configuration. */
#ifdef WT_STANDALONE_BUILD
    __wt_process.fast_truncate_2022 = true;
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
