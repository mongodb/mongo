/*-
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

static int  __hazard_exclusive(WT_SESSION_IMPL *, WT_REF *, int);
static void __rec_discard_tree(WT_SESSION_IMPL *, WT_PAGE *, int);
static void __rec_excl_clear(WT_SESSION_IMPL *);
static void __rec_page_clean_update(WT_SESSION_IMPL *, WT_REF *);
static int  __rec_page_dirty_update(WT_SESSION_IMPL *, WT_REF *, WT_PAGE *);
static int  __rec_review(
    WT_SESSION_IMPL *, WT_REF *, WT_PAGE *, int, int, int *);
static void __rec_root_update(WT_SESSION_IMPL *);

/*
 * __wt_rec_evict --
 *	Reconciliation plus eviction.
 */
int
__wt_rec_evict(WT_SESSION_IMPL *session, WT_PAGE **pagep, int exclusive)
{
	WT_DECL_RET;
	WT_PAGE *page;
	WT_PAGE_MODIFY *mod;
	WT_REF *parent_ref;
	int istree;

	page = *pagep;
	istree = 0;

	WT_VERBOSE_RET(session, evict,
	    "page %p (%s)", page, __wt_page_type_string(page->type));

	/*
	 * Get exclusive access to the page and review the page and its subtree
	 * for conditions that would block our eviction of the page.  If the
	 * check fails (for example, we find a child page that can't be merged),
	 * we're done.  We have to make this check for clean pages, too: while
	 * unlikely eviction would choose an internal page with children, it's
	 * not disallowed anywhere.
	 *
	 * Note that parent_ref may be NULL in some cases (e.g., for root pages
	 * or during salvage).  That's OK if exclusive is set: we won't check
	 * hazard pointers in that case.
	 */
	parent_ref = __wt_page_ref(session, page);
	WT_ERR(__rec_review(session, parent_ref, page, exclusive, 1, &istree));

	/*
	 * Update the page's modification reference, reconciliation might have
	 * changed it.
	 */
	mod = page->modify;

	/* Count evictions of internal pages during normal operation. */
	if (!exclusive &&
	    (page->type == WT_PAGE_COL_INT || page->type == WT_PAGE_ROW_INT)) {
		WT_STAT_FAST_CONN_INCR(session, cache_eviction_internal);
		WT_STAT_FAST_DATA_INCR(session, cache_eviction_internal);
	}

	/*
	 * Update the parent and discard the page.
	 */
	if (mod == NULL || !F_ISSET(mod, WT_PM_REC_MASK)) {
		WT_ASSERT(session,
		    exclusive || parent_ref->state == WT_REF_LOCKED);

		if (WT_PAGE_IS_ROOT(page))
			__rec_root_update(session);
		else
			__rec_page_clean_update(session, parent_ref);

		WT_STAT_FAST_CONN_INCR(session, cache_eviction_clean);
		WT_STAT_FAST_DATA_INCR(session, cache_eviction_clean);
	} else {
		if (WT_PAGE_IS_ROOT(page))
			__rec_root_update(session);
		else
			WT_ERR(__rec_page_dirty_update(
			    session, parent_ref, page));

		WT_STAT_FAST_CONN_INCR(session, cache_eviction_dirty);
		WT_STAT_FAST_DATA_INCR(session, cache_eviction_dirty);
	}

	/* Discard the page or tree rooted in this page. */
	if (istree)
		__rec_discard_tree(session, page, exclusive);
	else
		__wt_page_out(session, pagep);

	if (0) {
err:		/*
		 * If unable to evict this page, release exclusive reference(s)
		 * we've acquired.
		 */
		__rec_excl_clear(session);

		WT_STAT_FAST_CONN_INCR(session, cache_eviction_fail);
		WT_STAT_FAST_DATA_INCR(session, cache_eviction_fail);
	}
	session->excl_next = 0;

	return (ret);
}

/*
 * __rec_root_update --
 *	Update a root page's reference on eviction (clean or dirty).
 */
static void
__rec_root_update(WT_SESSION_IMPL *session)
{
	S2BT(session)->root_page = NULL;
}

/*
 * __rec_page_clean_update --
 *	Update a clean page's reference on eviction.
 */
static void
__rec_page_clean_update(WT_SESSION_IMPL *session, WT_REF *parent_ref)
{
	/*
	 * Update the WT_REF structure in the parent.  If the page has an
	 * address, it's a disk page; if it has no address, it must be a
	 * deleted page that was re-instantiated (for example, by searching)
	 * and never written.
	 */
	parent_ref->page = NULL;
	WT_PUBLISH(parent_ref->state,
	    parent_ref->addr == NULL ? WT_REF_DELETED : WT_REF_DISK);

	WT_UNUSED(session);
}

/*
 * __rec_split_list_alloc --
 *	Allocate room for a new WT_REF array as necessary.
 */
