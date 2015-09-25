/*-
 * Copyright (c) 2014-2015 MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

static int  __evict_page_dirty_update(WT_SESSION_IMPL *, WT_REF *, bool);
static int  __evict_review(WT_SESSION_IMPL *, WT_REF *, bool *, bool);

/*
 * __evict_exclusive_clear --
 *	Release exclusive access to a page.
 */
static inline void
__evict_exclusive_clear(WT_SESSION_IMPL *session, WT_REF *ref)
{
	WT_ASSERT(session, ref->state == WT_REF_LOCKED && ref->page != NULL);

	ref->state = WT_REF_MEM;
}

/*
 * __evict_exclusive --
 *	Acquire exclusive access to a page.
 */
static inline int
__evict_exclusive(WT_SESSION_IMPL *session, WT_REF *ref)
{
	WT_ASSERT(session, ref->state == WT_REF_LOCKED);

	/*
	 * Check for a hazard pointer indicating another thread is using the
	 * page, meaning the page cannot be evicted.
	 */
	if (__wt_page_hazard_check(session, ref->page) == NULL)
		return (0);

	WT_STAT_FAST_DATA_INCR(session, cache_eviction_hazard);
	WT_STAT_FAST_CONN_INCR(session, cache_eviction_hazard);
	return (EBUSY);
}

/*
 * __wt_evict --
 *	Evict a page.
 */
