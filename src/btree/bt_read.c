/*-
 * Copyright (c) 2008-2014 WiredTiger, Inc.
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
__cache_read_row_deleted(WT_SESSION_IMPL *session, WT_REF *ref)
{
	WT_BTREE *btree;
	WT_DECL_RET;
	WT_PAGE *page;
	WT_PAGE_DELETED *page_del;
	WT_UPDATE **upd_array, *upd;
	uint32_t i;

	btree = S2BT(session);
	page = ref->page;
	page_del = ref->page_del;

	/*
	 * Give the page a modify structure.
	 *
	 * If the tree is already dirty and so will be written, mark the page
	 * dirty.  (We'd like to free the deleted pages, but if the handle is
	 * read-only or if the application never modifies the tree, we're not
	 * able to do so.)
	 */
	if (btree->modified) {
		WT_RET(__wt_page_modify_init(session, page));
		__wt_page_modify_set(session, page);
	}

	/*
	 * An operation is accessing a "deleted" page, and we're building an
	 * in-memory version of the page (making it look like all entries in
	 * the page were individually updated by a remove operation).  There
	 * are two cases where we end up here:
	 *
	 * First, a running transaction used a truncate call to delete the page
	 * without reading it, in which case the page reference includes a
	 * structure with a transaction ID; the page we're building might split
	 * in the future, so we update that structure to include references to
	 * all of the update structures we create, so the transaction can abort.
	 *
	 * Second, a truncate call deleted a page and the truncate committed,
	 * but an older transaction in the system forced us to keep the old
	 * version of the page around, then we crashed and recovered, and now
	 * we're being forced to read that page.
	 *
	 * In the first case, we have a page reference structure, in the second
	 * second, we don't.
	 *
	 * Allocate the per-reference update array; in the case of instantiating
	 * a page, deleted by a running transaction that might eventually abort,
	 * we need a list of the update structures so we can do that abort.  The
	 * hard case is if a page splits: the update structures might be moved
	 * to different pages, and we still have to find them all for an abort.
	 */

	if (page_del != NULL)
		WT_RET(__wt_calloc_def(
		    session, page->pg_row_entries + 1, &page_del->update_list));

	/* Allocate the per-page update array. */
	WT_ERR(__wt_calloc_def(session, page->pg_row_entries, &upd_array));
	page->pg_row_upd = upd_array;

	/*
	 * Fill in the per-reference update array with references to update
	 * structures, fill in the per-page update array with references to
	 * deleted items.
	 */
	for (i = 0; i < page->pg_row_entries; ++i) {
		WT_ERR(__wt_calloc_def(session, 1, &upd));
		WT_UPDATE_DELETED_SET(upd);

		if (page_del == NULL)
			upd->txnid = WT_TXN_NONE;	/* Globally visible */
		else {
			upd->txnid = page_del->txnid;
			page_del->update_list[i] = upd;
		}

		upd->next = upd_array[i];
		upd_array[i] = upd;
	}

	__wt_cache_page_inmem_incr(session, page,
	    page->pg_row_entries * (sizeof(WT_UPDATE *) + sizeof(WT_UPDATE)));

	return (0);

err:	/*
	 * There's no need to free the page update structures on error, our
	 * caller will discard the page and do that work for us.  We could
	 * similarly leave the per-reference update array alone because it
	 * won't ever be used by any page that's not in-memory, but cleaning
	 * it up makes sense, especially if we come back in to this function
	 * attempting to instantiate this page again.
	 */
	if (page_del != NULL)
		__wt_free(session, page_del->update_list);
	return (ret);
}

/*
 * __wt_cache_read --
 *	Read a page from the file.
 */
int
__wt_cache_read(WT_SESSION_IMPL *session, WT_REF *ref)
{
	WT_DECL_RET;
	WT_ITEM tmp;
	WT_PAGE *page;
	WT_PAGE_STATE previous_state;
	size_t addr_size;
	const uint8_t *addr;

	page = NULL;

	/*
	 * Don't pass an allocated buffer to the underlying block read function,
	 * force allocation of new memory of the appropriate size.
	 */
	WT_CLEAR(tmp);

	/*
	 * Attempt to set the state to WT_REF_READING for normal reads, or
	 * WT_REF_LOCKED, for deleted pages.  If successful, we've won the
	 * race, read the page.
	 */
	if (WT_ATOMIC_CAS(ref->state, WT_REF_DISK, WT_REF_READING))
		previous_state = WT_REF_DISK;
	else if (WT_ATOMIC_CAS(ref->state, WT_REF_DELETED, WT_REF_LOCKED))
		previous_state = WT_REF_DELETED;
	else
		return (0);

	/*
	 * Get the address: if there is no address, the page was deleted, but a
	 * subsequent search or insert is forcing re-creation of the name space.
	 * Otherwise, there's an address, read the backing disk page and build
	 * an in-memory version of the page.
	 */
	WT_ERR(__wt_ref_info(session, ref, &addr, &addr_size, NULL));
	if (addr == NULL) {
		WT_ASSERT(session, previous_state == WT_REF_DELETED);

		WT_ERR(__wt_btree_new_leaf_page(session, &page));
		ref->page = page;
	} else {
		/* Read the backing disk page. */
		WT_ERR(__wt_bt_read(session, &tmp, addr, addr_size));

		/* Build the in-memory version of the page. */
		WT_ERR(__wt_page_inmem(session, ref, tmp.mem,
		    F_ISSET(&tmp, WT_ITEM_MAPPED) ?
		    WT_PAGE_DISK_MAPPED : WT_PAGE_DISK_ALLOC, &page));

		/* If the page was deleted, instantiate that information. */
		if (previous_state == WT_REF_DELETED)
			WT_ERR(__cache_read_row_deleted(session, ref));
	}

	WT_VERBOSE_ERR(session, read,
	    "page %p: %s", page, __wt_page_type_string(page->type));

	WT_PUBLISH(ref->state, WT_REF_MEM);
	return (0);

err:	WT_PUBLISH(ref->state, previous_state);

	/*
	 * If the function building an in-memory version of the page failed,
	 * it discarded the page, but not the disk image.  Discard the page
	 * and separately discard the disk image in all cases.
	 */
	if (ref->page != NULL)
		__wt_ref_out(session, ref);
	__wt_buf_free(session, &tmp);

	return (ret);
}