static int
__rec_split_list_alloc(
    WT_SESSION_IMPL *session, WT_PAGE_MODIFY *mod, uint32_t *ip)
{
	size_t bytes_allocated;
	uint32_t i;

	for (i = 0; i < mod->splits_entries; ++i)
		if (mod->splits[i].refs == NULL)
			break;
	if (i == mod->splits_entries) {
		/*
		 * Calculate the bytes-allocated explicitly, this information
		 * lives in the page-modify structure, and it's worth keeping
		 * that as small as possible.
		 */
		bytes_allocated = mod->splits_entries * sizeof(mod->splits[0]);
		WT_RET(__wt_realloc(session, &bytes_allocated,
		    (i + 5) * sizeof(mod->splits[0]), &mod->splits));
		mod->splits_entries = i + 5;
	}
	*ip = i;
	return (0);
}

/*
 * __rec_split_copy_addr --
 *	Copy an address into allocated memory.
 */
static int
__rec_split_copy_addr(
    WT_SESSION_IMPL *session, WT_PAGE *page, WT_ADDR *addr, void *addrp)
{
	WT_ADDR *alloc_addr;
	WT_CELL_UNPACK unpack;
	WT_DECL_RET;

	/*
	 * If there's no address set, this page has never been written, there's
	 * nothing to copy.
	 */
	if (addr == NULL) {
		*(void **)addrp = NULL;
		return (0);
	}

	/*
	 * If the address has been instantiated, everything we need is there,
	 * copy it.  Otherwise, we have to get the address from the on-page
	 * cell.
	 */
	WT_RET(__wt_calloc_def(session, 1, &alloc_addr));
	if (__wt_off_page(page, addr)) {
		WT_ERR(__wt_strndup(session, addr->addr,
		    alloc_addr->size = addr->size, &alloc_addr->addr));
		alloc_addr->type = addr->type;
	} else {
		__wt_cell_unpack((WT_CELL *)addr, &unpack);
		WT_ERR(__wt_strndup(
		    session, unpack.data, unpack.size, &alloc_addr->addr));
		alloc_addr->size = (uint8_t)unpack.size;
		alloc_addr->type =
		    unpack.raw == WT_CELL_ADDR_INT ? WT_ADDR_INT : WT_ADDR_LEAF;
	}

	*(void **)addrp = alloc_addr;
	return (0);

err:	__wt_free(session, alloc_addr);
	return (ret);
}

/*
 * __rec_split_deepen --
 *	Split an internal page in-memory, deepening the tree.
 */
