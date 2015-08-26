/*-
 * Copyright (c) 2014-2015 MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

#define	WT_MAX_PREFIX_SIZE						\
    (sizeof(char) +							\
    sizeof(uint32_t) + sizeof(uint8_t) + WT_BTREE_MAX_ADDR_COOKIE)

/*
 * __las_build_prefix --
 *	Build the unique file/address prefix.
 */
static void
__las_build_prefix(WT_SESSION_IMPL *session, uint32_t btree_id,
    const uint8_t *addr, size_t addr_size, uint8_t *prefix, size_t *prefix_lenp)
{
	size_t len;
	void *p;

	/*
	 * Build the page's unique key prefix we'll search for in the lookaside
	 * table, based on the file's ID and the page's block address.
	 */
	p = prefix;
	*(char *)p = WT_LAS_RECONCILE_UPDATE;
	p = (uint8_t *)p + sizeof(char);
	memcpy(p, &btree_id, sizeof(uint32_t));
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
__wt_las_remove_block(WT_SESSION_IMPL *session, WT_CURSOR *cursor_arg,
    uint32_t btree_id, const uint8_t *addr, size_t addr_size)
{
	WT_CURSOR *cursor;
	WT_DECL_ITEM(klas);
	WT_DECL_RET;
	size_t prefix_len;
	int exact, reset_evict;
	uint8_t prefix[WT_MAX_PREFIX_SIZE];

	cursor = NULL;
	reset_evict = 0;		/* [-Werror=maybe-uninitialized] */

	/* Build the unique file/address prefix. */
	__las_build_prefix(
	    session, btree_id, addr, addr_size, prefix, &prefix_len);

	/* Copy the unique prefix into the key. */
	WT_RET(__wt_scr_alloc(session, WT_MAX_PREFIX_SIZE, &klas));
	memcpy(klas->mem, prefix, prefix_len);
	klas->size = prefix_len;

	/*
	 * If not provided a lookaside table cursor, open one. Search for the
	 * matching prefix and step through all matching records, removing them.
	 */
	if ((cursor = cursor_arg) == NULL)
		WT_ERR(__wt_las_cursor(session, &cursor, &reset_evict));
	cursor->set_key(cursor, klas);
	if ((ret = cursor->search_near(cursor, &exact)) == 0 && exact < 0)
		ret = cursor->next(cursor);
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
		 * Cursor opened overwrite=true: it won't return WT_NOTFOUND
		 * if another thread removes the record before we do, and the
		 * cursor remains positioned in that case.
		 */
		WT_ERR(cursor->remove(cursor));
	}
	WT_ERR_NOTFOUND_OK(ret);

err:	if (cursor_arg == NULL)
		WT_TRET(__wt_las_cursor_close(session, &cursor, reset_evict));

	__wt_scr_free(session, &klas);
	return (ret);
}

/*
 * __col_instantiate --
 *	Update a column-store page entry based on a lookaside table update list.
 */
static int
__col_instantiate(WT_SESSION_IMPL *session,
    uint64_t recno, WT_REF *ref, WT_CURSOR_BTREE *cbt, WT_UPDATE *upd)
{
	/* Search the page and add updates. */
	WT_RET(__wt_col_search(session, recno, ref, cbt));
	WT_RET(__wt_col_modify(session, cbt, recno, NULL, upd, 0));
	return (0);
}

/*
 * __row_instantiate --
 *	Update a row-store page entry based on a lookaside table update list.
 */
static int
__row_instantiate(WT_SESSION_IMPL *session,
    WT_ITEM *key, WT_REF *ref, WT_CURSOR_BTREE *cbt, WT_UPDATE *upd)
{
	/* Search the page and add updates. */
	WT_RET(__wt_row_search(session, key, ref, cbt, 1));
	WT_RET(__wt_row_modify(session, cbt, key, NULL, upd, 0));
	return (0);
}

/*
 * __las_page_instantiate --
 *	Instantiate lookaside update records in a recently read page.
 */
