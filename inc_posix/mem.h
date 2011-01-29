/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 *
 * $Id$
 */

#if defined(__cplusplus)
extern "C" {
#endif

#ifdef HAVE_DIAGNOSTIC
/*
 * When WiredTiger is compiled with HAVE_DIAGNOSTIC, it optionally tracks memory
 * allocations and frees to find memory leaks.   (NB: the memory checking code
 * is not thread-safe, and so running with threaded programs won't necessarily
 * work correctly.)
 */
/*
 * WT_MEM --
 *	A memory object.
 */
typedef struct {
	const void *addr;		/* Memory reference */
	const char *file;		/* Associated file */
	int	    line;		/* Associated line */
} WT_MEM;
/*
 * WT_MTRACK --
 *	A memory tracking object.
 */
typedef struct __wt_mem {
	WT_MEM	*list;			/* Memory array */
	WT_MEM	*next;			/* Next open slot */
	u_int	 slots;			/* Slots */
} WT_MTRACK;
#endif

/*
 * Our internal free function clears the underlying address atomically so there
 * is no chance of racing threads seeing intermediate results while a structure
 * is being free'd.   (That would be a bug, of course, but I'd rather not drop
 * core, just the same.)  That's a non-standard "free" API, and the resulting
 * bug is a mother to find -- make sure we get it right, don't make the caller
 * remember to put the & operator on the pointer.
 */
#ifdef HAVE_DIAGNOSTIC
#define	__wt_calloc(a, b, c, d)	__wt_calloc_func(a, b, c, d, __FILE__, __LINE__)
#define	__wt_free(a, b, c)	__wt_free_func(a, &(b), c)
#define	__wt_realloc(a, b, c,d)	__wt_realloc_func(a, b, c,d, __FILE__, __LINE__)
#define	__wt_strdup(a, b, c)	__wt_strdup_func(a, b, c, __FILE__, __LINE__)
#else
#define	__wt_calloc(a, b, c, d)	__wt_calloc_func(a, b, c, d)
#define	__wt_free(a, b, c)	__wt_free_func(a, &(b))
#define	__wt_realloc(a, b, c,d)	__wt_realloc_func(a, b, c, d)
#define	__wt_strdup(a, b, c)	__wt_strdup_func(a, b, c)
#endif

#if defined(__cplusplus)
}
#endif
