/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wt_internal.h"

static int __col_insert_alloc(
	WT_SESSION_IMPL *, uint64_t, u_int, WT_INSERT **, size_t *);

/*
 * __wt_col_modify --
 *	Column-store delete insert, and update.
 */
int
__wt_col_modify(WT_SESSION_IMPL *session, WT_CURSOR_BTREE *cbt, int is_remove)
{
	WT_BTREE *btree;
	WT_INSERT *ins;
	WT_INSERT_HEAD **inshead, *new_inshead, **new_inslist;
	WT_ITEM *value, _value;
	WT_PAGE *page;
	WT_SESSION_BUFFER *sb;
	WT_UPDATE *upd;
	size_t ins_size, new_inslist_size, new_inshead_size, upd_size;
	uint64_t recno;
	u_int skipdepth;
	int hazard_ref, i, ret;

	btree = cbt->btree;
	page = cbt->page;

	recno = cbt->iface.recno;
	if (is_remove) {
		if (btree->type == BTREE_COL_FIX) {
			value = &_value;
			value->data = "";
			value->size = 1;
		} else
			value = NULL;
	} else
		value = (WT_ITEM *)&cbt->iface.value;

	/*
	 * Append a column-store entry (the only place you can insert into a
	 * column-store file is after the key space, column-store records are
	 * immutable).  If we don't have an exact match, it's an append and we
	 * need to extend the file.
	 */
	if (cbt->compare != 0) {
		/*
		 * We may have, and need to hold, a hazard reference on a page,
		 * but we're possibly doing some page shuffling of the root,
		 * which means the standard test to determine whether we should
		 * release a hazard reference on the page isn't right.  Check
		 * now, before we do the page shuffling.
		 */
		hazard_ref = page == session->btree->root_page.page ? 0 : 1;
		ret = __wt_col_extend(session, page, recno);
		if (hazard_ref) {
			__wt_page_release(session, page);
			cbt->page = NULL;		/* XXX WRONG */
		}
		return (ret == 0 ? WT_RESTART : 0);
	}

	ins = NULL;
	new_inshead = NULL;
	new_inslist = NULL;
	upd = NULL;
	ret = 0;

	/*
	 * Delete or update a column-store entry.
	 * Column-store changes mean working in a WT_INSERT list.
	 */
	if (cbt->ins != NULL) {
		/*
		 * If changing an already changed record, create a new WT_UPDATE
		 * entry and have the workQ link it into an existing WT_INSERT
		 * entry's WT_UPDATE list.
		 */
		WT_ERR(__wt_update_alloc(session, value, &upd, &upd_size));

		/* workQ: insert the WT_UPDATE structure. */
		ret = __wt_update_serial(session, page,
		    cbt->write_gen, &cbt->ins->upd, NULL, 0, &upd, upd_size);
	} else {
		/*
		 * We may not have an WT_INSERT_HEAD array (in the case of
		 * variable length column store) or WT_INSERT_HEAD slot (in the
		 * case of fixed length column store).  Also, there may be an
		 * insert array but no list at the point we are inserting.
		 * Allocate as necessary.
		 */
		new_inshead_size = new_inslist_size = 0;
		if (page->u.col_leaf.ins == NULL)
			switch (page->type) {
			case WT_PAGE_COL_FIX:
				new_inslist_size = 1 *
				    sizeof(WT_INSERT_HEAD *);
				WT_ERR(__wt_calloc_def(session,
				    1, &new_inslist));
				inshead = &new_inslist[0];
				break;
			case WT_PAGE_COL_VAR:
				new_inslist_size = page->entries *
				    sizeof(WT_INSERT_HEAD *);
				WT_ERR(__wt_calloc_def(
				    session, page->entries, &new_inslist));
				inshead = &new_inslist[cbt->slot];
				break;
			WT_ILLEGAL_FORMAT(session);
			}
		else
			switch (page->type) {
			case WT_PAGE_COL_FIX:
				inshead = &page->u.col_leaf.ins[0];
				break;
			case WT_PAGE_COL_VAR:
				inshead = &page->u.col_leaf.ins[cbt->slot];
				break;
			WT_ILLEGAL_FORMAT(session);
			}

		if (*inshead == NULL) {
			new_inshead_size = sizeof(WT_INSERT_HEAD);
			WT_RET(__wt_sb_alloc(session,
			    sizeof(WT_INSERT_HEAD), &new_inshead, &sb));
			new_inshead->sb = sb;
			for (i = 0; i < WT_SKIP_MAXDEPTH; i++)
				cbt->ins_stack[i] = &new_inshead->head[i];
			cbt->ins_head = new_inshead;
		}

		/* Choose a skiplist depth for this insert. */
		skipdepth = __wt_skip_choose_depth();

		/*
		 * Allocate a new WT_INSERT/WT_UPDATE pair, link it into the
		 * WT_INSERT array.
		 */
		WT_ERR(__col_insert_alloc(
		    session, recno, skipdepth, &ins, &ins_size));
		WT_ERR(__wt_update_alloc(session, value, &upd, &upd_size));
		ins->upd = upd;
		ins_size += upd_size;
		cbt->ins = ins;

		/*
		 * workQ: insert the WT_INSERT structure.
		 *
		 * For fixed-width stores, we are installing a single insert
		 * head for the page.  Pass NULL to the insert serialization
		 * function, there is no need to set it again, and we only want
		 * to account for it once.
		 */
		ret = __wt_insert_serial(session,
		    page, cbt->write_gen,
		    inshead, cbt->ins_stack,
		    &new_inslist, new_inslist_size,
		    &new_inshead, new_inshead_size,
		    &ins, ins_size, skipdepth);
	}

	if (ret != 0) {
err:		if (ins != NULL)
			__wt_sb_decrement(session, ins->sb);
		if (upd != NULL)
			__wt_sb_decrement(session, upd->sb);
	}

	__wt_free(session, new_inslist);

	return (ret);
}

/*
 * __col_insert_alloc --
 *	Column-store insert: allocate a WT_INSERT structure from the session's
 *	buffer and fill it in.
 */
static int
__col_insert_alloc(WT_SESSION_IMPL *session,
    uint64_t recno, u_int skipdepth, WT_INSERT **insp, size_t *ins_sizep)
{
	WT_SESSION_BUFFER *sb;
	WT_INSERT *ins;
	size_t ins_size;

	/*
	 * Allocate the WT_INSERT structure and skiplist pointers, then copy
	 * the record number into place.
	 */
	ins_size = sizeof(WT_INSERT) +
	    skipdepth * sizeof(WT_INSERT *);
	WT_RET(__wt_sb_alloc(session, ins_size, &ins, &sb));

	ins->sb = sb;
	WT_INSERT_RECNO(ins) = recno;

	*insp = ins;
	*ins_sizep = ins_size;
	return (0);
}
