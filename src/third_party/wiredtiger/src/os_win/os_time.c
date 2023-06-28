/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __wt_epoch_raw --
 *     Return the time since the Epoch as reported by the system.
 */
void
__wt_epoch_raw(WT_SESSION_IMPL *session, struct timespec *tsp)
{
    FILETIME time;
    uint64_t ns100;

    WT_UNUSED(session);

    GetSystemTimeAsFileTime(&time);

    ns100 = (((int64_t)time.dwHighDateTime << 32) + time.dwLowDateTime) - 116444736000000000LL;
    tsp->tv_sec = ns100 / (10 * WT_MILLION);
    tsp->tv_nsec = (long)((ns100 % (10 * WT_MILLION)) * 100);
}

/*
 * __wt_localtime --
 *     Return the current local broken-down time.
 */
int
__wt_localtime(WT_SESSION_IMPL *session, const time_t *timep, struct tm *result)
{
    errno_t err;

    if ((err = localtime_s(result, timep)) == 0)
        return (0);

    WT_RET_MSG(session, err, "localtime_s");
}
