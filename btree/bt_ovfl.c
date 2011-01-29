/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2010 WiredTiger, Inc.
 *	All rights reserved.
 *
 * $Id$
 */

#include "wt_internal.h"

/*
 * __wt_ovfl_in --
 *	Read an overflow item from the disk.
 */
int
__wt_ovfl_in(WT_TOC *toc, WT_OVFL *ovfl, DBT *store)
{
	DB *db;
	ENV *env;
	WT_PAGE *page, _page;
	WT_STATS *stats;

	env = toc->env;
	db = toc->db;
	stats = env->ienv->cache->stats;

	/*
	 * Read an overflow page, using an overflow structure from a page for
	 * which we (better) have a hazard reference.
	 *
	 * Overflow reads are synchronous. That may bite me at some point, but
	 * WiredTiger supports large page sizes, and overflow items should be
	 * rare.
	 */
	WT_VERBOSE(env, WT_VERB_READ, (env,
	    "overflow read addr/size %lu/%lu",
	    (u_long)ovfl->addr, (u_long)ovfl->size));
	WT_STAT_INCR(stats, OVERFLOW_READ);

	/*
	 * The only caller that wants a copy of the overflow pages (as opposed
	 * to the contents of the overflow pages), is the verify code.  For that
	 * reason, it reads its own overflow pages, it doesn't call this code.
	 *
	 * But, we still have to verify the checksum, which means we have to
	 * read the entire set of pages, then copy the interesting information
	 * to the beginning of the buffer.   The copy is a shift in a single
	 * buffer and so should be fast, but it's still not a good thing.  If
	 * it ever becomes a problem, then we either have to pass the fact that
	 * it's a "page" back to our caller and let them deal with the offset,
	 * or add a new field to the DBT that flags the start of the allocated
	 * buffer, instead of using the "data" field to indicate both the start
	 * of the data and the start of the allocated memory.
	 */
	WT_CLEAR(_page);
	page = &_page;
	page->addr = ovfl->addr;
	page->size = WT_HDR_BYTES_TO_ALLOC(db, ovfl->size);

	/* Re-allocate memory as necessary to hold the overflow pages. */
	if (store->mem_size < page->size)
		WT_RET(__wt_realloc(
		    env, &store->mem_size, page->size, &store->data));
	page->hdr = store->data;

	/* Read the page. */
	WT_RET(__wt_page_read(db, page));

	/* Copy the actual data in the DBT down to the start of the data. */
	(void)memmove(store->data,
	    (uint8_t *)store->data + sizeof(WT_PAGE_HDR), ovfl->size);
	store->size = ovfl->size;

	return (0);
}