static int
__rec_split_deepen(WT_SESSION_IMPL *session, WT_PAGE *page)
{
	WT_BTREE *btree;
	WT_DECL_RET;
	WT_PAGE *child;
	WT_PAGE_INDEX *alloc_index, *pindex;
	WT_REF **alloc, *alloc_ref, *parent_ref, **refp, *ref;
	size_t incr, parent_incr, size;
	uint32_t chunk, entries, i, j, remain, slots;
	void *p;

	btree = S2BT(session);
	alloc_index = NULL;
	alloc_ref = NULL;

	pindex = page->pg_intl_index;
	entries = (uint32_t)btree->split_deepen;

	WT_STAT_FAST_CONN_INCR(session, cache_eviction_deepen);
	WT_VERBOSE_ERR(session, split,
	    "%p: %" PRIu32 " elements, splitting into %" PRIu32 " children",
	    page, pindex->entries, entries);

	/*
	 * If the workload is prepending/appending to the tree, we could deepen
	 * without bound.  Don't let that happen, keep the first/last pages of
	 * the tree at their current level.
	 *
	 * XXX
	 * To improve this, we could track which pages were last merged into
	 * this page by eviction, and leave those pages alone, to prevent any
	 * sustained insert into the tree from deepening a single location.
	 */
#undef	SPLIT_CORRECT_1
#define	SPLIT_CORRECT_1	1		/* First page correction */
#undef	SPLIT_CORRECT_2
#define	SPLIT_CORRECT_2	2		/* First/last page correction */

	/* Allocate a new parent WT_PAGE_INDEX. */
	WT_ERR(__wt_calloc(session, 1,
	    sizeof(WT_PAGE_INDEX) +
	    (entries + SPLIT_CORRECT_2) * sizeof(WT_REF *), &alloc_index));
	alloc_index->index = (WT_REF **)(alloc_index + 1);
	alloc_index->entries = entries + SPLIT_CORRECT_2;

	/* Allocate a new parent WT_REF array.  */
	WT_ERR(__wt_calloc_def(session, entries, &alloc_ref));

	/*
	 * Initialize the first/last slots of the WT_PAGE_INDEX to point to the
	 * first/last pages we're keeping around, and the rest of the slots to
	 * reference the new WT_REF array.
	 */
	alloc_index->index[0] = pindex->index[0];
	alloc_index->index[alloc_index->entries - 1] =
	    pindex->index[pindex->entries - 1];
	for (alloc = alloc_index->index + SPLIT_CORRECT_1,
	    parent_ref = alloc_ref,
	    i = 0; i < entries; ++alloc, ++parent_ref, ++i)
		(*alloc) = parent_ref;

	/* Allocate new child pages, and insert into the WT_REF array. */
	chunk = (pindex->entries - SPLIT_CORRECT_2) / entries;
	remain = (pindex->entries - SPLIT_CORRECT_2) - chunk * (entries - 1);
	parent_incr = 0;
	for (refp = pindex->index + SPLIT_CORRECT_1,
	    parent_ref = alloc_ref, i = 0; i < entries; ++parent_ref, ++i) {
		slots = i == entries - 1 ? remain : chunk;
		WT_ERR(__wt_page_alloc(session, page->type, 0, slots, &child));

		/* Initialize the parent page's child reference. */
		parent_ref->page = child;
		parent_ref->addr = NULL;
		if (page->type == WT_PAGE_ROW_INT) {
			__wt_ref_key(page, *refp, &p, &size);
			WT_ERR(__wt_row_ikey(
			    session, 0, p, size, &parent_ref->key.ikey));
			parent_incr += sizeof(WT_IKEY) + size;
		} else
			parent_ref->key.recno = (*refp)->key.recno;
		parent_ref->txnid = 0;			/* XXXKEITH 0? */
		parent_ref->state = WT_REF_MEM;

		/* Initialize the page, mark it dirty. */
		if (page->type == WT_PAGE_COL_INT)
			child->pg_intl_recno = (*refp)->key.recno;
		child->parent = page;
		child->ref_hint = i;
		child->type = page->type;
		WT_ERR(__wt_page_modify_init(session, child));
		__wt_page_only_modify_set(session, child);

		/*
		 * The child page references the same page as the parent.  Copy
		 * the parent's key/address pair, they may reference block image
		 * information.  (The key may or may not reference block image
		 * information, but even if the key is instantiated, the parent
		 * needs some of its keys, it uses them to find the new child
		 * pages, we'd have to copy in some cases.   The parent doesn't
		 * need any previous address image information, we could steal
		 * them if they're instantiated, as long as we do it in an order
		 * that won't confuse other threads of control in the page.  For
		 * now, I'm just copying everything.)
		 */
		for (ref = child->pg_intl_orig_index, incr = 0,
		    j = 0; j < slots; ++refp, ++ref, ++j) {
			ref->page = (*refp)->page;
			WT_ERR(__rec_split_copy_addr(
			    session, page, (*refp)->addr, &ref->addr));
			if (page->type == WT_PAGE_ROW_INT) {
				__wt_ref_key(page, *refp, &p, &size);
				WT_ERR(__wt_row_ikey(
				    session, 0, p, size, &ref->key.ikey));
				incr += sizeof(WT_IKEY) + size;
			} else
				ref->key.recno = (*refp)->key.recno;
			ref->txnid = (*refp)->txnid;
			ref->state = (*refp)->state;
		}
		if (incr != 0)
			__wt_cache_page_inmem_incr(session, child, incr);
		WT_ASSERT(session, ref - child->pg_intl_orig_index == slots);
	}
	if (parent_incr != 0)
		__wt_cache_page_inmem_incr(session, page, parent_incr);
	WT_ASSERT(session, parent_ref - alloc_ref == entries);
	WT_ASSERT(session,
	    refp - pindex->index == pindex->entries - SPLIT_CORRECT_1);

	/* Add the WT_REF array into the page's list. */
	WT_ERR(__rec_split_list_alloc(session, page->modify, &i));
	page->modify->splits[i].refs = alloc_ref;
	page->modify->splits[i].entries = entries;
	alloc_ref = NULL;

	/*
	 * We can't discard the previous page index, there may be threads using
	 * it.  Add it to the session's discard list, to be freed once we know
	 * no threads can still be using it.
	 */
	WT_ERR(__wt_session_fotxn_add(session, page->pg_intl_index));

	/*
	 * Update the page's index; this is the change which splits the page,
	 * making the split visible to threads descending the tree.
	 *
	 * Threads reading child pages will become confused after this update,
	 * they will no longer be able to find their associated WT_REF, the
	 * parent page no longer references them.  When it happens, the child
	 * will wait for its parent reference to be updated, so once we've
	 * updated the parent, walk the children and fix them up.
	 */
	WT_PUBLISH(page->pg_intl_index, alloc_index);
	alloc_index = NULL;

	/*
	 * Fix up the children; this is the change that makes the split visible
	 * to threads already in the tree.
	 */
	pindex = page->pg_intl_index;
	for (refp = pindex->index + SPLIT_CORRECT_1,
	    i = pindex->entries - SPLIT_CORRECT_1; i > 0; ++refp, --i) {
		parent_ref = *refp;
		if (parent_ref->state != WT_REF_MEM)
			continue;
		child = parent_ref->page;
		if (child->type != WT_PAGE_ROW_INT &&
		    child->type != WT_PAGE_COL_INT)
			continue;
		WT_ASSERT(session, child->parent == page);
		WT_INTL_FOREACH_BEGIN(child, ref) {
			if (ref->state == WT_REF_MEM) {
				WT_ASSERT(session, ref->page->parent == page);
				ref->page->parent = child;
				ref->page->ref_hint = 0;
			}
		} WT_INTL_FOREACH_END;
	}
	WT_FULL_BARRIER();

	if (0) {
err:		__wt_free(session, alloc_index);
		__wt_free_ref_array(session, page, alloc_ref, entries);
		__wt_free(session, alloc_ref);
	}
	return (ret);
}

