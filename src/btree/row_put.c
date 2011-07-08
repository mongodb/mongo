/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wt_internal.h"

static int __wt_row_update(WT_SESSION_IMPL *, WT_ITEM *, WT_ITEM *, int);

/*
 * __wt_btree_row_del --
 *	Db.row_del method.
 */
int
__wt_btree_row_del(WT_SESSION_IMPL *session, WT_ITEM *key)
{
	int ret;

	while ((ret = __wt_row_update(session, key, NULL, 0)) == WT_RESTART)
		;
	return (ret);
}

/*
 * __wt_btree_row_put --
 *	Db.row_put method.
 */
int
__wt_btree_row_put(WT_SESSION_IMPL *session, WT_ITEM *key, WT_ITEM *value)
{
	int ret;

	while ((ret = __wt_row_update(session, key, value, 1)) == WT_RESTART)
		;
	return (ret);
}

/*
 * __wt_row_update --
 *	Row-store delete and update.
 */
static int
__wt_row_update(
    WT_SESSION_IMPL *session, WT_ITEM *key, WT_ITEM *value, int is_write)
{
	WT_INSERT **new_ins, *ins;
	WT_PAGE *page;
	WT_UPDATE **new_upd, *upd;
	uint32_t ins_size, new_ins_size, new_upd_size, upd_size;
	int ret;

	new_ins = NULL;
	ins = NULL;
	new_upd = NULL;
	upd = NULL;
	ins_size = new_ins_size = new_upd_size = upd_size = 0;
	ret = 0;

	/* Search the btree for the key. */
	WT_RET(__wt_row_search(session, key, is_write ? WT_WRITE : 0));
	page = session->srch_page;

	/*
	 * Replace: allocate an update array as necessary, build a WT_UPDATE
	 * structure in per-thread memory, and schedule the workQ to insert
	 * the WT_UPDATE structure.
	 *
	 * Insert: allocate an insert array as necessary, build a WT_INSERT
	 * structure in per-thread memory, and schedule the workQ to insert
	 * the WT_INSERT structure.
	 */
	if (session->srch_match == 1) {
		/* Allocate an update array as necessary. */
		if (session->srch_upd == NULL) {
			WT_ERR(__wt_calloc_def(
			    session, page->entries, &new_upd));
			new_upd_size = page->entries * sizeof(WT_UPDATE *);
			/*
			 * If there was no update array, the search function
			 * could not have set the WT_UPDATE location.
			 */
			session->srch_upd = &new_upd[session->srch_slot];
		}

		/* Allocate room for the new value from per-thread memory. */
		WT_ERR(__wt_update_alloc(session, value, &upd, &upd_size));

		/* workQ: to insert the WT_UPDATE structure. */
		ret = __wt_update_serial(
		    session, page, session->srch_write_gen,
		    &new_upd, new_upd_size, session->srch_upd, &upd, upd_size);
	} else {
		/*
		 * Allocate an insert array as necessary -- note we allocate
		 * one additional slot for insert keys sorting less than any
		 * original key on the page.
		 */
		if (session->srch_ins == NULL) {
			WT_ERR(__wt_calloc_def(
			    session, page->entries + 1, &new_ins));
			new_ins_size = page->entries + 1 * sizeof(WT_INSERT *);
			/*
			 * If there was no insert array, the search function
			 * could not have set the WT_INSERT location.
			 */
			session->srch_ins = &new_ins[session->srch_slot];
		}

		/* Allocate a WT_INSERT/WT_UPDATE pair. */
		WT_ERR(__wt_row_insert_alloc(session, key, &ins, &ins_size));
		WT_ERR(__wt_update_alloc(session, value, &upd, &upd_size));
		ins->upd = upd;
		ins_size += upd_size;

		/* workQ: insert the WT_INSERT structure. */
		ret = __wt_insert_serial(
		    session, page, session->srch_write_gen,
		    &new_ins, new_ins_size, session->srch_ins, &ins, ins_size);
	}

	if (ret != 0) {
err:		if (ins != NULL)
			__wt_sb_decrement(session, ins->sb);
		if (upd != NULL)
			__wt_sb_decrement(session, upd->sb);
	}

	/* Free any insert array. */
	if (new_ins != NULL)
		__wt_free(session, new_ins);

	/* Free any update array. */
	if (new_upd != NULL)
		__wt_free(session, new_upd);

	WT_PAGE_OUT(session, page);

	return (ret);
}

/*
 * __wt_row_insert_alloc --
 *	Row-store insert: allocate a WT_INSERT structure from the session's
 *	buffer and fill it in.
 */
int
__wt_row_insert_alloc(WT_SESSION_IMPL *session,
    WT_ITEM *key, WT_INSERT **insp, uint32_t *ins_sizep)
{
	WT_SESSION_BUFFER *sb;
	WT_INSERT *ins;
	uint32_t size;

	/*
	 * Allocate the WT_INSERT structure and room for the key, then copy
	 * the key into place.
	 */
	size = key->size;
	WT_RET(__wt_sb_alloc(
	    session, sizeof(WT_INSERT) + sizeof(uint32_t) + size, &ins, &sb));

	ins->sb = sb;
	WT_INSERT_KEY_SIZE(ins) = size;
	memcpy(WT_INSERT_KEY(ins), key->data, size);

	*insp = ins;
	*ins_sizep = sizeof(WT_INSERT) + sizeof(uint32_t) + size;

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
	WT_INSERT **new_ins, **srch_ins, *ins;
	uint32_t write_gen;
	int ret;

	ret = 0;

	__wt_insert_unpack(
	    session, &page, &write_gen, &new_ins, &srch_ins, &ins);

	/* Check the page's write-generation. */
	WT_ERR(__wt_page_write_gen_check(page, write_gen));

	/*
	 * If the page does not yet have an insert array, our caller passed
	 * us one of the correct size.   (It's the caller's responsibility to
	 * detect and free the passed-in expansion array if we don't use it.)
	 */
	switch (page->type) {
	case WT_PAGE_ROW_LEAF:
		if (page->u.row_leaf.ins == NULL) {
			page->u.row_leaf.ins = new_ins;
			__wt_insert_new_ins_taken(session, page);
		}
		break;
	default:
		if (page->u.col_leaf.ins == NULL) {
			page->u.col_leaf.ins = new_ins;
			__wt_insert_new_ins_taken(session, page);
		}
		break;
	}

	/*
	 * Insert the new WT_INSERT item into the linked list and flush memory
	 * to ensure the list is never broken.
	 */
	__wt_insert_ins_taken(session, page);
	ins->next = *srch_ins;
	WT_MEMORY_FLUSH;
	*srch_ins = ins;

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

	*upd_sizep = size + sizeof(WT_UPDATE);
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
	 * If the page does not yet have an update array, our caller passed
	 * us one of the correct size.   (It's the caller's responsibility to
	 * detect and free the passed-in expansion array if we don't use it.)
	 */
	if (page->type == WT_PAGE_ROW_LEAF) {
		if (page->u.row_leaf.upd == NULL) {
			page->u.row_leaf.upd = new_upd;
			__wt_update_new_upd_taken(session, page);
		}
	} else
		if (page->u.col_leaf.upd == NULL) {
			page->u.col_leaf.upd = new_upd;
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
