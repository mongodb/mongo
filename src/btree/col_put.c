/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wt_internal.h"
#include "btree.i"

static int __col_extend(WT_SESSION_IMPL *, WT_PAGE *, uint64_t);
static int __col_insert_alloc(WT_SESSION_IMPL *, uint64_t, WT_INSERT **);
static int __col_next_recno(WT_SESSION_IMPL *, WT_PAGE *, uint64_t *);
static int __col_update(WT_SESSION_IMPL *, uint64_t, WT_ITEM *, int);
static int __col_wrong_fixed_size(WT_SESSION_IMPL *, uint32_t, uint32_t);

/*
 * __wt_btree_col_del --
 *	Db.col_del method.
 */
int
__wt_btree_col_del(WT_SESSION_IMPL *session, uint64_t recno)
{
	int ret;

	while ((ret = __col_update(session, recno, NULL, 0)) == WT_RESTART)
		;
	return (ret);
}

/*
 * __wt_btree_col_put --
 *	Db.put method.
 */
int
__wt_btree_col_put(WT_SESSION_IMPL *session, uint64_t recno, WT_ITEM *value)
{
	WT_BTREE *btree;
	int ret;

	btree = session->btree;

	if (btree->type == BTREE_COL_FIX && value->size != btree->fixed_len)
		WT_RET(__col_wrong_fixed_size(
		    session, value->size, btree->fixed_len));

	while ((ret = __col_update(session, recno, value, 1)) == WT_RESTART)
		;
	return (ret);
}

/*
 * __col_update --
 *	Column-store delete and update.
 */