/*
 * __wt_multi_inmem_build --
 *	Instantiate a page in a multi-block set, when an update couldn't be
 * written.
 */
static int
__wt_multi_inmem_build(
    WT_SESSION_IMPL *session, WT_PAGE *page, WT_REF *ref, WT_MULTI *multi)
{
	WT_CURSOR_BTREE cbt;
	WT_ITEM key;
	WT_PAGE *new;
	WT_UPDATE *upd, **updp;
	WT_UPD_SKIPPED *skip;
	uint64_t recno;
	uint32_t i;

	WT_CLEAR(key);

	/*
	 * When a page is evicted, we can find unresolved updates, which cannot
	 * be written.  We simply fail those evictions in most cases, but one
	 * case we must handle is when forcibly evicting a page grown too-large
	 * because the application inserted lots of new records.  In that case,
	 * the page is expected to split into many on-disk chunks we write, plus
	 * some on-disk chunks we don't write.  This code deals with the latter:
	 * any chunk we didn't write is re-created as a page, and then we apply
	 * the unresolved updates to that page.
	 *
	 * Create an in-memory version of the page, and link it to its parent.
	 */
	WT_RET(__wt_page_inmem(session,
	    NULL, NULL, multi->skip_dsk, WT_PAGE_DISK_ALLOC, &ref->page));
	multi->skip_dsk = NULL;
	new = ref->page;

	/* Re-create each modification we couldn't write. */
	for (i = 0, skip = multi->skip; i < multi->skip_entries; ++i, ++skip) {
		/*
		 * XXXKEITH:
		 * Remove the list of WT_UPDATEs from the row-leaf WT_UPDATE
		 * array or arbitrary WT_INSERT list, and move it to a
		 * different page/list.   This is a problem: on failure,
		 * discarding the created page will free it, and that's wrong.
		 * This problem needs to be revisited once we decide this whole
		 * approach is a viable one.
		 */
		if (skip->is_insert)
			updp = &((WT_INSERT *)skip->head)->upd;
		else
			updp = &page->pg_row_upd[
			    WT_ROW_SLOT(page, skip->head)];
		upd = *updp;
		*updp = NULL;

		switch (page->type) {
		case WT_PAGE_COL_FIX:
		case WT_PAGE_COL_VAR:
			/* Build a key. */
			recno = WT_INSERT_RECNO(skip->head);

			/* Search the page. */
			WT_RET(__wt_col_search(session, recno, new, &cbt));

			/* Apply the modification. */
			WT_RET(__wt_col_modify(session, &cbt, recno,
			    NULL, upd, WT_UPDATE_DELETED_ISSET(upd)));
			break;
		case WT_PAGE_ROW_LEAF:
			/* Build a key. */
			if (skip->is_insert) {
				key.data = WT_INSERT_KEY(skip->head);
				key.size = WT_INSERT_KEY_SIZE(skip->head);
			} else
				WT_RET(__wt_row_leaf_key(session,
				    page, skip->head, &key, 0));

			/* Search the page. */
			WT_RET(__wt_row_search(session, &key, new, &cbt));

			/* Apply the modification. */
			WT_RET(__wt_row_modify(session, &cbt, &key,
			    NULL, upd, WT_UPDATE_DELETED_ISSET(upd)));
			break;
		WT_ILLEGAL_VALUE(session);
		}

	}
	__wt_free(session, multi->skip);

	WT_LINK_PAGE(page->parent, ref, new);

	return (0);
}

/*
 * __wt_multi_to_ref --
 *	Move a multi-block list into an array of WT_REF structures.
 */
int
__wt_multi_to_ref(WT_SESSION_IMPL *session,
    WT_PAGE *page, WT_MULTI *multi, WT_REF *refarg, uint32_t entries)
{
	WT_ADDR *addr;
	WT_DECL_RET;
	WT_REF *ref;
	uint32_t i;

	addr = NULL;
	for (ref = refarg, i = 0; i < entries; ++multi, ++ref, ++i) {
		if (multi->skip == NULL) {
			WT_ERR(__wt_calloc_def(session, 1, &addr));
			ref->addr = addr;
			addr->size = multi->addr.size;
			addr->type = multi->addr.type;
			WT_ERR(__wt_strndup(session,
			    multi->addr.addr, addr->size = multi->addr.size,
			    &addr->addr));
		} else
			WT_ERR(
			    __wt_multi_inmem_build(session, page, ref, multi));

		switch (page->type) {
		case WT_PAGE_ROW_INT:
		case WT_PAGE_ROW_LEAF:
			WT_ERR(__wt_strndup(session,
			    multi->key.ikey,
			    multi->key.ikey->size + sizeof(WT_IKEY),
			    &ref->key.ikey));
			break;
		default:
			ref->key.recno = multi->key.recno;
			break;
		}

		ref->txnid = 0;
		ref->state = ref->page == NULL ? WT_REF_DISK : WT_REF_MEM;
	}
	return (0);

err:	__wt_free_ref_array(session, page, refarg, entries);
	return (ret);
}

