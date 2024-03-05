/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#pragma once

/*
 * WT_GENERATION_COOKIE --
 *   State passed through to callbacks during the session walk logic when
 *   tracking a generation across all sessions.
 */
struct __wt_generation_cookie {
    bool ret_active;
    uint64_t ret_oldest_gen;
    int which;
    uint64_t target_generation;
};

/*
 * WT_GENERATION_DRAIN_COOKIE --
 *   State passed through to callbacks during the session walk logic when
 *   performing a generation drain.
 */
struct __wt_generation_drain_cookie {
    WT_GENERATION_COOKIE base;

    struct timespec start;
    uint64_t minutes;
    int pause_cnt;
    bool verbose_timeout_flags;
};
