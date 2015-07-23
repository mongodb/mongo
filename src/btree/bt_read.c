/*-
 * Copyright (c) 2014-2015 MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __las_build_prefix --
 *	Build the unique file/address prefix.
 */
static void
__las_build_prefix(WT_SESSION_IMPL *session,
    const uint8_t *addr, size_t addr_size, uint8_t *prefix, size_t *prefix_lenp)
{
	WT_BTREE *btree;
	size_t len;
	void *p;

	btree = S2BT(session);

	/*
	 * Build the page's unique key prefix we'll search for in the lookaside
	 * table, based on the file's ID and the page's block address.
	 */
	p = prefix;
	*(char *)p = WT_LAS_RECONCILE_UPDATE;
	p = (uint8_t *)p + sizeof(char);
	memcpy(p, &btree->id, sizeof(uint32_t));
	p = (uint8_t *)p + sizeof(uint32_t);
	*(uint8_t *)p = (uint8_t)addr_size;
	p = (uint8_t *)p + sizeof(uint8_t);
	memcpy(p, addr, addr_size);
	p = (uint8_t *)p + addr_size;
	len = sizeof(char) + sizeof(uint32_t) + sizeof(uint8_t) + addr_size;
	WT_ASSERT(session, WT_PTRDIFF(p, prefix) == len);

	*prefix_lenp = len;
}

/*
 * __wt_las_remove_block --
 *	Remove all records matching a key prefix from the lookaside store.
 */
int
__wt_las_remove_block(
    WT_SESSION_IMPL *session, const uint8_t *addr, size_t addr_size)
{
	WT_CURSOR *cursor;
	WT_DECL_ITEM(klas);
	WT_DECL_RET;
	size_t prefix_len;
	int exact;
	uint32_t saved_flags;
	uint8_t prefix[100];

	cursor = NULL;

	/*
	 * Called whenever a block is freed; if the lookaside store isn't yet
	 * open, there's no work to do.
	 */
	if (S2C(session)->las_cursor == 0)
		return (0);

	/* Build the unique file/address prefix. */
	__las_build_prefix(session, addr, addr_size, prefix, &prefix_len);

	/* Copy the unique prefix into the key. */
	WT_RET(__wt_scr_alloc(session, addr_size + 100, &klas));
	memcpy(klas->mem, prefix, prefix_len);
	klas->size = prefix_len;

	WT_RET(__wt_las_cursor(session, &cursor, &saved_flags));

	cursor->set_key(cursor, klas);
	while ((ret = cursor->search_near(cursor, &exact)) == 0) {
		WT_ERR(cursor->get_key(cursor, klas));

		/*
		 * Confirm the search using the unique prefix; if not a match,
		 * we're done searching for records for this page.
		 */
		if (klas->size <= prefix_len ||
		    memcmp(klas->data, prefix, prefix_len) != 0)
			break;

		/* Make sure we have a local copy of the record. */
		if (!WT_DATA_IN_ITEM(klas))
			WT_ERR(__wt_buf_set(
			    session, klas, klas->data, klas->size));

		WT_ERR(cursor->remove(cursor));
		klas->size = prefix_len;
	}
	WT_ERR_NOTFOUND_OK(ret);

err:	WT_TRET(__wt_las_cursor_close(session, &cursor, saved_flags));

	__wt_scr_free(session, &klas);
	return (ret);
}

/*
 * __las_page_instantiate --
 *	Instantiate lookaside update records in a recently read page.
 */
