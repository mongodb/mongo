/*-
 * Copyright (c) 2014-2015 MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

static int  __evict_exclusive(WT_SESSION_IMPL *, WT_REF *, int);
static int  __evict_page_dirty_update(WT_SESSION_IMPL *, WT_REF *, int);
static int  __evict_review(WT_SESSION_IMPL *, WT_REF *, int, int, int *, int *);
static void __evict_discard_tree(WT_SESSION_IMPL *, WT_REF *, int, int);
static void __evict_excl_clear(WT_SESSION_IMPL *);

/*
 * __wt_evict --
 *	Eviction.
 */
int
__wt_evict(WT_SESSION_IMPL *session, WT_REF *ref, int exclusive)
{
	WT_CONNECTION_IMPL *conn;
	WT_DECL_RET;
	WT_PAGE *page;
	WT_PAGE_MODIFY *mod;
	WT_TXN_STATE *txn_state;
	int forced_eviction, inmem_split, istree;

	conn = S2C(session);

	page = ref->page;
	forced_eviction = (page->read_gen == WT_READGEN_OLDEST);
	inmem_split = istree = 0;

	WT_RET(__wt_verbose(session, WT_VERB_EVICT,
	    "page %p (%s)", page, __wt_page_type_string(page->type)));

	/*
	 * Pin the oldest transaction ID: eviction looks at page structures
	 * that are freed when no transaction in the system needs them.
	 */
	txn_state = WT_SESSION_TXN_STATE(session);
	if (txn_state->snap_min == WT_TXN_NONE)
		txn_state->snap_min = conn->txn_global.oldest_id;
	else
		txn_state = NULL;

	/*
	 * Get exclusive access to the page and review the page and its subtree
	 * for conditions that would block our eviction of the page.  If the
	 * check fails (for example, we find a child page that can't be merged),
	 * we're done.  We have to make this check for clean pages, too: while
	 * unlikely eviction would choose an internal page with children, it's
	 * not disallowed anywhere.
	 */
	WT_ERR(
	    __evict_review(session, ref, exclusive, 1, &inmem_split, &istree));

	/*
	 * If there was an in-memory split, the tree has been left in the state
	 * we want: there is nothing more to do.
	 */
	if (inmem_split)
		goto done;

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
	 * Track the largest page size seen at eviction, it tells us something
	 * about our ability to force pages out before they're larger than the
	 * cache.
	 */
	if (page->memory_footprint > conn->cache->evict_max_page_size)
		conn->cache->evict_max_page_size = page->memory_footprint;

	/* Discard any subtree rooted in this page. */
	if (istree)
		WT_WITH_PAGE_INDEX(session,
		    __evict_discard_tree(session, ref, exclusive, 1));

	/* Update the reference and discard the page. */
	if (mod == NULL || !F_ISSET(mod, WT_PM_REC_MASK)) {
		WT_ASSERT(session, exclusive || ref->state == WT_REF_LOCKED);

		if (__wt_ref_is_root(ref))
			__wt_ref_out(session, ref);
		else
			__wt_rec_page_clean_update(session, ref);

		WT_STAT_FAST_CONN_INCR(session, cache_eviction_clean);
		WT_STAT_FAST_DATA_INCR(session, cache_eviction_clean);
	} else {
		if (__wt_ref_is_root(ref))
			__wt_ref_out(session, ref);
		else
			WT_ERR(
			    __evict_page_dirty_update(session, ref, exclusive));

		WT_STAT_FAST_CONN_INCR(session, cache_eviction_dirty);
		WT_STAT_FAST_DATA_INCR(session, cache_eviction_dirty);
	}

	if (0) {
err:		/*
		 * If unable to evict this page, release exclusive reference(s)
		 * we've acquired.
		 */
		if (!exclusive)
			__evict_excl_clear(session);

		WT_STAT_FAST_CONN_INCR(session, cache_eviction_fail);
		WT_STAT_FAST_DATA_INCR(session, cache_eviction_fail);
	}
done:	session->excl_next = 0;

	if (txn_state != NULL)
		txn_state->snap_min = WT_TXN_NONE;

	if ((inmem_split || (forced_eviction && ret == EBUSY)) &&
	    !F_ISSET(conn->cache, WT_EVICT_WOULD_BLOCK)) {
		F_SET(conn->cache, WT_EVICT_WOULD_BLOCK);
		WT_TRET(__wt_evict_server_wake(session));
	}

	return (ret);
}

