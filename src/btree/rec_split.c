/*-
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

static u_int __split_deepen_child_count = 250;		/* Tuning... */
static u_int __split_deepen_entries = 250;
static u_int __split_deepen_max_internal_pages = 250;

/*
 * __split_should_deepen --
 *	Return if we should deepen the tree.
 */
static int
__split_should_deepen(WT_SESSION_IMPL *session, WT_PAGE *page)
{
	WT_PAGE_MODIFY *mod;

	mod = page->modify;

	/*
	 * Splits are based on either the number of child pages that will be
	 * created by the split (avoiding an internal page that will be slow
	 * to search), or by the memory footprint of the parent page (avoiding
	 * an internal page that will eat up all of the cache and put eviction
	 * pressure on the system).
	 *
	 * Paranoia: don't try and split if we don't have anything to split.
	 */
	if (mod->mod_splits_entries < 50)
		return (0);

	/*
	 * Split to deepen the tree if the page's memory footprint is N times
	 * the maximum internal page size of the backing file.
	 */
	if (page->memory_footprint >
	    __split_deepen_max_internal_pages * S2BT(session)->maxintlpage)
		return (1);

	/*
	 * Split to deepen the tree if the split will result in at least N
	 * children in the newly created intermediate layer.
	 */
	if (mod->mod_splits_entries > __split_deepen_child_count)
		return (1);

	return (0);
}

/*
 * __split_list_alloc --
 *	Allocate room for a new WT_REF array as necessary.
 */
static int
__split_list_alloc(
    WT_SESSION_IMPL *session, WT_PAGE_MODIFY *mod, uint32_t *ip)
{
	size_t bytes_allocated;
	uint32_t i;

	for (i = 0; i < mod->mod_splits_entries; ++i)
		if (mod->mod_splits[i].refs == NULL)
			break;
	if (i == mod->mod_splits_entries) {
		/*
		 * Calculate the bytes-allocated explicitly, this information
		 * lives in the page-modify structure, and it's worth keeping
		 * that as small as possible.
		 */
		bytes_allocated =
		    mod->mod_splits_entries * sizeof(mod->mod_splits[0]);
		WT_RET(__wt_realloc(session, &bytes_allocated,
		    (i + 5) * sizeof(mod->mod_splits[0]), &mod->mod_splits));
		mod->mod_splits_entries = i + 5;
	}
	*ip = i;
	return (0);
}

/*
 * __split_ref_instantiate --
 *	Instantiate key/address pairs in memory.
 */
static int
__split_ref_instantiate(
    WT_SESSION_IMPL *session, WT_PAGE *page, WT_REF *ref, size_t *incrp)
{
	WT_ADDR *addr;
	WT_CELL_UNPACK unpack;
	WT_DECL_RET;
	size_t size;
	void *key;

	/*
	 * Instantiate row-store keys, and column- and row-store addresses in
	 * the WT_REF structures referenced by a page that's being split (and
	 * deepening the tree).  The WT_REF structures aren't moving, but the
	 * index references are moving from the page we're splitting to a set
	 * of child pages, and so we can no longer reference the block image
	 * that remains with the page being split.
	 *
	 * No locking is required to update the WT_REF structure because we're
	 * the only thread splitting the parent page, and there's no way for
	 * readers to race with our updates of single pointers.
	 *
	 * Row-store keys, first.
	 */
	if (page->type == WT_PAGE_ROW_INT && !__wt_ref_key_instantiated(ref)) {
		__wt_ref_key(page, ref, &key, &size);
		WT_RET(__wt_row_ikey(session, 0, key, size, &ref->key.ikey));
		*incrp += sizeof(WT_IKEY) + size;
	}

	/*
	 * If there's no address (the page has never been written), or the
	 * address has been instantiated, there's no work to do.  Otherwise,
	 * get the address from the on-page cell.
	 */
	if (ref->addr != NULL && !__wt_off_page(page, ref->addr)) {
		WT_RET(__wt_calloc_def(session, 1, &addr));
		__wt_cell_unpack((WT_CELL *)ref->addr, &unpack);
		if ((ret = __wt_strndup(
		    session, unpack.data, unpack.size, &addr->addr)) != 0) {
			__wt_free(session, addr);
			return (ret);
		}
		addr->size = (uint8_t)unpack.size;
		addr->type =
		    unpack.raw == WT_CELL_ADDR_INT ? WT_ADDR_INT : WT_ADDR_LEAF;
		ref->addr = addr;
		*incrp += sizeof(WT_ADDR) + unpack.size;
	}
	return (0);
}

