/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wt_internal.h"

static int __col_insert_alloc(WT_SESSION_IMPL *, uint64_t, u_int, WT_INSERT **);

/*
 * __wt_col_modify --
 *	Column-store delete, insert, and update.
 */
int
__wt_col_modify(WT_SESSION_IMPL *session, WT_CURSOR_BTREE *cbt, int op)
{
	WT_BTREE *btree;
	WT_INSERT *ins;
	WT_INSERT_HEAD **inshead, *new_inshead, **new_inslist;
	WT_ITEM *value, _value;
	WT_PAGE *page;
	WT_UPDATE *upd;
	size_t new_inshead_size, new_inslist_size;
	uint64_t recno;
	u_int skipdepth;
	int i, ret;

	btree = cbt->btree;
	page = cbt->page;

	switch (op) {
	case 1:						/* Insert */
		page = btree->last_page;
		__cursor_search_clear(cbt);

		value = (WT_ITEM *)&cbt->iface.value;
		recno = 0;				/* Engine allocates */
		break;
	case 2:						/* Remove */
		if (btree->type == BTREE_COL_FIX) {
			value = &_value;
			value->data = "";
			value->size = 1;
		} else
			value = NULL;
		recno = cbt->iface.recno;		/* App specified */
		break;
	case 3:						/* Update */
	default:
		value = (WT_ITEM *)&cbt->iface.value;
		recno = cbt->iface.recno;		/* App specified */
		break;
	}

	ins = NULL;
	new_inshead = NULL;
	new_inslist = NULL;
	upd = NULL;
	ret = 0;

	/*
	 * Delete, insert or update a column-store entry.
	 */
	if (cbt->ins == NULL) {
		/* There may be no WT_INSERT_HEAD, allocate as necessary. */
		new_inshead_size = new_inslist_size = 0;
		if (op == 1) {
			if (page == NULL || page->u.col_leaf.append == NULL) {
				new_inslist_size = 1 *
				    sizeof(WT_INSERT_HEAD *);
				WT_ERR(
				    __wt_calloc_def(session, 1, &new_inslist));
				inshead = &new_inslist[0];
			} else
				inshead = &page->u.col_leaf.append[0];
			cbt->ins_head = *inshead;
		} else if (page->type == WT_PAGE_COL_FIX) {
			if (page->u.col_leaf.update == NULL) {
				new_inslist_size = 1 *
				    sizeof(WT_INSERT_HEAD *);
				WT_ERR(
				    __wt_calloc_def(session, 1, &new_inslist));
				inshead = &new_inslist[0];
			} else
				inshead = &page->u.col_leaf.update[0];
		} else {
			if (page->u.col_leaf.update == NULL) {
				new_inslist_size = page->entries *
				    sizeof(WT_INSERT_HEAD *);
				WT_ERR(__wt_calloc_def(
				    session, page->entries, &new_inslist));
				inshead = &new_inslist[cbt->slot];
			} else
				inshead = &page->u.col_leaf.update[cbt->slot];
		}

		/* There may be no WT_INSERT list, allocate as necessary. */
		if (*inshead == NULL) {
			new_inshead_size = sizeof(WT_INSERT_HEAD);
			WT_RET(__wt_calloc_def(session, 1, &new_inshead));
			for (i = 0; i < WT_SKIP_MAXDEPTH; i++)
				cbt->ins_stack[i] = &new_inshead->head[i];
			cbt->ins_head = new_inshead;
		}

		/* Choose a skiplist depth for this insert. */
		skipdepth = __wt_skip_choose_depth();

		/*
		 * Allocate a WT_INSERT/WT_UPDATE pair, and update the cursor
		 * to reference it.
		 */
		WT_ERR(__col_insert_alloc(session, recno, skipdepth, &ins));
		WT_ERR(__wt_update_alloc(session, value, &upd));
		ins->upd = upd;
		cbt->ins = ins;

		/*
		 * workQ: insert or append the WT_INSERT structure.
		 */
		if (op == 1) {
			WT_ERR(__wt_col_append_serial(session,
			    inshead, cbt->ins_stack,
			    &new_inslist, new_inslist_size,
			    &new_inshead, new_inshead_size, ins, skipdepth));

			/* Set up the cursor for the inserted page and value. */
			cbt->page = btree->last_page;
			cbt->recno = WT_INSERT_RECNO(ins);
		} else
			WT_ERR(__wt_insert_serial(session,
			    page, cbt->write_gen,
			    inshead, cbt->ins_stack,
			    &new_inslist, new_inslist_size,
			    &new_inshead, new_inshead_size, ins, skipdepth));
	} else {
		/*
		 * If changing an already changed record, create a new WT_UPDATE
		 * entry and have the workQ link it into an existing WT_INSERT
		 * entry's WT_UPDATE list.
		 */
		WT_ERR(__wt_update_alloc(session, value, &upd));

		/* workQ: insert the WT_UPDATE structure. */
		ret = __wt_update_serial(session, page,
		    cbt->write_gen, &cbt->ins->upd, NULL, 0, upd);
	}

	if (ret != 0) {
err:		if (ins != NULL)
			__wt_sb_decrement(session, ins->sb);
		if (upd != NULL)
			__wt_sb_decrement(session, upd->sb);
	}

	__wt_free(session, new_inslist);
	__wt_free(session, new_inshead);

	return (ret);
}

