/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wt_internal.h"

static int __col_extend(WT_SESSION_IMPL *, WT_PAGE *, uint64_t);
static int __col_insert_alloc(
		WT_SESSION_IMPL *, uint64_t, u_int, WT_INSERT **, uint32_t *);
static int __col_next_recno(WT_SESSION_IMPL *, WT_PAGE *, uint64_t *);

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
	uint64_t recno;
	uint32_t ins_size, new_inslist_size, new_inshead_size, upd_size;
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
	if (!cbt->match) {
		/*
		 * We may have, and need to hold, a hazard reference on a page,
		 * but we're possibly doing some page shuffling of the root,
		 * which means the standard test to determine whether we should
		 * release a hazard reference on the page isn't right.  Check
		 * now, before we do the page shuffling.
		 */
		hazard_ref = page == session->btree->root_page.page ? 0 : 1;
		ret = __col_extend(session, page, recno);
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
				    WT_SIZEOF32(WT_INSERT_HEAD *);
				WT_ERR(__wt_calloc_def(
				    session, 1, &new_inslist));
				inshead = &new_inslist[0];
				break;
			case WT_PAGE_COL_VAR:
				new_inslist_size = page->entries *
				    WT_SIZEOF32(WT_INSERT_HEAD *);
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
			new_inshead_size = WT_SIZEOF32(WT_INSERT_HEAD);
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
    uint64_t recno, u_int skipdepth, WT_INSERT **insp, uint32_t *ins_sizep)
{
	WT_SESSION_BUFFER *sb;
	WT_INSERT *ins;
	uint32_t ins_size;

	/*
	 * Allocate the WT_INSERT structure and skiplist pointers, then copy
	 * the record number into place.
	 */
	ins_size = WT_SIZEOF32(WT_INSERT) +
	    skipdepth * WT_SIZEOF32(WT_INSERT *);
	WT_RET(__wt_sb_alloc(session, ins_size, &ins, &sb));

	ins->sb = sb;
	WT_INSERT_RECNO(ins) = recno;

	*insp = ins;
	*ins_sizep = ins_size;
	return (0);
}

/*
 * __col_extend --
 *	Extend a column-store file.
 */