#ifdef HAVE_DIAGNOSTIC
/*
 * __split_verify_intl_key_order --
 *	Verify the key order on an internal page after a split, diagnostic only.
 */
static void
__split_verify_intl_key_order(WT_SESSION_IMPL *session, WT_PAGE *page)
{
	WT_BTREE *btree;
	WT_ITEM *next, _next, *last, _last, *tmp;
	WT_REF *ref;
	uint64_t recno;
	int skip_first, cmp;

	btree = S2BT(session);

	switch (page->type) {
	case WT_PAGE_COL_INT:
		recno = 0;
		WT_INTL_FOREACH_BEGIN(page, ref) {
			WT_ASSERT(session, ref->key.recno > recno);
			recno = ref->key.recno;
		} WT_INTL_FOREACH_END;
		break;
	case WT_PAGE_ROW_INT:
		next = &_next;
		WT_CLEAR(_next);
		last = &_last;
		WT_CLEAR(_last);
		skip_first = WT_PAGE_IS_ROOT(page);

		WT_INTL_FOREACH_BEGIN(page, ref) {
			__wt_ref_key(page, ref, &next->data, &next->size);
			if (last->size == 0) {
				if (skip_first)
					skip_first = 0;
				else {
					(void)WT_LEX_CMP(session,
					    btree->collator, last, next, cmp);
					WT_ASSERT(session, cmp < 0);
				}
			}
			tmp = last;
			last = next;
			next = tmp;
		} WT_INTL_FOREACH_END;
		break;
	}
}
#endif

/*
 * __split_deepen --
 *	Split an internal page in-memory, deepening the tree.
 */
