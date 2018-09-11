/*-
 * Copyright (c) 2014-2018 MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __cursor_fix_append_next --
 *	Return the next entry on the append list.
 */
static inline int
__cursor_fix_append_next(WT_CURSOR_BTREE *cbt, bool newpage)
{
	WT_SESSION_IMPL *session;
	WT_UPDATE *upd;

	session = (WT_SESSION_IMPL *)cbt->iface.session;

	if (newpage) {
		if ((cbt->ins = WT_SKIP_FIRST(cbt->ins_head)) == NULL)
			return (WT_NOTFOUND);
	} else
		if (cbt->recno >= WT_INSERT_RECNO(cbt->ins) &&
		    (cbt->ins = WT_SKIP_NEXT(cbt->ins)) == NULL)
			return (WT_NOTFOUND);

	/*
	 * This code looks different from the cursor-previous code. The append
	 * list may be preceded by other rows, which means the cursor's recno
	 * will be set to a value and we simply want to increment it. If the
	 * cursor's recno is NOT set, we're starting an iteration in a tree with
	 * only appended items. In that case, recno will be 0 and happily enough
	 * the increment will set it to 1, which is correct.
	 */
	__cursor_set_recno(cbt, cbt->recno + 1);

	/*
	 * Fixed-width column store appends are inherently non-transactional.
	 * Even a non-visible update by a concurrent or aborted transaction
	 * changes the effective end of the data.  The effect is subtle because
	 * of the blurring between deleted and empty values, but ideally we
	 * would skip all uncommitted changes at the end of the data.  This
	 * doesn't apply to variable-width column stores because the implicitly
	 * created records written by reconciliation are deleted and so can be
	 * never seen by a read.
	 *
	 * The problem is that we don't know at this point whether there may be
	 * multiple uncommitted changes at the end of the data, and it would be
	 * expensive to check every time we hit an aborted update.  If an
	 * insert is aborted, we simply return zero (empty), regardless of
	 * whether we are at the end of the data.
	 */
	if (cbt->recno < WT_INSERT_RECNO(cbt->ins)) {
		cbt->v = 0;
		cbt->iface.value.data = &cbt->v;
	} else {
		WT_RET(__wt_txn_read(session, cbt->ins->upd, &upd));
		if (upd == NULL) {
			cbt->v = 0;
			cbt->iface.value.data = &cbt->v;
		} else
			cbt->iface.value.data = upd->data;
	}
	cbt->iface.value.size = 1;
	return (0);
}

/*
 * __cursor_fix_next --
 *	Move to the next, fixed-length column-store item.
 */
static inline int
__cursor_fix_next(WT_CURSOR_BTREE *cbt, bool newpage)
{
	WT_BTREE *btree;
	WT_PAGE *page;
	WT_SESSION_IMPL *session;
	WT_UPDATE *upd;

	session = (WT_SESSION_IMPL *)cbt->iface.session;
	btree = S2BT(session);
	page = cbt->ref->page;
	upd = NULL;

	/* Initialize for each new page. */
	if (newpage) {
		cbt->last_standard_recno = __col_fix_last_recno(cbt->ref);
		if (cbt->last_standard_recno == 0)
			return (WT_NOTFOUND);
		__cursor_set_recno(cbt, cbt->ref->ref_recno);
		goto new_page;
	}

	/* Move to the next entry and return the item. */
	if (cbt->recno >= cbt->last_standard_recno)
		return (WT_NOTFOUND);
	__cursor_set_recno(cbt, cbt->recno + 1);

new_page:
	/* Check any insert list for a matching record. */
	cbt->ins_head = WT_COL_UPDATE_SINGLE(page);
	cbt->ins = __col_insert_search(
	    cbt->ins_head, cbt->ins_stack, cbt->next_stack, cbt->recno);
	if (cbt->ins != NULL && cbt->recno != WT_INSERT_RECNO(cbt->ins))
		cbt->ins = NULL;
	if (cbt->ins != NULL)
		WT_RET(__wt_txn_read(session, cbt->ins->upd, &upd));
	if (upd == NULL) {
		cbt->v = __bit_getv_recno(cbt->ref, cbt->recno, btree->bitcnt);
		cbt->iface.value.data = &cbt->v;
	} else
		cbt->iface.value.data = upd->data;
	cbt->iface.value.size = 1;
	return (0);
}