/*
 * __rec_split_evict --
 *	Resolve a page split, inserting new information into the parent.
 */
static int
__rec_split_evict(WT_SESSION_IMPL *session, WT_REF *parent_ref, WT_PAGE *page)
{
	WT_BTREE *btree;
	WT_CELL *cell;
	WT_CELL_UNPACK kpack;
	WT_DECL_RET;
	WT_IKEY *ikey;
	WT_PAGE *parent;
	WT_PAGE_INDEX *alloc_index, *pindex;
	WT_PAGE_MODIFY *mod, *parent_mod;
	WT_REF *alloc_ref, **refp, *split;
	uint64_t bytes;
	uint32_t i, j, parent_entries, result_entries, split_entries;
	int locked;

	btree = S2BT(session);
	alloc_index = NULL;
	alloc_ref = NULL;
	locked = 0;

	mod = page->modify;
	parent = page->parent;
	parent_mod = parent->modify;

	/* If the parent page hasn't yet been modified, now is the time. */
	WT_RET(__wt_page_modify_init(session, parent));
	__wt_page_only_modify_set(session, parent);

	/*
	 * Allocate an array of WT_REF structures, and move the page's multiple
	 * block reconciliation information into it.
	 */
	WT_RET(__wt_calloc_def(session, mod->multi_entries, &alloc_ref));
	WT_ERR(__wt_multi_to_ref(
	    session, page, mod->multi, alloc_ref, mod->multi_entries));

	/*
	 * Get a page-level lock on the parent to single-thread splits into the
	 * page.  It's OK to queue up multiple splits as the child pages split,
	 * but the actual split into the parent has to be serialized.  We do
	 * memory allocation inside of the lock, but I don't see a reason to
	 * tighten this down yet, we're only blocking other leaf pages trying
	 * to split into this parent, they can wait their turn.
	 */
	WT_PAGE_LOCK(session, parent);
	locked = 1;

	/*
	 * Append the underlying split page's WT_REF array into the parent
	 * page's list.
	 */
	WT_ERR(__rec_split_list_alloc(session, parent_mod, &i));
	parent_mod->splits[i].refs = alloc_ref;
	alloc_ref = NULL;
	parent_mod->splits[i].entries = mod->multi_entries;

	/* Allocate a new WT_REF index array and initialize it. */
	pindex = parent->pg_intl_index;
	parent_entries = pindex->entries;
	split = parent_mod->splits[i].refs;
	split_entries = parent_mod->splits[i].entries;
	result_entries = (parent_entries - 1) + split_entries;
	WT_ERR(__wt_calloc(session, 1, sizeof(WT_PAGE_INDEX) +
	    result_entries * sizeof(WT_REF *), &alloc_index));
	alloc_index->index = (WT_REF **)(alloc_index + 1);
	alloc_index->entries = result_entries;
	refp = alloc_index->index;
	for (i = 0; i < parent_entries; ++i)
		if ((*refp = pindex->index[i]) == parent_ref)
			for (j = 0; j < split_entries; ++j)
				*refp++ = &split[j];
		else
			refp++;

	/*
	 * We can't discard the previous page index, there may be threads using
	 * it.  Add it to the session's discard list, to be freed once we know
	 * no threads can still be using it.
	 */
	WT_ERR(__wt_session_fotxn_add(session, parent->pg_intl_index));

	/* Update the parent page's footprint. */
	__wt_cache_page_inmem_incr(session, parent, mod->multi_size);

	/*
	 * Update the parent page's index: this is the update that splits the
	 * parent page, making the split visible to other threads.
	 */
	WT_PUBLISH(parent->pg_intl_index, alloc_index);
	alloc_index = NULL;

	/*
	 * The key for the split WT_REF may be an onpage overflow key, and we're
	 * about to lose track of it.  Add it to the tracking list so it will be
	 * discarded the next time this page is reconciled.
	 */
	switch (parent->type) {
	case WT_PAGE_ROW_INT:
	case WT_PAGE_ROW_LEAF:
		ikey = __wt_ref_key_instantiated(parent_ref);
		if (ikey != NULL && ikey->cell_offset != 0) {
			cell = WT_PAGE_REF_OFFSET(parent, ikey->cell_offset);
			__wt_cell_unpack(cell, &kpack);
			if (kpack.ovfl)
				WT_ERR(__wt_ovfl_onpage_add(
				    session, parent, kpack.data, kpack.size));
		}
		break;
	}

	/*
	 * Reset the page's original WT_REF field to split, releasing any
	 * blocked threads.
	 */
	WT_PUBLISH(parent_ref->state, WT_REF_SPLIT);

	WT_STAT_FAST_CONN_INCR(session, cache_eviction_split);
	WT_VERBOSE_ERR(session, split,
	    "%p: %s merged into %p %" PRIu32 " -> %" PRIu32
	    " (%" PRIu32 ")",
	    page, __wt_page_type_string(page->type), parent, parent_entries,
	    result_entries, result_entries - parent_entries);

	/*
	 * We're already holding the parent page locked, see if the parent needs
	 * to split, deepening the tree.
	 *
	 * Page splits trickle up the tree, that is, as leaf pages grow large
	 * enough, they'll split into their parent, as that parent grows large
	 * enough, it will split into its parent and so on.  If the page split
	 * reaches the parent, then the tree will permanently deepen as some
	 * number of root pages are written.  However, that only helps if the
	 * tree is closed and re-opened from a disk image: to work in-memory,
	 * we check internal pages, and if they're large enough, we deepen the
	 * tree at that point.  This code is here because we've just split into
	 * a parent page, so check if the parent needs to split.
	 *
	 * A rough metric: addresses in the standard block manager are 10B, more
	 * or less, and let's pretend a standard key is 0B for column-store and
	 * 20B for row-store.  If writing the parent page requires more than N
	 * pages, deepen the tree to add those pages.
	 */
	bytes = 10;
	if (parent->type == WT_PAGE_ROW_INT)
		bytes += 20;
	if ((bytes * result_entries) /
	    btree->maxintlpage > (uint64_t)btree->split_deepen)
		ret = __rec_split_deepen(session, parent);

err:	if (locked)
		WT_PAGE_UNLOCK(session, parent);

	__wt_free(session, alloc_index);
	__wt_free_ref_array(session, page, alloc_ref, mod->multi_entries);
	__wt_free(session, alloc_ref);

	return (ret);
}

