/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#if defined(__cplusplus)
extern "C" {
#endif

/* Debug byte value */
#define	WT_DEBUG_BYTE	0xab
#define	WT_DEBUG_POINT	((void *)0xdeadbeef)

#ifdef HAVE_DIAGNOSTIC
#define	WT_ABORT(env, e)						\
	__wt_assert(env, e, __FILE__, __LINE__)
#define	WT_ASSERT(env, e)						\
	((e) ? (void)0 : __wt_assert(env, #e, __FILE__, __LINE__))
#else
#define	WT_ABORT(env, e)
#define	WT_ASSERT(env, e)
#endif

/*
 * The __wt_msg interface appends a <newline> to each message (which is correct,
 * we don't want output messages interspersed in the application's log file.
 * The following structures and macros allow routines to build messages and then
 * dump them to the application's logging file/function.
 */
typedef struct __wt_mbuf {
	ENV	 *env;			/* Enclosing environment */

	char  *first;			/* Allocated message buffer */
	char  *next;			/* Next available byte of the buffer */
	size_t len;			/* Allocated length of the buffer */
} WT_MBUF;

/*
 * Quiet a compiler warning by writing, and then reading, a variable.
 */
#define	WT_CC_QUIET(var, value)	do {				        \
	(var) = (value);					        \
	(var) = (var);						        \
} while (0)

#if defined(__cplusplus)
}
#endif