/*
 * __cursor_var_append_next --
 *	Return the next variable-length entry on the append list.
 */
static inline int
__cursor_var_append_next(WT_CURSOR_BTREE *cbt, bool newpage)
{
	WT_SESSION_IMPL *session;
	WT_UPDATE *upd;

	session = (WT_SESSION_IMPL *)cbt->iface.session;

	if (newpage) {
		cbt->ins = WT_SKIP_FIRST(cbt->ins_head);
		goto new_page;
	}

	for (;;) {
		cbt->ins = WT_SKIP_NEXT(cbt->ins);
new_page:	if (cbt->ins == NULL)
			return (WT_NOTFOUND);

		__cursor_set_recno(cbt, WT_INSERT_RECNO(cbt->ins));
		WT_RET(__wt_txn_read(session, cbt->ins->upd, &upd));
		if (upd == NULL)
			continue;
		if (upd->type == WT_UPDATE_TOMBSTONE) {
			if (upd->txnid != WT_TXN_NONE &&
			    __wt_txn_upd_visible_all(session, upd))
				++cbt->page_deleted_count;
			continue;
		}
		return (__wt_value_return(session, cbt, upd));
	}
	/* NOTREACHED */
}

/*
 * __cursor_var_next --
 *	Move to the next, variable-length column-store item.
 */
static inline int
__cursor_var_next(WT_CURSOR_BTREE *cbt, bool newpage)
{
	WT_CELL *cell;
	WT_CELL_UNPACK unpack;
	WT_COL *cip;
	WT_INSERT *ins;
	WT_PAGE *page;
	WT_SESSION_IMPL *session;
	WT_UPDATE *upd;
	uint64_t rle, rle_start;

	session = (WT_SESSION_IMPL *)cbt->iface.session;
	page = cbt->ref->page;

	rle_start = 0;			/* -Werror=maybe-uninitialized */

	/* Initialize for each new page. */
	if (newpage) {
		cbt->last_standard_recno = __col_var_last_recno(cbt->ref);
		if (cbt->last_standard_recno == 0)
			return (WT_NOTFOUND);
		__cursor_set_recno(cbt, cbt->ref->ref_recno);
		cbt->cip_saved = NULL;
		goto new_page;
	}

	/* Move to the next entry and return the item. */
	for (;;) {
		if (cbt->recno >= cbt->last_standard_recno)
			return (WT_NOTFOUND);
		__cursor_set_recno(cbt, cbt->recno + 1);

new_page:	/* Find the matching WT_COL slot. */
		if ((cip =
		    __col_var_search(cbt->ref, cbt->recno, &rle_start)) == NULL)
			return (WT_NOTFOUND);
		cbt->slot = WT_COL_SLOT(page, cip);

		/* Check any insert list for a matching record. */
		cbt->ins_head = WT_COL_UPDATE_SLOT(page, cbt->slot);
		cbt->ins = __col_insert_search_match(cbt->ins_head, cbt->recno);
		upd = NULL;
		if (cbt->ins != NULL)
			WT_RET(__wt_txn_read(session, cbt->ins->upd, &upd));
		if (upd != NULL) {
			if (upd->type == WT_UPDATE_TOMBSTONE) {
				if (upd->txnid != WT_TXN_NONE &&
				    __wt_txn_upd_visible_all(session, upd))
					++cbt->page_deleted_count;
				continue;
			}
			return (__wt_value_return(session, cbt, upd));
		}

		/*
		 * If we're at the same slot as the last reference and there's
		 * no matching insert list item, re-use the return information
		 * (so encoded items with large repeat counts aren't repeatedly
		 * decoded).  Otherwise, unpack the cell and build the return
		 * information.
		 */
		if (cbt->cip_saved != cip) {
			if ((cell = WT_COL_PTR(page, cip)) == NULL)
				continue;
			__wt_cell_unpack(cell, &unpack);
			if (unpack.type == WT_CELL_DEL) {
				if ((rle = __wt_cell_rle(&unpack)) == 1)
					continue;

				/*
				 * There can be huge gaps in the variable-length
				 * column-store name space appearing as deleted
				 * records. If more than one deleted record, do
				 * the work of finding the next record to return
				 * instead of looping through the records.
				 *
				 * First, find the smallest record in the update
				 * list that's larger than the current record.
				 */
				ins = __col_insert_search_gt(
				    cbt->ins_head, cbt->recno);

				/*
				 * Second, for records with RLEs greater than 1,
				 * the above call to __col_var_search located
				 * this record in the page's list of repeating
				 * records, and returned the starting record.
				 * The starting record plus the RLE is the
				 * record to which we could skip, if there was
				 * no smaller record in the update list.
				 */
				cbt->recno = rle_start + rle;
				if (ins != NULL &&
				    WT_INSERT_RECNO(ins) < cbt->recno)
					cbt->recno = WT_INSERT_RECNO(ins);

				/* Adjust for the outer loop increment. */
				--cbt->recno;
				continue;
			}
			WT_RET(__wt_page_cell_data_ref(
			    session, page, &unpack, cbt->tmp));

			cbt->cip_saved = cip;
		}
		cbt->iface.value.data = cbt->tmp->data;
		cbt->iface.value.size = cbt->tmp->size;
		return (0);
	}
	/* NOTREACHED */
}

