/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008 WiredTiger Software.  All rights reserved.
 *
 * $Id$
 */

#if defined(__cplusplus)
extern "C" {
#endif

/*******************************************
 * Cache support.
 *******************************************/
struct __wt_cache {
	WT_MTX mtx;			/* Cache server mutex */

#define	WT_CACHE_SIZE_DEFAULT	(20)	/* 20MB */

	/*
	 * Each in-memory page is in a hash bucket based on its "address".
	 *
	 * Our hash buckets are very simple list structures.   We depend on
	 * the ability to add/remove an element from the list by writing a
	 * single pointer.  The underlying assumption is that writing a
	 * single pointer will never been seen as a partial write by any
	 * other thread of control, that is, the linked list will always
	 * be consistent.  The end result is that while we have to serialize
	 * the actual manipulation of the memory, we can support multiple
	 * threads of control using the linked lists even while they are
	 * being modified.
	 */
#define	WT_CACHE_HASH_SIZE_DEFAULT	0
#define	WT_HASH(cache, addr)	((addr) % (cache)->hash_size)
	u_int32_t hash_size;

	WT_PAGE **hb;

	u_int32_t flags;
};

/*
 * WT_REF --
 *	A structure to pair a memory reference to a generation number.
 */
typedef struct __wt_ref {
	void	 *ref;				/* Memory reference */
	u_int32_t gen;				/* Associated generation */
} WT_REF;

#if defined(__cplusplus)
}
#endif
