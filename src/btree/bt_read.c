/*-
 * Copyright (c) 2008-2012 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __wt_cache_read --
 *	Read a page from the file.
 */
int
__wt_cache_read(WT_SESSION_IMPL *session, WT_PAGE *parent, WT_REF *ref)
{
	WT_ITEM tmp;
	WT_PAGE *page;
	uint32_t size;
	const uint8_t *addr;
	size_t inmem_size;
	int ret;

	/*
	 * We don't pass in an allocated buffer, force allocation of new memory
	 * of the appropriate size.
	 */
	WT_CLEAR(tmp);
	ret = 0;

	WT_ASSERT(session, ref->state == WT_REF_READING);

	/* Get the address. */
	__wt_get_addr(parent, ref, &addr, &size);

	/* Force allocation of new memory. */
	WT_ERR(__wt_bm_read(session, &tmp, addr, size));

	/* Build the in-memory version of the page. */
	WT_ERR(__wt_page_inmem(
	    session, parent, ref, tmp.mem, &inmem_size, &page));

	__wt_cache_page_read(session, page, inmem_size);

	WT_VERBOSE(session, read,
	    "page %p, %s", page, __wt_page_type_string(page->type));

	WT_ASSERT(session, page != NULL);
	ref->page = page;
	WT_PUBLISH(ref->state, WT_REF_MEM);
	return (0);

err:	ref->state = WT_REF_DISK;
	__wt_buf_free(session, &tmp);
	return (ret);
}