/*
 * __cursor_row_next --
 *	Move to the next row-store item.
 */
static inline int
__cursor_row_next(WT_CURSOR_BTREE *cbt, bool newpage)
{
	WT_INSERT *ins;
	WT_ITEM *key;
	WT_PAGE *page;
	WT_ROW *rip;
	WT_SESSION_IMPL *session;
	WT_UPDATE *upd;

	session = (WT_SESSION_IMPL *)cbt->iface.session;
	page = cbt->ref->page;
	key = &cbt->iface.key;

	/*
	 * For row-store pages, we need a single item that tells us the part
	 * of the page we're walking (otherwise switching from next to prev
	 * and vice-versa is just too complicated), so we map the WT_ROW and
	 * WT_INSERT_HEAD insert array slots into a single name space: slot 1
	 * is the "smallest key insert list", slot 2 is WT_ROW[0], slot 3 is
	 * WT_INSERT_HEAD[0], and so on.  This means WT_INSERT lists are
	 * odd-numbered slots, and WT_ROW array slots are even-numbered slots.
	 *
	 * Initialize for each new page.
	 */
	if (newpage) {
		cbt->ins_head = WT_ROW_INSERT_SMALLEST(page);
		cbt->ins = WT_SKIP_FIRST(cbt->ins_head);
		cbt->row_iteration_slot = 1;
		cbt->rip_saved = NULL;
		goto new_insert;
	}

	/* Move to the next entry and return the item. */
	for (;;) {
		/*
		 * Continue traversing any insert list; maintain the insert list
		 * head reference and entry count in case we switch to a cursor
		 * previous movement.
		 */
		if (cbt->ins != NULL)
			cbt->ins = WT_SKIP_NEXT(cbt->ins);

new_insert:	if ((ins = cbt->ins) != NULL) {
			WT_RET(__wt_txn_read(session, ins->upd, &upd));
			if (upd == NULL)
				continue;
			if (upd->type == WT_UPDATE_TOMBSTONE) {
				if (upd->txnid != WT_TXN_NONE &&
				    __wt_txn_upd_visible_all(session, upd))
					++cbt->page_deleted_count;
				continue;
			}
			key->data = WT_INSERT_KEY(ins);
			key->size = WT_INSERT_KEY_SIZE(ins);
			return (__wt_value_return(session, cbt, upd));
		}

		/* Check for the end of the page. */
		if (cbt->row_iteration_slot >= page->entries * 2 + 1)
			return (WT_NOTFOUND);
		++cbt->row_iteration_slot;

		/*
		 * Odd-numbered slots configure as WT_INSERT_HEAD entries,
		 * even-numbered slots configure as WT_ROW entries.
		 */
		if (cbt->row_iteration_slot & 0x01) {
			cbt->ins_head = WT_ROW_INSERT_SLOT(
			    page, cbt->row_iteration_slot / 2 - 1);
			cbt->ins = WT_SKIP_FIRST(cbt->ins_head);
			goto new_insert;
		}
		cbt->ins_head = NULL;
		cbt->ins = NULL;

		cbt->slot = cbt->row_iteration_slot / 2 - 1;
		rip = &page->pg_row[cbt->slot];
		WT_RET(__wt_txn_read(session, WT_ROW_UPDATE(page, rip), &upd));
		if (upd != NULL && upd->type == WT_UPDATE_TOMBSTONE) {
			if (upd->txnid != WT_TXN_NONE &&
			    __wt_txn_upd_visible_all(session, upd))
				++cbt->page_deleted_count;
			continue;
		}
		return (__cursor_row_slot_return(cbt, rip, upd));
	}
	/* NOTREACHED */
}

