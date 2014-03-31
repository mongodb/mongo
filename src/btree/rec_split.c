/*-
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * Tuning; global variables to allow the binary to be patched, we don't yet have
 * any real understanding of what might be useful to surface to applications.
 */
static u_int __split_deepen_max_internal_image = 100;
static u_int __split_deepen_min_child = 200;
static u_int __split_deepen_per_child = 1000;

/*
 * __split_should_deepen --
 *	Return if we should deepen the tree.
 */
static int
__split_should_deepen(WT_SESSION_IMPL *session, WT_PAGE *page)
{
	WT_PAGE_INDEX *pindex;

	/*
	 * Splits are based on either the number of child pages that will be
	 * created by the split (splitting an internal page that will be slow
	 * to search), or by the memory footprint of the parent page (avoiding
	 * an internal page that will eat up all of the cache and put eviction
	 * pressure on the system).
	 *
	 * Paranoia: don't try and split if we don't have anything to split.
	 */
	pindex = WT_INTL_INDEX_COPY(page);
	if (pindex->entries < 50)
		return (0);

	/*
	 * Split to deepen the tree if the page's memory footprint is N times
	 * the maximum internal page size chunk in the backing file.
	 */
	if (page->memory_footprint >
	    __split_deepen_max_internal_image * S2BT(session)->maxintlpage)
		return (1);

	/*
	 * Split to deepen the tree if the split will result in at least N
	 * children in the newly created intermediate layer.
	 */
	if (pindex->entries >
	    (__split_deepen_per_child * __split_deepen_min_child))
		return (1);

	return (0);
}

/*
 * __split_ref_instantiate --
 *	Instantiate key/address pairs in memory in service of a split.
 */