/*
 * __rec_page_dirty_update --
 *	Update a dirty page's reference on eviction.
 */
static int
__rec_page_dirty_update(
    WT_SESSION_IMPL *session, WT_REF *parent_ref, WT_PAGE *page)
{
	WT_ADDR *addr;
	WT_PAGE_MODIFY *mod;

	mod = page->modify;
	switch (F_ISSET(mod, WT_PM_REC_MASK)) {
	case WT_PM_REC_EMPTY:				/* Page is empty */
		if (parent_ref->addr != NULL &&
		    __wt_off_page(page->parent, parent_ref->addr)) {
			__wt_free(session, ((WT_ADDR *)parent_ref->addr)->addr);
			__wt_free(session, parent_ref->addr);
		}

		/*
		 * Update the parent to reference an empty page.
		 *
		 * Set the transaction ID to WT_TXN_NONE because the fact that
		 * reconciliation left the page "empty" means there's no older
		 * transaction in the system that might need to see an earlier
		 * version of the page.  It isn't necessary (WT_TXN_NONE is 0),
		 * but it's the right thing to do.
		 *
		 * Publish: a barrier to ensure the structure fields are set
		 * before the state change makes the page available to readers.
		 */
		parent_ref->page = NULL;
		parent_ref->addr = NULL;
		parent_ref->txnid = WT_TXN_NONE;
		WT_PUBLISH(parent_ref->state, WT_REF_DELETED);
		break;
	case WT_PM_REC_REPLACE: 			/* 1-for-1 page swap */
		if (parent_ref->addr != NULL &&
		    __wt_off_page(page->parent, parent_ref->addr)) {
			__wt_free(session, ((WT_ADDR *)parent_ref->addr)->addr);
			__wt_free(session, parent_ref->addr);
		}

		/*
		 * Update the parent to reference the replacement page.
		 *
		 * Publish: a barrier to ensure the structure fields are set
		 * before the state change makes the page available to readers.
		 */
		WT_RET(__wt_calloc(session, 1, sizeof(WT_ADDR), &addr));
		*addr = mod->replace;
		mod->replace.addr = NULL;
		mod->replace.size = 0;

		parent_ref->page = NULL;
		parent_ref->addr = addr;
		WT_PUBLISH(parent_ref->state, WT_REF_DISK);
		break;
	case WT_PM_REC_SPLIT:				/* Page split */
		WT_RET(__rec_split_evict(session, parent_ref, page));
		break;
	WT_ILLEGAL_VALUE(session);
	}

	return (0);
}

/*
 * __rec_discard_tree --
 *	Discard the tree rooted a page (that is, any pages merged into it),
 * then the page itself.
 */
