/*-
 * Copyright (c) 2014-2016 MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

/*
 * __wt_cond_wait --
 *	Wait on a mutex, optionally timing out.
 */
static inline int
__wt_cond_wait(WT_SESSION_IMPL *session, WT_CONDVAR *cond, uint64_t usecs)
{
	bool notused;

	return (__wt_cond_wait_signal(session, cond, usecs, &notused));
}

/*
 * __wt_strdup --
 *	ANSI strdup function.
 */
static inline int
__wt_strdup(WT_SESSION_IMPL *session, const char *str, void *retp)
{
	return (__wt_strndup(
	    session, str, (str == NULL) ? 0 : strlen(str), retp));
}

/*
 * __wt_seconds --
 *	Return the seconds since the Epoch.
 */
static inline int
__wt_seconds(WT_SESSION_IMPL *session, time_t *timep)
{
	struct timespec t;

	WT_RET(__wt_epoch(session, &t));

	*timep = t.tv_sec;

	return (0);
}

/*
 * __wt_verbose --
 * 	Verbose message.
 */
static inline int
__wt_verbose(WT_SESSION_IMPL *session, int flag, const char *fmt, ...)
    WT_GCC_FUNC_ATTRIBUTE((format (printf, 2, 3)))
{
#ifdef HAVE_VERBOSE
	WT_DECL_RET;
	va_list ap;

	if (WT_VERBOSE_ISSET(session, flag)) {
		va_start(ap, fmt);
		ret = __wt_eventv(session, true, 0, NULL, 0, fmt, ap);
		va_end(ap);
	}
	return (ret);
#else
	WT_UNUSED(session);
	WT_UNUSED(flag);
	WT_UNUSED(fmt);
	return (0);
#endif
}
