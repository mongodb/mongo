/*-
 * Copyright (c) 2014-2017 MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __wt_epoch --
 *	Return the time since the Epoch.
 */
void
__wt_epoch(WT_SESSION_IMPL *session, struct timespec *tsp)
{
	struct timespec tmp;
	FILETIME time;
	uint64_t ns100;

	GetSystemTimeAsFileTime(&time);

	ns100 = (((int64_t)time.dwHighDateTime << 32) + time.dwLowDateTime)
	    - 116444736000000000LL;
	tmp.tv_sec = ns100 / 10000000;
	tmp.tv_nsec = (long)((ns100 % 10000000) * 100);
	__wt_time_check_monotonic(session, &tmp);
	*tsp = tmp;
}

/*
 * localtime_r --
 *	Return the current local time.
 */
struct tm *
localtime_r(const time_t *timer, struct tm *result)
{
	errno_t err;

	err = localtime_s(result, timer);
	if (err != 0) {
		__wt_err(NULL, err, "localtime_s");
		return (NULL);
	}

	return (result);
}
