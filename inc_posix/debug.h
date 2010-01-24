/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008 WiredTiger Software.
 *	All rights reserved.
 *
 * $Id$
 */

#if defined(__cplusplus)
extern "C" {
#endif

/* Debug byte value */
#define	WT_DEBUG_BYTE	0xab

/*
 * Our private free function clears the underlying address atomically so there's
 * no chance of racing threads looking an intermediate results while a structure
 * is being free'd.   That's a non-standard "free" API, and the resulting bug is
 * a mother to find -- make sure we get it right, don't make the caller remember
 * to put the & on the pointer.
 */
#define	__wt_free(a, b, c)	__wt_free_worker(a, &(b), c)

#ifdef HAVE_DIAGNOSTIC
#define	WT_ASSERT(env, e)						\
	((e) ? (void)0 : __wt_assert(env, #e, __FILE__, __LINE__))
#else
#define	WT_ASSERT(ienv, e)
#endif

#if defined(__cplusplus)
}
#endif
