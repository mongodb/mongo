/*-
 * Copyright (c) 2008-2012 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __cache_read_row_deleted --
 *	Instantiate an entirely deleted row-store leaf page.
 */
static int
__cache_read_row_deleted(
    WT_SESSION_IMPL *session, WT_REF *ref, WT_PAGE *page)
{
	WT_UPDATE **upd_array, *upd;
	uint32_t i;

	/* The page has been modified, by definition. */
	WT_RET(__wt_page_modify_init(session, page));
	__wt_page_modify_set(page);

	/* Record the transaction ID for the first update to the page. */
	page->modify->first_id = ref->txnid;

	/* Allocate the update array. */
	WT_RET(__wt_calloc_def(session, page->entries, &upd_array));
	page->u.row.upd = upd_array;

	/* Fill in the update array with deleted items. */
	for (i = 0; i < page->entries; ++i) {
		WT_RET(__wt_calloc_def(session, 1, &upd));
		upd->next = upd_array[i];
		upd_array[i] = upd;

		WT_UPDATE_DELETED_SET(upd);
		upd->txnid = ref->txnid;
	}

	__wt_cache_page_inmem_incr(
	    session, page, sizeof(WT_UPDATE) * page->entries);

	return (0);
}

/*
 * __wt_cache_read --
 *	Read a page from the file.
 */
int
__wt_cache_read(WT_SESSION_IMPL *session, WT_PAGE *parent, WT_REF *ref)
{
	WT_DECL_RET;
	WT_ITEM tmp;
	WT_PAGE *page;
	WT_PAGE_STATE previous_state;
	uint32_t size;
	const uint8_t *addr;

	page = NULL;

	/*
	 * We don't pass in an allocated buffer to the underlying block read
	 * function, force allocation of new memory of the appropriate size.
	 */
	WT_CLEAR(tmp);

	/*
	 * Attempt to set the state to WT_REF_READING; if successful, we've
	 * won the race, read the page.
	 */
	if (WT_ATOMIC_CAS(ref->state, WT_REF_DISK, WT_REF_READING))
		previous_state = WT_REF_DISK;
	else if (WT_ATOMIC_CAS(ref->state, WT_REF_DELETED, WT_REF_READING))
		previous_state = WT_REF_DELETED;
	else
		return (0);

	/* Get the address and read the page. */
	__wt_get_addr(parent, ref, &addr, &size);
	WT_ERR(__wt_bm_read(session, &tmp, addr, size));

	/* Build the in-memory version of the page. */
	WT_ERR(__wt_page_inmem(session, parent, ref, tmp.mem, &page));

	WT_VERBOSE_ERR(session, read,
	    "page %p: %s", page, __wt_page_type_string(page->type));

	/* If the page was already deleted, instantiate that information. */
	if (previous_state == WT_REF_DELETED)
		WT_ERR(__cache_read_row_deleted(session, ref, page));

	ref->page = page;
	WT_PUBLISH(ref->state, WT_REF_MEM);
	return (0);

err:	WT_PUBLISH(ref->state, previous_state);

	/*
	 * If the function building an in-memory version of the page failed,
	 * it discarded the page, but not the disk image.  Discard the page
	 * and separately discard the disk image.
	 */
	if (page != NULL)
		__wt_page_out(session, &page, WT_PAGE_FREE_IGNORE_DISK);

	__wt_buf_free(session, &tmp);
	return (ret);
}
