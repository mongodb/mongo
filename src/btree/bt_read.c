/*-
 * Copyright (c) 2014-2015 MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __las_page_instantiate --
 *	Instantiate lookaside update records in a recently read page.
 */
static int
__las_page_instantiate(WT_SESSION_IMPL *session,
    WT_REF *ref, const uint8_t *addr, size_t addr_size)
{
	WT_BTREE *btree;
	WT_CURSOR_BTREE cbt;
	WT_DECL_ITEM(klas);
	WT_DECL_ITEM(vlas);
	WT_DECL_RET;
	WT_PAGE *page;
	WT_UPDATE *upd;
	size_t incr, prefix_len, total_incr, upd_size, saved_size;
	uint32_t key_len;
	uint64_t recno, txnid;
	uint8_t prefix_key[100];
	void *p;
	const void *saved_data, *t;

	btree = S2BT(session);
	page = ref->page;
	upd = NULL;
	total_incr = 0;
	recno = 0;			/* [-Werror=maybe-uninitialized] */

	__wt_btcur_init(session, &cbt);
	__wt_btcur_open(&cbt);

	WT_ERR(__wt_scr_alloc(session, addr_size + 100, &klas));
	WT_ERR(__wt_scr_alloc(session, 0, &vlas));

	/*
	 * Build the page's unique key prefix we'll search for in the lookaside
	 * table, based on the file's ID and the page's block address.
	 */
	p = prefix_key;
	*(char *)p = WT_LAS_RECONCILE_UPDATE;
	p = (uint8_t *)p + sizeof(char);
	memcpy(p, &btree->id, sizeof(uint32_t));
	p = (uint8_t *)p + sizeof(uint32_t);
	*(uint8_t *)p = (uint8_t)addr_size;
	p = (uint8_t *)p + sizeof(uint8_t);
	memcpy(p, addr, addr_size);
	p = (uint8_t *)p + addr_size;
	prefix_len =
	    sizeof(char) + sizeof(uint32_t) + sizeof(uint8_t) + addr_size;
	WT_ASSERT(session, WT_PTRDIFF(p, prefix_key) == prefix_len);

	/* Copy the unique prefix into the key. */
	memcpy(klas->mem, prefix_key, prefix_len);
	klas->size = prefix_len;

	while ((ret = __wt_las_search(session, klas, vlas)) == 0) {
		/*
		 * Confirm the search using the unique prefix; if not a match,
		 * we're done searching for records for this page.
		 */
		if (klas->size <= prefix_len ||
		    memcmp(klas->data, prefix_key, prefix_len) != 0)
			break;

		/* Make sure we have a local copy of the record. */
		if (!WT_DATA_IN_ITEM(klas))
			WT_ERR(__wt_buf_set(
			    session, klas, klas->data, klas->size));
		saved_data = klas->data;
		saved_size = klas->size;

		/*
		 * Skip to the on-page transaction ID stored in the key; if it's
		 * globally visible, we no longer need this record, the on-page
		 * record is just as good.
		 */
		p = (uint8_t *)klas->mem + prefix_len;
		memcpy(&txnid, p, sizeof(uint64_t));
		p = (uint8_t *)p + sizeof(uint64_t);
		if (__wt_txn_visible_all(session, txnid))
			goto skip_row;

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
			klas->data = (uint8_t *)p + sizeof(uint32_t);
			klas->size = key_len;
			break;
		WT_ILLEGAL_VALUE_ERR(session);
		}

		/* Crack the value. */
		t = vlas->data;
		memcpy(&txnid, t, sizeof(uint64_t));
		t = (uint8_t *)t + sizeof(uint64_t);
		memcpy(&upd_size, t, sizeof(uint32_t));
		t = (uint8_t *)t + sizeof(uint32_t);

		/* Allocate the WT_UPDATE structure. */
		if (upd_size == WT_UPDATE_DELETED_VALUE)
			WT_ERR(__wt_update_alloc(session, NULL, &upd, &incr));
		else {
			vlas->data = t;
			vlas->size = upd_size;
			WT_ERR(__wt_update_alloc(session, vlas, &upd, &incr));
		}
		total_incr += incr;
		upd->txnid = txnid;

		/* Search the page and insert the update structure. */
		switch (page->type) {
		case WT_PAGE_COL_FIX:
		case WT_PAGE_COL_VAR:
			/* Search the page. */
			WT_ERR(__wt_col_search(session, recno, ref, &cbt));

			/* Apply the modification. */
			WT_ERR(__wt_col_modify(
			    session, &cbt, recno, NULL, upd, 0));
			break;
		case WT_PAGE_ROW_LEAF:
			/* Search the page. */
			WT_ERR(__wt_row_search(session, klas, ref, &cbt, 1));

			/* Apply the modification. */
			WT_ERR(
			    __wt_row_modify(session, &cbt, klas, NULL, upd, 0));
			break;
		WT_ILLEGAL_VALUE_ERR(session);
		}

		/* Don't discard any appended structures on error. */
		upd = NULL;

		/*
		 * Reset the key WT_ITEM to reference the start of the stored
		 * record, then remove the record from the lookaside table.
		 *
		 * After removing the stored record, reset the key's WT_ITEM
		 * length to the prefix length to find the next record.
		 *
		 * KEITH: we need to consider error handling, or panic if the
		 * remove fails?
		 */
skip_row:	klas->data = saved_data;
		klas->size = saved_size;
		WT_ERR(__wt_las_remove(session, klas));
		klas->size = prefix_len;
	}
	WT_ERR_NOTFOUND_OK(ret);

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

	/*
	 * KEITH: don't release the page, we don't have a hazard pointer on it;
	 * why is this is necessary, why doesn't __split_multi_inmem have the
	 * same problem?
	 */
err:	cbt.ref = NULL;
	WT_TRET(__wt_btcur_close(&cbt));

	if (upd != NULL)
		__wt_free(session, upd);

	__wt_scr_free(session, &klas);
	__wt_scr_free(session, &vlas);

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