#ifdef HAVE_DIAGNOSTIC
/*
 * __cursor_key_order_check_col --
 *	Check key ordering for column-store cursor movements.
 */
static int
__cursor_key_order_check_col(
    WT_SESSION_IMPL *session, WT_CURSOR_BTREE *cbt, bool next)
{
	int cmp;

	cmp = 0;			/* -Werror=maybe-uninitialized */

	if (cbt->lastrecno != WT_RECNO_OOB) {
		if (cbt->lastrecno < cbt->recno)
			cmp = -1;
		if (cbt->lastrecno > cbt->recno)
			cmp = 1;
	}

	if (cbt->lastrecno == WT_RECNO_OOB ||
	    (next && cmp < 0) || (!next && cmp > 0)) {
		cbt->lastrecno = cbt->recno;
		return (0);
	}

	WT_PANIC_RET(session, EINVAL,
	    "WT_CURSOR.%s out-of-order returns: returned key %" PRIu64 " then "
	    "key %" PRIu64,
	    next ? "next" : "prev", cbt->lastrecno, cbt->recno);
}

/*
 * __cursor_key_order_check_row --
 *	Check key ordering for row-store cursor movements.
 */
static int
__cursor_key_order_check_row(
    WT_SESSION_IMPL *session, WT_CURSOR_BTREE *cbt, bool next)
{
	WT_BTREE *btree;
	WT_DECL_ITEM(a);
	WT_DECL_ITEM(b);
	WT_DECL_RET;
	WT_ITEM *key;
	int cmp;

	btree = S2BT(session);
	key = &cbt->iface.key;
	cmp = 0;			/* -Werror=maybe-uninitialized */

	if (cbt->lastkey->size != 0)
		WT_RET(__wt_compare(
		    session, btree->collator, cbt->lastkey, key, &cmp));

	if (cbt->lastkey->size == 0 || (next && cmp < 0) || (!next && cmp > 0))
		return (__wt_buf_set(session,
		    cbt->lastkey, cbt->iface.key.data, cbt->iface.key.size));

	WT_ERR(__wt_scr_alloc(session, 512, &a));
	WT_ERR(__wt_scr_alloc(session, 512, &b));

	WT_PANIC_ERR(session, EINVAL,
	    "WT_CURSOR.%s out-of-order returns: returned key %.1024s then "
	    "key %.1024s",
	    next ? "next" : "prev",
	    __wt_buf_set_printable_format(session,
	    cbt->lastkey->data, cbt->lastkey->size, btree->key_format, a),
	    __wt_buf_set_printable_format(session,
	    key->data, key->size, btree->key_format, b));

err:	__wt_scr_free(session, &a);
	__wt_scr_free(session, &b);

	return (ret);
}

/*
 * __wt_cursor_key_order_check --
 *	Check key ordering for cursor movements.
 */
int
__wt_cursor_key_order_check(
    WT_SESSION_IMPL *session, WT_CURSOR_BTREE *cbt, bool next)
{
	switch (cbt->ref->page->type) {
	case WT_PAGE_COL_FIX:
	case WT_PAGE_COL_VAR:
		return (__cursor_key_order_check_col(session, cbt, next));
	case WT_PAGE_ROW_LEAF:
		return (__cursor_key_order_check_row(session, cbt, next));
	WT_ILLEGAL_VALUE(session, cbt->ref->page->type);
	}
	/* NOTREACHED */
}

/*
 * __wt_cursor_key_order_init --
 *	Initialize key ordering checks for cursor movements after a successful
 * search.
 */
int
__wt_cursor_key_order_init(WT_SESSION_IMPL *session, WT_CURSOR_BTREE *cbt)
{
	/*
	 * Cursor searches set the position for cursor movements, set the
	 * last-key value for diagnostic checking.
	 */
	switch (cbt->ref->page->type) {
	case WT_PAGE_COL_FIX:
	case WT_PAGE_COL_VAR:
		cbt->lastrecno = cbt->recno;
		return (0);
	case WT_PAGE_ROW_LEAF:
		return (__wt_buf_set(session,
		    cbt->lastkey, cbt->iface.key.data, cbt->iface.key.size));
	WT_ILLEGAL_VALUE(session, cbt->ref->page->type);
	}
	/* NOTREACHED */
}

