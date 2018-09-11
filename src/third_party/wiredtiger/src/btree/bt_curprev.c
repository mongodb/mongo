/*-
 * Copyright (c) 2014-2018 MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * Walking backwards through skip lists.
 *
 * The skip list stack is an array of pointers set up by a search.  It points
 * to the position a node should go in the skip list.  In other words, the skip
 * list search stack always points *after* the search item (that is, into the
 * search item's next array).
 *
 * Helper macros to go from a stack pointer at level i, pointing into a next
 * array, back to the insert node containing that next array.
 */
#undef	PREV_ITEM
#define	PREV_ITEM(ins_head, insp, i)					\
	(((insp) == &(ins_head)->head[i] || (insp) == NULL) ? NULL :	\
	    (WT_INSERT *)((char *)((insp) - (i)) - offsetof(WT_INSERT, next)))

#undef	PREV_INS
#define	PREV_INS(cbt, i)						\
	PREV_ITEM((cbt)->ins_head, (cbt)->ins_stack[(i)], (i))

/*
 * __cursor_skip_prev --
 *	Move back one position in a skip list stack (aka "finger").
 */
static inline int
__cursor_skip_prev(WT_CURSOR_BTREE *cbt)
{
	WT_INSERT *current, *ins;
	WT_ITEM key;
	WT_SESSION_IMPL *session;
	int i;

	session = (WT_SESSION_IMPL *)cbt->iface.session;

restart:
	/*
	 * If the search stack does not point at the current item, fill it in
	 * with a search.
	 */
	while ((current = cbt->ins) != PREV_INS(cbt, 0)) {
		if (cbt->btree->type == BTREE_ROW) {
			key.data = WT_INSERT_KEY(current);
			key.size = WT_INSERT_KEY_SIZE(current);
			WT_RET(__wt_search_insert(
			    session, cbt, cbt->ins_head, &key));
		} else
			cbt->ins = __col_insert_search(cbt->ins_head,
			    cbt->ins_stack, cbt->next_stack,
			    WT_INSERT_RECNO(current));
	}

	/*
	 * Find the first node up the search stack that does not move.
	 *
	 * The depth of the current item must be at least this level, since we
	 * see it in that many levels of the stack.
	 *
	 * !!! Watch these loops carefully: they all rely on the value of i,
	 * and the exit conditions to end up with the right values are
	 * non-trivial.
	 */
	ins = NULL;			/* -Wconditional-uninitialized */
	for (i = 0; i < WT_SKIP_MAXDEPTH - 1; i++)
		if ((ins = PREV_INS(cbt, i + 1)) != current)
			break;

	/*
	 * Find a starting point for the new search.  That is either at the
	 * non-moving node if we found a valid node, or the beginning of the
	 * next list down that is not the current node.
	 *
	 * Since it is the beginning of a list, and we know the current node is
	 * has a skip depth at least this high, any node we find must sort
	 * before the current node.
	 */
	if (ins == NULL || ins == current)
		for (; i >= 0; i--) {
			cbt->ins_stack[i] = NULL;
			cbt->next_stack[i] = NULL;
			ins = cbt->ins_head->head[i];
			if (ins != NULL && ins != current)
				break;
		}

	/* Walk any remaining levels until just before the current node. */
	while (i >= 0) {
		/*
		 * If we get to the end of a list without finding the current
		 * item, we must have raced with an insert.  Restart the search.
		 */
		if (ins == NULL) {
			cbt->ins_stack[0] = NULL;
			cbt->next_stack[0] = NULL;
			goto restart;
		}
		if (ins->next[i] != current)		/* Stay at this level */
			ins = ins->next[i];
		else {					/* Drop down a level */
			cbt->ins_stack[i] = &ins->next[i];
			cbt->next_stack[i] = ins->next[i];
			--i;
		}
	}

	/* If we found a previous node, the next one must be current. */
	if (cbt->ins_stack[0] != NULL && *cbt->ins_stack[0] != current)
		goto restart;

	cbt->ins = PREV_INS(cbt, 0);
	return (0);
}

/*
 * __cursor_fix_append_prev --
 *	Return the previous fixed-length entry on the append list.
 */
