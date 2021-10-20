/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __wt_sleep --
 *     Pause the thread of control.
 */
void
__wt_sleep(uint64_t seconds, uint64_t micro_seconds) WT_GCC_FUNC_ATTRIBUTE((visibility("default")))
{
    struct timeval t;

    /*
     * Sleeping isn't documented as a memory barrier, and it's a reasonable expectation to have.
     * There's no reason not to explicitly include a barrier since we're giving up the CPU, and
     * ensures callers are never surprised.
     */
    WT_FULL_BARRIER();

    t.tv_sec = (time_t)(seconds + micro_seconds / WT_MILLION);
    t.tv_usec = (suseconds_t)(micro_seconds % WT_MILLION);

    (void)select(0, NULL, NULL, NULL, &t);
}
