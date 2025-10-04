/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#pragma once

typedef struct __bench_timer {
    const char *name;
    uint64_t total_nsec;
    uint64_t count;
    uint64_t start_nsec;
} BENCH_TIMER;

void bench_timer_init(BENCH_TIMER *timer, const char *name);
void bench_timer_start(BENCH_TIMER *timer, WT_SESSION *session);
void bench_timer_stop(BENCH_TIMER *timer, WT_SESSION *session);
void bench_timer_add(BENCH_TIMER *timer, const BENCH_TIMER *that);
void bench_timer_add_from_shared(BENCH_TIMER *timer, BENCH_TIMER *that);
void bench_timer_add_to_shared(BENCH_TIMER *timer, BENCH_TIMER *that);
void bench_timer_add_to_shared_2(BENCH_TIMER *timer, uint64_t nsec, uint64_t count);

bool bench_timer_show_change(BENCH_TIMER *before, BENCH_TIMER *after);

#define BENCH_TIME_SINGLE(timer, session, stmt) \
    do {                                        \
        bench_timer_init(timer, NULL);          \
        bench_timer_start(timer, session);      \
        stmt;                                   \
        bench_timer_stop(timer, session);       \
    } while (0)

#define BENCH_TIME_CUMULATIVE(timer, session, stmt)       \
    do {                                                  \
        BENCH_TIMER _single_timer;                        \
        BENCH_TIME_SINGLE(&_single_timer, session, stmt); \
        bench_timer_add_to_shared(timer, &_single_timer); \
    } while (0)