static int
__split_deepen(WT_SESSION_IMPL *session, WT_PAGE *parent)
{
	WT_BTREE *btree;
	WT_DECL_RET;
	WT_PAGE *child;
	WT_PAGE_INDEX *alloc_index, *pindex;
	WT_REF **alloc, *alloc_ref, **child_refp, *parent_ref, *ref, **refp;
	size_t incr, parent_incr, size;
	uint32_t chunk, entries, i, j, remain, slots;
	int panic;
	void *p;

	btree = S2BT(session);
	alloc_index = NULL;
	alloc_ref = NULL;
	panic = 0;

	pindex = parent->pg_intl_index;
	entries = __split_deepen_entries;

	WT_STAT_FAST_CONN_INCR(session, cache_eviction_deepen);
	WT_VERBOSE_ERR(session, split,
	    "%p: %" PRIu32 " elements, splitting into %" PRIu32 " children",
	    parent, pindex->entries, entries);

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

	/* Allocate a new parent WT_PAGE_INDEX and WT_REF array. */
	WT_ERR(__wt_calloc(session, 1,
	    sizeof(WT_PAGE_INDEX) +
	    (entries + SPLIT_CORRECT_2) * sizeof(WT_REF *), &alloc_index));
	alloc_index->index = (WT_REF **)(alloc_index + 1);
	alloc_index->entries = entries + SPLIT_CORRECT_2;
	WT_ERR(__wt_calloc_def(session, entries, &alloc_ref));

	/*
	 * Initialize the first/last slots of the allocated WT_PAGE_INDEX to
	 * point to the first/last pages we're keeping at the current level,
	 * and the rest of the slots to point to the new WT_REF array.
	 */
	alloc_index->index[0] = pindex->index[0];
	alloc_index->index[alloc_index->entries - 1] =
	    pindex->index[pindex->entries - 1];
	for (alloc = alloc_index->index + SPLIT_CORRECT_1,
	    ref = alloc_ref, i = 0; i < entries; ++alloc, ++ref, ++i)
		(*alloc) = ref;

	/* Allocate new child pages, and insert into the WT_REF array. */
	chunk = (pindex->entries - SPLIT_CORRECT_2) / entries;
	remain = (pindex->entries - SPLIT_CORRECT_2) - chunk * (entries - 1);
	parent_incr = 0;
	for (refp = pindex->index + SPLIT_CORRECT_1,
	    ref = alloc_ref, i = 0; i < entries; ++ref, ++i) {
		slots = i == entries - 1 ? remain : chunk;
		WT_ERR(__wt_page_alloc(
		    session, parent->type, 0, slots, 0, &child));

		/* Initialize the parent page's child reference. */
		ref->page = child;
		ref->addr = NULL;
		if (parent->type == WT_PAGE_ROW_INT) {
			__wt_ref_key(parent, *refp, &p, &size);
			WT_ERR(__wt_row_ikey(
			    session, 0, p, size, &ref->key.ikey));
			parent_incr += sizeof(WT_IKEY) + size;
		} else
			ref->key.recno = (*refp)->key.recno;
		ref->txnid = WT_TXN_NONE;
		ref->state = WT_REF_MEM;

		/* Initialize the child page, mark it dirty. */
		if (parent->type == WT_PAGE_COL_INT)
			child->pg_intl_recno = (*refp)->key.recno;
		child->parent = parent;
		child->ref_hint = i;
		child->type = parent->type;
		WT_ERR(__wt_page_modify_init(session, child));
		__wt_page_only_modify_set(session, child);

		/*
		 * The newly allocated child's WT_REF index references the same
		 * structures as the parent.  (We cannot move WT_REF structures,
		 * threads may be underneath us right now changing the structure
		 * state.)  However, if the WT_REF structures reference on-page
		 * information, we have to fix that, because the disk image for
		 * the page that has an page index entry for the WT_REF is about
		 * to change.
		 */
		incr = 0;
		for (child_refp =
		    child->pg_intl_index->index, j = 0; j < slots; ++j) {
			WT_ERR(__split_ref_instantiate(
			    session, parent, *refp, &incr));
			*child_refp++ = *refp++;
		}
		if (incr != 0)
			__wt_cache_page_inmem_incr(session, child, incr);
	}
	WT_ASSERT(session, ref - alloc_ref == entries);
	WT_ASSERT(session,
	    refp - pindex->index == pindex->entries - SPLIT_CORRECT_1);

	/*
	 * A note on error handling: until this point, there's no problem with
	 * unwinding on error.  We allocated a new page index, a new array of
	 * WT_REFs and a new set of child pages -- if an error occurred, the
	 * parent remained unchanged.  From now on we're going to modify the
	 * parent page, and attention needs to be paid.
	 *
	 * Increment the parent's memory footprint (no effect on error handling,
	 * the failure mode is a page with an incorrect in-memory footprint).
	 */
	if (parent_incr != 0)
		__wt_cache_page_inmem_incr(session, parent, parent_incr);

	/*
	 * Add the WT_REF array into the parent's list (no effect on error
	 * handling, the failure mode is a chunk of memory on the parent's
	 * splits list that isn't needed, and will be free'd when the page
	 * is discarded).
	 */
	WT_ERR(__split_list_alloc(session, parent->modify, &i));
	parent->modify->mod_splits[i].refs = alloc_ref;
	parent->modify->mod_splits[i].entries = entries;
	alloc_ref = NULL;

	/*
	 * We can't free the previous parent's index, there may be threads using
	 * it.  Add to the session's discard list, to be freed once we know no
	 * threads can still be using it.
	 *
	 * This change affects error handling, we'd have to unwind this change
	 * in order to revert to the previous parent page's state.
	 */
	WT_ERR(__wt_session_fotxn_add(session, pindex,
	    sizeof(WT_PAGE_INDEX) + pindex->entries * sizeof(WT_REF *)));

	/*
	 * Update the parent's index; this is the update which splits the page,
	 * making the change visible to threads descending the tree.  From now
	 * on, we're committed to the split.  If any subsequent work fails, we
	 * have to panic because we potentially have threads of control using
	 * the new page index we just swapped in.
	 */
	WT_PUBLISH(parent->pg_intl_index, alloc_index);
	alloc_index = NULL;
	panic = 1;

#ifdef HAVE_DIAGNOSTIC
	__split_verify_intl_key_order(session, parent);
#endif

	/*
	 * Children of the newly created pages now reference the wrong parent
	 * page, and we have to fix that up.  The problem is revealed when a
	 * thread of control attempts to find a page's WT_REF structure, does
	 * a search of the parent page's index, and fails to find its WT_REF
	 * structure, as the parent page no longer references it.  When that
	 * failure happens, the thread waits for the page's parent reference
	 * to be updated, which we do here: walk the children and fix them up.
	 */
	pindex = parent->pg_intl_index;
	for (refp = pindex->index + SPLIT_CORRECT_1,
	    i = pindex->entries - SPLIT_CORRECT_1; i > 0; ++refp, --i) {
		parent_ref = *refp;
		if (parent_ref->state != WT_REF_MEM)
			continue;
		child = parent_ref->page;
		if (child->type != WT_PAGE_ROW_INT &&
		    child->type != WT_PAGE_COL_INT)
			continue;
		WT_ASSERT(session, child->parent == parent);
#ifdef HAVE_DIAGNOSTIC
		__split_verify_intl_key_order(session, child);
#endif

		WT_INTL_FOREACH_BEGIN(child, ref) {
			/*
			 * For each in-memory page the child references, get a
			 * hazard pointer for the page so it can't be evicted
			 * out from under us, and update its parent reference
			 * as necessary.  If we don't find the page, eviction
			 * got it first, but that's OK too.
			 */
			if ((ret = __wt_page_in(session,
			    child, ref, WT_READ_CACHE)) == WT_NOTFOUND) {
				ret = 0;
				continue;
			}
			WT_ERR(ret);

			/*
			 * The page's parent reference may not be wrong, as we
			 * opened up access from the top of the tree already,
			 * pages may have been read in since then.  Check and
			 * only update pages that reference the original page,
			 * they must be wrong.
			 */
			if (ref->page->parent == parent) {
				ref->page->parent = child;
				ref->page->ref_hint = 0;
			}
			WT_ERR(__wt_page_release(session, ref->page));
		} WT_INTL_FOREACH_END;
	}

	/*
	 * Push out the changes: not required for correctness, but we don't
	 * want threads spinning on incorrect page parent references longer
	 * than necessary.
	 */
	WT_FULL_BARRIER();

	if (0) {
err:		__wt_free(session, alloc_index);
		__wt_free_ref_array(session, parent, alloc_ref, entries, 1);
		__wt_free(session, alloc_ref);

		/*
		 * If panic is set, we saw an error after opening up the tree
		 * to descent through the parent page's new index.  There is
		 * nothing we can do, the tree is inconsistent and there are
		 * threads potentially active in both versions of the tree.
		 */
		if (panic)
			ret = __wt_panic(session);
	}
	return (ret);
}

