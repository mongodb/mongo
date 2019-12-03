/*-
 * Copyright (c) 2014-2019 MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __wt_rdtsc --
 *     Get a timestamp from CPU registers.
 */
static inline uint64_t
__wt_rdtsc(void)
{
#if defined(__i386)
    {
        uint64_t x;

        __asm__ volatile("rdtsc" : "=A"(x));
        return (x);
    }
#elif defined(__amd64)
    {
        uint64_t a, d;

        __asm__ volatile("rdtsc" : "=a"(a), "=d"(d));
        return ((d << 32) | a);
    }
#else
    return (0);
#endif
}

/*
 * __time_check_monotonic --
 *     Check and prevent time running backward. If we detect that it has, we set the time structure
 *     to the previous values, making time stand still until we see a time in the future of the
 *     highest value seen so far.
 */
static inline void
__time_check_monotonic(WT_SESSION_IMPL *session, struct timespec *tsp)
{
    /*
     * Detect time going backward. If so, use the last saved timestamp.
     */
    if (session == NULL)
        return;

    if (tsp->tv_sec < session->last_epoch.tv_sec ||
      (tsp->tv_sec == session->last_epoch.tv_sec && tsp->tv_nsec < session->last_epoch.tv_nsec)) {
        WT_STAT_CONN_INCR(session, time_travel);
        *tsp = session->last_epoch;
    } else
        session->last_epoch = *tsp;
}

/*
 * __wt_epoch --
 *     Return the time since the Epoch.
 */
static inline void
__wt_epoch(WT_SESSION_IMPL *session, struct timespec *tsp)
{
    struct timespec tmp;

    /*
     * Read into a local variable, then check for monotonically increasing time, ensuring single
     * threads never see time move backward. We don't prevent multiple threads from seeing time move
     * backwards (even when reading time serially, the saved last-read time is per thread, not per
     * timer, so multiple threads can race the time). Nor do we prevent multiple threads
     * simultaneously reading the time from seeing random time or time moving backwards (assigning
     * the time structure to the returned memory location implies multicycle writes to memory).
     */
    __wt_epoch_raw(session, &tmp);
    __time_check_monotonic(session, &tmp);
    *tsp = tmp;
}

/*
 * __wt_clock --
 *     Obtain a timestamp via either a CPU register or via a system call on platforms where
 *     obtaining it directly from the hardware register is not supported.
 */
static inline uint64_t
__wt_clock(WT_SESSION_IMPL *session)
{
    struct timespec tsp;

    /*
     * In one case we return nanoseconds, in the other we return clock ticks. That looks wrong, but
     * it's not. When simply comparing before and after values, which is returned doesn't matter.
     * When trying to calculate wall-clock time (that is, comparing a starting time with an ending
     * time), we'll subtract the two values and then call a function to convert the result of the
     * subtraction into nanoseconds. In the case where we already have nanoseconds, that function
     * has a conversion constant of 1 and we'll skip the conversion, in the case where we have clock
     * ticks, the conversion constant will be real. The reason is because doing it that way avoids a
     * floating-point operation per wall-clock time calculation.
     */
    if (__wt_process.use_epochtime) {
        __wt_epoch(session, &tsp);
        return ((uint64_t)(tsp.tv_sec * WT_BILLION + tsp.tv_nsec));
    }
    return (__wt_rdtsc());
}

/*
 * __wt_seconds --
 *     Return the seconds since the Epoch.
 */
static inline void
__wt_seconds(WT_SESSION_IMPL *session, uint64_t *secondsp)
{
    struct timespec t;

    __wt_epoch(session, &t);

    *secondsp = (uint64_t)(t.tv_sec + t.tv_nsec / WT_BILLION);
}

/*
 * __wt_seconds32 --
 *     Return the seconds since the Epoch in 32 bits.
 */
static inline void
__wt_seconds32(WT_SESSION_IMPL *session, uint32_t *secondsp)
{
    uint64_t seconds;

    /* This won't work in 2038. But for now allow it. */
    __wt_seconds(session, &seconds);
    *secondsp = (uint32_t)seconds;
}

/*
 * __wt_clock_to_nsec --
 *     Convert from clock ticks to nanoseconds.
 */
static inline uint64_t
__wt_clock_to_nsec(uint64_t end, uint64_t begin)
{
    double clock_diff;

    /*
     * If the ticks were reset, consider it an invalid check and just return zero as the time
     * difference because we cannot compute anything meaningful.
     */
    if (end < begin)
        return (0);
    clock_diff = (double)(end - begin);
    return ((uint64_t)(clock_diff / __wt_process.tsc_nsec_ratio));
}

/*
 * __wt_op_timer_start --
 *     Start the operations timer.
 */
static inline void
__wt_op_timer_start(WT_SESSION_IMPL *session)
{
    uint64_t timeout_us;

    /* Timer can be configured per-transaction, and defaults to per-connection. */
    if ((timeout_us = session->txn.operation_timeout_us) == 0)
        timeout_us = S2C(session)->operation_timeout_us;
    if (timeout_us == 0)
        session->operation_start_us = session->operation_timeout_us = 0;
    else {
        session->operation_start_us = __wt_clock(session);
        session->operation_timeout_us = timeout_us;
    }

#ifdef HAVE_DIAGNOSTIC
    /*
     * This is called at the beginning of each API call. We need to clear out any old values from
     * this debugging field so that we don't leave a stale value in there that may then give a false
     * positive.
     */
    session->op_5043_seconds = 0;
#endif
}

/*
 * __wt_op_timer_stop --
 *     Stop the operations timer.
 */
static inline void
__wt_op_timer_stop(WT_SESSION_IMPL *session)
{
    session->operation_start_us = session->operation_timeout_us = 0;
}

/*
 * __wt_op_timer_fired --
 *     Check the operations timers.
 */
static inline bool
__wt_op_timer_fired(WT_SESSION_IMPL *session)
{
    uint64_t diff, now;

    if (session->operation_start_us == 0 || session->operation_timeout_us == 0)
        return (false);

    now = __wt_clock(session);
    diff = WT_CLOCKDIFF_US(now, session->operation_start_us);
    return (diff > session->operation_timeout_us);
}