static int
__split_ref_instantiate(WT_SESSION_IMPL *session,
    WT_PAGE *page, WT_REF *ref, size_t *parent_decrp, size_t *child_incrp)
{
	WT_ADDR *addr;
	WT_CELL_UNPACK unpack;
	WT_DECL_RET;
	WT_IKEY *ikey;
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
	 * Track how much memory the parent is losing and the child gaining.
	 *
	 * No locking is required to update the WT_REF structure because we're
	 * the only thread splitting the parent page, and there's no way for
	 * readers to race with our updates of single pointers.  We change does
	 * have to be written before the page goes away, though, our caller
	 * owns that problem.
	 *
	 * Row-store keys, first.
	 */
	if (page->type == WT_PAGE_ROW_INT) {
		if ((ikey = __wt_ref_key_instantiated(ref)) == NULL) {
			__wt_ref_key(page, ref, &key, &size);
			WT_RET(__wt_row_ikey(session, 0, key, size, &ikey));
			ref->key.ikey = ikey;
		} else
			*parent_decrp += sizeof(WT_IKEY) + ikey->size;
		*child_incrp += sizeof(WT_IKEY) + ikey->size;
	}

	/*
	 * If there's no address (the page has never been written), or the
	 * address has been instantiated, there's no work to do.  Otherwise,
	 * get the address from the on-page cell.
	 */
	if ((addr = ref->addr) == NULL)
		return (0);
	if (__wt_off_page(page, addr)) {
		*child_incrp += sizeof(WT_ADDR) + addr->size;
		*parent_decrp += sizeof(WT_ADDR) + addr->size;
	} else {
		__wt_cell_unpack((WT_CELL *)ref->addr, &unpack);
		WT_RET(__wt_calloc_def(session, 1, &addr));
		if ((ret = __wt_strndup(
		    session, unpack.data, unpack.size, &addr->addr)) != 0) {
			__wt_free(session, addr);
			return (ret);
		}
		addr->size = (uint8_t)unpack.size;
		addr->type =
		    unpack.raw == WT_CELL_ADDR_INT ? WT_ADDR_INT : WT_ADDR_LEAF;
		ref->addr = addr;
		*child_incrp += sizeof(WT_ADDR) + addr->size;
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
	WT_REF *child;
	uint64_t recno;
	int cmp, first;

	btree = S2BT(session);

	switch (page->type) {
	case WT_PAGE_COL_INT:
		recno = 0;
		WT_INTL_FOREACH_BEGIN(page, child) {
			WT_ASSERT(session, child->key.recno > recno);
			recno = child->key.recno;
		} WT_INTL_FOREACH_END;
		break;
	case WT_PAGE_ROW_INT:
		next = &_next;
		WT_CLEAR(_next);
		last = &_last;
		WT_CLEAR(_last);

		first = 1;
		WT_INTL_FOREACH_BEGIN(page, child) {
			__wt_ref_key(page, child, &next->data, &next->size);
			if (last->size == 0) {
				if (first)
					first = 0;
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
	WT_DECL_RET;
	WT_PAGE *child;
	WT_PAGE_INDEX *alloc_index, *child_pindex, *pindex;
	WT_REF **alloc_refp;
	WT_REF *child_ref, **child_refp, *parent_ref, **parent_refp, *ref;
	size_t child_incr, parent_decr, parent_incr, size;
	uint32_t children, chunk, i, j, remain, slots;
	int panic;
	void *p;

	alloc_index = NULL;
	parent_incr = parent_decr = 0;
	panic = 0;

	pindex = WT_INTL_INDEX_COPY(parent);
	children = pindex->entries / __split_deepen_per_child;

	WT_STAT_FAST_CONN_INCR(session, cache_eviction_deepen);
	WT_VERBOSE_ERR(session, split,
	    "%p: %" PRIu32 " elements, splitting into %" PRIu32 " children",
	    parent, pindex->entries, children);

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

	/*
	 * Allocate a new WT_PAGE_INDEX and set of WT_REF objects.  Initialize
	 * the first/last slots of the allocated WT_PAGE_INDEX to point to the
	 * first/last pages we're keeping at the current level, and the rest of
	 * the slots to point to new WT_REF objects.
	 */
	size = sizeof(WT_PAGE_INDEX) +
	    (children + SPLIT_CORRECT_2) * sizeof(WT_REF *);
	parent_incr += size;
	WT_ERR(__wt_calloc(session, 1, size, &alloc_index));
	alloc_index->index = (WT_REF **)(alloc_index + 1);
	alloc_index->entries = children + SPLIT_CORRECT_2;
	alloc_index->index[0] = pindex->index[0];
	alloc_index->index[alloc_index->entries - 1] =
	    pindex->index[pindex->entries - 1];
	for (alloc_refp = alloc_index->index + SPLIT_CORRECT_1,
	    i = 0; i < children; ++alloc_refp, ++i) {
		parent_incr += sizeof(WT_REF);
		WT_ERR(__wt_calloc_def(session, 1, alloc_refp));
	}

	/* Allocate child pages, and connect them into the new page index. */
	chunk = (pindex->entries - SPLIT_CORRECT_2) / children;
	remain = (pindex->entries - SPLIT_CORRECT_2) - chunk * (children - 1);
	for (parent_refp = pindex->index + SPLIT_CORRECT_1,
	    alloc_refp = alloc_index->index + SPLIT_CORRECT_1,
	    i = 0; i < children; ++i) {
		slots = i == children - 1 ? remain : chunk;
		WT_ERR(__wt_page_alloc(
		    session, parent->type, 0, slots, 0, &child));

		/*
		 * Initialize the parent page's child reference; we need a copy
		 * of the page's key.
		 */
		ref = *alloc_refp++;
		ref->home = parent;
		ref->page = child;
		ref->addr = NULL;
		if (parent->type == WT_PAGE_ROW_INT) {
			__wt_ref_key(parent, *parent_refp, &p, &size);
			WT_ERR(
			    __wt_row_ikey(session, 0, p, size, &ref->key.ikey));
			parent_incr += sizeof(WT_IKEY) + size;
		} else
			ref->key.recno = (*parent_refp)->key.recno;
		ref->txnid = WT_TXN_NONE;
		ref->state = WT_REF_MEM;

		/* Initialize the child page. */
		if (parent->type == WT_PAGE_COL_INT)
			child->pg_intl_recno = (*parent_refp)->key.recno;
		child->pg_intl_parent_ref = ref;

		/* Mark it dirty. */
		WT_ERR(__wt_page_modify_init(session, child));
		__wt_page_only_modify_set(session, child);

		/*
		 * The newly allocated child's page index references the same
		 * structures as the parent.  (We cannot move WT_REF structures,
		 * threads may be underneath us right now changing the structure
		 * state.)  However, if the WT_REF structures reference on-page
		 * information, we have to fix that, because the disk image for
		 * the page that has an page index entry for the WT_REF is about
		 * to change.
		 */
		child_incr = 0;
		child_pindex = WT_INTL_INDEX_COPY(child);
		for (child_refp = child_pindex->index, j = 0; j < slots; ++j) {
			WT_ERR(__split_ref_instantiate(session,
			    parent, *parent_refp, &parent_decr, &child_incr));
			*child_refp++ = *parent_refp++;

			child_incr += sizeof(WT_REF);
			parent_decr += sizeof(WT_REF);
		}
		__wt_cache_page_inmem_incr(session, child, child_incr);
	}
	WT_ASSERT(session, alloc_refp -
	    alloc_index->index == alloc_index->entries - SPLIT_CORRECT_1);
	WT_ASSERT(session,
	    parent_refp - pindex->index == pindex->entries - SPLIT_CORRECT_1);

	/*
	 * We can't free the previous parent's index, there may be threads using
	 * it.  Add to the session's discard list, to be freed once we know no
	 * threads can still be using it.
	 *
	 * This change affects error handling, we'd have to unwind this change
	 * in order to revert to the previous parent page's state.
	 */
	size = sizeof(WT_PAGE_INDEX) + pindex->entries * sizeof(WT_REF *);
	parent_decr += size;
	WT_ERR(__wt_session_fotxn_add(session, pindex, size));

	/* Adjust the parent's memory footprint. */
	if (parent_incr >= parent_decr) {
		parent_incr -= parent_decr;
		parent_decr = 0;
	}
	if (parent_decr >= parent_incr) {
		parent_decr -= parent_incr;
		parent_incr = 0;
	}
	if (parent_incr != 0)
		__wt_cache_page_inmem_incr(session, parent, parent_incr);
	if (parent_decr != 0)
		__wt_cache_page_inmem_decr(session, parent, parent_decr);

	/*
	 * Update the parent's index; this is the update which splits the page,
	 * making the change visible to threads descending the tree.  From now
	 * on, we're committed to the split.  If any subsequent work fails, we
	 * have to panic because we potentially have threads of control using
	 * the new page index we just swapped in.
	 *
	 * A note on error handling: until this point, there's no problem with
	 * unwinding on error.  We allocated a new page index, a new set of
	 * WT_REFs and a new set of child pages -- if an error occurred, the
	 * parent remained unchanged, although it may have an incorrect memory
	 * footprint.  From now on we've modified the parent page, attention
	 * needs to be paid.
	 */
	WT_INTL_INDEX_SET(parent, alloc_index);
	alloc_index = NULL;
	panic = 1;

#ifdef HAVE_DIAGNOSTIC
	__split_verify_intl_key_order(session, parent);
#endif

	/*
	 * The moved reference structures now reference the wrong parent page,
	 * and we have to fix that up.  The problem is revealed when a thread
	 * of control searches for a page's reference structure slot, and fails
	 * to find it because the page it's searching no longer references it.
	 * When that failure happens, the thread waits for the reference's home
	 * page to be updated, which we do here: walk the children and fix them
	 * up.
	 */
	pindex = WT_INTL_INDEX_COPY(parent);
	for (parent_refp = pindex->index + SPLIT_CORRECT_1,
	    i = pindex->entries - SPLIT_CORRECT_1; i > 0; ++parent_refp, --i) {
		parent_ref = *parent_refp;
		if (parent_ref->state != WT_REF_MEM)
			continue;
		WT_ASSERT(session, parent_ref->home == parent);

		child = parent_ref->page;
		if (child->type != WT_PAGE_ROW_INT &&
		    child->type != WT_PAGE_COL_INT)
			continue;
#ifdef HAVE_DIAGNOSTIC
		__split_verify_intl_key_order(session, child);
#endif
		WT_INTL_FOREACH_BEGIN(child, child_ref) {
			/*
			 * The page's parent reference may not be wrong, as we
			 * opened up access from the top of the tree already,
			 * pages may have been read in since then.  Check and
			 * only update pages that reference the original page,
			 * they must be wrong.
			 */
			if (child_ref->home == parent) {
				child_ref->home = child;
				child_ref->ref_hint = 0;
			}
		} WT_INTL_FOREACH_END;
	}

	/*
	 * Push out the changes: not required for correctness, but don't let
	 * threads spin on incorrect page references longer than necessary.
	 */
	WT_FULL_BARRIER();

	if (0) {
err:		__wt_free_ref_index(session, parent, alloc_index, 1);

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
    WT_SESSION_IMPL *session, WT_PAGE *orig, WT_REF *ref, WT_MULTI *multi)
{
	WT_CURSOR_BTREE cbt;
	WT_ITEM key;
	WT_PAGE *page;
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
	WT_RET(__wt_page_inmem(
	    session, ref, multi->skip_dsk, WT_PAGE_DISK_ALLOC, &page));

	/*
	 * Clear the disk image and link the page into the passed-in WT_REF to
	 * simplify error handling: our caller will not discard the disk image
	 * when discarding the original page, and our caller will discard the
	 * allocated page on error, when discarding the allocated WT_REF.
	 */
	multi->skip_dsk = NULL;

	/* Re-create each modification we couldn't write. */
	for (i = 0, skip = multi->skip; i < multi->skip_entries; ++i, ++skip) {
		if (skip->ins == NULL) {
			upd = orig->pg_row_upd[WT_ROW_SLOT(orig, skip->rip)];
			orig->pg_row_upd[WT_ROW_SLOT(orig, skip->rip)] = NULL;
		} else {
			upd = skip->ins->upd;
			skip->ins->upd = NULL;
		}

		switch (orig->type) {
		case WT_PAGE_COL_FIX:
		case WT_PAGE_COL_VAR:
			/* Build a key. */
			recno = WT_INSERT_RECNO(skip->ins);

			/* Search the page. */
			WT_RET(__wt_col_search(session, recno, ref, &cbt));

			/* Apply the modification. */
			WT_RET(__wt_col_modify(
			    session, &cbt, recno, NULL, upd, 0));
			break;
		case WT_PAGE_ROW_LEAF:
			/* Build a key. */
			if (skip->ins == NULL)
				WT_RET(__wt_row_leaf_key(
				    session, orig, skip->rip, &key, 0));
			else {
				key.data = WT_INSERT_KEY(skip->ins);
				key.size = WT_INSERT_KEY_SIZE(skip->ins);
			}

			/* Search the page. */
			WT_RET(__wt_row_search(session, &key, ref, &cbt));

			/* Apply the modification. */
			WT_RET(__wt_row_modify(
			    session, &cbt, &key, NULL, upd, 0));
			break;
		WT_ILLEGAL_VALUE(session);
		}
	}

	return (0);
}

/*
 * __wt_multi_to_ref --
 *	Move a multi-block list into an array of WT_REF structures.
 */
int
__wt_multi_to_ref(WT_SESSION_IMPL *session, WT_PAGE *parent,
    WT_PAGE *orig, WT_MULTI *multi, WT_REF **refp, size_t *incrp)
{
	WT_ADDR *addr;
	WT_REF *ref;
	size_t incr;

	addr = NULL;
	incr = 0;

	/* In some cases, the underlying WT_REF has not yet been allocated. */
	if (*refp == NULL) {
		incr += sizeof(WT_REF);
		WT_RET(__wt_calloc_def(session, 1, refp));
	}
	ref = *refp;
	ref->home = parent;

	if (multi->skip == NULL) {
		WT_RET(__wt_calloc_def(session, 1, &addr));
		ref->addr = addr;
		addr->size = multi->addr.size;
		addr->type = multi->addr.type;
		WT_RET(__wt_strndup(session,
		    multi->addr.addr, addr->size, &addr->addr));
		incr += sizeof(WT_ADDR) + addr->size;
	} else
		WT_RET(__split_inmem_build(session, orig, ref, multi));

	switch (orig->type) {
	case WT_PAGE_ROW_INT:
	case WT_PAGE_ROW_LEAF:
		WT_RET(__wt_strndup(session,
		    multi->key.ikey, multi->key.ikey->size + sizeof(WT_IKEY),
		    &ref->key.ikey));
		incr += sizeof(WT_IKEY) + multi->key.ikey->size;
		break;
	default:
		ref->key.recno = multi->key.recno;
		break;
	}

	ref->txnid = WT_TXN_NONE;
	ref->state = multi->skip == NULL ? WT_REF_DISK : WT_REF_MEM;

	/*
	 * If our caller wants to track the memory allocations, we have a return
	 * reference.
	 */
	if (incrp != NULL)
		*incrp += incr;
	return (0);
}

/*
 * __wt_split_evict --
 *	Resolve a page split, inserting new information into the parent.
 */
int
__wt_split_evict(WT_SESSION_IMPL *session, WT_REF *ref, int exclusive)
{
	WT_CELL *cell;
	WT_CELL_UNPACK *kpack, _kpack;
	WT_DECL_RET;
	WT_IKEY *ikey;
	WT_PAGE *parent, *child;
	WT_PAGE_INDEX *alloc_index, *pindex;
	WT_PAGE_MODIFY *mod;
	WT_REF **alloc_refp;
	size_t size;
	uint64_t parent_decr, parent_incr;
	uint32_t i, j, parent_entries, result_entries, split_entries;
	int complete;

	kpack = &_kpack;
	alloc_index = NULL;
	parent_decr = parent_incr = 0;
	complete = 0;

	/*
	 * Get a page-level lock on the parent to single-thread splits into the
	 * page because we need to single-thread sizing/growing the page index.
	 * It's OK to queue up multiple splits as the child pages split, but the
	 * actual split into the parent has to be serialized.  Note we allocate
	 * memory inside of the lock and may want to invest effort in making the
	 * locked period shorter.
	 *
	 * We could race with another thread deepening our parent.  To deal
	 * with that, read the parent pointer each time we try to lock it, and
	 * check that it's still correct after it is locked.
	 */
	for (;;) {
		parent = ref->home;
		F_CAS_ATOMIC(parent, WT_PAGE_SPLITTING, ret);
		if (ret == 0) {
			if (parent != ref->home) {
				F_CLR_ATOMIC(parent, WT_PAGE_SPLITTING);
				continue;
			}
			break;
		} else if (ret == EBUSY)
			__wt_yield();
		else
			return (ret);
	}

	child = ref->page;
	mod = child->modify;

	pindex = WT_INTL_INDEX_COPY(parent);
	parent_entries = pindex->entries;
	split_entries = mod->mod_multi_entries;
	result_entries = (parent_entries - 1) + split_entries;

	WT_STAT_FAST_CONN_INCR(session, cache_eviction_split);
	WT_VERBOSE_ERR(session, split,
	    "%p: %s split into parent %p %" PRIu32 " -> %" PRIu32
	    " (%" PRIu32 ")",
	    child, __wt_page_type_string(child->type), parent, parent_entries,
	    result_entries, result_entries - parent_entries);

	/* Allocate and initialize a new page index array for the parent. */
	size = sizeof(WT_PAGE_INDEX) + result_entries * sizeof(WT_REF *);
	parent_incr += size;
	WT_ERR(__wt_calloc(session, 1, size, &alloc_index));
	alloc_index->index = (WT_REF **)(alloc_index + 1);
	alloc_index->entries = result_entries;
	for (alloc_refp = alloc_index->index, i = 0; i < parent_entries; ++i)
		if (pindex->index[i] == ref)
			for (j = 0; j < split_entries; ++j)
				WT_ERR(__wt_multi_to_ref(session, parent,
				    child, &mod->mod_multi[j],
				    alloc_refp++, &parent_incr));
		else
			*alloc_refp++ = pindex->index[i];

	/*
	 * Update the parent page's index: this update makes the split visible
	 * to threads descending the tree.
	 */
	WT_INTL_INDEX_SET(parent, alloc_index);
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
	WT_PUBLISH(ref->state, WT_REF_SPLIT);

	/*
	 * Pages with unresolved changes are not marked clean by reconciliation;
	 * mark the page clean, and discard it.
	 */
	if (__wt_page_is_modified(child)) {
		 mod->write_gen = 0;
		 __wt_cache_dirty_decr(session, child);
	}
	__wt_ref_out(session, ref);

	/*
	 * A note on error handling: failures before we swapped the new page
	 * index into the parent can be resolved by simply freeing allocated
	 * memory because the original page is unchanged, we can continue to
	 * use it and we have not yet modified the parent.  (See below for an
	 * exception, we cannot discard pages referencing unresolved changes.)
	 * Failures after we swap the new page index into the parent are also
	 * relatively benign because the split is OK and complete and the page
	 * is reset so it will be discarded by eviction.  For that reason, we
	 * ignore further errors unless there's a panic.
	 */
	complete = 1;

	/*
	 * We can't free the previous page index, or the page's original WT_REF
	 * structure, there may be threads using them.  Add both to the session
	 * discard list, to be freed once we know it's safe.
	 */
	size = sizeof(WT_PAGE_INDEX) + pindex->entries * sizeof(WT_REF *);
	WT_ERR(__wt_session_fotxn_add(session, pindex, size));
	WT_ERR(__wt_session_fotxn_add(session, ref, sizeof(WT_REF)));
	parent_decr += size + sizeof(WT_REF);

	/* Adjust the parent's memory footprint. */
	if (parent_incr > parent_decr) {
		parent_incr -= parent_decr;
		parent_decr = 0;
	}
	if (parent_decr > parent_incr) {
		parent_decr -= parent_incr;
		parent_incr = 0;
	}
	if (parent_incr != 0)
		__wt_cache_page_inmem_incr(session, parent, parent_incr);
	if (parent_decr != 0)
		__wt_cache_page_inmem_decr(session, parent, parent_decr);

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
		ikey = __wt_ref_key_instantiated(ref);
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
	if (!exclusive && __split_should_deepen(session, parent))
		ret = __split_deepen(session, parent);

err:	F_CLR_ATOMIC(parent, WT_PAGE_SPLITTING);

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
	__wt_free_ref_index(session, NULL, alloc_index, 0);

	/*
	 * A note on error handling: if we completed the split, return success,
	 * nothing really bad can have happened.
	 */
	return (ret == WT_PANIC || !complete ? ret : 0);
}