static int
__las_page_instantiate(
    WT_SESSION_IMPL *session, WT_REF *ref, uint32_t read_id,
    const uint8_t *addr, size_t addr_size, int *need_las_removep)
{
	WT_CURSOR *cursor;
	WT_CURSOR_BTREE cbt;
	WT_DECL_ITEM(current_key);
	WT_DECL_ITEM(klas);
	WT_DECL_ITEM(vlas);
	WT_DECL_RET;
	WT_PAGE *page;
	WT_UPDATE *first_upd, *last_upd, *upd;
	size_t incr, prefix_len, total_incr;
	uint32_t key_len, upd_size;
	uint64_t current_recno, recno, txnid;
	uint8_t prefix[WT_MAX_PREFIX_SIZE];
	int exact, reset_evict;
	void *p;

	*need_las_removep = 0;

	cursor = NULL;
	page = ref->page;
	first_upd = last_upd = upd = NULL;
	total_incr = 0;
	current_recno = recno = WT_RECNO_OOB;
	reset_evict = 0;		/* [-Werror=maybe-uninitialized] */

	__wt_btcur_init(session, &cbt);
	__wt_btcur_open(&cbt);

	WT_ERR(__wt_scr_alloc(session, 0, &current_key));
	WT_ERR(__wt_scr_alloc(session, WT_MAX_PREFIX_SIZE, &klas));
	WT_ERR(__wt_scr_alloc(session, 0, &vlas));

	/* Build the unique file/address prefix. */
	__las_build_prefix(
	    session, read_id, addr, addr_size, prefix, &prefix_len);

	/* Copy the unique prefix into the key. */
	memcpy(klas->mem, prefix, prefix_len);
	klas->size = prefix_len;

	/*
	 * Open a lookaside table cursor and search for the matching prefix,
	 * stepping through any matching records.
	 *
	 * The lookaside records are in key and update order, that is, there
	 * will be a set of in-order updates for a key, then another set of
	 * in-order updates for a subsequent key. We process all of the updates
	 * for a key and then insert those updates into the page, then all the
	 * updates for the next key, and so on.
	 */
	WT_ERR(__wt_las_cursor(session, &cursor, &reset_evict));
	cursor->set_key(cursor, klas);
	if ((ret = cursor->search_near(cursor, &exact)) == 0 && exact < 0)
		ret = cursor->next(cursor);
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
		 * If we find/instantiate records, we'll want to remove them
		 * once this page instantiation is successful.
		 */
		*need_las_removep = 1;

		/*
		 * Skip to the on-page transaction ID stored in the key; if it's
		 * globally visible, we no longer need this record, the on-page
		 * record is just as good.
		 */
		p = (uint8_t *)klas->data + prefix_len;
		memcpy(&txnid, p, sizeof(uint64_t));
		p = (uint8_t *)p + sizeof(uint64_t);
		if (__wt_txn_visible_all(session, txnid))
			continue;

		/*
		 * Skip past the counter (it's only needed to ensure records are
		 * read in their original, listed order), leaving p referencing
		 * the first byte of the key.
		 */
		p = (uint8_t *)p + sizeof(uint32_t);

		/* Allocate the WT_UPDATE structure. */
		WT_ERR(cursor->get_value(cursor, &txnid, &upd_size, vlas));
		WT_ERR(__wt_update_alloc(session,
		    (upd_size == WT_UPDATE_DELETED_VALUE) ? NULL : vlas,
		    &upd, &incr));
		total_incr += incr;
		upd->txnid = txnid;

		switch (page->type) {
		case WT_PAGE_COL_FIX:
		case WT_PAGE_COL_VAR:
			memcpy(&recno, p, sizeof(uint64_t));
			if (current_recno == recno)
				break;

			if (first_upd != NULL) {
				WT_ERR(__col_instantiate(session,
				    current_recno, ref, &cbt, first_upd));
				first_upd = NULL;
			}
			current_recno = recno;
			break;
		case WT_PAGE_ROW_LEAF:
			memcpy(&key_len, p, sizeof(uint32_t));
			p = (uint8_t *)p + sizeof(uint32_t);
			if (current_key->size == key_len && memcmp(
			    current_key->data, p, current_key->size) == 0)
				break;

			if (first_upd != NULL) {
				WT_ERR(__row_instantiate(session,
				    current_key, ref, &cbt, first_upd));
				first_upd = NULL;
			}
			WT_ERR(__wt_buf_set(session, current_key, p, key_len));
			break;
		WT_ILLEGAL_VALUE_ERR(session);
		}

		/* Append the latest update to the list. */
		if (first_upd == NULL)
			first_upd = last_upd = upd;
		else {
			last_upd->next = upd;
			last_upd = upd;
		}
		upd = NULL;
	}
	WT_ERR_NOTFOUND_OK(ret);

	/* Insert the last set of updates, if any. */
	if (first_upd != NULL)
		switch (page->type) {
		case WT_PAGE_COL_FIX:
		case WT_PAGE_COL_VAR:
			WT_ERR(__col_instantiate(session,
			    current_recno, ref, &cbt, first_upd));
			first_upd = NULL;
			break;
		case WT_PAGE_ROW_LEAF:
			WT_ERR(__row_instantiate(session,
			    current_key, ref, &cbt, first_upd));
			first_upd = NULL;
			break;
		WT_ILLEGAL_VALUE_ERR(session);
		}

	/* Discard the cursor. */
	WT_ERR(__wt_las_cursor_close(session, &cursor, reset_evict));

	if (total_incr != 0) {
		__wt_cache_page_inmem_incr(session, page, total_incr);

		/*
		 * We've modified/dirtied the page, but that's not necessary and
		 * if we keep the page clean, it's easier to evict. We leave the
		 * lookaside table updates in place, so if we evict this page
		 * without dirtying it, any future instantiation of it will find
		 * the records it needs. If the page is dirtied before eviction,
		 * then we'll write any needed lookaside table records for the
		 * new location of the page.
		 */
		__wt_page_modify_clear(session, page);
	}