/*
 * __wt_rec_page_clean_update --
 *	Update a clean page's reference on eviction.
 */
void
__wt_rec_page_clean_update(WT_SESSION_IMPL *session, WT_REF *ref)
{
	/*
	 * Discard the page and update the reference structure; if the page has
	 * an address, it's a disk page; if it has no address, it's a deleted
	 * page re-instantiated (for example, by searching) and never written.
	 */
	__wt_ref_out(session, ref);
	WT_PUBLISH(ref->state,
	    ref->addr == NULL ? WT_REF_DELETED : WT_REF_DISK);
}

/*
 * __evict_page_dirty_update --
 *	Update a dirty page's reference on eviction.
 */
static int
__evict_page_dirty_update(WT_SESSION_IMPL *session, WT_REF *ref, int exclusive)
{
	WT_ADDR *addr;
	WT_PAGE *parent;
	WT_PAGE_MODIFY *mod;

	parent = ref->home;
	mod = ref->page->modify;

	switch (F_ISSET(mod, WT_PM_REC_MASK)) {
	case WT_PM_REC_EMPTY:				/* Page is empty */
		if (ref->addr != NULL && __wt_off_page(parent, ref->addr)) {
			__wt_free(session, ((WT_ADDR *)ref->addr)->addr);
			__wt_free(session, ref->addr);
		}

		/*
		 * Update the parent to reference a deleted page.  The fact that
		 * reconciliation left the page "empty" means there's no older
		 * transaction in the system that might need to see an earlier
		 * version of the page.  For that reason, we clear the address
		 * of the page, if we're forced to "read" into that namespace,
		 * we'll instantiate a new page instead of trying to read from
		 * the backing store.
		 *
		 * Publish: a barrier to ensure the structure fields are set
		 * before the state change makes the page available to readers.
		 */
		__wt_ref_out(session, ref);
		ref->addr = NULL;
		WT_PUBLISH(ref->state, WT_REF_DELETED);
		break;
	case WT_PM_REC_MULTIBLOCK:			/* Multiple blocks */
		/*
		 * There are two cases in this code.
		 *
		 * First, an in-memory page that got too large, we forcibly
		 * evicted it, and there wasn't anything to write. (Imagine two
		 * threads updating a small set keys on a leaf page. The page is
		 * too large so we try to evict it, but after reconciliation
		 * there's only a small amount of data (so it's a single page we
		 * can't split), and because there are two threads, there's some
		 * data we can't write (so we can't evict it). In that case, we
		 * take advantage of the fact we have exclusive access to the
		 * page and rewrite it in memory.)
		 *
		 * Second, a real split where we reconciled a page and it turned
		 * into a lot of pages.
		 */
		if (mod->mod_multi_entries == 1)
			WT_RET(__wt_split_rewrite(session, ref));
		else
			WT_RET(__wt_split_multi(session, ref, exclusive));
		break;
	case WT_PM_REC_REPLACE: 			/* 1-for-1 page swap */
		if (ref->addr != NULL && __wt_off_page(parent, ref->addr)) {
			__wt_free(session, ((WT_ADDR *)ref->addr)->addr);
			__wt_free(session, ref->addr);
		}

		/*
		 * Update the parent to reference the replacement page.
		 *
		 * Publish: a barrier to ensure the structure fields are set
		 * before the state change makes the page available to readers.
		 */
		WT_RET(__wt_calloc_one(session, &addr));
		*addr = mod->mod_replace;
		mod->mod_replace.addr = NULL;
		mod->mod_replace.size = 0;

		__wt_ref_out(session, ref);
		ref->addr = addr;
		WT_PUBLISH(ref->state, WT_REF_DISK);
		break;
	WT_ILLEGAL_VALUE(session);
	}

	return (0);
}

