/*-
 * Copyright (c) 2008-2012 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __wt_epoch --
 *	Return the seconds and nanoseconds since the Epoch.
 */
int
__wt_epoch(WT_SESSION_IMPL *session, uintmax_t *secp, uintmax_t *nsecp)
{
	WT_DECL_RET;

#if defined(HAVE_CLOCK_GETTIME)
	struct timespec v;
	WT_SYSCALL_RETRY(clock_gettime(CLOCK_REALTIME, &v), ret);
	if (ret == 0) {
		if (secp != NULL)
			*secp = (uintmax_t)v.tv_sec;
		if (nsecp != NULL)
			*nsecp = (uintmax_t)v.tv_nsec;
		return (0);
	}
	WT_RET_MSG(session, ret, "clock_gettime");
#elif defined(HAVE_GETTIMEOFDAY)
	struct timeval v;

	WT_SYSCALL_RETRY(gettimeofday(&v, NULL), ret);
	if (ret == 0) {
		if (secp != NULL)
			*secp = (uintmax_t)v.tv_sec;
		if (nsecp != NULL)	/* nanoseconds in a microsecond */
			*nsecp = (uintmax_t)(v.tv_usec * 1000);
		return (0);
	}
	WT_RET_MSG(session, ret, "gettimeofday");
#else
	NO TIME-OF-DAY IMPLEMENTATION: see src/os_posix/os_time.c
#endif
}
