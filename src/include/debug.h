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
#define	WT_ABORT(session, e)						\
	__wt_assert(session, e, __FILE__, __LINE__)
#define	WT_ASSERT(session, e)						\
	((e) ? (void)0 : __wt_assert(session, #e, __FILE__, __LINE__))
#else
#define	WT_ABORT(session, e)	do {} while (0)
#define	WT_ASSERT(session, e)	do {} while (0)
#endif

#if defined(__cplusplus)
}
#endif