static int
__las_page_instantiate(WT_SESSION_IMPL *session,
    WT_REF *ref, const uint8_t *addr, size_t addr_size)
{
	WT_CURSOR *cursor;
	WT_CURSOR_BTREE cbt;
	WT_DECL_ITEM(klas);
	WT_DECL_RET;
	WT_ITEM(vlas);
	WT_PAGE *page;
	WT_UPDATE *first_upd, *last_upd, *upd;
	size_t incr, prefix_len, total_incr;
	uint32_t key_len, saved_flags, upd_size;
	uint64_t recno, txnid;
	uint8_t prefix[100];
	int exact;
	void *p;

	cursor = NULL;
	page = ref->page;
	first_upd = last_upd = upd = NULL;
	total_incr = 0;
	recno = 0;			/* [-Werror=maybe-uninitialized] */
	saved_flags = 0;		/* [-Werror=maybe-uninitialized] */

	__wt_btcur_init(session, &cbt);
	__wt_btcur_open(&cbt);

	WT_ERR(__wt_scr_alloc(session, addr_size + 100, &klas));

	/* Build the unique file/address prefix. */
	__las_build_prefix(session, addr, addr_size, prefix, &prefix_len);

	/* Copy the unique prefix into the key. */
	memcpy(klas->mem, prefix, prefix_len);
	klas->size = prefix_len;

	/*
	 * Open a lookaside table cursor and search for the matching prefix.
	 * If we don't find any records, we're done.
	 */
	WT_ERR(__wt_las_cursor(session, &cursor, &saved_flags));
	cursor->set_key(cursor, klas);
	if ((ret = cursor->search_near(cursor, &exact)) != 0 || exact < 0) {
		if (ret == WT_NOTFOUND)
			ret = 0;
		goto done;
	}

	/* Step through the lookaside records. */
	for (; ret == 0; ret = cursor->next(cursor)) {
		WT_ERR(cursor->get_key(cursor, klas));

		/*
		 * Confirm the search using the unique prefix; if not a match,
		 * we're done searching for records for this page.
		 */
		if (klas->size <= prefix_len ||
		    memcmp(klas->data, prefix, prefix_len) != 0)
			break;

		/*
		 * Skip to the on-page transaction ID stored in the key; if it's
		 * globally visible, we no longer need this record, the on-page
		 * record is just as good.
		 */
		p = (uint8_t *)klas->mem + prefix_len;
		memcpy(&txnid, p, sizeof(uint64_t));
		p = (uint8_t *)p + sizeof(uint64_t);
		if (__wt_txn_visible_all(session, txnid))
			continue;

		/*
		 * Skip past the counter (it's only needed to ensure records are
		 * read in their original, listed order), then crack the key.
		 */
		p = (uint8_t *)p + sizeof(uint32_t);
		switch (page->type) {
		case WT_PAGE_COL_FIX:
		case WT_PAGE_COL_VAR:
			memcpy(&recno, p, sizeof(uint64_t));
			break;
		case WT_PAGE_ROW_LEAF:
			memcpy(&key_len, p, sizeof(uint32_t));
			p = (uint8_t *)p + sizeof(uint32_t);
			klas->data = p;
			klas->size = key_len;
			break;
		WT_ILLEGAL_VALUE_ERR(session);
		}

		/* Allocate the WT_UPDATE structure. */
		WT_ERR(cursor->get_value(cursor, &txnid, &upd_size, &vlas));
		WT_ERR(__wt_update_alloc(session,
		    (upd_size == WT_UPDATE_DELETED_VALUE) ? NULL : &vlas,
		    &upd, &incr));
		total_incr += incr;
		upd->txnid = txnid;

		if (first_upd == NULL)
			first_upd = last_upd = upd;
		else {
			last_upd->next = upd;
			last_upd = upd;
		}
	}
	WT_ERR_NOTFOUND_OK(ret);

	/* Search the page and insert the updates. */
	if (first_upd != NULL) {
		switch (page->type) {
		case WT_PAGE_COL_FIX:
		case WT_PAGE_COL_VAR:
			/* Search the page. */
			WT_ERR(__wt_col_search(session, recno, ref, &cbt));

			/* Apply the modification. */
			WT_ERR(__wt_col_modify(
			    session, &cbt, recno, NULL, first_upd, 0));
			break;
		case WT_PAGE_ROW_LEAF:
			/* Search the page. */
			WT_ERR(__wt_row_search(session, klas, ref, &cbt, 1));

			/* Apply the modification. */
			WT_ERR(__wt_row_modify(
			    session, &cbt, klas, NULL, first_upd, 0));
			break;
		WT_ILLEGAL_VALUE_ERR(session);
		}

		/* Don't discard any appended structures on error. */
		first_upd = NULL;
	}

	/* Discard the cursor. */
	WT_ERR(__wt_las_cursor_close(session, &cursor, saved_flags));

	/* Remove this block's entries from the lookaside table. */
	WT_ERR(__wt_las_remove_block(session, addr, addr_size));

	if (total_incr != 0) {
		__wt_cache_page_inmem_incr(session, page, total_incr);

		/*
		 * We modified the page above, which will have set the first
		 * dirty transaction to the last transaction currently running.
		 * However, the updates we installed may be older than that.
		 * Set the first dirty transaction to an impossibly old value
		 * so this page is never skipped in a checkpoint.
		 *
		 * KEITH: is this correct? We don't care about checkpoints, we
		 * care about older readers in the system.
		 */
		page->modify->first_dirty_txn = WT_TXN_FIRST;
	}

done: err:
	WT_TRET(__wt_las_cursor_close(session, &cursor, saved_flags));

	/*
	 * KEITH: don't release the page, we don't have a hazard pointer on it;
	 * why is this is necessary, why doesn't __split_multi_inmem have the
	 * same problem?
	 */
	cbt.ref = NULL;
	WT_TRET(__wt_btcur_close(&cbt));

	if (first_upd != NULL)
		__wt_free_update_list(session, first_upd);

	__wt_scr_free(session, &klas);

	return (ret);
}

