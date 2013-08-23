/*-
 * Copyright (c) 2008-2013 WiredTiger, Inc.
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
__wt_col_modify(WT_SESSION_IMPL *session, WT_CURSOR_BTREE *cbt, int op)
{
	WT_BTREE *btree;
	WT_DECL_RET;
	WT_INSERT *ins;
	WT_INSERT_HEAD *inshead, **insheadp, *t;
	WT_ITEM *value, _value;
	WT_PAGE *page;
	WT_UPDATE *old_upd, *upd, *upd_obsolete;
	size_t ins_size, upd_size;
	uint64_t recno;
	u_int skipdepth;
	int i, logged;

	btree = cbt->btree;
	page = cbt->page;
	recno = cbt->iface.recno;
	logged = 0;

	WT_ASSERT(session, op != 1);

	switch (op) {
	case 2:						/* Remove */
		if (btree->type == BTREE_COL_FIX) {
			value = &_value;
			value->data = "";
			value->size = 1;
		} else
			value = NULL;
		break;
	case 3:						/* Insert/Update */
	default:
		value = &cbt->iface.value;

		/*
		 * There's some chance the application specified a record past
		 * the last record on the page.  If that's the case, and we're
		 * inserting a new WT_INSERT/WT_UPDATE pair, it goes on the
		 * append list, not the update list.   In addition, a recno of
		 * 0 implies an append operation, we're allocating a new row.
		 */
		if (recno == 0 || recno > __col_last_recno(page))
			op = 1;
		break;
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
		WT_ERR(__wt_txn_modify(session, &upd->txnid));
		logged = 1;

		/* Serialize the update. */
		WT_ERR(__wt_update_serial(session, page,
		    cbt->write_gen, &cbt->ins->upd, old_upd,
		    &upd, upd_size, &upd_obsolete));

		/* Discard any obsolete WT_UPDATE structures. */
		if (upd_obsolete != NULL)
			__wt_update_obsolete_free(session, page, upd_obsolete);
	} else {
		/* Allocate the append/update list reference as necessary. */
		if (op == 1) {
			if (page->modify->append == NULL) {
				WT_ERR(__wt_calloc_def(session, 1, &insheadp));
				if (WT_ATOMIC_CAS(
				    page->modify->append, NULL, insheadp))
					__wt_cache_page_inmem_incr(session,
					    page, sizeof(WT_INSERT_HEAD *));
				else
					__wt_free(session, insheadp);
			}
			insheadp = &page->modify->append[0];
		} else if (page->type == WT_PAGE_COL_FIX) {
			if (page->modify->update == NULL) {
				WT_ERR(__wt_calloc_def(session, 1, &insheadp));
				if (WT_ATOMIC_CAS(
				    page->modify->update, NULL, insheadp))
					__wt_cache_page_inmem_incr(session,
					    page, sizeof(WT_INSERT_HEAD *));
				else
					__wt_free(session, insheadp);
			}
			insheadp = &page->modify->update[0];
		} else {
			if (page->modify->update == NULL) {
				WT_ERR(__wt_calloc_def(
				    session, page->entries, &insheadp));
				if (WT_ATOMIC_CAS(
				    page->modify->update, NULL, insheadp))
					__wt_cache_page_inmem_incr(session,
					    page, page->entries *
					    sizeof(WT_INSERT_HEAD *));
				else
					__wt_free(session, insheadp);
			}
			insheadp = &page->modify->update[cbt->slot];
		}

		/* Allocate the WT_INSERT_HEAD structure as necessary. */
		if ((inshead = *insheadp) == NULL) {
			WT_ERR(__wt_calloc_def(session, 1, &t));
			if (WT_ATOMIC_CAS(*insheadp, NULL, t)) {
				__wt_cache_page_inmem_incr(session,
				    page, sizeof(WT_INSERT_HEAD));

				/*
				 * If allocating a new insert list head, we have
				 * to initialize the cursor's insert list stack
				 * and insert head reference as well, search
				 * couldn't have.
				 */
				for (i = 0; i < WT_SKIP_MAXDEPTH; i++) {
					cbt->ins_stack[i] = &t->head[i];
					cbt->next_stack[i] = NULL;
				}
				cbt->ins_head = t;
			} else {
				/*
				 * I'm not returning restart here, even though
				 * the update will fail (the cursor's insert
				 * stack is by definition wrong because it was
				 * never set).   The reason is because it won't
				 * close the race, it only makes it less likely
				 * (and maybe simplifies the serialization
				 * function check).  Let the serialization code
				 * own the problem.
				 *
				 * !!!
				 * Caveat: the comment here is the same as in
				 * the row-modify code, and it's correct with
				 * the exception that column-append actually
				 * will work because we're going to repeat the
				 * search.
				 */
				__wt_free(session, t);
			}

			inshead = *insheadp;
		}

		/* Choose a skiplist depth for this insert. */
		skipdepth = __wt_skip_choose_depth();

		/*
		 * Allocate a WT_INSERT/WT_UPDATE pair and transaction ID, and
		 * update the cursor to reference it.
		 */
		WT_ERR(__col_insert_alloc(
		    session, recno, skipdepth, &ins, &ins_size));
		WT_ERR(__wt_update_alloc(session, value, &upd, &upd_size));
		WT_ERR(__wt_txn_modify(session, &upd->txnid));
		logged = 1;
		ins->upd = upd;
		ins_size += upd_size;
		cbt->ins = ins;

		/* Insert or append the WT_INSERT structure. */
		if (op == 1)
			WT_ERR(__wt_col_append_serial(session,
			    page, cbt->write_gen, inshead,
			    cbt->ins_stack, cbt->next_stack,
			    &ins, ins_size, &cbt->recno, skipdepth));
		else
			WT_ERR(__wt_insert_serial(session,
			    page, cbt->write_gen, inshead,
			    cbt->ins_stack, cbt->next_stack,
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
 * __wt_col_append_serial_func --
 *	Server function to append an WT_INSERT entry to the tree.
 */
int
__wt_col_append_serial_func(WT_SESSION_IMPL *session, void *args)
{
	WT_BTREE *btree;
	WT_INSERT *ins, *new_ins, ***ins_stack, **next_stack;
	WT_INSERT_HEAD *inshead;
	WT_PAGE *page;
	uint64_t recno, *recnop;
	uint32_t write_gen;
	u_int i, skipdepth;

	btree = S2BT(session);

	__wt_col_append_unpack(args, &page, &write_gen,
	    &inshead, &ins_stack, &next_stack, &new_ins, &recnop, &skipdepth);

	/*
	 * Largely ignore the page's write-generation, just confirm it hasn't
	 * wrapped.
	 */
	WT_RET(__wt_page_write_gen_wrapped_check(page));

	/*
	 * If the application specified a record number, there's a race: the
	 * application may have searched for the record, not found it, then
	 * called into the append code, and another thread might have added
	 * the record.  Fortunately, we're in the right place because if the
	 * record didn't exist at some point, it can only have been created
	 * on this list.  Search for the record, if specified.
	 *
	 * If the application didn't specify a record number, we still have
	 * to do a search in order to create the insert stack we need.
	 */
	if ((recno = WT_INSERT_RECNO(new_ins)) == 0)
		recno = *recnop  =
		    WT_INSERT_RECNO(new_ins) = ++btree->last_recno;

	ins = __col_insert_search(inshead, ins_stack, next_stack, recno);

	/*
	 * If we find the record number, there's been a race, and we should
	 * be updating an existing record, restart so that happens.
	 */
	if (ins != NULL && WT_INSERT_RECNO(ins) == recno)
		WT_RET(WT_RESTART);

	/*
	 * Publish: First, point the new WT_INSERT item's skiplist references
	 * to the next elements in the insert list, then flush memory.  Second,
	 * update the skiplist elements that reference the new WT_INSERT item,
	 * this ensures the list is never inconsistent.
	 */
	for (i = 0; i < skipdepth; i++)
		new_ins->next[i] = ins_stack[i] == NULL ? NULL : *ins_stack[i];
	WT_WRITE_BARRIER();
	for (i = 0; i < skipdepth; i++) {
		if (inshead->tail[i] == NULL ||
		    ins_stack[i] == &inshead->tail[i]->next[i])
			inshead->tail[i] = new_ins;
		if (ins_stack[i] != NULL)
			*ins_stack[i] = new_ins;
	}

	__wt_col_append_new_ins_taken(args);

	/*
	 * Check to see if we extended the file, and update the last record
	 * number.
	 */
	if (recno > btree->last_recno)
		btree->last_recno = recno;

	__wt_page_and_tree_modify_set(session, page);
	return (0);
}

/*
 * __wt_col_leaf_obsolete --
 *	Discard all obsolete updates on a column-store leaf page.
 */
void
__wt_col_leaf_obsolete(WT_SESSION_IMPL *session, WT_PAGE *page)
{
	uint32_t i;
	WT_COL *cip;
	WT_INSERT *ins;
	WT_UPDATE *upd;

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