static inline int
__cursor_fix_append_prev(WT_CURSOR_BTREE *cbt, bool newpage)
{
	WT_SESSION_IMPL *session;
	WT_UPDATE *upd;

	session = (WT_SESSION_IMPL *)cbt->iface.session;

	if (newpage) {
		if ((cbt->ins = WT_SKIP_LAST(cbt->ins_head)) == NULL)
			return (WT_NOTFOUND);
	} else {
		/* Move to the previous record in the append list, if any. */
		if (cbt->ins != NULL &&
		    cbt->recno <= WT_INSERT_RECNO(cbt->ins))
			WT_RET(__cursor_skip_prev(cbt));

		/*
		 * Handle the special case of leading implicit records, that is,
		 * there aren't any records in the page not on the append list,
		 * and the append list's first record isn't the first record on
		 * the page. (Although implemented as a test of the page values,
		 * this is really a test for a tree where the first inserted
		 * record wasn't record 1, any other page with only an append
		 * list will have a first page record number matching the first
		 * record in the append list.)
		 *
		 * The "right" place to handle this is probably in our caller.
		 * The high-level cursor-previous routine would:
		 *    -- call this routine to walk the append list
		 *    -- call the routine to walk the standard page items
		 *    -- call the tree walk routine looking for a previous page
		 * Each of them returns WT_NOTFOUND, at which point our caller
		 * checks the cursor record number, and if it's larger than 1,
		 * returns the implicit records.  Instead, I'm trying to detect
		 * the case here, mostly because I don't want to put that code
		 * into our caller.  Anyway, if this code breaks for any reason,
		 * that's the way I'd go.
		 *
		 * If we're not pointing to a WT_INSERT entry (we didn't find a
		 * WT_INSERT record preceding our record name-space), check if
		 * we've reached the beginning of this page, a possibility if a
		 * page had a large number of items appended, and then split.
		 * If not, check if there are any records on the page. If there
		 * aren't, then we're in the magic zone, keep going until we get
		 * to a record number matching the first record on the page.
		 */
		if (cbt->ins == NULL &&
		    (cbt->recno == cbt->ref->ref_recno ||
		    __col_fix_last_recno(cbt->ref) != 0))
			return (WT_NOTFOUND);
	}

	/*
	 * This code looks different from the cursor-next code. The append list
	 * may be preceded by other rows. If we're iterating through the tree,
	 * starting at the last record in the tree, by definition we're starting
	 * a new iteration and we set the record number to the last record found
	 * on the page. Otherwise, decrement the record.
	 */
	if (newpage)
		__cursor_set_recno(cbt, WT_INSERT_RECNO(cbt->ins));
	else
		__cursor_set_recno(cbt, cbt->recno - 1);

	/*
	 * Fixed-width column store appends are inherently non-transactional.
	 * Even a non-visible update by a concurrent or aborted transaction
	 * changes the effective end of the data.  The effect is subtle because
	 * of the blurring between deleted and empty values, but ideally we
	 * would skip all uncommitted changes at the end of the data.  This
	 * doesn't apply to variable-width column stores because the implicitly
	 * created records written by reconciliation are deleted and so can be
	 * never seen by a read.
	 */
	if (cbt->ins == NULL || cbt->recno > WT_INSERT_RECNO(cbt->ins)) {
		cbt->v = 0;
		cbt->iface.value.data = &cbt->v;
	} else {
		upd = NULL;
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
 * __cursor_fix_prev --
 *	Move to the previous, fixed-length column-store item.
 */
static inline int
__cursor_fix_prev(WT_CURSOR_BTREE *cbt, bool newpage)
{
	WT_BTREE *btree;
	WT_PAGE *page;
	WT_SESSION_IMPL *session;
	WT_UPDATE *upd;

	session = (WT_SESSION_IMPL *)cbt->iface.session;
	page = cbt->ref->page;
	btree = S2BT(session);

	/* Initialize for each new page. */
	if (newpage) {
		cbt->last_standard_recno = __col_fix_last_recno(cbt->ref);
		if (cbt->last_standard_recno == 0)
			return (WT_NOTFOUND);
		__cursor_set_recno(cbt, cbt->last_standard_recno);
		goto new_page;
	}

	/* Move to the previous entry and return the item. */
	if (cbt->recno == cbt->ref->ref_recno)
		return (WT_NOTFOUND);
	__cursor_set_recno(cbt, cbt->recno - 1);

new_page:
	/* Check any insert list for a matching record. */
	cbt->ins_head = WT_COL_UPDATE_SINGLE(page);
	cbt->ins = __col_insert_search(
	    cbt->ins_head, cbt->ins_stack, cbt->next_stack, cbt->recno);
	if (cbt->ins != NULL && cbt->recno != WT_INSERT_RECNO(cbt->ins))
		cbt->ins = NULL;
	upd = NULL;
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
 * __cursor_var_append_prev --
 *	Return the previous variable-length entry on the append list.
 */
static inline int
__cursor_var_append_prev(WT_CURSOR_BTREE *cbt, bool newpage)
{
	WT_SESSION_IMPL *session;
	WT_UPDATE *upd;

	session = (WT_SESSION_IMPL *)cbt->iface.session;

	if (newpage) {
		cbt->ins = WT_SKIP_LAST(cbt->ins_head);
		goto new_page;
	}

	for (;;) {
		WT_RET(__cursor_skip_prev(cbt));
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
 * __cursor_var_prev --
 *	Move to the previous, variable-length column-store item.
 */
static inline int
__cursor_var_prev(WT_CURSOR_BTREE *cbt, bool newpage)
{
	WT_CELL *cell;
	WT_CELL_UNPACK unpack;
	WT_COL *cip;
	WT_INSERT *ins;
	WT_PAGE *page;
	WT_SESSION_IMPL *session;
	WT_UPDATE *upd;
	uint64_t rle_start;

	session = (WT_SESSION_IMPL *)cbt->iface.session;
	page = cbt->ref->page;

	rle_start = 0;			/* -Werror=maybe-uninitialized */

	/* Initialize for each new page. */
	if (newpage) {
		cbt->last_standard_recno = __col_var_last_recno(cbt->ref);
		if (cbt->last_standard_recno == 0)
			return (WT_NOTFOUND);
		__cursor_set_recno(cbt, cbt->last_standard_recno);
		cbt->cip_saved = NULL;
		goto new_page;
	}

	/* Move to the previous entry and return the item. */
	for (;;) {
		__cursor_set_recno(cbt, cbt->recno - 1);

new_page:	if (cbt->recno < cbt->ref->ref_recno)
			return (WT_NOTFOUND);

		/* Find the matching WT_COL slot. */
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
				if (__wt_cell_rle(&unpack) == 1)
					continue;
				/*
				 * There can be huge gaps in the variable-length
				 * column-store name space appearing as deleted
				 * records. If more than one deleted record, do
				 * the work of finding the next record to return
				 * instead of looping through the records.
				 *
				 * First, find the largest record in the update
				 * list that's smaller than the current record.
				 */
				ins = __col_insert_search_lt(
				    cbt->ins_head, cbt->recno);

				/*
				 * Second, for records with RLEs greater than 1,
				 * the above call to __col_var_search located
				 * this record in the page's list of repeating
				 * records, and returned the starting record.
				 * The starting record - 1 is the record to
				 * which we could skip, if there was no larger
				 * record in the update list.
				 */
				cbt->recno = rle_start - 1;
				if (ins != NULL &&
				    WT_INSERT_RECNO(ins) > cbt->recno)
					cbt->recno = WT_INSERT_RECNO(ins);

				/* Adjust for the outer loop decrement. */
				++cbt->recno;
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
 * __cursor_row_prev --
 *	Move to the previous row-store item.
 */
static inline int
__cursor_row_prev(WT_CURSOR_BTREE *cbt, bool newpage)
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
		/*
		 * If we haven't instantiated keys on this page, do so, else it
		 * is a very, very slow traversal.
		 */
		if (!F_ISSET_ATOMIC(page, WT_PAGE_BUILD_KEYS))
			WT_RET(__wt_row_leaf_keys(session, page));

		if (page->entries == 0)
			cbt->ins_head = WT_ROW_INSERT_SMALLEST(page);
		else
			cbt->ins_head =
			    WT_ROW_INSERT_SLOT(page, page->entries - 1);
		cbt->ins = WT_SKIP_LAST(cbt->ins_head);
		cbt->row_iteration_slot = page->entries * 2 + 1;
		cbt->rip_saved = NULL;
		goto new_insert;
	}

	/* Move to the previous entry and return the item. */
	for (;;) {
		/*
		 * Continue traversing any insert list.  Maintain the reference
		 * to the current insert element in case we switch to a cursor
		 * next movement.
		 */
		if (cbt->ins != NULL)
			WT_RET(__cursor_skip_prev(cbt));

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

		/* Check for the beginning of the page. */
		if (cbt->row_iteration_slot == 1)
			return (WT_NOTFOUND);
		--cbt->row_iteration_slot;

		/*
		 * Odd-numbered slots configure as WT_INSERT_HEAD entries,
		 * even-numbered slots configure as WT_ROW entries.
		 */
		if (cbt->row_iteration_slot & 0x01) {
			cbt->ins_head = cbt->row_iteration_slot == 1 ?
			    WT_ROW_INSERT_SMALLEST(page) :
			    WT_ROW_INSERT_SLOT(
				page, cbt->row_iteration_slot / 2 - 1);
			cbt->ins = WT_SKIP_LAST(cbt->ins_head);
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

/*
 * __wt_btcur_prev --
 *	Move to the previous record in the tree.
 */
int
__wt_btcur_prev(WT_CURSOR_BTREE *cbt, bool truncating)
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

	WT_STAT_CONN_INCR(session, cursor_prev);
	WT_STAT_DATA_INCR(session, cursor_prev);

	F_CLR(cursor, WT_CURSTD_KEY_SET | WT_CURSTD_VALUE_SET);

	/*
	 * In case of retrying a prev operation due to a prepare conflict,
	 * cursor would have been already positioned at an update structure
	 * which resulted in conflict. So, now when retrying we should examine
	 * the same update again instead of starting from the next one in the
	 * update chain.
	 */
	F_CLR(cbt, WT_CBT_RETRY_NEXT);
	if (F_ISSET(cbt, WT_CBT_RETRY_PREV)) {
		WT_RET(__wt_cursor_valid(cbt, &upd, &valid));
		F_CLR(cbt, WT_CBT_RETRY_PREV);
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
	if (!F_ISSET(cbt, WT_CBT_ITERATE_PREV))
		__wt_btcur_iterate_setup(cbt);

	/*
	 * Walk any page we're holding until the underlying call returns not-
	 * found.  Then, move to the previous page, until we reach the start
	 * of the file.
	 */
	flags =						/* tree walk flags */
	    WT_READ_NO_SPLIT | WT_READ_PREV | WT_READ_SKIP_INTL;
	if (truncating)
		LF_SET(WT_READ_TRUNCATE);
	for (newpage = false;; newpage = true) {
		page = cbt->ref == NULL ? NULL : cbt->ref->page;

		/*
		 * Column-store pages may have appended entries. Handle it
		 * separately from the usual cursor code, it's in a simple
		 * format.
		 */
		if (newpage && page != NULL && page->type != WT_PAGE_ROW_LEAF &&
		    (cbt->ins_head = WT_COL_APPEND(page)) != NULL)
			F_SET(cbt, WT_CBT_ITERATE_APPEND);

		if (F_ISSET(cbt, WT_CBT_ITERATE_APPEND)) {
			switch (page->type) {
			case WT_PAGE_COL_FIX:
				ret = __cursor_fix_append_prev(cbt, newpage);
				break;
			case WT_PAGE_COL_VAR:
				ret = __cursor_var_append_prev(cbt, newpage);
				break;
			WT_ILLEGAL_VALUE_ERR(session, page->type);
			}
			if (ret == 0)
				break;
			F_CLR(cbt, WT_CBT_ITERATE_APPEND);
			if (ret != WT_NOTFOUND)
				break;
			newpage = true;
		}
		if (page != NULL) {
			switch (page->type) {
			case WT_PAGE_COL_FIX:
				ret = __cursor_fix_prev(cbt, newpage);
				break;
			case WT_PAGE_COL_VAR:
				ret = __cursor_var_prev(cbt, newpage);
				break;
			case WT_PAGE_ROW_LEAF:
				ret = __cursor_row_prev(cbt, newpage);
				break;
			WT_ILLEGAL_VALUE_ERR(session, page->type);
			}
			if (ret != WT_NOTFOUND)
				break;
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
		WT_ERR(__wt_cursor_key_order_check(session, cbt, false));
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
		F_SET(cbt, WT_CBT_RETRY_PREV);
		break;
	default:
		WT_TRET(__cursor_reset(cbt));
	}
	return (ret);
}