err:	WT_TRET(__wt_las_cursor_close(session, &cursor, reset_evict));
	WT_TRET(__wt_btcur_close(&cbt, 1));

	/*
	 * On error, upd points to a single unlinked WT_UPDATE structure,
	 * first_upd points to a list.
	 */
	if (upd != NULL)
		__wt_free(session, upd);
	if (first_upd != NULL)
		__wt_free_update_list(session, first_upd);

	__wt_scr_free(session, &current_key);
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
	WT_BTREE *btree;
	WT_DECL_RET;
	WT_ITEM tmp;
	WT_PAGE *page;
	size_t addr_size;
	uint32_t previous_state;
	int need_las_remove;
	const uint8_t *addr;

	btree = S2BT(session);
	page = NULL;
	need_las_remove = 0;

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
	if (__wt_atomic_casv32(&ref->state, WT_REF_DISK, WT_REF_READING))
		previous_state = WT_REF_DISK;
	else if (__wt_atomic_casv32(&ref->state, WT_REF_DELETED, WT_REF_LOCKED))
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

		/*
		 * Instantiate updates from the database's lookaside table. The
		 * flag might have been set a long time ago, and we only care
		 * if the lookaside table is currently active, check that before
		 * doing any work.
		 */
		dsk = tmp.data;
		if (F_ISSET(dsk, WT_PAGE_LAS_UPDATE) &&
		    __wt_las_is_written(session)) {
			WT_STAT_FAST_CONN_INCR(session, cache_read_lookaside);
			WT_STAT_FAST_DATA_INCR(session, cache_read_lookaside);

			WT_ERR(__las_page_instantiate(session, ref,
			    btree->id, addr, addr_size, &need_las_remove));
		}
	}

	WT_ERR(__wt_verbose(session, WT_VERB_READ,
	    "page %p: %s", page, __wt_page_type_string(page->type)));

	/*
	 * We successfully instantiated the page, remove any update entries from
	 * the lookaside table.
	 */
	if (need_las_remove)
		WT_ERR(__wt_las_remove_block(
		    session, NULL, btree->id, addr, addr_size));

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
