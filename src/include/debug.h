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
		WT_FAILURE(session, "%s", #exp);			\
} while (0)
#define	WT_ASSERT_RET(session, exp) do {				\
	if (!(exp))							\
		WT_FAILURE_RET(session, WT_ERROR, "%s", #exp);		\
} while (0)

/*
 * WT_FAILURE, WT_FAILURE_RET --
 *	Abort in diagnostic mode, otherwise, optionally return an error.
 */
#define	WT_FAILURE(session, ...)					\
	(void)__wt_failure(session, 0, __FILE__, __LINE__, __VA_ARGS__)
#define	WT_FAILURE_RET(session, error, ...)				\
	return (__wt_failure(session, error, __FILE__, __LINE__, __VA_ARGS__))
