/*-
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

static int __col_insert_alloc(
    WT_SESSION_IMPL *, uint64_t, u_int, WT_INSERT **, size_t *);

/*
 * __wt_col_modify --
 *	Column-store delete, insert, and update.
 */
int
__wt_col_modify(WT_SESSION_IMPL *session, WT_CURSOR_BTREE *cbt, int is_remove)
{
	WT_BTREE *btree;
	WT_DECL_RET;
	WT_INSERT *ins;
	WT_INSERT_HEAD *ins_head, **ins_headp;
	WT_ITEM *value, _value;
	WT_PAGE *page;
	WT_UPDATE *old_upd, *upd;
	size_t ins_size, upd_size;
	uint64_t recno;
	u_int i, skipdepth;
	int append, logged;

	btree = cbt->btree;
	page = cbt->page;
	recno = cbt->iface.recno;
	append = logged = 0;

	if (is_remove) {
		if (btree->type == BTREE_COL_FIX) {
			value = &_value;
			value->data = "";
			value->size = 1;
		} else
			value = NULL;
	} else {
		value = &cbt->iface.value;

		/*
		 * There's some chance the application specified a record past
		 * the last record on the page.  If that's the case, and we're
		 * inserting a new WT_INSERT/WT_UPDATE pair, it goes on the
		 * append list, not the update list.   In addition, a recno of
		 * 0 implies an append operation, we're allocating a new row.
		 */
		if (recno == 0 || recno > __col_last_recno(page))
			append = 1;
	}

	/* If we don't yet have a modify structure, we'll need one. */
	WT_RET(__wt_page_modify_init(session, page));

	ins = NULL;
	upd = NULL;

	/*
	 * Delete, insert or update a column-store entry.
	 *
	 * If modifying a previously modified record, create a new WT_UPDATE
	 * entry and have a serialized function link it into an existing
	 * WT_INSERT entry's WT_UPDATE list.
	 *
	 * Else, allocate an insert array as necessary, build a WT_INSERT and
	 * WT_UPDATE structure pair, and call a serialized function to insert
	 * the WT_INSERT structure.
	 */
	if (cbt->compare == 0 && cbt->ins != NULL) {
		/* Make sure the update can proceed. */
		WT_ERR(__wt_txn_update_check(session, old_upd = cbt->ins->upd));

		/* Allocate the WT_UPDATE structure and transaction ID. */
		WT_ERR(__wt_update_alloc(session, value, &upd, &upd_size));
		WT_ERR(__wt_txn_modify(session, cbt, upd));
		logged = 1;

		/*
		 * Point the new WT_UPDATE item to the next element in the list.
		 * If we get it right, the serialization function lock acts as
		 * our memory barrier to flush this write.
		 */
		upd->next = old_upd;

		/* Serialize the update. */
		WT_ERR(__wt_update_serial(
		    session, page, &cbt->ins->upd, &upd, upd_size));
	} else {
		/* Allocate the append/update list reference as necessary. */
		if (append) {
			WT_PAGE_ALLOC_AND_SWAP(
			    session, page, page->modify->append, ins_headp, 1);
			ins_headp = &page->modify->append[0];
		} else if (page->type == WT_PAGE_COL_FIX) {
			WT_PAGE_ALLOC_AND_SWAP(
			    session, page, page->modify->update, ins_headp, 1);
			ins_headp = &page->modify->update[0];
		} else {
			WT_PAGE_ALLOC_AND_SWAP(session, page,
			    page->modify->update, ins_headp, page->entries);
			ins_headp = &page->modify->update[cbt->slot];
		}

		/* Allocate the WT_INSERT_HEAD structure as necessary. */
		WT_PAGE_ALLOC_AND_SWAP(session, page, *ins_headp, ins_head, 1);
		ins_head = *ins_headp;

		/* Choose a skiplist depth for this insert. */
		skipdepth = __wt_skip_choose_depth();

		/*
		 * Allocate a WT_INSERT/WT_UPDATE pair and transaction ID, and
		 * update the cursor to reference it.
		 */
		WT_ERR(__col_insert_alloc(
		    session, recno, skipdepth, &ins, &ins_size));
		WT_ERR(__wt_update_alloc(session, value, &upd, &upd_size));
		ins->upd = upd;
		ins_size += upd_size;

		/*
		 * Update the cursor: the insert head may have been allocated,
		 * the ins field was allocated.
		 */
		cbt->ins_head = ins_head;
		cbt->ins = ins;
		WT_ERR(__wt_txn_modify(session, cbt, upd));
		logged = 1;

		/*
		 * If there was no insert list during the search, or there was
		 * no search because the record number has not been allocated
		 * yet, the cursor's information cannot be correct, search
		 * couldn't have initialized it.
		 *
		 * Otherwise, point the new WT_INSERT item's skiplist to the
		 * next elements in the insert list (which we will check are
		 * still valid inside the serialization function).
		 *
		 * The serial mutex acts as our memory barrier to flush these
		 * writes before inserting them into the list.
		 */
		if (WT_SKIP_FIRST(ins_head) == NULL || recno == 0)
			for (i = 0; i < skipdepth; i++) {
				cbt->ins_stack[i] = &ins_head->head[i];
				ins->next[i] = cbt->next_stack[i] = NULL;
			}
		else
			for (i = 0; i < skipdepth; i++)
				ins->next[i] = cbt->next_stack[i];

		/* Append or insert the WT_INSERT structure. */
		if (append)
			WT_ERR(__wt_col_append_serial(
			    session, page, cbt->ins_head, cbt->ins_stack,
			    &ins, ins_size, &cbt->recno, skipdepth));
		else
			WT_ERR(__wt_insert_serial(
			    session, page, cbt->ins_head, cbt->ins_stack,
			    &ins, ins_size, skipdepth));
	}

	if (0) {
err:		/*
		 * Remove the update from the current transaction, so we don't
		 * try to modify it on rollback.
		 */
		if (logged)
			__wt_txn_unmodify(session);
		__wt_free(session, ins);
		__wt_free(session, upd);
	}

	return (ret);
}

