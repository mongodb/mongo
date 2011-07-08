/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#if defined(__cplusplus)
extern "C" {
#endif

#define	WT_DEBUG_POINT	((void *)0xdeadbeef)

#ifdef HAVE_DIAGNOSTIC
#define	WT_ASSERT(session, exp)						\
	((exp) ? (void)0 : __wt_failure(session, #exp, __FILE__, __LINE__))
#define	WT_FAILURE(session, str)					\
	__wt_failure(session, str, __FILE__, __LINE__)
#else
#define	WT_ASSERT(session, exp)		do {} while (0)
#define	WT_FAILURE(session, str)	do {} while (0)
#endif

#if defined(__cplusplus)
}
#endif
