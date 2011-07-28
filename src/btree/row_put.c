/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wt_internal.h"

/*
 * __wt_row_modify --
 *	Row-store delete and update.
 */
int
__wt_row_modify(
    WT_SESSION_IMPL *session, WT_ITEM *key, WT_ITEM *value, int is_write)
{
	WT_INSERT_HEAD **new_inslist, *new_inshead;
	WT_INSERT *ins;
	WT_PAGE *page;
	WT_SESSION_BUFFER *sb;
	WT_UPDATE **new_upd, *upd;
	uint32_t ins_size, new_inshead_size, new_inslist_size;
	uint32_t new_upd_size, upd_size;
	int i, ret, skipdepth;

	new_inshead = NULL;
	new_inslist = NULL;
	ins = NULL;
	new_upd = NULL;
	upd = NULL;
	ins_size = new_inshead_size = new_inslist_size = 0;
	new_upd_size = upd_size = 0;
	ret = skipdepth = 0;

	/* Search the btree for the key. */
	WT_RET(__wt_row_search(session, key, is_write ? WT_WRITE : 0));
	page = session->srch.page;

	/*
	 * Replace: allocate an update array as necessary, build a WT_UPDATE
	 * structure in per-thread memory, and schedule the workQ to insert
	 * the WT_UPDATE structure.
	 *
	 * Insert: allocate an insert array as necessary, build a WT_INSERT
	 * structure in per-thread memory, and schedule the workQ to insert
	 * the WT_INSERT structure.
	 */
	if (session->srch.match == 1) {
		/* Allocate an update array as necessary. */
		if (session->srch.upd == NULL) {
			WT_ERR(__wt_calloc_def(
			    session, page->entries, &new_upd));
			new_upd_size =
			    page->entries * WT_SIZEOF32(WT_UPDATE *);
			/*
			 * If there was no update array, the search function
			 * could not have set the WT_UPDATE location.
			 */
			session->srch.upd = &new_upd[session->srch.slot];
		}

		/* Allocate room for the new value from per-thread memory. */
		WT_ERR(__wt_update_alloc(session, value, &upd, &upd_size));

		/* workQ: to insert the WT_UPDATE structure. */
		ret = __wt_update_serial(
		    session, page, session->srch.write_gen,
		    &new_upd, new_upd_size, session->srch.upd, &upd, upd_size);
	} else {
		/*
		 * Allocate an insert array and/or an insert list head as
		 * necessary -- note we allocate one additional slot for insert
		 * keys sorting less than any original key on the page.
		 */
		if (session->srch.inshead == NULL) {
			WT_ERR(__wt_calloc_def(
			    session, page->entries + 1, &new_inslist));
			new_inslist_size = (page->entries + 1) *
			    WT_SIZEOF32(WT_INSERT_HEAD *);
			/*
			 * If there was no insert array, the search function
			 * could not have set the WT_INSERT location.
			 */
			session->srch.inshead =
			    &new_inslist[session->srch.slot];
		}

		if (*session->srch.inshead == NULL) {
			WT_ERR(__wt_sb_alloc(session, sizeof(WT_INSERT_HEAD),
			    &new_inshead, &sb));
			new_inshead->sb = sb;
			new_inshead_size = WT_SIZEOF32(WT_INSERT_HEAD);
			for (i = 0; i < WT_SKIP_MAXDEPTH; i++)
				session->srch.ins[i] = &new_inshead->head[i];
		}

		/* Choose a skiplist depth for this insert. */
		WT_SKIP_CHOOSE_DEPTH(skipdepth);

		/* Allocate a WT_INSERT/WT_UPDATE pair. */
		WT_ERR(__wt_row_insert_alloc(session,
		    key, skipdepth, &ins, &ins_size));
		WT_ERR(__wt_update_alloc(session, value, &upd, &upd_size));
		ins->upd = upd;
		ins_size += upd_size;

		/* workQ: insert the WT_INSERT structure. */
		ret = __wt_insert_serial(session, &session->srch,
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

	__wt_page_release(session, page);

	return (ret);
}

/*
 * __wt_row_insert_alloc --
 *	Row-store insert: allocate a WT_INSERT structure from the session's
 *	buffer and fill it in.
 */
int
__wt_row_insert_alloc(WT_SESSION_IMPL *session,
    WT_ITEM *key, int skipdepth, WT_INSERT **insp, uint32_t *ins_sizep)
{
	WT_SESSION_BUFFER *sb;
	WT_INSERT *ins;
	uint32_t ins_size;

	/*
	 * Allocate the WT_INSERT structure, next pointers for the skip list,
	 * and room for the key.  Then copy the key into place.
	 */
	ins_size = WT_SIZEOF32(WT_INSERT) +
	    skipdepth * WT_SIZEOF32(WT_INSERT *) +
	    key->size;
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
	int i, ret, skipdepth;

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
	WT_UPDATE **new_upd, **srch_upd, *upd;
	uint32_t write_gen;
	int ret;

	ret = 0;

	__wt_update_unpack(
	    session, &page, &write_gen, &new_upd, &srch_upd, &upd);

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
		__wt_update_new_upd_taken(session, page);
	}

	/*
	 * Insert the new WT_UPDATE item into the linked list and flush memory
	 * to ensure the list is never broken.
	 */
	__wt_update_upd_taken(session, page);
	upd->next = *srch_upd;
	WT_MEMORY_FLUSH;
	*srch_upd = upd;

err:	__wt_session_serialize_wrapup(session, page, 0);
	return (ret);
}
