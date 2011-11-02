/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#define	WT_DEBUG_POINT	((void *)0xdeadbeef)

/*
 * WT_ASSERT, WT_ASSERT_RET --
 *	Assert an expression, abort in diagnostic mode, otherwise, optionally
 * return an error.
 */
#define	WT_ASSERT(session, exp) do {					\
	if (!(exp))							\
		(void)__wt_assert(					\
		    session, 0, __FILE__, __LINE__, "%s", #exp);	\
} while (0)
#define	WT_ASSERT_RET(session, exp) do {				\
	if (!(exp))							\
		return (__wt_assert(					\
		    session, WT_ERROR, __FILE__, __LINE__, "%s", #exp));\
} while (0)