static void
__rec_discard_tree(WT_SESSION_IMPL *session, WT_PAGE *page, int exclusive)
{
	WT_REF *ref;

	switch (page->type) {
	case WT_PAGE_COL_INT:
	case WT_PAGE_ROW_INT:
		/* For each entry in the page... */
		WT_INTL_FOREACH_BEGIN(page, ref) {
			if (ref->state == WT_REF_DISK ||
			    ref->state == WT_REF_DELETED)
				continue;
			WT_ASSERT(session,
			    exclusive || ref->state == WT_REF_LOCKED);
			__rec_discard_tree(session, ref->page, exclusive);
		} WT_INTL_FOREACH_END;
		/* FALLTHROUGH */
	default:
		__wt_page_out(session, &page);
		break;
	}
}

/*
 * __rec_review --
 *	Get exclusive access to the page and review the page and its subtree
 *	for conditions that would block its eviction.
 *
 *	The ref and page arguments may appear to be redundant, because usually
 *	ref->page == page and page->ref == ref.  However, we need both because
 *	(a) there are cases where ref == NULL (e.g., for root page or during
 *	salvage), and (b) we can't safely look at page->ref until we have a
 *	hazard pointer.
 */
static int
__rec_review(WT_SESSION_IMPL *session,
    WT_REF *ref, WT_PAGE *page, int exclusive, int top, int *istree)
{
	WT_BTREE *btree;
	WT_PAGE_MODIFY *mod;
	WT_PAGE *t;

	btree = S2BT(session);

	/*
	 * Get exclusive access to the page if our caller doesn't have the tree
	 * locked down.
	 */
	if (!exclusive) {
		WT_RET(__hazard_exclusive(session, ref, top));

		/*
		 * Now the page is locked, remove it from the LRU eviction
		 * queue.  We have to do this before freeing the page memory or
		 * otherwise touching the reference because eviction paths
		 * assume a non-NULL reference on the queue is pointing at
		 * valid memory.
		 */
		__wt_evict_list_clr_page(session, page);
	}

	/*
	 * Recurse through the page's subtree: this happens first because we
	 * have to write pages in depth-first order, otherwise we'll dirty
	 * pages after we've written them.
	 */
	if (page->type == WT_PAGE_COL_INT || page->type == WT_PAGE_ROW_INT)
		WT_INTL_FOREACH_BEGIN(page, ref) {
			switch (ref->state) {
			case WT_REF_DISK:		/* On-disk */
			case WT_REF_DELETED:		/* On-disk, deleted */
				break;
			case WT_REF_MEM:		/* In-memory */
				/*
				 * Tell our caller if there's a subtree so we
				 * know to do a full walk when discarding the
				 * page.
				 */
				*istree = 1;
				WT_RET(__rec_review(session,
				    ref, ref->page, exclusive, 0, istree));
				break;
			case WT_REF_EVICT_WALK:		/* Walk point */
			case WT_REF_LOCKED:		/* Being evicted */
			case WT_REF_READING:		/* Being read */
			case WT_REF_SPLIT:		/* Being split */
				return (EBUSY);
			WT_ILLEGAL_VALUE(session);
			}
		} WT_INTL_FOREACH_END;

	/*
	 * If the file is being checkpointed, we cannot evict dirty pages,
	 * because that may free a page that appears on an internal page in the
	 * checkpoint.  Don't rely on new updates being skipped by the
	 * transaction used for transaction reads: (1) there are paths that
	 * dirty pages for artificial reasons; (2) internal pages aren't
	 * transactional; and (3) if an update was skipped during the
	 * checkpoint (leaving the page dirty), then rolled back, we could
	 * still successfully overwrite a page and corrupt the checkpoint.
	 *
	 * Further, even for clean pages, the checkpoint's reconciliation of an
	 * internal page might race with us as we evict a child in the page's
	 * subtree.
	 *
	 * One half of that test is in the reconciliation code: the checkpoint
	 * thread waits for eviction-locked pages to settle before determining
	 * their status.  The other half of the test is here: after acquiring
	 * the exclusive eviction lock on a page, confirm no page in the page's
	 * stack of pages from the root is being reconciled in a checkpoint.
	 * This ensures we either see the checkpoint-walk state here, or the
	 * reconciliation of the internal page sees our exclusive lock on the
	 * child page and waits until we're finished evicting the child page
	 * (or give up if eviction isn't possible).
	 *
	 * We must check the full stack (we might be attempting to evict a leaf
	 * page multiple levels beneath the internal page being reconciled as
	 * part of the checkpoint, and  all of the intermediate nodes are being
	 * merged into the internal page).
	 *
	 * There's no simple test for knowing if a page in our page stack is
	 * involved in a checkpoint.  The internal page's checkpoint-walk flag
	 * is the best test, but it's not set anywhere for the root page, it's
	 * not a complete test.
	 *
	 * Quit for any page that's not a simple, in-memory page.  (Almost the
	 * same as checking for the checkpoint-walk flag.  I don't think there
	 * are code paths that change the page's status from checkpoint-walk,
	 * but these races are hard enough I'm not going to proceed if there's
	 * anything other than a vanilla, in-memory tree stack.)  Climb until
	 * we find a page which can't be merged into its parent, and failing if
	 * we never find such a page.
	 */
	if (btree->checkpointing && __wt_page_is_modified(page)) {
ckpt:		WT_STAT_FAST_CONN_INCR(session, cache_eviction_checkpoint);
		WT_STAT_FAST_DATA_INCR(session, cache_eviction_checkpoint);
		return (EBUSY);
	}

	if (btree->checkpointing && top)
		for (t = page->parent;; t = t->parent) {
			if (t == NULL || t->parent == NULL)	/* root */
				goto ckpt;
								/* scary */
			if (__wt_page_ref(session, t)->state != WT_REF_MEM)
				goto ckpt;
			if (t->modify == NULL ||		/* not merged */
			    !F_ISSET(t->modify,
			    WT_PM_REC_EMPTY | WT_PM_REC_SPLIT))
				break;
		}

	/*
	 * Fail if any page in the top-level page's subtree won't be merged into
	 * its parent, the page that cannot be merged must be evicted first.
	 * The test is necessary but should not fire much: the eviction code is
	 * biased for leaf pages, an internal page shouldn't be selected for
	 * eviction until its children have been evicted.
	 *
	 * We have to write dirty pages to know their final state, a page marked
	 * empty may have had records added since reconciliation, a page marked
	 * split may have had records deleted and no longer need to split.
	 * Split-merge pages are the exception: they can never be change into
	 * anything other than a split-merge page and are merged regardless of
	 * being clean or dirty.
	 *
	 * Writing the page is expensive, do a cheap test first: if it doesn't
	 * appear a subtree page can be merged, quit.  It's possible the page
	 * has been emptied since it was last reconciled, and writing it before
	 * testing might be worthwhile, but it's more probable we're attempting
	 * to evict an internal page with live children, and that's a waste of
	 * time.
	 */
	mod = page->modify;
	if (!top &&
	    (mod == NULL || !F_ISSET(mod, WT_PM_REC_EMPTY | WT_PM_REC_SPLIT)))
		return (EBUSY);

	/*
	 * If the page is dirty and can possibly change state, write it so we
	 * know the final state.
	 */
	if (__wt_page_is_modified(page)) {
		WT_RET(__wt_rec_write(session, page,
		    NULL, WT_EVICTION_SERVER_LOCKED | WT_SKIP_UPDATE_RESTORE));

		/*
		 * Update the page's modification reference, reconciliation
		 * might have changed it.
		 *
		 * XXXKEITH: I don't think this is true, I don't think the
		 * page's modify reference ever moves (or can move).
		 */
		mod = page->modify;
	}

	/*
	 * If the page was ever modified, make sure all of the updates on the
	 * page are old enough that they can be discarded from cache.
	 */
	if (!exclusive && mod != NULL &&
	    !__wt_txn_visible_all(session, mod->rec_max_txn))
		return (EBUSY);

	/*
	 * Repeat the test: fail if any page in the top-level page's subtree
	 * won't be merged into its parent.
	 */
	if (!top &&
	    (mod == NULL || !F_ISSET(mod, WT_PM_REC_EMPTY | WT_PM_REC_SPLIT)))
		return (EBUSY);

	return (0);
}