/*
 * __wt_cursor_key_order_reset --
 *	Turn off key ordering checks for cursor movements.
 */
void
__wt_cursor_key_order_reset(WT_CURSOR_BTREE *cbt)
{
	/*
	 * Clear the last-key returned, it doesn't apply.
	 */
	cbt->lastkey->size = 0;
	cbt->lastrecno = WT_RECNO_OOB;
}
#endif

/*
 * __wt_btcur_iterate_setup --
 *	Initialize a cursor for iteration, usually based on a search.
 */
void
__wt_btcur_iterate_setup(WT_CURSOR_BTREE *cbt)
{
	WT_PAGE *page;

	/*
	 * We don't currently have to do any setup when we switch between next
	 * and prev calls, but I'm sure we will someday -- I'm leaving support
	 * here for both flags for that reason.
	 */
	F_SET(cbt, WT_CBT_ITERATE_NEXT | WT_CBT_ITERATE_PREV);

	/* Clear the count of deleted items on the page. */
	cbt->page_deleted_count = 0;

	/* Clear saved iteration cursor position information. */
	cbt->cip_saved = NULL;
	cbt->rip_saved = NULL;

	/*
	 * If we don't have a search page, then we're done, we're starting at
	 * the beginning or end of the tree, not as a result of a search.
	 */
	if (cbt->ref == NULL) {
#ifdef HAVE_DIAGNOSTIC
		__wt_cursor_key_order_reset(cbt);
#endif
		return;
	}

	page = cbt->ref->page;
	if (page->type == WT_PAGE_ROW_LEAF) {
		/*
		 * For row-store pages, we need a single item that tells us the
		 * part of the page we're walking (otherwise switching from next
		 * to prev and vice-versa is just too complicated), so we map
		 * the WT_ROW and WT_INSERT_HEAD insert array slots into a
		 * single name space: slot 1 is the "smallest key insert list",
		 * slot 2 is WT_ROW[0], slot 3 is WT_INSERT_HEAD[0], and so on.
		 * This means WT_INSERT lists are odd-numbered slots, and WT_ROW
		 * array slots are even-numbered slots.
		 */
		cbt->row_iteration_slot = (cbt->slot + 1) * 2;
		if (cbt->ins_head != NULL) {
			if (cbt->ins_head == WT_ROW_INSERT_SMALLEST(page))
				cbt->row_iteration_slot = 1;
			else
				cbt->row_iteration_slot += 1;
		}
	} else {
		/*
		 * For column-store pages, calculate the largest record on the
		 * page.
		 */
		cbt->last_standard_recno = page->type == WT_PAGE_COL_VAR ?
		    __col_var_last_recno(cbt->ref) :
		    __col_fix_last_recno(cbt->ref);

		/* If we're traversing the append list, set the reference. */
		if (cbt->ins_head != NULL &&
		    cbt->ins_head == WT_COL_APPEND(page))
			F_SET(cbt, WT_CBT_ITERATE_APPEND);
	}
}

/*
 * __wt_btcur_next --
 *	Move to the next record in the tree.
 */