static int
__col_extend(WT_SESSION_IMPL *session, WT_PAGE *page, uint64_t recno)
{
	WT_BTREE *btree;
	WT_COL *d;
	WT_COL_REF *t;
	WT_CONFIG_ITEM cval;
	WT_PAGE *new_intl, *new_leaf, *parent;
	uint64_t next;
	uint32_t entries_size, new_intl_size, new_leaf_size, t_size;
	uint32_t internal_extend, leaf_extend;
	uint8_t *bitf;
	int ret;
	void *entries;

	btree = session->btree;
	d = NULL;
	t = NULL;
	new_intl = new_leaf = NULL;
	entries_size = new_intl_size = new_leaf_size = t_size = 0;
	internal_extend = leaf_extend = 0;
	bitf = NULL;
	ret = 0;

	/*
	 * Another thread may have already done the work, or a default extension
	 * may not be sufficient.  Get the starting record for the next page and
	 * make sure we're doing what we need to do.
	 */
	WT_RET(__col_next_recno(session, page, &next));
	if (recno < next)			/* Fits on the current page. */
		return (WT_RESTART);

	/*
	 * Figure out how much we'll extend the leaf key space.
	 *
	 * If it's a fixed-length store, we can't allocate more than maximum
	 * leaf page size number of bits, because we can't ever split those
	 * pages.
	 *
	 * If it's a variable-length store, we can split those pages so we
	 * can allocate whatever we need.
	 *
	 * XXX
	 * If the application is extending the file by more than will reasonably
	 * fit on a page, insert an RLE record that gets us all the way to the
	 * insert record.
	 *
	 * We always need a new bitfield or entries array, allocate them.
	 */
	switch (page->type) {
	case WT_PAGE_COL_FIX:
		leaf_extend = WT_FIX_NRECS(btree);

		WT_RET(__wt_calloc_def(session, (size_t)leaf_extend, &bitf));
		entries = bitf;
		entries_size = leaf_extend;
		break;
	case WT_PAGE_COL_VAR:
		WT_RET(__wt_config_getones(session,
		    session->btree->config, "column_leaf_extend", &cval));
		leaf_extend = (uint32_t)cval.val;
		if (recno >= next + leaf_extend)
			leaf_extend = (uint32_t)(recno - next) + 100;

		WT_RET(__wt_calloc_def(session, (size_t)leaf_extend, &d));
		entries = d;
		entries_size = leaf_extend * WT_SIZEOF32(WT_COL);
		break;
	}

	/*
	 * Check if the page is a newly created page: all we'll need is a new
	 * entries array.
	 */
	if (page->entries == 0)
		goto done;

	/* We'll need a new leaf page. */
	WT_ERR(__wt_calloc_def(session, 1, &new_leaf));
	new_leaf_size = sizeof(WT_PAGE);

	/* Check if there's a parent page with room for a new leaf page. */
	parent = page->parent;
	if (!WT_PAGE_IS_ROOT(page) &&
	    parent->u.col_int.ext_entries > parent->entries)
		goto done;

	/* We'll need a new parent page, with its own entries array. */
	WT_ERR(__wt_calloc_def(session, 1, &new_intl));
	new_intl_size = sizeof(WT_PAGE);
	WT_RET(__wt_config_getones(session,
	    session->btree->config, "column_internal_extend", &cval));
	internal_extend = (uint32_t)cval.val;
	WT_ERR(__wt_calloc_def(session, (size_t)internal_extend, &t));
	t_size = internal_extend * WT_SIZEOF32(WT_COL_REF);

done:	return (__wt_col_extend_serial(session, page, &new_intl, new_intl_size,
	    &t, t_size, internal_extend, &new_leaf, new_leaf_size, &entries,
	    entries_size, leaf_extend, recno));

err:
	if (d != NULL)
		__wt_free(session, d);
	if (t != NULL)
		__wt_free(session, t);
	if (new_intl != NULL)
		__wt_free(session, new_intl);
	if (new_leaf != NULL)
		__wt_free(session, new_leaf);
	if (bitf != NULL)
		__wt_free(session, bitf);
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
	WT_PAGE *new_leaf, *new_intl, *page, *parent;
	WT_REF *orig_ref;
	uint64_t next, recno;
	uint32_t internal_extend, leaf_extend;
	int ret;
	void *entries;

	__wt_col_extend_unpack(session, &page, &new_intl,
	    &t, &internal_extend, &new_leaf, &entries, &leaf_extend, &recno);

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
	 *
	 * We need a new entries array or bitfield, make sure our caller passed
	 * us one.
	 */
	if (entries == NULL)
		goto done;

	/*
	 * Check if the current page needs an entries array.
	 *
	 * Setting the page's entries value turns on the change.
	 */
	switch (page->type) {
	case WT_PAGE_COL_FIX:
		if (page->u.col_leaf.bitf == NULL) {
			page->u.col_leaf.bitf = entries;
			goto entries;
		}
		break;
	case WT_PAGE_COL_VAR:
		if (page->u.col_leaf.d == NULL) {
			page->u.col_leaf.d = entries;

entries:		__wt_col_extend_entries_taken(session, page);
			WT_MEMORY_FLUSH;
			page->entries = leaf_extend;
			goto done;
		}
		break;
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
		new_leaf->u.col_leaf.d = entries;
		new_leaf->u.col_leaf.bitf = entries;
		new_leaf->dsk = NULL;
		new_leaf->entries = leaf_extend;
		new_leaf->type = page->type;
		WT_PAGE_SET_MODIFIED(new_leaf);
		__wt_cache_page_workq(session);

		WT_MEMORY_FLUSH;
		++parent->entries;

		__wt_col_extend_new_leaf_taken(session, new_leaf);
		__wt_col_extend_entries_taken(session, new_leaf);

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
	if (!WT_PAGE_IS_ROOT(page))
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
	WT_PAGE_SET_MODIFIED(new_intl);
	__wt_cache_page_workq(session);

	/*
	 * If the new internal page isn't the root page, then we should merge
	 * it into its parent, we don't want the tree to deepen permanently.
	 */
	if (!WT_PAGE_IS_ROOT(page))
		F_SET(new_intl, WT_PAGE_MERGE);

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
	new_leaf->u.col_leaf.d = entries;
	new_leaf->u.col_leaf.bitf = entries;
	new_leaf->dsk = NULL;
	new_leaf->entries = leaf_extend;
	new_leaf->type = page->type;
	WT_PAGE_SET_MODIFIED(new_leaf);
	__wt_cache_page_workq(session);

	__wt_col_extend_new_intl_taken(session, new_intl);
	__wt_col_extend_t_taken(session, new_intl);
	__wt_col_extend_new_leaf_taken(session, new_leaf);
	__wt_col_extend_entries_taken(session, new_leaf);

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
	WT_CELL *cell;
	WT_CELL_UNPACK *unpack, _unpack;
	WT_COL *cip;
	uint32_t i;
	uint64_t recno;

	recno = page->u.col_leaf.recno;
	unpack = &_unpack;

	switch (page->type) {
	case WT_PAGE_COL_FIX:
		recno += page->entries;
		break;
	case WT_PAGE_COL_VAR:
		WT_COL_FOREACH(page, cip, i)
			if ((cell = WT_COL_PTR(page, cip)) == NULL)
				++recno;
			else {
				__wt_cell_unpack(cell, unpack);
				recno += unpack->rle;
			}
		break;
	WT_ILLEGAL_FORMAT(session);
	}

	*recnop = recno;
	return (0);
}
