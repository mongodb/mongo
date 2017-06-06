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
    WT_GCC_FUNC_ATTRIBUTE((visibility("default")))
{
	struct timespec tmp;
	WT_DECL_RET;

	/*
	 * This function doesn't return an error, but panics on failure (which
	 * should never happen, it's done this way to simplify error handling
	 * in the caller). However, some compilers complain about using garbage
	 * values. Initializing the values avoids the complaint.
	 */
	tsp->tv_sec = 0;
	tsp->tv_nsec = 0;

	/*
	 * Read into a local variable so that we're comparing the correct
	 * value when we check for monotonic increasing time.  There are
	 * many places we read into an unlocked global variable.
	 */
#if defined(HAVE_CLOCK_GETTIME)
	WT_SYSCALL_RETRY(clock_gettime(CLOCK_REALTIME, &tmp), ret);
	if (ret == 0) {
		__wt_time_check_monotonic(session, &tmp);
		tsp->tv_sec = tmp.tv_sec;
		tsp->tv_nsec = tmp.tv_nsec;
		return;
	}
	WT_PANIC_MSG(session, ret, "clock_gettime");
#elif defined(HAVE_GETTIMEOFDAY)
	{
	struct timeval v;

	WT_SYSCALL_RETRY(gettimeofday(&v, NULL), ret);
	if (ret == 0) {
		tmp.tv_sec = v.tv_sec;
		tmp.tv_nsec = v.tv_usec * WT_THOUSAND;
		__wt_time_check_monotonic(session, &tmp);
		*tsp = tmp;
		return;
	}
	WT_PANIC_MSG(session, ret, "gettimeofday");
	}
#else
	NO TIME-OF-DAY IMPLEMENTATION: see src/os_posix/os_time.c
#endif
}