/*
 * __col_insert_alloc --
 *	Column-store insert: allocate a WT_INSERT structure and fill it in.
 */
static int
__col_insert_alloc(WT_SESSION_IMPL *session,
    uint64_t recno, u_int skipdepth, WT_INSERT **insp, size_t *ins_sizep)
{
	WT_INSERT *ins;
	size_t ins_size;

	/*
	 * Allocate the WT_INSERT structure and skiplist pointers, then copy
	 * the record number into place.
	 */
	ins_size = sizeof(WT_INSERT) + skipdepth * sizeof(WT_INSERT *);
	WT_RET(__wt_calloc(session, 1, ins_size, &ins));

	WT_INSERT_RECNO(ins) = recno;

	*insp = ins;
	*ins_sizep = ins_size;
	return (0);
}

/*
 * __wt_col_leaf_obsolete --
 *	Discard all obsolete updates on a column-store leaf page.
 */
void
__wt_col_leaf_obsolete(WT_SESSION_IMPL *session, WT_PAGE *page)
{
	WT_COL *cip;
	WT_INSERT *ins;
	WT_UPDATE *upd;
	uint32_t i;

	switch (page->type) {
	case WT_PAGE_COL_FIX:
		WT_SKIP_FOREACH(ins, WT_COL_UPDATE_SINGLE(page))
			if ((upd = __wt_update_obsolete_check(
			    session, ins->upd)) != NULL)
				__wt_update_obsolete_free(session, page, upd);
		break;

	case WT_PAGE_COL_VAR:
		WT_COL_FOREACH(page, cip, i)
			WT_SKIP_FOREACH(ins, WT_COL_UPDATE(page, cip))
				if ((upd = __wt_update_obsolete_check(
				    session, ins->upd)) != NULL)
					__wt_update_obsolete_free(
					    session, page, upd);
		break;
	}

	/* Walk any append list. */
	WT_SKIP_FOREACH(ins, WT_COL_APPEND(page)) {
		if ((upd =
		    __wt_update_obsolete_check(session, ins->upd)) != NULL)
			__wt_update_obsolete_free(session, page, upd);
	}
}
