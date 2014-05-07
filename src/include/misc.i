/*-
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

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
		ret = __wt_eventv(session, 1, 0, NULL, 0, fmt, ap);
		va_end(ap);
	}
	return (ret);
#else
	WT_UNUSED(session);
	WT_UNUSED(fmt);
	WT_UNUSED(flag);
	return (0);
#endif
}