/*
 * __evict_discard_tree --
 *	Discard the tree rooted a page (that is, any pages merged into it),
 * then the page itself.
 */
static void
__evict_discard_tree(
    WT_SESSION_IMPL *session, WT_REF *ref, int exclusive, int top)
{
	WT_REF *child;

	switch (ref->page->type) {
	case WT_PAGE_COL_INT:
	case WT_PAGE_ROW_INT:
		/* For each entry in the page... */
		WT_INTL_FOREACH_BEGIN(session, ref->page, child) {
			if (child->state == WT_REF_DISK ||
			    child->state == WT_REF_DELETED)
				continue;
			WT_ASSERT(session,
			    exclusive || child->state == WT_REF_LOCKED);
			__evict_discard_tree(session, child, exclusive, 0);
		} WT_INTL_FOREACH_END;
		/* FALLTHROUGH */
	default:
		if (!top)
			__wt_ref_out(session, ref);
		break;
	}
}

/*
 * __evict_review_subtree --
 *	Review a subtree for conditions that would block its eviction.
 */
static int
__evict_review_subtree(WT_SESSION_IMPL *session,
    WT_REF *ref, int exclusive, int *inmem_splitp, int *istreep)
{
	WT_PAGE *page;
	WT_REF *child;

	page = ref->page;

	WT_INTL_FOREACH_BEGIN(session, page, child) {
		switch (child->state) {
		case WT_REF_DISK:		/* On-disk */
		case WT_REF_DELETED:		/* On-disk, deleted */
			break;
		case WT_REF_MEM:		/* In-memory */
			/*
			 * Tell our caller if there's a subtree so we
			 * know to do a full walk when discarding the
			 * page.
			 */
			*istreep = 1;
			WT_RET(__evict_review(session, child, exclusive,
			    0, inmem_splitp, istreep));
			break;
		case WT_REF_LOCKED:		/* Being evicted */
		case WT_REF_READING:		/* Being read */
		case WT_REF_SPLIT:		/* Being split */
			return (EBUSY);
		WT_ILLEGAL_VALUE(session);
		}
	} WT_INTL_FOREACH_END;

	return (0);
}

/*
 * __evict_review --
 *	Get exclusive access to the page and review the page and its subtree
 *	for conditions that would block its eviction.
 */
