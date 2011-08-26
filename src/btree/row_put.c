/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wt_internal.h"

/*
 * __wt_row_modify --
 *	Row-store delete insert, and update.
 */
int
__wt_row_modify(WT_SESSION_IMPL *session, WT_CURSOR_BTREE *cbt, int is_remove)
{
	WT_INSERT *ins;
	WT_INSERT_HEAD **inshead, *new_inshead, **new_inslist;
	WT_ITEM *key, *value;
	WT_PAGE *page;
	WT_SESSION_BUFFER *sb;
	WT_UPDATE **new_upd, *upd, **upd_entry;
	uint32_t ins_size, new_inshead_size, new_inslist_size, new_upd_size;
	uint32_t skipdepth, upd_size;
	int i, ret;

	key = (WT_ITEM *)&cbt->iface.key;
	value = is_remove ? NULL : (WT_ITEM *)&cbt->iface.value;

	page = cbt->page;

	ins = NULL;
	new_inshead = NULL;
	new_inslist = NULL;
	new_upd = NULL;
	upd = NULL;
	ret = 0;

	/*
	 * Modify: allocate an update array as necessary, build a WT_UPDATE
	 * structure in per-thread memory, and schedule the workQ to insert
	 * the WT_UPDATE structure.
	 *
	 * Insert: allocate an insert array as necessary, build a WT_INSERT
	 * and WT_UPDATE structure pair in per-thread memory, and schedule
	 * the workQ to insert the WT_INSERT structure.
	 */
	if (cbt->match == 1) {
		new_upd_size = 0;
		if (cbt->ins == NULL) {
			/*
			 * Allocate an update array as necessary.
			 *
			 * Set the WT_UPDATE array reference.
			 */
			if (page->u.row_leaf.upd == NULL) {
				WT_ERR(__wt_calloc_def(
				    session, page->entries, &new_upd));
				new_upd_size =
				    page->entries * WT_SIZEOF32(WT_UPDATE *);
				upd_entry = &new_upd[cbt->slot];
			} else
				upd_entry = &page->u.row_leaf.upd[cbt->slot];
		} else
			upd_entry = &cbt->ins->upd;

		/* Allocate room for the new value from per-thread memory. */
		WT_ERR(__wt_update_alloc(session, value, &upd, &upd_size));

		/* workQ: to insert the WT_UPDATE structure. */
		ret = __wt_xxxupdate_serial(session, page, cbt->write_gen,
		    upd_entry, &new_upd, new_upd_size, &upd, upd_size);
	} else {
		/*
		 * Allocate insert array necessary (we allocate an additional
		 * slot for insert keys sorting less than any original key on
		 * the page).
		 *
		 * Set the WT_INSERT array reference.
		 */
		new_inshead_size = new_inslist_size = 0;
		if (page->u.row_leaf.ins == NULL) {
			WT_ERR(__wt_calloc_def(
			    session, page->entries + 1, &new_inslist));
			new_inslist_size =
			    (page->entries + 1) * WT_SIZEOF32(WT_INSERT_HEAD *);
			inshead = &new_inslist[cbt->slot];
		} else
			inshead = &page->u.row_leaf.ins[cbt->slot];

		/*
		 * Allocate a new insert list head as necessary.
		 *
		 * If allocating a new insert list head, we have to initialize
		 * the cursor's insert list stack and insert head reference as
		 * well, search couldn't have.
		 */
		if (*inshead == NULL) {
			WT_ERR(__wt_sb_alloc(session,
			    sizeof(WT_INSERT_HEAD), &new_inshead, &sb));
			new_inshead->sb = sb;
			new_inshead_size = WT_SIZEOF32(WT_INSERT_HEAD);
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
		WT_ERR(__wt_row_insert_alloc(
		    session, key, skipdepth, &ins, &ins_size));
		WT_ERR(__wt_update_alloc(session, value, &upd, &upd_size));
		ins->upd = upd;
		ins_size += upd_size;
		cbt->ins = ins;

		/* workQ: insert the WT_INSERT structure. */
		ret = __wt_xxxinsert_serial(session,
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

	/* Free any insert, update arrays. */
	__wt_free(session, new_inshead);
	__wt_free(session, new_inslist);
	__wt_free(session, new_upd);

	return (ret);
}

/*
 * __wt_row_insert_alloc --
 *	Row-store insert: allocate a WT_INSERT structure from the session's
 *	buffer and fill it in.
 */
int
__wt_row_insert_alloc(WT_SESSION_IMPL *session,
    WT_ITEM *key, uint32_t skipdepth, WT_INSERT **insp, uint32_t *ins_sizep)
{
	WT_SESSION_BUFFER *sb;
	WT_INSERT *ins;
	uint32_t ins_size;

	/*
	 * Allocate the WT_INSERT structure, next pointers for the skip list,
	 * and room for the key.  Then copy the key into place.
	 */
	ins_size = WT_SIZEOF32(WT_INSERT) +
	    skipdepth * WT_SIZEOF32(WT_INSERT *) + key->size;
	WT_RET(__wt_sb_alloc(session, ins_size, &ins, &sb));

	ins->sb = sb;
	ins->u.key.offset = ins_size - key->size;
	WT_INSERT_KEY_SIZE(ins) = key->size;
	memcpy(WT_INSERT_KEY(ins), key->data, key->size);

	*insp = ins;
	*ins_sizep = ins_size;

	return (0);
}

/*
 * __wt_xxxinsert_serial_func --
 *	Server function to add an WT_INSERT entry to the page tree.
 */
int
__wt_xxxinsert_serial_func(WT_SESSION_IMPL *session)
{
	WT_PAGE *page;
	WT_INSERT_HEAD **inshead, **new_inslist, *new_inshead;
	WT_INSERT *ins, ***ins_stack;
	uint32_t i, skipdepth, write_gen;
	int ret;

	ret = 0;

	__wt_xxxinsert_unpack(session, &page, &write_gen, &inshead,
	    &ins_stack, &new_inslist, &new_inshead, &ins, &skipdepth);

	/* Check the page's write-generation. */
	WT_ERR(__wt_page_write_gen_check(page, write_gen));

	/*
	 * If the page does not yet have an insert array, our caller passed
	 * us one.
	 */
	if (page->type == WT_PAGE_ROW_LEAF) {
		if (page->u.row_leaf.ins == NULL) {
			page->u.row_leaf.ins = new_inslist;
			__wt_xxxinsert_new_inslist_taken(session, page);
		}
	} else
		if (page->u.col_leaf.ins == NULL) {
			page->u.col_leaf.ins = new_inslist;
			__wt_xxxinsert_new_inslist_taken(session, page);
		}

	/*
	 * If the slot does not yet have an insert list, our caller passed us
	 * one.
	 */
	if (*inshead == NULL) {
		*inshead = new_inshead;
		__wt_xxxinsert_new_inshead_taken(session, page);
	}

	/*
	 * First, point the new WT_INSERT item's skiplist references to the next
	 * elements in the insert list, then flush memory.  Second, update the
	 * skiplist elements that reference the new WT_INSERT item, this ensures
	 * the list is never inconsistent.
	 */
	for (i = 0; i < skipdepth; i++)
		ins->next[i] = *ins_stack[i];
	WT_MEMORY_FLUSH;
	for (i = 0; i < skipdepth; i++)
		*ins_stack[i] = ins;
	__wt_xxxinsert_ins_taken(session, page);

err:	__wt_session_serialize_wrapup(session, page, 0);
	return (ret);
}

/*
 * __wt_insert_serial_func --
 *	Server function to add an WT_INSERT entry to the page tree.
 */
int
__wt_insert_serial_func(WT_SESSION_IMPL *session)
{
	WT_PAGE *page;
	WT_INSERT_HEAD **new_inslist, *new_inshead;
	WT_INSERT *ins;
	WT_SEARCH *srch;
	uint32_t i, skipdepth;
	int ret;

	ret = 0;

	__wt_insert_unpack(session,
	    &srch, &new_inslist, &new_inshead, &ins, &skipdepth);
	page = srch->page;

	/* Check the page's write-generation. */
	WT_ERR(__wt_page_write_gen_check(page, srch->write_gen));

	/*
	 * If the page does not yet have an insert array, our caller passed
	 * us one of the correct size.	 (It's the caller's responsibility to
	 * detect and free the passed-in expansion array if we don't use it.)
	 */
	switch (page->type) {
	case WT_PAGE_ROW_LEAF:
		if (page->u.row_leaf.ins == NULL) {
			page->u.row_leaf.ins = new_inslist;
			__wt_insert_new_inslist_taken(session, page);
		}
		break;
	default:
		if (page->u.col_leaf.ins == NULL) {
			page->u.col_leaf.ins = new_inslist;
			__wt_insert_new_inslist_taken(session, page);
		}
		break;
	}

	if (*srch->inshead == NULL) {
		*srch->inshead = new_inshead;
		__wt_insert_new_inshead_taken(session, page);
	}

	/*
	 * Insert the new WT_INSERT item into the linked list and flush memory
	 * to ensure the list is never broken.
	 */
	__wt_insert_ins_taken(session, page);
	for (i = 0; i < skipdepth; i++)
		ins->next[i] = *srch->ins[i];
	WT_MEMORY_FLUSH;
	for (i = 0; i < skipdepth; i++)
		*srch->ins[i] = ins;

err:	__wt_session_serialize_wrapup(session, page, 0);
	return (ret);
}

/*
 * __wt_update_alloc --
 *	Allocate a WT_UPDATE structure and associated value from the session's
 *	buffer and fill it in.
 */
int
__wt_update_alloc(WT_SESSION_IMPL *session,
    WT_ITEM *value, WT_UPDATE **updp, uint32_t *upd_sizep)
{
	WT_SESSION_BUFFER *sb;
	WT_UPDATE *upd;
	uint32_t size;

	/*
	 * Allocate the WT_UPDATE structure and room for the value, then copy
	 * the value into place.
	 */
	size = value == NULL ? 0 : value->size;
	WT_RET(__wt_sb_alloc(session, sizeof(WT_UPDATE) + size, &upd, &sb));
	upd->sb = sb;
	if (value == NULL)
		WT_UPDATE_DELETED_SET(upd);
	else {
		upd->size = size;
		memcpy(WT_UPDATE_DATA(upd), value->data, size);
	}

	*upd_sizep = size + WT_SIZEOF32(WT_UPDATE);
	*updp = upd;

	return (0);
}

/*
 * __wt_update_serial_func --
 *	Server function to add an WT_UPDATE entry in the page array.
 */
int
__wt_update_serial_func(WT_SESSION_IMPL *session)
{
	WT_PAGE *page;
	WT_SEARCH *srch;
	WT_UPDATE **new_upd, *upd;
	int ret;

	ret = 0;

	__wt_update_unpack(
	    session, &srch, &new_upd, &upd);
	page = srch->page;

	/* Check the page's write-generation. */
	WT_ERR(__wt_page_write_gen_check(page, srch->write_gen));

	/*
	 * If the page needs an update array (column-store pages and inserts on
	 * row-store pages do not use the update array), our caller passed us
	 * one of the correct size.   Check the page still needs one (the write
	 * generation test should have caught that, though).
	 */
	if (new_upd != NULL && page->u.row_leaf.upd == NULL) {
		page->u.row_leaf.upd = new_upd;
		__wt_update_new_upd_taken(session, page);
	}

	/*
	 * Insert the new WT_UPDATE item into the linked list and flush memory
	 * to ensure the list is never broken.
	 */
	__wt_update_upd_taken(session, page);
	upd->next = *srch->upd;
	WT_MEMORY_FLUSH;
	*srch->upd = upd;

err:	__wt_session_serialize_wrapup(session, page, 0);
	return (ret);
}

/*
 * __wt_xxxupdate_serial_func --
 *	Server function to add an WT_UPDATE entry in the page array.
 */
int
__wt_xxxupdate_serial_func(WT_SESSION_IMPL *session)
{
	WT_PAGE *page;
	WT_UPDATE **new_upd, *upd, **upd_entry;
	uint32_t write_gen;
	int ret;

	ret = 0;

	__wt_xxxupdate_unpack(
	    session, &page, &write_gen, &upd_entry, &new_upd, &upd);

	/* Check the page's write-generation. */
	WT_ERR(__wt_page_write_gen_check(page, write_gen));

	/*
	 * If the page needs an update array (column-store pages and inserts on
	 * row-store pages do not use the update array), our caller passed us
	 * one of the correct size.   Check the page still needs one (the write
	 * generation test should have caught that, though).
	 */
	if (new_upd != NULL && page->u.row_leaf.upd == NULL) {
		page->u.row_leaf.upd = new_upd;
		__wt_xxxupdate_new_upd_taken(session, page);
	}

	/*
	 * Insert the new WT_UPDATE item into the linked list and flush memory
	 * to ensure the list is never broken.
	 */
	upd->next = *upd_entry;
	WT_MEMORY_FLUSH;
	*upd_entry = upd;
	__wt_xxxupdate_upd_taken(session, page);

err:	__wt_session_serialize_wrapup(session, page, 0);
	return (ret);
}