/*
 * __split_inmem_build --
 *	Instantiate a page in a multi-block set, when an update couldn't be
 * written.
 */
static int
__split_inmem_build(
    WT_SESSION_IMPL *session, WT_PAGE *page, WT_REF *ref, WT_MULTI *multi)
{
	WT_CURSOR_BTREE cbt;
	WT_ITEM key;
	WT_PAGE *new;
	WT_UPDATE *upd;
	WT_UPD_SKIPPED *skip;
	uint64_t recno;
	uint32_t i;

	WT_CLEAR(cbt);
	cbt.btree = S2BT(session);
	WT_CLEAR(key);

	/*
	 * We can find unresolved updates when attempting to evict a page, which
	 * cannot be written.  We could fail those evictions, but if the page is
	 * never quiescent and is growing too large for the cache, we can only
	 * avoid the problem for so long.  The solution is to split those pages
	 * into many on-disk chunks we write, plus some on-disk chunks we don't
	 * write.  This code deals with the latter: any chunk we didn't write is
	 * re-created as an in-memory page, then we apply the unresolved updates
	 * to that page.
	 */
	WT_RET(__wt_page_inmem(session,
	    NULL, NULL, multi->skip_dsk, WT_PAGE_DISK_ALLOC, &ref->page));
	multi->skip_dsk = NULL;
	new = ref->page;

	/* Re-create each modification we couldn't write. */
	for (i = 0, skip = multi->skip; i < multi->skip_entries; ++i, ++skip) {
		if (skip->ins == NULL) {
			upd = page->pg_row_upd[WT_ROW_SLOT(page, skip->rip)];
			page->pg_row_upd[WT_ROW_SLOT(page, skip->rip)] = NULL;
		} else {
			upd = skip->ins->upd;
			skip->ins->upd = NULL;
		}

		switch (page->type) {
		case WT_PAGE_COL_FIX:
		case WT_PAGE_COL_VAR:
			/* Build a key. */
			recno = WT_INSERT_RECNO(skip->ins);

			/* Search the page. */
			WT_RET(__wt_col_search(session, recno, new, &cbt));

			/* Apply the modification. */
			WT_RET(__wt_col_modify(
			    session, &cbt, recno, NULL, upd, 0));
			break;
		case WT_PAGE_ROW_LEAF:
			/* Build a key. */
			if (skip->ins == NULL)
				WT_RET(__wt_row_leaf_key(
				    session, page, skip->rip, &key, 0));
			else {
				key.data = WT_INSERT_KEY(skip->ins);
				key.size = WT_INSERT_KEY_SIZE(skip->ins);
			}

			/* Search the page. */
			WT_RET(__wt_row_search(session, &key, new, &cbt));

			/* Apply the modification. */
			WT_RET(__wt_row_modify(
			    session, &cbt, &key, NULL, upd, 0));
			break;
		WT_ILLEGAL_VALUE(session);
		}
	}

	/* Link the page to its parent. */
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
	WT_REF *ref;
	uint32_t i;

	addr = NULL;
	for (ref = refarg, i = 0; i < entries; ++multi, ++ref, ++i) {
		if (multi->skip == NULL) {
			WT_RET(__wt_calloc_def(session, 1, &addr));
			ref->addr = addr;
			addr->size = multi->addr.size;
			addr->type = multi->addr.type;
			WT_RET(__wt_strndup(session,
			    multi->addr.addr, addr->size, &addr->addr));
		} else
			WT_RET(
			    __split_inmem_build(session, page, ref, multi));

		switch (page->type) {
		case WT_PAGE_ROW_INT:
		case WT_PAGE_ROW_LEAF:
			WT_RET(__wt_strndup(session,
			    multi->key.ikey,
			    multi->key.ikey->size + sizeof(WT_IKEY),
			    &ref->key.ikey));
			break;
		default:
			ref->key.recno = multi->key.recno;
			break;
		}

		ref->txnid = WT_TXN_NONE;

		if (multi->skip == NULL)
			ref->state = WT_REF_DISK;
		else {
			ref->state = WT_REF_MEM;
			__wt_free(session, multi->skip);
		}
	}
	return (0);
}

