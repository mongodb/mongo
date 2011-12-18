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
	WT_BUF tmp;
	WT_PAGE *page;
	uint32_t size;
	const uint8_t *addr;
	int ret;

	ret = 0;

	WT_ASSERT(session, ref->state = WT_REF_READING);

	/* Get the address. */
	__wt_get_addr(parent, ref, &addr, &size);

	/* Force allocation of new memory. */
	WT_CLEAR(tmp);
	WT_RET(__wt_block_read(
	    session, &tmp, addr, size, dsk_verify ? WT_VERIFY : 0));

	/* Build the in-memory version of the page. */
	WT_ERR(__wt_page_inmem(session, parent, ref, tmp.mem, &page));

	__wt_cache_page_read(
	    session, page, sizeof(WT_PAGE) + page->dsk->memsize);

	WT_VERBOSE(session, read,
	    "page %p, %s", page, __wt_page_type_string(page->type));

	ref->page = page;
	ref->state = WT_REF_MEM;
	return (0);

err:	ref->state = WT_REF_DISK;
	__wt_buf_free(session, &tmp);
	return (ret);
}