static int
__col_update(
    WT_SESSION_IMPL *session, uint64_t recno, WT_ITEM *value, int is_write)
{
	WT_PAGE *page;
	WT_INSERT **new_ins, *ins;
	WT_UPDATE **new_upd, *upd;
	int hazard_ref, ret;

	new_ins = NULL;
	ins = NULL;
	new_upd = NULL;
	upd = NULL;
	ret = 0;

	/* Search the btree for the key. */
	WT_RET(__wt_col_search(session, recno, is_write ? WT_WRITE : 0));
	page = session->srch_page;

	/*
	 * Append a column-store entry (the only place you can insert into a
	 * column-store file is at or after the key space, WiredTiger records
	 * are immutable).  If we don't have an exact match, it's an append
	 * and we need to extend the file.
	 */
	if (!session->srch_match) {
		/*
		 * We may have, and need to hold, a hazard reference on a page,
		 * but we're possibly doing some page shuffling of the root,
		 * which means the standard test to determine whether we should
		 * release a hazard reference on the page isn't right.  Check
		 * now, before we do the page shuffling.
		 */
		hazard_ref = page == session->btree->root_page.page ? 0 : 1;
		ret = __col_extend(session, page, recno);
		if (hazard_ref)
			WT_PAGE_OUT(session, page);
		return (ret == 0 ? WT_RESTART : 0);
	}

	/*
	 * Delete or update a column-store entry.
	 *
	 * Run-length encoded (RLE) column-store changes are hard because each
	 * original on-disk index for an RLE can represent a large number of
	 * records, and we're only changing a single one of those records,
	 * which means working in the WT_INSERT array.  All other column-store
	 * modifications are simple, adding a new WT_UPDATE entry to the page's
	 * modification array.  There are three code paths:
	 *
	 * 1: column-store changes other than RLE column stores: update the
	 * original on-disk page entry by creating a new WT_UPDATE entry and
	 * linking it into the WT_UPDATE array.
	 *
	 * 2: RLE column-store changes of an already changed record: create
	 * a new WT_UPDATE entry, and link it to an existing WT_INSERT entry's
	 * WT_UPDATE list.
	 *
	 * 3: RLE column-store change of not-yet-changed record: create a new
	 * WT_INSERT/WT_UPDATE pair and link it into the WT_INSERT array.
	 */
	switch (page->type) {
	case WT_PAGE_COL_FIX:					/* #1 */
	case WT_PAGE_COL_VAR:
		/* Allocate an update array as necessary. */
		if (session->srch_upd == NULL) {
			WT_ERR(__wt_calloc_def(
			    session, page->entries, &new_upd));
			/*
			 * If there was no update array, the search function
			 * could not have set the WT_UPDATE location.
			 */
			session->srch_upd = &new_upd[session->srch_slot];
		}
		goto simple_update;
	case WT_PAGE_COL_RLE:
		/* Allocate an update array as necessary. */
		if (session->srch_upd != NULL) {		/* #2 */
simple_update:		WT_ERR(__wt_update_alloc(session, value, &upd));

			/* workQ: insert the WT_UPDATE structure. */
			ret = __wt_update_serial(
			    session, page, session->srch_write_gen,
			    &new_upd, session->srch_upd, &upd);
			break;
		}
								/* #3 */
		/* Allocate an insert array as necessary. */
		if (session->srch_ins == NULL) {
			WT_ERR(__wt_calloc_def(
			    session, page->entries, &new_ins));
			/*
			 * If there was no insert array, the search function
			 * could not have set the WT_INSERT location.
			 */
			session->srch_ins = &new_ins[session->srch_slot];
		}

		/* Allocate a WT_INSERT/WT_UPDATE pair. */
		WT_ERR(__col_insert_alloc(session, recno, &ins));
		WT_ERR(__wt_update_alloc(session, value, &upd));
		ins->upd = upd;

		/* workQ: insert the WT_INSERT structure. */
		ret = __wt_insert_serial(
		    session, page, session->srch_write_gen,
		    &new_ins, session->srch_ins, &ins);
		break;
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

	return (0);
}

/*
 * __col_insert_alloc --
 *	Column-store insert: allocate a WT_INSERT structure from the session's
 *	buffer and fill it in.
 */
static int
__col_insert_alloc(WT_SESSION_IMPL *session, uint64_t recno, WT_INSERT **insp)
{
	WT_SESSION_BUFFER *sb;
	WT_INSERT *ins;

	/*
	 * Allocate the WT_INSERT structure and room for the key, then copy
	 * the key into place.
	 */
	WT_RET(__wt_sb_alloc(
	    session, sizeof(WT_INSERT) + sizeof(uint64_t), &ins, &sb));

	ins->sb = sb;
	WT_INSERT_RECNO(ins) = recno;

	*insp = ins;
	return (0);
}

/*
 * __wt_file_wrong_fixed_size --
 *	Print a standard error message on attempts to put the wrong size element
 *	into a fixed-size file.
 */
static int
__col_wrong_fixed_size(
    WT_SESSION_IMPL *session, uint32_t len, uint32_t config_len)
{
	__wt_errx(session, "length of %" PRIu32
	    " does not match fixed-length file configuration of %" PRIu32,
	    len, config_len);
	return (WT_ERROR);
}

/*
 * __col_extend --
 *	Extend a column-store file.
 */
static int
__col_extend(WT_SESSION_IMPL *session, WT_PAGE *page, uint64_t recno)
{
	WT_COL *d;
	WT_COL_REF *t;
	WT_CONFIG_ITEM cval;
	WT_PAGE *new_intl, *new_leaf, *parent;
	uint64_t next;
	uint32_t internal_extend, leaf_extend;
	int ret;

	new_intl = new_leaf = NULL;
	d = NULL;
	t = NULL;
	internal_extend = leaf_extend = 0;
	ret = 0;

	/* Find out by how much we'll extend the leaf key space. */
	WT_RET(__wt_config_getones(session,
	    session->btree->config, "btree_column_leaf_extend", &cval));
	leaf_extend = (uint32_t)cval.val;

	/*
	 * Another thread may have already done the work, or a default extension
	 * may not be sufficient.  Get the starting record for the next page and
	 * make sure we're doing what we need to do.
	 */
	WT_RET(__col_next_recno(session, page, &next));
	if (recno < next)			/* Fits on the current page. */
		return (WT_RESTART);
	if (recno >= next + leaf_extend)	/* Fits in default extension. */
		leaf_extend = (recno - next) + 100;

	/* We always need a new entries array, allocate it. */
	WT_RET(__wt_calloc_def(session, (size_t)leaf_extend, &d));

	/*
	 * Check if the page is a newly created page: all we'll need is a new
	 * entries array.
	 */
	if (page->entries == 0)
		goto done;

	/* We'll need a new leaf page. */
	WT_ERR(__wt_calloc_def(session, 1, &new_leaf));

	/* Check if there's a parent page with room for a new leaf page. */
	parent = page->parent;
	if (!WT_PAGE_IS_ROOT(page) &&
	    parent->u.col_int.ext_entries > parent->entries)
		goto done;

	/* We'll need a new parent page, with its own entries array. */
	WT_ERR(__wt_calloc_def(session, 1, &new_intl));
	WT_RET(__wt_config_getones(session,
	    session->btree->config, "btree_column_internal_extend", &cval));
	internal_extend = (uint32_t)cval.val;
	WT_ERR(__wt_calloc_def(session, (size_t)internal_extend, &t));

done:	return (__wt_col_extend_serial(session, page,
	    &new_intl, &t, internal_extend, &new_leaf, &d, leaf_extend, recno));

err:
	if (new_intl != NULL)
		__wt_free(session, new_intl);
	if (t != NULL)
		__wt_free(session, t);
	if (new_leaf != NULL)
		__wt_free(session, new_leaf);
	if (d != NULL)
		__wt_free(session, d);
	return (ret);
}

/*
 * __wt_col_extend_serial_func --
 *	Server function to extend a column-store page.
 */
int
__wt_col_extend_serial_func(WT_SESSION_IMPL *session)
{
	WT_COL_REF *cref, *t;
	WT_COL *d;
	WT_PAGE *new_leaf, *new_intl, *page, *parent;
	WT_REF *orig_ref;
	uint64_t next, recno;
	uint32_t internal_extend, leaf_extend;
	int ret;

	__wt_col_extend_unpack(session, &page, &new_intl,
	    &t, &internal_extend, &new_leaf, &d, &leaf_extend, &recno);

	ret = 0;

	/*
	 * We don't care about write generations in this code: in the hard cases
	 * we're working in the tree above the page in which we ran out of room,
	 * not the search page, and the search page's write generation doesn't
	 * matter.  In other words, we depend on our review of the situation on
	 * ground.
	 *
	 * This is safe because the reconciliation code can't touch the subtree
	 * we're in: we have a hazard reference on the lowest page, that fixes
	 * the tree into memory.
	 */

	/* We need a new entries array, make sure our caller passed us one. */
	if (d == NULL)
		goto done;

	/*
	 * Check if the current page needs an entries array.
	 *
	 * Setting the page's entries value turns on the change.
	 */
	if (page->u.col_leaf.d == NULL) {
		page->u.col_leaf.d = d;
		__wt_col_extend_d_taken(session);

		WT_MEMORY_FLUSH;
		page->entries = leaf_extend;
		goto done;
	}

	/* We need a new leaf page, make sure our caller passed us one. */
	if (new_leaf == NULL)
		goto done;

	/*
	 * Get the starting record for the next page, but check, another thread
	 * may have already done the work.
	 */
	WT_RET(__col_next_recno(session, page, &next));
	if (next > recno)
		goto done;

	/*
	 * Check if the page's parent has room for a new leaf page.
	 *
	 * Setting the parent page's entries value turns on the change.
	 */
	parent = page->parent;
	if (!WT_PAGE_IS_ROOT(page) &&
	    parent->u.col_int.ext_entries > parent->entries) {
		cref = &parent->u.col_int.t[parent->entries];
		cref->recno = next;
		WT_COL_REF_ADDR(cref) = WT_ADDR_INVALID;
		WT_COL_REF_PAGE(cref) = new_leaf;
		WT_COL_REF_SIZE(cref) = 0;
		WT_COL_REF_STATE(cref) = WT_REF_MEM;
		WT_PAGE_SET_MODIFIED(parent);

		new_leaf->parent = page->parent;
		new_leaf->parent_ref = &cref->ref;
		new_leaf->read_gen = __wt_cache_read_gen(session);
		new_leaf->u.col_leaf.recno = next;
		new_leaf->u.col_leaf.d = d;
		new_leaf->dsk = NULL;
		new_leaf->entries = leaf_extend;
		new_leaf->type = page->type;
		WT_PAGE_SET_MODIFIED(new_leaf);

		WT_MEMORY_FLUSH;
		++parent->entries;

		__wt_col_extend_new_leaf_taken(session);
		__wt_col_extend_d_taken(session);

		goto done;
	}

	/* We need a new internal page, make sure our caller passed us one. */
	if (new_intl == NULL)
		goto done;

	/*
	 * Split by replacing the existing leaf page with an internal page that
	 * references the leaf page (which deepens the tree by a level).  This
	 * is a little like splits in the reconciliation code, but it's all done
	 * while other threads of control are going through the structures.
	 *
	 * Get a reference to the top WT_REF structure, and mark the top-level
	 * page dirty, we're going to have to reconcile it so our newly created
	 * level is merged back in.
	 */
	orig_ref = page->parent_ref;
	WT_PAGE_SET_MODIFIED(page->parent);

	/*
	 * Configure the new internal page.
	 */
	new_intl->parent = page->parent;
	new_intl->parent_ref = page->parent_ref;
	new_intl->read_gen = __wt_cache_read_gen(session);
	new_intl->u.col_int.recno = page->u.col_leaf.recno;
	new_intl->u.col_int.ext_entries = internal_extend;
	new_intl->u.col_int.t = t;
	new_intl->dsk = NULL;
	new_intl->entries = 2;
	new_intl->type = WT_PAGE_COL_INT;

	/*
	 * If the new internal page isn't the root page, then we should merge
	 * it into its parent, we don't want the tree to deepen permanently.
	 */
	if (!WT_PAGE_IS_ROOT(page))
		F_SET(new_intl, WT_PAGE_MERGE);
	WT_PAGE_SET_MODIFIED(new_intl);

	/* Slot 0 of the new internal page references the original leaf page. */
	cref = &new_intl->u.col_int.t[0];
	cref->recno = page->u.col_leaf.recno;
	cref->ref = *page->parent_ref;

	/* Re-point the original page. */
	page->parent = new_intl;
	page->parent_ref = &new_intl->u.col_int.t[0].ref;

	/* Slot 1 of the new internal page references the new leaf page. */
	cref = &new_intl->u.col_int.t[1];
	cref->recno = next;
	WT_COL_REF_ADDR(cref) = WT_ADDR_INVALID;
	WT_COL_REF_PAGE(cref) = new_leaf;
	WT_COL_REF_SIZE(cref) = 0;
	WT_COL_REF_STATE(cref) = WT_REF_MEM;

	/* Configure the new leaf page. */
	new_leaf->parent = new_intl;
	new_leaf->parent_ref = &new_intl->u.col_int.t[1].ref;
	new_leaf->read_gen = __wt_cache_read_gen(session);
	new_leaf->u.col_leaf.recno = next;
	new_leaf->u.col_leaf.d = d;
	new_leaf->dsk = NULL;
	new_leaf->entries = leaf_extend;
	new_leaf->type = page->type;
	WT_PAGE_SET_MODIFIED(new_leaf);

	__wt_col_extend_new_intl_taken(session);
	__wt_col_extend_t_taken(session);
	__wt_col_extend_new_leaf_taken(session);
	__wt_col_extend_d_taken(session);

	/*
	 * Make the switch: set the addr/size pair then update the pointer (we
	 * are not changing the state, nor are we changing the record number).
	 * This is safe as we're updating one set of structures for another set
	 * of structures which reference identical information.  Eviction can't
	 * get in here because we hold a hazard reference on the original page.
	 * Setting the original page parent's in-memory pointer to reference our
	 * new internal page turns on the change.
	 */
	orig_ref->addr = WT_ADDR_INVALID;
	orig_ref->size = 0;
	WT_MEMORY_FLUSH;
	orig_ref->page = new_intl;
	WT_MEMORY_FLUSH;

done:	__wt_session_serialize_wrapup(session, page, ret);
	return (ret);
}

/*
 * __col_next_recno --
 *	Return the recno of the next page following the argument page.
 */
static int
__col_next_recno(WT_SESSION_IMPL *session, WT_PAGE *page, uint64_t *recnop)
{
	WT_COL *cip;
	uint64_t recno;
	uint32_t i;
	void *cipdata;

	recno = page->u.col_leaf.recno;

	switch (page->type) {
	case WT_PAGE_COL_FIX:
	case WT_PAGE_COL_VAR:
		recno += page->entries;
		break;
	case WT_PAGE_COL_RLE:
		WT_COL_FOREACH(page, cip, i) {
			cipdata = WT_COL_PTR(page, cip);
			recno +=
			    cipdata == NULL ? 1 : WT_RLE_REPEAT_COUNT(cipdata);
		}
		++recno;
		break;
	WT_ILLEGAL_FORMAT(session);
	}

	*recnop = recno;
	return (0);
}