/*
 * __wt_cache_read --
 *	Read a page from the file.
 */
int
__wt_cache_read(WT_SESSION_IMPL *session, WT_REF *ref)
{
	const WT_PAGE_HEADER *dsk;
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
	if (WT_ATOMIC_CAS4(ref->state, WT_REF_DISK, WT_REF_READING))
		previous_state = WT_REF_DISK;
	else if (WT_ATOMIC_CAS4(ref->state, WT_REF_DELETED, WT_REF_LOCKED))
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
		/*
		 * Read the page, then build the in-memory version of the page.
		 * Clear any local reference to an allocated copy of the disk
		 * image on return, the page steals it.
		 */
		WT_ERR(__wt_bt_read(session, &tmp, addr, addr_size));
		WT_ERR(__wt_page_inmem(session, ref, tmp.data, tmp.memsize,
		    WT_DATA_IN_ITEM(&tmp) ?
		    WT_PAGE_DISK_ALLOC : WT_PAGE_DISK_MAPPED, &page));
		tmp.mem = NULL;

		/* If the page was deleted, instantiate that information. */
		if (previous_state == WT_REF_DELETED)
			WT_ERR(__wt_delete_page_instantiate(session, ref));

		/* Instantiate entries from the database's look-aside buffer. */
		dsk = tmp.data;
		if (F_ISSET(dsk, WT_PAGE_LAS_UPDATE))
			WT_ERR(__las_page_instantiate(
			    session, ref, addr, addr_size));
	}

	WT_ERR(__wt_verbose(session, WT_VERB_READ,
	    "page %p: %s", page, __wt_page_type_string(page->type)));

	WT_PUBLISH(ref->state, WT_REF_MEM);
	return (0);

err:	/*
	 * If the function building an in-memory version of the page failed,
	 * it discarded the page, but not the disk image.  Discard the page
	 * and separately discard the disk image in all cases.
	 */
	if (ref->page != NULL)
		__wt_ref_out(session, ref);
	WT_PUBLISH(ref->state, previous_state);

	__wt_buf_free(session, &tmp);

	return (ret);
}
