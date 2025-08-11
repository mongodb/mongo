/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "test_util.h"
#include "bench_timer.h"

/*
 * bench_timer_init --
 *     Initialize the bench timer structure.
 */
void
bench_timer_init(BENCH_TIMER *timer, const char *name)
{
    timer->name = name;
    timer->total_nsec = 0;
    timer->start_nsec = 0;
    timer->count = 0;
}

/*
 * bench_timer_start --
 *     Start a timing.
 */
void
bench_timer_start(BENCH_TIMER *timer, WT_SESSION *session)
{
    struct timespec start;

    assert(timer->start_nsec == 0);

    __wt_epoch((WT_SESSION_IMPL *)session, &start);
    /* Will overflow sometime > 2100 AD */
    timer->start_nsec = (uint64_t)(start.tv_sec * WT_BILLION + start.tv_nsec);
}

/*
 * bench_timer_stop --
 *     Stop a timing.
 */
void
bench_timer_stop(BENCH_TIMER *timer, WT_SESSION *session)
{
    uint64_t nsec;
    struct timespec start;

    assert(timer->start_nsec != 0);

    __wt_epoch((WT_SESSION_IMPL *)session, &start);
    /* Will overflow sometime > 2100 AD */
    nsec = (uint64_t)(start.tv_sec * WT_BILLION + start.tv_nsec);
    assert(nsec > timer->start_nsec);
    timer->total_nsec += (nsec - timer->start_nsec);
    timer->count++;
}

/*
 * bench_timer_add --
 *     Add results from another timer to this one.
 */
void
bench_timer_add(BENCH_TIMER *timer, const BENCH_TIMER *that)
{
    timer->total_nsec += that->total_nsec;
    timer->count += that->count;
}

/*
 * bench_timer_add_to_shared --
 *     Add timing results to this timer, that is shared among multiple threads.
 */
void
bench_timer_add_to_shared(BENCH_TIMER *timer, BENCH_TIMER *that)
{
    WT_RELEASE_WRITE_WITH_BARRIER(timer->total_nsec, timer->total_nsec + that->total_nsec);
    WT_RELEASE_WRITE_WITH_BARRIER(timer->count, timer->count + that->count);
}

/*
 * bench_timer_add_to_shared_2 --
 *     Add timing results to this timer, that is shared among multiple threads.
 */
void
bench_timer_add_to_shared_2(BENCH_TIMER *timer, uint64_t nsec, uint64_t count)
{
    WT_RELEASE_WRITE_WITH_BARRIER(timer->total_nsec, timer->total_nsec + nsec);
    WT_RELEASE_WRITE_WITH_BARRIER(timer->count, timer->count + count);
}

/*
 * bench_timer_add_from_shared --
 *     Add results from another shared timer to this (non-shared) timer.
 */
void
bench_timer_add_from_shared(BENCH_TIMER *timer, BENCH_TIMER *that)
{
    uint64_t ns, count;

    WT_ACQUIRE_READ_WITH_BARRIER(ns, that->total_nsec);
    WT_ACQUIRE_READ_WITH_BARRIER(count, that->count);
    timer->total_nsec += ns;
    timer->count += count;
}

/*
 * bench_timer_format --
 *     Format a number, given as nanoseconds per operation, in a readable way.
 */
static void
__bench_timer_format(char *buf, size_t len, double nsec_per_op)
{
    if (nsec_per_op > WT_BILLION)
        testutil_assert(snprintf(buf, len, "%10.3f secs/op", nsec_per_op / WT_BILLION) < (int)len);
    else if (nsec_per_op > WT_MILLION)
        testutil_assert(snprintf(buf, len, "%10.3f msecs/op", nsec_per_op / WT_MILLION) < (int)len);
    else if (nsec_per_op > WT_THOUSAND)
        testutil_assert(
          snprintf(buf, len, "%10.3f usecs/op", nsec_per_op / WT_THOUSAND) < (int)len);
    else
        testutil_assert(snprintf(buf, len, "%10.3f nsecs/op", nsec_per_op) < (int)len);
}

/*
 * bench_timer_show_change --
 *     For a difference between two timers, show a summary of the number of operations, and the time
 *     taken per operation.
 */
bool
bench_timer_show_change(BENCH_TIMER *before, BENCH_TIMER *after)
{
    uint64_t ns, count;
    char num[512];

    if (before->count != after->count) {
        assert(before->count < after->count);
        assert(before->total_nsec <= after->total_nsec);
        ns = after->total_nsec - before->total_nsec;
        count = after->count - before->count;
        __bench_timer_format(num, sizeof(num), (double)ns / count);
        fprintf(stderr, " %ss: %" PRIu64 " ops, %s\n", after->name, count, num);
        return (true);
    }
    return (false);
}