int
__wt_evict(WT_SESSION_IMPL *session, WT_REF *ref, bool closing)
{
	WT_CONNECTION_IMPL *conn;
	WT_DECL_RET;
	WT_PAGE *page;
	WT_PAGE_MODIFY *mod;
	bool forced_eviction, inmem_split;

	conn = S2C(session);

	/* Checkpoints should never do eviction. */
	WT_ASSERT(session, !WT_SESSION_IS_CHECKPOINT(session));

	page = ref->page;
	forced_eviction = page->read_gen == WT_READGEN_OLDEST;
	inmem_split = false;

	WT_RET(__wt_verbose(session, WT_VERB_EVICT,
	    "page %p (%s)", page, __wt_page_type_string(page->type)));

	/*
	 * Get exclusive access to the page and review it for conditions that
	 * would block our eviction of the page.  If the check fails (for
	 * example, we find a page with active children), we're done.  We have
	 * to make this check for clean pages, too: while unlikely eviction
	 * would choose an internal page with children, it's not disallowed.
	 */
	WT_ERR(__evict_review(session, ref, &inmem_split, closing));

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
	if (!closing && WT_PAGE_IS_INTERNAL(page)) {
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

	/* Update the reference and discard the page. */
	if (mod == NULL || mod->rec_result == 0) {
		if (__wt_ref_is_root(ref))
			__wt_ref_out(session, ref);
		else
			WT_ERR(__wt_evict_page_clean_update(
			    session, ref, closing));

		WT_STAT_FAST_CONN_INCR(session, cache_eviction_clean);
		WT_STAT_FAST_DATA_INCR(session, cache_eviction_clean);
	} else {
		if (__wt_ref_is_root(ref))
			__wt_ref_out(session, ref);
		else
			WT_ERR(__evict_page_dirty_update(
			    session, ref, closing));

		WT_STAT_FAST_CONN_INCR(session, cache_eviction_dirty);
		WT_STAT_FAST_DATA_INCR(session, cache_eviction_dirty);
	}

	if (0) {
err:		if (!closing)
			__evict_exclusive_clear(session, ref);

		WT_STAT_FAST_CONN_INCR(session, cache_eviction_fail);
		WT_STAT_FAST_DATA_INCR(session, cache_eviction_fail);
	}

done:	if (((inmem_split && ret == 0) || (forced_eviction && ret == EBUSY)) &&
	    !F_ISSET(conn->cache, WT_CACHE_WOULD_BLOCK)) {
		F_SET(conn->cache, WT_CACHE_WOULD_BLOCK);
		WT_TRET(__wt_evict_server_wake(session));
	}

	return (ret);
}

/*
 * __wt_evict_page_clean_update --
 *	Update a clean page's reference on eviction.
 */
int
__wt_evict_page_clean_update(
    WT_SESSION_IMPL *session, WT_REF *ref, bool closing)
{
	/*
	 * If doing normal system eviction, but only in the service of reducing
	 * the number of dirty pages, leave the clean page in cache.
	 */
	if (!closing && __wt_eviction_dirty_target(session))
		return (EBUSY);

	/*
	 * Discard the page and update the reference structure; if the page has
	 * an address, it's a disk page; if it has no address, it's a deleted
	 * page re-instantiated (for example, by searching) and never written.
	 */
	__wt_ref_out(session, ref);
	WT_PUBLISH(ref->state,
	    ref->addr == NULL ? WT_REF_DELETED : WT_REF_DISK);

	return (0);
}

/*
 * __evict_page_dirty_update --
 *	Update a dirty page's reference on eviction.
 */
static int
__evict_page_dirty_update(WT_SESSION_IMPL *session, WT_REF *ref, bool closing)
{
	WT_ADDR *addr;
	WT_PAGE *parent;
	WT_PAGE_MODIFY *mod;

	parent = ref->home;
	mod = ref->page->modify;

	switch (mod->rec_result) {
	case WT_PM_REC_EMPTY:				/* Page is empty */
		/* Discard the parent's address. */
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
		 * A real split where we reconciled a page and it turned into a
		 * lot of pages.
		 */
		WT_RET(__wt_split_multi(session, ref, closing));
		break;
	case WT_PM_REC_REPLACE: 			/* 1-for-1 page swap */
		/*
		 * If doing normal system eviction, but only in the service of
		 * reducing the number of dirty pages, leave the clean page in
		 * cache. Only do this when replacing a page with another one,
		 * because when a page splits into multiple pages, we want to
		 * push it out of cache (and read it back in, when needed), we
		 * would rather have more, smaller pages than fewer large pages.
		 */
		if (!closing && __wt_eviction_dirty_target(session))
			return (EBUSY);

		/* Discard the parent's address. */
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
	case WT_PM_REC_REWRITE:
		/*
		 * An in-memory page that got too large, we forcibly evicted
		 * it, and there wasn't anything to write. (Imagine two threads
		 * updating a small set keys on a leaf page. The page is too
		 * large so we try to evict it, but after reconciliation
		 * there's only a small amount of data (so it's a single page
		 * we can't split), and because there are two threads, there's
		 * some data we can't write (so we can't evict it). In that
		 * case, we take advantage of the fact we have exclusive access
		 * to the page and rewrite it in memory.)
		 */
		WT_RET(__wt_split_rewrite(session, ref));
		break;
	WT_ILLEGAL_VALUE(session);
	}

	return (0);
}

/*
 * __evict_child_check --
 *	Review an internal page for active children.
 */
static int
__evict_child_check(WT_SESSION_IMPL *session, WT_REF *parent)
{
	WT_REF *child;

	WT_INTL_FOREACH_BEGIN(session, parent->page, child) {
		switch (child->state) {
		case WT_REF_DISK:		/* On-disk */
		case WT_REF_DELETED:		/* On-disk, deleted */
			break;
		default:
			return (EBUSY);
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
__evict_review(
    WT_SESSION_IMPL *session, WT_REF *ref, bool *inmem_splitp, bool closing)
{
	WT_DECL_RET;
	WT_PAGE *page;
	uint32_t flags;

	/*
	 * Get exclusive access to the page if our caller doesn't have the tree
	 * locked down.
	 */
	if (!closing) {
		WT_RET(__evict_exclusive(session, ref));

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

	/*
	 * Fail if an internal has active children, the children must be evicted
	 * first. The test is necessary but shouldn't fire much: the eviction
	 * code is biased for leaf pages, an internal page shouldn't be selected
	 * for eviction until all children have been evicted.
	 */
	if (WT_PAGE_IS_INTERNAL(page)) {
		WT_WITH_PAGE_INDEX(session,
		    ret = __evict_child_check(session, ref));
		WT_RET(ret);
	}

	/* Check if the page can be evicted. */
	if (!closing) {
		/*
		 * Update the oldest ID to avoid wasted effort should it have
		 * fallen behind current.
		 */
		if (__wt_page_is_modified(page))
			__wt_txn_update_oldest(session, true);

		if (!__wt_page_can_evict(session, page, false, inmem_splitp))
			return (EBUSY);

		/*
		 * Check for an append-only workload needing an in-memory
		 * split; we can't do this earlier because in-memory splits
		 * require exclusive access. If an in-memory split completes,
		 * the page stays in memory and the tree is left in the desired
		 * state: avoid the usual cleanup.
		 */
		if (*inmem_splitp)
			return (__wt_split_insert(session, ref));
	}

	/* If the page is clean, we're done and we can evict. */
	if (!__wt_page_is_modified(page))
		return (0);

	/*
	 * If the page is dirty, reconcile it to decide if we can evict it.
	 *
	 * If we have an exclusive lock (we're discarding the tree), assert
	 * there are no updates we cannot read.
	 *
	 * Otherwise, if the page we're evicting is a leaf page marked for
	 * forced eviction, set the update-restore flag, so reconciliation will
	 * write blocks it can write and create a list of skipped updates for
	 * blocks it cannot write.  This is how forced eviction of active, huge
	 * pages works: we take a big page and reconcile it into blocks, some of
	 * which we write and discard, the rest of which we re-create as smaller
	 * in-memory pages, (restoring the updates that stopped us from writing
	 * the block), and inserting the whole mess into the page's parent.
	 *
	 * Otherwise, if eviction is getting pressed, configure reconciliation
	 * to write not-yet-globally-visible updates to the lookaside table,
	 * allowing the eviction of pages we'd otherwise have to retain in cache
	 * to support older readers.
	 *
	 * Don't set the update-restore or lookaside table flags for internal
	 * pages, they don't have update lists that can be saved and restored.
	 */
	flags = WT_EVICTING;
	if (closing)
		LF_SET(WT_VISIBILITY_ERR);
	else if (!WT_PAGE_IS_INTERNAL(page)) {
		if (page->read_gen == WT_READGEN_OLDEST)
			LF_SET(WT_EVICT_UPDATE_RESTORE);
		else if (__wt_eviction_aggressive(session))
			LF_SET(WT_EVICT_LOOKASIDE);
	}

	WT_RET(__wt_reconcile(session, ref, NULL, flags));

	/*
	 * Success: assert the page is clean or reconciliation was configured
	 * for an update/restore split, and if the page is clean, reconciliation
	 * was configured for a lookaside table or all updates on the page are
	 * globally visible.
	 */
	WT_ASSERT(session,
	    LF_ISSET(WT_EVICT_UPDATE_RESTORE) || !__wt_page_is_modified(page));
	WT_ASSERT(session,
	    LF_SET(WT_EVICT_LOOKASIDE) ||
	    __wt_page_is_modified(page) ||
	    __wt_txn_visible_all(session, page->modify->rec_max_txn));

	return (0);
}