static int
__evict_review(WT_SESSION_IMPL *session, WT_REF *ref,
    int exclusive, int top, int *inmem_splitp, int *istreep)
{
	WT_BTREE *btree;
	WT_DECL_RET;
	WT_PAGE *page;
	WT_PAGE_MODIFY *mod;
	uint32_t flags;

	btree = S2BT(session);

	/*
	 * Get exclusive access to the page if our caller doesn't have the tree
	 * locked down.
	 */
	if (!exclusive) {
		WT_RET(__evict_exclusive(session, ref, top));

		/*
		 * Now the page is locked, remove it from the LRU eviction
		 * queue.  We have to do this before freeing the page memory or
		 * otherwise touching the reference because eviction paths
		 * assume a non-NULL reference on the queue is pointing at
		 * valid memory.
		 */
		__wt_evict_list_clear_page(session, ref);
	}

	/* Now that we have exclusive access, review the page. */
	page = ref->page;
	mod = page->modify;

	/*
	 * Recurse through the page's subtree: this happens first because we
	 * have to write pages in depth-first order, otherwise we'll dirty
	 * pages after we've written them.
	 */
	if (WT_PAGE_IS_INTERNAL(page)) {
		/*
		 * Quit if we're trying to push out a "tree", an internal page
		 * with live internal pages as children, it's not likely to
		 * succeed.
		 */
		if (!top && !exclusive)
			return (EBUSY);

		WT_WITH_PAGE_INDEX(session, ret = __evict_review_subtree(
		    session, ref, exclusive, inmem_splitp, istreep));
		WT_RET(ret);
	}

	/*
	 * If the tree was deepened, there's a requirement that newly created
	 * internal pages not be evicted until all threads are known to have
	 * exited the original page index array, because evicting an internal
	 * page discards its WT_REF array, and a thread traversing the original
	 * page index array might see an freed WT_REF.  During the split we set
	 * a transaction value, once that's globally visible, we know we can
	 * evict the created page.
	 */
	if (!exclusive && mod != NULL && WT_PAGE_IS_INTERNAL(page) &&
	    !__wt_txn_visible_all(session, mod->mod_split_txn))
		return (EBUSY);

	/*
	 * If the file is being checkpointed, we can't evict dirty pages:
	 * if we write a page and free the previous version of the page, that
	 * previous version might be referenced by an internal page already
	 * been written in the checkpoint, leaving the checkpoint inconsistent.
	 *
	 * Don't rely on new updates being skipped by the transaction used
	 * for transaction reads: (1) there are paths that dirty pages for
	 * artificial reasons; (2) internal pages aren't transactional; and
	 * (3) if an update was skipped during the checkpoint (leaving the page
	 * dirty), then rolled back, we could still successfully overwrite a
	 * page and corrupt the checkpoint.
	 *
	 * Further, we can't race with the checkpoint's reconciliation of
	 * an internal page as we evict a clean child from the page's subtree.
	 * This works in the usual way: eviction locks the page and then checks
	 * for existing hazard pointers, the checkpoint thread reconciling an
	 * internal page acquires hazard pointers on child pages it reads, and
	 * is blocked by the exclusive lock.
	 */
	if (mod != NULL && btree->checkpointing &&
	    (__wt_page_is_modified(page) ||
	    F_ISSET(mod, WT_PM_REC_MULTIBLOCK))) {
		WT_STAT_FAST_CONN_INCR(session, cache_eviction_checkpoint);
		WT_STAT_FAST_DATA_INCR(session, cache_eviction_checkpoint);
		return (EBUSY);
	}

	/*
	 * Check for an append-only workload needing an in-memory split.
	 *
	 * We can't do this earlier because in-memory splits require exclusive
	 * access.  If an in-memory split completes, the page stays in memory
	 * and the tree is left in the desired state: avoid the usual cleanup.
	 *
	 * Attempt the split before checking whether a checkpoint is running -
	 * that's not a problem here because we aren't evicting any dirty
	 * pages.
	 */
	if (top && !exclusive) {
		WT_RET(__wt_split_insert(session, ref, inmem_splitp));
		if (*inmem_splitp)
			return (0);
	}

	/*
	 * Fail if any page in the top-level page's subtree won't be merged into
	 * its parent, the page that cannot be merged must be evicted first.
	 * The test is necessary but should not fire much: the eviction code is
	 * biased for leaf pages, an internal page shouldn't be selected for
	 * eviction until its children have been evicted.
	 *
	 * We have to write dirty pages to know their final state, a page marked
	 * empty may have had records added since reconciliation.  Writing the
	 * page is expensive, do a cheap test first: if it doesn't seem likely a
	 * subtree page can be merged, quit.
	 */
	if (!top && (mod == NULL || !F_ISSET(mod, WT_PM_REC_EMPTY)))
		return (EBUSY);

	/*
	 * If the page is dirty and can possibly change state, write it so we
	 * know the final state.
	 *
	 * If we have an exclusive lock (we're discarding the tree), assert
	 * there are no updates we cannot read.
	 *
	 * Otherwise, if the top-level page we're evicting is a leaf page, set
	 * the update-restore flag, so reconciliation will write blocks it can
	 * write and create a list of skipped updates for blocks it cannot
	 * write.  This is how forced eviction of huge pages works: we take a
	 * big page and reconcile it into blocks, some of which we write and
	 * discard, the rest of which we re-create as smaller in-memory pages,
	 * (restoring the updates that stopped us from writing the block), and
	 * inserting the whole mess into the page's parent.
	 *
	 * Don't set the update-restore flag for internal pages, they don't
	 * have updates that can be saved and restored.
	 *
	 * Don't set the update-restore flag for small pages.  (If a small
	 * page were selected by eviction and then modified, and we configure it
	 * for update-restore, we'll end up splitting one or two pages into the
	 * parent, which is a waste of effort.  If we don't set update-restore,
	 * eviction will return EBUSY, which makes more sense, the page was just
	 * modified.)
	 *
	 * Don't set the update-restore flag for any page other than the
	 * top one; only the reconciled top page goes through the split path
	 * (and child pages are pages we expect to merge into the top page, they
	 * they are not expected to split).
	 */
	if (__wt_page_is_modified(page)) {
		flags = WT_EVICTING;
		if (exclusive)
			LF_SET(WT_SKIP_UPDATE_ERR);
		else if (top && !WT_PAGE_IS_INTERNAL(page) &&
		    page->read_gen == WT_READGEN_OLDEST)
			LF_SET(WT_SKIP_UPDATE_RESTORE);
		WT_RET(__wt_reconcile(session, ref, NULL, flags));
		WT_ASSERT(session,
		    !__wt_page_is_modified(page) ||
		    LF_ISSET(WT_SKIP_UPDATE_RESTORE));
	} else {
		/*
		 * If the page was ever modified, make sure all of the updates
		 * on the page are old enough they can be discarded from cache.
		 */
		if (!exclusive && mod != NULL &&
		    !__wt_txn_visible_all(session, mod->rec_max_txn))
			return (EBUSY);
	}

	/*
	 * Repeat the test: fail if any page in the top-level page's subtree
	 * won't be merged into its parent.
	 */
	if (!top && (mod == NULL || !F_ISSET(mod, WT_PM_REC_EMPTY)))
		return (EBUSY);

	return (0);
}