/*
 * __rec_excl_clear --
 *	Discard exclusive access and return a page's subtree to availability.
 */
static void
__rec_excl_clear(WT_SESSION_IMPL *session)
{
	WT_REF *ref;
	uint32_t i;

	for (i = 0; i < session->excl_next; ++i) {
		if ((ref = session->excl[i]) == NULL)
			break;
		WT_ASSERT(session,
		    ref->state == WT_REF_LOCKED && ref->page != NULL);
		ref->state = WT_REF_MEM;
	}
}

/*
 * __hazard_exclusive --
 *	Request exclusive access to a page.
 */
static int
__hazard_exclusive(WT_SESSION_IMPL *session, WT_REF *ref, int top)
{
	/*
	 * Make sure there is space to track exclusive access so we can unlock
	 * to clean up.
	 */
	WT_RET(__wt_realloc_def(session, &session->excl_allocated,
	    session->excl_next + 1, &session->excl));

	/*
	 * Hazard pointers are acquired down the tree, which means we can't
	 * deadlock.
	 *
	 * Request exclusive access to the page.  The top-level page should
	 * already be in the locked state, lock child pages in memory.
	 * If another thread already has this page, give up.
	 */
	if (!top && !WT_ATOMIC_CAS(ref->state, WT_REF_MEM, WT_REF_LOCKED))
		return (EBUSY);	/* We couldn't change the state. */
	WT_ASSERT(session, ref->state == WT_REF_LOCKED);

	session->excl[session->excl_next++] = ref;

	/* Check for a matching hazard pointer. */
	if (__wt_page_hazard_check(session, ref->page) == NULL)
		return (0);

	WT_STAT_FAST_DATA_INCR(session, cache_eviction_hazard);
	WT_STAT_FAST_CONN_INCR(session, cache_eviction_hazard);

	WT_VERBOSE_RET(
	    session, evict, "page %p hazard request failed", ref->page);
	return (EBUSY);
}