/*
 * __col_insert_alloc --
 *	Column-store insert: allocate a WT_INSERT structure from the session's
 *	buffer and fill it in.
 */
static int
__col_insert_alloc(
    WT_SESSION_IMPL *session, uint64_t recno, u_int skipdepth, WT_INSERT **insp)
{
	WT_SESSION_BUFFER *sb;
	WT_INSERT *ins;
	size_t ins_size;

	/*
	 * Allocate the WT_INSERT structure and skiplist pointers, then copy
	 * the record number into place.
	 */
	ins_size = sizeof(WT_INSERT) + skipdepth * sizeof(WT_INSERT *);
	WT_RET(__wt_sb_alloc(session, ins_size, &ins, &sb));

	ins->sb = sb;
	WT_INSERT_RECNO(ins) = recno;

	*insp = ins;
	return (0);
}

/*
 * __wt_col_append_serial_func --
 *	Server function to append an WT_INSERT entry to the tree.
 */
void
__wt_col_append_serial_func(WT_SESSION_IMPL *session)
{
	WT_BTREE *btree;
	WT_PAGE *page;
	WT_INSERT_HEAD **inshead, **new_inslist, *new_inshead;
	WT_INSERT *new_ins, ***ins_stack;
	uint64_t recno;
	u_int i, skipdepth;

	btree = session->btree;
	page = btree->last_page;

	__wt_col_append_unpack(session, &inshead, &ins_stack,
	    &new_inslist, &new_inshead, &new_ins, &skipdepth);

	/*
	 * If the page does not yet have an insert array, our caller passed
	 * us one.
	 */
	if (page->u.col_leaf.append == NULL) {
		page->u.col_leaf.append = new_inslist;
		__wt_col_append_new_inslist_taken(session, page);
	}

	/*
	 * If the insert head does not yet have an insert list, our caller
	 * passed us one.
	 */
	if (*inshead == NULL) {
		*inshead = new_inshead;
		__wt_col_append_new_inshead_taken(session, page);
	}

	/*
	 * If the application specified a record number, there's a race: the
	 * application may have searched for the record, not found it, then
	 * called into the append code, and another thread might have added
	 * the record.  Fortunately, we're in the right place because if the
	 * record didn't exist at some point, it can only have been created
	 * on this list.  Search for the record, if specified.
	 */
	if ((recno = WT_INSERT_RECNO(new_ins)) == 0)
		recno = WT_INSERT_RECNO(new_ins) = ++btree->last_recno;
	(void)__col_insert_search_stack(*inshead, ins_stack, recno);

	/*
	 * First, point the new WT_INSERT item's skiplist references to the next
	 * elements in the insert list, then flush memory.  Second, update the
	 * skiplist elements that reference the new WT_INSERT item, this ensures
	 * the list is never inconsistent.
	 */
	for (i = 0; i < skipdepth; i++)
		new_ins->next[i] = *ins_stack[i];
	WT_MEMORY_FLUSH;
	for (i = 0; i < skipdepth; i++) {
		if ((*inshead)->tail[i] == NULL ||
		    ins_stack[i] == &(*inshead)->tail[i]->next[i])
			(*inshead)->tail[i] = new_ins;
		*ins_stack[i] = new_ins;
	}

	__wt_session_serialize_wrapup(session, page, 0);
}