/*
 * __evict_excl_clear --
 *	Discard exclusive access and return a page's subtree to availability.
 */
static void
__evict_excl_clear(WT_SESSION_IMPL *session)
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
 * __evict_exclusive --
 *	Request exclusive access to a page.
 */
static int
__evict_exclusive(WT_SESSION_IMPL *session, WT_REF *ref, int top)
{
	/*
	 * Make sure there is space to track exclusive access so we can unlock
	 * to clean up.
	 */
	WT_RET(__wt_realloc_def(session, &session->excl_allocated,
	    session->excl_next + 1, &session->excl));

	/*
	 * Request exclusive access to the page.  The top-level page should
	 * already be in the locked state, lock child pages in memory.
	 * If another thread already has this page, give up.
	 */
	if (!top && !WT_ATOMIC_CAS4(ref->state, WT_REF_MEM, WT_REF_LOCKED))
		return (EBUSY);	/* We couldn't change the state. */
	WT_ASSERT(session, ref->state == WT_REF_LOCKED);

	session->excl[session->excl_next++] = ref;

	/* Check for a matching hazard pointer. */
	if (__wt_page_hazard_check(session, ref->page) == NULL)
		return (0);

	WT_STAT_FAST_DATA_INCR(session, cache_eviction_hazard);
	WT_STAT_FAST_CONN_INCR(session, cache_eviction_hazard);

	WT_RET(__wt_verbose(session, WT_VERB_EVICT,
	    "page %p hazard request failed", ref->page));
	return (EBUSY);
}