int
__wt_btcur_next(WT_CURSOR_BTREE *cbt, bool truncating)
{
	WT_CURSOR *cursor;
	WT_DECL_RET;
	WT_PAGE *page;
	WT_SESSION_IMPL *session;
	WT_UPDATE *upd;
	uint32_t flags;
	bool newpage, valid;

	cursor = &cbt->iface;
	session = (WT_SESSION_IMPL *)cbt->iface.session;

	WT_STAT_CONN_INCR(session, cursor_next);
	WT_STAT_DATA_INCR(session, cursor_next);

	F_CLR(cursor, WT_CURSTD_KEY_SET | WT_CURSTD_VALUE_SET);

	/*
	 * In case of retrying a next operation due to a prepare conflict,
	 * cursor would have been already positioned at an update structure
	 * which resulted in conflict. So, now when retrying we should examine
	 * the same update again instead of starting from the next one in the
	 * update chain.
	 */
	F_CLR(cbt, WT_CBT_RETRY_PREV);
	if (F_ISSET(cbt, WT_CBT_RETRY_NEXT)) {
		WT_RET(__wt_cursor_valid(cbt, &upd, &valid));
		F_CLR(cbt, WT_CBT_RETRY_NEXT);
		if (valid) {
			/*
			 * If the update, which returned prepared conflict is
			 * visible, return the value.
			 */
			return (__cursor_kv_return(session, cbt, upd));
		}
	}

	WT_RET(__cursor_func_init(cbt, false));

	/*
	 * If we aren't already iterating in the right direction, there's
	 * some setup to do.
	 */
	if (!F_ISSET(cbt, WT_CBT_ITERATE_NEXT))
		__wt_btcur_iterate_setup(cbt);

	/*
	 * Walk any page we're holding until the underlying call returns not-
	 * found.  Then, move to the next page, until we reach the end of the
	 * file.
	 */
	flags = WT_READ_NO_SPLIT | WT_READ_SKIP_INTL;	/* tree walk flags */
	if (truncating)
		LF_SET(WT_READ_TRUNCATE);
	for (newpage = false;; newpage = true) {
		page = cbt->ref == NULL ? NULL : cbt->ref->page;

		if (F_ISSET(cbt, WT_CBT_ITERATE_APPEND)) {
			switch (page->type) {
			case WT_PAGE_COL_FIX:
				ret = __cursor_fix_append_next(cbt, newpage);
				break;
			case WT_PAGE_COL_VAR:
				ret = __cursor_var_append_next(cbt, newpage);
				break;
			WT_ILLEGAL_VALUE_ERR(session, page->type);
			}
			if (ret == 0)
				break;
			F_CLR(cbt, WT_CBT_ITERATE_APPEND);
			if (ret != WT_NOTFOUND)
				break;
		} else if (page != NULL) {
			switch (page->type) {
			case WT_PAGE_COL_FIX:
				ret = __cursor_fix_next(cbt, newpage);
				break;
			case WT_PAGE_COL_VAR:
				ret = __cursor_var_next(cbt, newpage);
				break;
			case WT_PAGE_ROW_LEAF:
				ret = __cursor_row_next(cbt, newpage);
				break;
			WT_ILLEGAL_VALUE_ERR(session, page->type);
			}
			if (ret != WT_NOTFOUND)
				break;

			/*
			 * Column-store pages may have appended entries. Handle
			 * it separately from the usual cursor code, it's in a
			 * simple format.
			 */
			if (page->type != WT_PAGE_ROW_LEAF &&
			    (cbt->ins_head = WT_COL_APPEND(page)) != NULL) {
				F_SET(cbt, WT_CBT_ITERATE_APPEND);
				continue;
			}
		}

		/*
		 * If we saw a lot of deleted records on this page, or we went
		 * all the way through a page and only saw deleted records, try
		 * to evict the page when we release it.  Otherwise repeatedly
		 * deleting from the beginning of a tree can have quadratic
		 * performance.  Take care not to force eviction of pages that
		 * are genuinely empty, in new trees.
		 */
		if (page != NULL &&
		    (cbt->page_deleted_count > WT_BTREE_DELETE_THRESHOLD ||
		    (newpage && cbt->page_deleted_count > 0)))
			__wt_page_evict_soon(session, cbt->ref);
		cbt->page_deleted_count = 0;

		if (F_ISSET(cbt, WT_CBT_READ_ONCE))
			LF_SET(WT_READ_WONT_NEED);
		WT_ERR(__wt_tree_walk(session, &cbt->ref, flags));
		WT_ERR_TEST(cbt->ref == NULL, WT_NOTFOUND);
	}
#ifdef HAVE_DIAGNOSTIC
	if (ret == 0)
		WT_ERR(__wt_cursor_key_order_check(session, cbt, true));
#endif
err:	switch (ret) {
	case 0:
		F_SET(cursor, WT_CURSTD_KEY_INT | WT_CURSTD_VALUE_INT);
		break;
	case WT_PREPARE_CONFLICT:
		/*
		 * If prepare conflict occurs, cursor should not be reset,
		 * as current cursor position will be reused in case of a
		 * retry from user.
		 */
		F_SET(cbt, WT_CBT_RETRY_NEXT);
		break;
	default:
		WT_TRET(__cursor_reset(cbt));
	}
	return (ret);
}
