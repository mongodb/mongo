/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wt_internal.h"

/*
 * __wt_cache_read --
 *	Read a page from the file.
 */
int
__wt_cache_read(
    WT_SESSION_IMPL *session, WT_PAGE *parent, WT_REF *ref, int dsk_verify)
{
	WT_BUF *tmp;
	WT_PAGE_DISK *dsk;
	uint32_t addr, size;
	int ret;

	tmp = NULL;
	dsk = NULL;
	addr = ref->addr;
	size = ref->size;
	ret = 0;

	/* Review the possible page states. */
	switch (ref->state) {
	case WT_REF_DISK:
	case WT_REF_READING:
		/* Page is on disk, and that's our problem.  Read it. */
		break;
	case WT_REF_MEM:
		/* Page is in memory, must have already been read. */
		return (0);
	case WT_REF_LOCKED:
		/* Page being considered for eviction: not our problem. */
		return (0);
	}

	/*
	 * The page isn't in the cache, and since we're the only path for the
	 * page to get into the cache, we don't have to worry further, and we
	 * might as well get to it.
	 *
	 * Allocate memory for the page's disk image.
	 */
	WT_RET(__wt_scr_alloc(session, size, &tmp));

	/* Read the page and steal the resulting buffer. */
	WT_ERR(__wt_block_read(
	    session, tmp, addr, size, dsk_verify ? WT_VERIFY : 0));

	dsk = __wt_buf_steal(session, tmp, NULL);
	__wt_scr_free(&tmp);

	/*
	 * Build the in-memory version of the page, then re-load the disk
	 * reference: the disk image may have been discarded, use whatever
	 * the page has, or NULL if it doesn't have one.
	 */
	WT_ERR(__wt_page_inmem(session, parent, ref, dsk, &ref->page));

	dsk = ref->page->dsk;
	__wt_cache_page_read(session, ref->page,
	    sizeof(WT_PAGE) + ((dsk == NULL) ? 0 : dsk->memsize));
	ref->state = WT_REF_MEM;

	WT_VERBOSE(session, read,
	    "page %p (%" PRIu32 "/%" PRIu32 ", %s)",
	    ref->page, addr, size, __wt_page_type_string(ref->page->type));
	return (0);

err:	__wt_scr_free(&tmp);
	__wt_free(session, dsk);
	ref->state = WT_REF_DISK;

	return (ret);
}
