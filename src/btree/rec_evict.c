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
	uint32_t i;

	for (i = 0; i < mod->splits_slots; ++i)
		if (mod->splits[i] == NULL)
			break;
	if (i == mod->splits_slots) {
		WT_RET(__wt_realloc(session,
		    NULL, (i + 5) * sizeof(mod->splits[0]), &mod->splits));
		mod->splits_slots = i + 5;
	}
	*ip = i;
	return (0);
}

/*
 * __rec_split_deepen --
 *	Split an internal page in-memory, deepening the tree.
 */
static int
__rec_split_deepen(WT_SESSION_IMPL *session, WT_PAGE *page)
{
	WT_ADDR *addr, *refaddr;
	WT_BTREE *btree;
	WT_DECL_RET;
	WT_PAGE *child;
	WT_PAGE_INDEX *alloc_index, *pindex;
	WT_REF **alloc, *alloc_ref, *parent_ref, **refp, *ref;
	size_t size;
	uint32_t chunk, entries, i, j, remain, slots;
	void *p;

	btree = S2BT(session);
	alloc_index = NULL;
	alloc_ref = NULL;

	pindex = page->pu_intl_index;
	entries = (uint32_t)btree->split_deepen;

	WT_VERBOSE_ERR(session, split,
	    "page %p with %" PRIu32
	    " elements, splitting in-memory into %" PRIu32 " elements",
	    page, pindex->entries, entries);

	/* Allocate a new parent child-page index. */
	WT_ERR(__wt_calloc(session, 1,
	    sizeof(WT_PAGE_INDEX) + entries * sizeof(WT_REF *), &alloc_index));
	alloc_index->index = (WT_REF **)(alloc_index + 1);
	alloc_index->entries = entries;

	/* Allocate a new parent WT_REF array, then connect the two. */
	WT_ERR(__wt_calloc(session, 1, entries * sizeof(WT_REF), &alloc_ref));
	for (alloc = alloc_index->index, parent_ref = alloc_ref,
	    i = 0; i < entries; ++alloc, ++parent_ref, ++i)
		(*alloc) = parent_ref;

	/*
	 * Allocate new child pages, and insert into the child-page reference
	 * array.
	 */
	chunk = pindex->entries / entries;
	remain = pindex->entries - chunk * (entries - 1);
	for (refp = pindex->index,
	    parent_ref = alloc_ref, i = 0; i < entries; ++parent_ref, ++i) {
		slots = i == entries - 1 ? remain : chunk;
		WT_ERR(__wt_page_alloc(session, page->type, 0, slots, &child));

		/* Initialize the parent page reference. */
		parent_ref->page = child;
		parent_ref->addr = NULL;
		parent_ref->key = (*refp)->key;		/* XXXKEITH stolen */
		parent_ref->txnid = 0;			/* XXXKEITH 0? */
		parent_ref->state = WT_REF_MEM;

		/* Initialize the page, mark it dirty. */
		if (page->type == WT_PAGE_COL_INT)
			child->pu_intl_recno = (*refp)->key.recno;
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
		 * pages.   The parent doesn't need any of its address image
		 * information, we could steal them if they're instantiated, as
		 * long as we do it in an order that won't confuse other threads
		 * of control in the page.  For now, I'm copying everything.)
		 */
		for (ref = child->pu_intl_oindex,
		    j = 0; j < slots; ++refp, ++ref, ++j) {
			ref->page = (*refp)->page;

			refaddr = (*refp)->addr;
			WT_ERR(__wt_calloc_def(session, 1, &addr));
			if ((ret =
			    __wt_calloc(session, 1, refaddr->size, &p)) != 0) {
				__wt_free(session, addr);
				WT_ERR(ret);
			}
			addr->addr = p;
			addr->size = refaddr->size;
			addr->type = refaddr->type;
			memcpy(addr->addr, refaddr->addr, refaddr->size);
			ref->addr = addr;

			if (page->type == WT_PAGE_ROW_INT) {
				__wt_ref_key(page, *refp, &p, &size);
				WT_ERR(__wt_row_ikey_incr(session,
				    page, 0, p, size, &ref->key.ikey));
			} else
				ref->key.recno = (*refp)->key.recno;
			ref->txnid = (*refp)->txnid;
			ref->state = (*refp)->state;
		}
		WT_ASSERT(session, ref - child->pu_intl_oindex == slots);
	}
	WT_ASSERT(session, parent_ref - alloc_ref == entries);
	WT_ASSERT(session, refp - pindex->index == pindex->entries);

	/* Add the WT_REF array into the page's list. */
	WT_ERR(__rec_split_list_alloc(session, page->modify, &i));
	page->modify->splits[i] = alloc_ref;
	alloc_ref = NULL;

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
	WT_PUBLISH(page->pu_intl_index, alloc_index);
	alloc_index = NULL;
	/*
	 * XXXKEITH
	 * We just leaked the old parent index reference.
	 */

	/*
	 * Fix up the children; this is the change that makes the split visible
	 * to threads already in the tree.
	 *
	 * This is really two nested WT_INTL_FOREACH_BEGIN calls, but that won't
	 * work, hard-code one of them.
	 */
	pindex = page->pu_intl_index;
	for (refp = pindex->index, i = pindex->entries; i > 0; ++refp, --i) {
		child = (*refp)->page;
		WT_INTL_FOREACH_BEGIN(child, ref) {
			if (ref->state == WT_REF_MEM) {
				ref->page->parent = child;
				ref->page->ref_hint = 0;
			}
		} WT_INTL_FOREACH_END;
	}
	WT_FULL_BARRIER();

	if (0) {
err:		if (alloc_index != NULL)
			__wt_free(session, alloc_index);
		if (alloc_ref != NULL)
			__wt_free(session, alloc_ref);
	}
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
	WT_DECL_RET;
	WT_PAGE *parent;
	WT_PAGE_INDEX *alloc_index, *pindex;
	WT_PAGE_MODIFY *mod, *parent_mod;
	WT_REF **refp, *split;
	uint64_t bytes;
	uint32_t i, j, parent_entries, result_entries, split_entries;

	btree = S2BT(session);
	alloc_index = NULL;

	mod = page->modify;
	parent = page->parent;
	parent_mod = parent->modify;

	/* If the parent page hasn't yet been modified, now is the time. */
	WT_RET(__wt_page_modify_init(session, parent));
	__wt_page_only_modify_set(session, parent);

	/*
	 * Get a page-level lock on the parent to single-thread splits into the
	 * page.  It's OK to queue up multiple splits as the child pages split,
	 * but the actual split into the parent has to be serialized.  We do
	 * memory allocation inside of the lock, but I don't see a reason to
	 * tighten this down yet, we're only blocking other leaf pages trying
	 * to split into this parent, they can wait their turn.
	 */
	WT_PAGE_LOCK(session, parent);

	/*
	 * Append the underlying split page's WT_REF array into the parent
	 * page's list.
	 */
	WT_ERR(__rec_split_list_alloc(session, parent_mod, &i));
	split = parent_mod->splits[i] = mod->split_ref;

	/* Allocate a new WT_REF index array and initialize it. */
	pindex = parent->pu_intl_index;
	parent_entries = pindex->entries;
	split_entries = mod->split_entries;
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

	/* Update the parent page's footprint. */
	__wt_cache_page_inmem_incr(session, parent, mod->split_size);

	/* We've stolen the page's WT_REF structures, clear the references. */
	mod->split_ref = NULL;
	mod->split_entries = 0;
	mod->split_size = 0;

	/*
	 * Update the parent page's index: this is the update that splits the
	 * parent page, making the split visible to other threads.
	 */
	WT_PUBLISH(parent->pu_intl_index, alloc_index);
	alloc_index = NULL;
	/*
	 * XXXKEITH
	 * We just leaked the old parent index reference.
	 */

	/*
	 * Reset the page's original WT_REF field to split, releasing any
	 * blocked threads.
	 */
	WT_PUBLISH(parent_ref->state, WT_REF_SPLIT);
	/*
	 * XXXKEITH
	 * We just leaked any memory held by this WT_REF structure, we don't
	 * find it when we discard the page.  I suspect that the solution is
	 * that discarding a page walks all of the WT_REF arrays instead of
	 * walking the higher-level index, but that has problems for the code
	 * that deepens the split tree, it "steals" keys from one WT_REF array
	 * to another.
	 */

	WT_STAT_FAST_CONN_INCR(session, cache_eviction_split);
	WT_VERBOSE_ERR(session, split,
	    "page %p split into parent %p %" PRIu32 " -> %" PRIu32,
	    page, parent, parent_entries, result_entries);

	/*
	 * We're already holding the parent page locked, see if the parent needs
	 * to split.
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

err:	if (alloc_index != NULL)
		__wt_free(session, alloc_index);

	WT_PAGE_UNLOCK(session, parent);
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
		*addr = mod->u.replace;
		mod->u.replace.addr = NULL;
		mod->u.replace.size = 0;

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
	WT_DECL_RET;
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
				return (EBUSY);
			case WT_REF_SPLIT:		/* Impossible */
				/* FALLTHROUGH */
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
#ifdef XXXKEITH
		/*
		 * If the page is larger than the maximum allowed, attempt to
		 * split the page in memory before evicting it.  The in-memory
		 * split checks for left and right splits, and prevents the
		 * tree deepening unnecessarily.
		 *
		 * Note, we won't be here if recursively descending a tree of
		 * pages: dirty row-store leaf pages can't be merged into their
		 * parents, which means if top wasn't true in this test, we'd
		 * have returned busy before attempting reconciliation.
		 */
		if (page->type == WT_PAGE_ROW_LEAF &&
		    !F_ISSET_ATOMIC(page, WT_PAGE_WAS_SPLIT) &&
		    __wt_eviction_force_check(session, page)) {
			*inmem_split = 1;
			return (0);
		}
#endif

		ret = __wt_rec_write(session, page,
		    NULL, WT_EVICTION_SERVER_LOCKED | WT_SKIP_UPDATE_QUIT);

		/*
		 * Update the page's modification reference, reconciliation
		 * might have changed it.
		 */
		mod = page->modify;
		if (ret == EBUSY) {
			/* Give up if there are unwritten changes */
			WT_VERBOSE_RET(session, evict,
			    "eviction failed, reconciled page"
			    " contained active updates");

			/* 
			 * We may be able to discard any "update" memory the
			 * page no longer needs.
			 */
			switch (page->type) {
			case WT_PAGE_COL_FIX:
			case WT_PAGE_COL_VAR:
				__wt_col_leaf_obsolete(session, page);
				break;
			case WT_PAGE_ROW_LEAF:
				__wt_row_leaf_obsolete(session, page);
				break;
			}
		}
		WT_RET(ret);

		WT_ASSERT(session, !__wt_page_is_modified(page));
	}

	/*
	 * If the page is clean, but was ever modified, make sure all of the
	 * updates on the page are old enough that they can be discarded from
	 * cache.
	 */
	if (!exclusive && mod != NULL &&
	    !__wt_txn_visible_all(session, mod->disk_txn))
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