/*
 * __wt_split_evict --
 *	Resolve a page split, inserting new information into the parent.
 */
int
__wt_split_evict(
    WT_SESSION_IMPL *session, WT_REF *parent_ref, WT_PAGE *page, int exclusive)
{
	WT_BTREE *btree;
	WT_CELL *cell;
	WT_CELL_UNPACK *kpack, _kpack;
	WT_DECL_RET;
	WT_IKEY *ikey;
	WT_PAGE *parent;
	WT_PAGE_INDEX *alloc_index, *pindex;
	WT_PAGE_MODIFY *mod, *parent_mod;
	WT_REF *alloc_ref, **refp, *split;
	uint32_t i, j, parent_entries, result_entries, split_entries;
	int complete, locked;

	btree = S2BT(session);
	kpack = &_kpack;
	alloc_index = NULL;
	alloc_ref = NULL;
	complete = locked = 0;

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
	WT_RET(__wt_calloc_def(session, mod->mod_multi_entries, &alloc_ref));
	WT_ERR(__wt_multi_to_ref(
	    session, page, mod->mod_multi, alloc_ref, mod->mod_multi_entries));

	/*
	 * Get a page-level lock on the parent to single-thread splits into the
	 * page because we need to single-thread sizing/growing the page index.
	 * It's OK to queue up multiple splits as the child pages split, but the
	 * actual split into the parent has to be serialized.  We do memory
	 * allocation inside of the lock and we may want to invest effort in
	 * making the locked period shorter.
	 */
	WT_PAGE_LOCK(session, parent);
	locked = 1;

	/* Append the allocated WT_REF array into the parent's list. */
	WT_ERR(__split_list_alloc(session, parent_mod, &i));
	parent_mod->mod_splits[i].refs = alloc_ref;
	alloc_ref = NULL;
	parent_mod->mod_splits[i].entries = mod->mod_multi_entries;

	/* Allocate a new WT_REF index array and initialize it. */
	pindex = parent->pg_intl_index;
	parent_entries = pindex->entries;
	split = parent_mod->mod_splits[i].refs;
	split_entries = parent_mod->mod_splits[i].entries;
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

	WT_STAT_FAST_CONN_INCR(session, cache_eviction_split);
	WT_VERBOSE_ERR(session, split,
	    "%p: %s merged into %p %" PRIu32 " -> %" PRIu32
	    " (%" PRIu32 ")",
	    page, __wt_page_type_string(page->type), parent, parent_entries,
	    result_entries, result_entries - parent_entries);

	/*
	 * We can't free the previous WT_REF index array, there may be threads
	 * using it.  Add it to the session's discard list, to be freed once we
	 * know no threads can still be using it.
	 */
	WT_ERR(__wt_session_fotxn_add(session, pindex,
	    sizeof(WT_PAGE_INDEX) + pindex->entries * sizeof(WT_REF *)));

	/* Update the parent page's footprint. */
	__wt_cache_page_inmem_incr(session, parent, mod->mod_multi_size);

	/*
	 * Update the parent page's index: this update makes the split visible
	 * to threads descending the tree.
	 */
	WT_PUBLISH(parent->pg_intl_index, alloc_index);
	alloc_index = NULL;

#ifdef HAVE_DIAGNOSTIC
	__split_verify_intl_key_order(session, parent);
#endif

	/*
	 * Reset the page's original WT_REF field to split.  Threads cursoring
	 * through the tree were blocked because that WT_REF state was set to
	 * locked.  This update changes the locked state to split, unblocking
	 * those threads and causing them to re-calculate their position based
	 * on the updated parent page's index.
	 */
	WT_PUBLISH(parent_ref->state, WT_REF_SPLIT);

	/*
	 * Pages with unresolved changes are not marked clean by reconciliation;
	 * mark the page clean now so it will be discarded.
	 */
	if (__wt_page_is_modified(page)) {
		 mod->write_gen = 0;
		 __wt_cache_dirty_decr(session, page);
	}

	/*
	 * A note on error handling: failures before we swapped the new page
	 * index into the parent can be resolved by simply freeing allocated
	 * memory because the original page is unchanged, we can continue to
	 * use it and we have not yet modified the parent.  (See below for
	 * an exception, we cannot discard pages to hold unresolved changes.)
	 * Failures after we swap the new page index into the parent are also
	 * relatively benign because the split is OK and complete and the page
	 * is reset so it will be discarded by eviction.  For that reason, we
	 * ignore further errors unless there's a panic.
	 */
	complete = 1;

	/*
	 * The key for the original page may be an onpage overflow key, and we
	 * just lost track of it as the parent's index no longer references the
	 * WT_REF pointing to it.  Discard it now, including the backing blocks.
	 *
	 * XXXKEITH
	 * I think this code goes away when we stop re-writing overflow blocks
	 * backing row-store keys in reconciliation, which simplifies the error
	 * handling here.  Right now, if this call fails (very unlikely, but
	 * technically possible), we could leak underlying file blocks.
	 */
	switch (parent->type) {
	case WT_PAGE_ROW_INT:
	case WT_PAGE_ROW_LEAF:
		ikey = __wt_ref_key_instantiated(parent_ref);
		if (ikey != NULL && ikey->cell_offset != 0) {
			cell = WT_PAGE_REF_OFFSET(parent, ikey->cell_offset);
			__wt_cell_unpack(cell, kpack);
			if (kpack->ovfl && kpack->raw != WT_CELL_KEY_OVFL_RM)
				WT_ERR(__wt_ovfl_discard(session, cell));
		}
		break;
	}

	/*
	 * Simple page splits trickle up the tree, that is, as leaf pages grow
	 * large enough and are evicted, they'll split into their parent.  And,
	 * as that parent grows large enough and is evicted, it will split into
	 * its parent and so on.  When the page split wave reaches the root,
	 * the tree will permanently deepen as multiple root pages are written.
	 *	However, this only helps if first, the pages are evicted (and
	 * we resist evicting internal pages for obvious reasons), and second,
	 * if the tree is closed and re-opened from a disk image, which is a
	 * rare event.
	 *	To avoid the case of internal pages becoming too large when they
	 * aren't being evicted, check internal pages each time a leaf page is
	 * split into them.  If they're big enough, deepen the tree that point.
	 *	Do the check here because we've just split into a parent page
	 * and we're already holding the page locked.
	 */
	if (!exclusive && __split_should_deepen(session, page))
		ret = __split_deepen(session, parent);

err:	if (locked)
		WT_PAGE_UNLOCK(session, parent);

	__wt_free(session, alloc_index);

	/*
	 * A note on error handling: in the case of evicting a page that has
	 * unresolved changes, we just instantiated some in-memory pages that
	 * reflect those unresolved changes.  The problem is those pages
	 * reference the same WT_UPDATE chains as the page we're splitting,
	 * that is, we simply copied references into the new pages.  If the
	 * split fails, the original page is fine, but discarding the created
	 * page would free those update chains, and that's wrong.  There isn't
	 * an easy solution, there's a lot of small memory allocations in some
	 * common code paths, and unwinding those changes will be difficult.
	 * For now, leak the memory by not discarding the instantiated pages.
	 */
	__wt_free_ref_array(
	    session, page, alloc_ref, mod->mod_multi_entries, 0);
	__wt_free(session, alloc_ref);

	/*
	 * A note on error handling: if we completed the split, return success,
	 * nothing really bad can have happened.
	 */
	return (ret == WT_PANIC || !complete ? ret : 0);
}
