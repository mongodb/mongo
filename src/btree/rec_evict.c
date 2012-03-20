/*-
 * Copyright (c) 2008-2012 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

static int  __hazard_exclusive(WT_SESSION_IMPL *, WT_REF *, int);
static int  __rec_discard(WT_SESSION_IMPL *, WT_PAGE *);
static int  __rec_discard_page(WT_SESSION_IMPL *, WT_PAGE *);
static void __rec_excl_clear(WT_SESSION_IMPL *);
static int  __rec_page_clean_update(WT_SESSION_IMPL *, WT_PAGE *);
static int  __rec_page_dirty_update(WT_SESSION_IMPL *, WT_PAGE *);
static int  __rec_review(WT_SESSION_IMPL *, WT_REF *, WT_PAGE *, uint32_t, int);
static int  __rec_root_addr_update(WT_SESSION_IMPL *, uint8_t *, uint32_t);
static int  __rec_root_clean_update(WT_SESSION_IMPL *, WT_PAGE *);
static int  __rec_root_dirty_update(WT_SESSION_IMPL *, WT_PAGE *);

/*
 * __wt_rec_evict --
 *	Reconciliation plus eviction.
 */
int
__wt_rec_evict(WT_SESSION_IMPL *session, WT_PAGE *page, uint32_t flags)
{
	WT_CONNECTION_IMPL *conn;
	int ret;

	conn = S2C(session);
	ret = 0;

	WT_VERBOSE(session, evict,
	    "page %p (%s)", page, __wt_page_type_string(page->type));

	WT_ASSERT(session, session->excl_next == 0);

	/*
	 * Get exclusive access to the page and review the page and its subtree
	 * for conditions that would block our eviction of the page.  If the
	 * check fails (for example, we find a child page that can't be merged),
	 * we're done.  We have to make this check for clean pages, too: while
	 * unlikely eviction would choose an internal page with children, it's
	 * not disallowed anywhere.
	 *
	 * Note that page->ref may be NULL in some cases (e.g., for root pages
	 * or during salvage).  That's OK if WT_REC_SINGLE is set: we won't
	 * check hazard references in that case.
	 */
	WT_ERR(__rec_review(session, page->ref, page, flags, 1));

	/* Count evictions of internal pages during normal operation. */
	if (!LF_ISSET(WT_REC_SINGLE) &&
	    (page->type == WT_PAGE_COL_INT || page->type == WT_PAGE_ROW_INT))
		WT_STAT_INCR(conn->stats, cache_evict_internal);

	/* Update the parent and discard the page. */
	if (F_ISSET(page, WT_PAGE_REC_MASK) == 0) {
		WT_STAT_INCR(conn->stats, cache_evict_unmodified);

		if (WT_PAGE_IS_ROOT(page))
			WT_ERR(__rec_root_clean_update(session, page));
		else
			WT_ERR(__rec_page_clean_update(session, page));
	} else {
		WT_STAT_INCR(conn->stats, cache_evict_modified);

		if (WT_PAGE_IS_ROOT(page))
			WT_ERR(__rec_root_dirty_update(session, page));
		else
			WT_ERR(__rec_page_dirty_update(session, page));
	}

	if (0) {
err:		/*
		 * If unable to evict this page, release exclusive reference(s)
		 * we've acquired.
		 */
		__rec_excl_clear(session);
	}
	session->excl_next = 0;
	return (ret);
}

/*
 * __rec_page_clean_update  --
 *	Update a page's reference for an evicted, clean page.
 */
static int
__rec_page_clean_update(WT_SESSION_IMPL *session, WT_PAGE *page)
{
	/* Update the relevant WT_REF structure. */
	WT_PUBLISH(page->ref->state, WT_REF_DISK);
	page->ref->page = NULL;

	return (__rec_discard_page(session, page));
}

/*
 * __rec_root_clean_update  --
 *	Update a page's reference for an evicted, clean page.
 */
static int
__rec_root_clean_update(WT_SESSION_IMPL *session, WT_PAGE *page)
{
	WT_BTREE *btree;

	btree = session->btree;

	btree->root_page = NULL;

	return (__rec_discard_page(session, page));
}

/*
 * __rec_page_dirty_update --
 *	Update a page's reference for an evicted, dirty page.
 */
static int
__rec_page_dirty_update(WT_SESSION_IMPL *session, WT_PAGE *page)
{
	WT_PAGE_MODIFY *mod;
	WT_REF *parent_ref;

	mod = page->modify;
	parent_ref = page->ref;

	switch (F_ISSET(page, WT_PAGE_REC_MASK)) {
	case WT_PAGE_REC_REPLACE: 			/* 1-for-1 page swap */
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
		WT_RET(__wt_calloc(
		    session, 1, sizeof(WT_ADDR), &parent_ref->addr));
		((WT_ADDR *)parent_ref->addr)->addr = mod->u.replace.addr;
		((WT_ADDR *)parent_ref->addr)->size = mod->u.replace.size;
		WT_PUBLISH(parent_ref->state, WT_REF_DISK);
		parent_ref->page = NULL;
		break;
	case WT_PAGE_REC_SPLIT:				/* Page split */
		/*
		 * Update the parent to reference new internal page(s).
		 *
		 * Publish: a barrier to ensure the structure fields are set
		 * before the state change makes the page available to readers.
		 */
		parent_ref->page = mod->u.split;
		WT_PUBLISH(parent_ref->state, WT_REF_MEM);

		/* Clear the reference else discarding the page will free it. */
		mod->u.split = NULL;
		break;
	case WT_PAGE_REC_EMPTY:				/* Page is empty */
		/* We checked if the page was empty when we reviewed it. */
		/* FALLTHROUGH */
	WT_ILLEGAL_VALUE(session);
	}

	/*
	 * Discard pages which were merged into this page during reconciliation,
	 * then discard the page itself.
	 */
	WT_RET(__rec_discard(session, page));

	return (0);
}

/*
 * __rec_root_addr_update --
 *	Update the root page's address.
 */
static int
__rec_root_addr_update(WT_SESSION_IMPL *session, uint8_t *addr, uint32_t size)
{
	WT_ADDR *root_addr;
	WT_BTREE *btree;

	btree = session->btree;
	root_addr = &btree->root_addr;

	/* Free any previously created root addresses. */
	if (root_addr->addr != NULL) {
		WT_RET(__wt_bm_free(session, root_addr->addr, root_addr->size));
		__wt_free(session, root_addr->addr);
	}
	btree->root_update = 1;

	root_addr->addr = addr;
	root_addr->size = size;

	return (0);
}

/*
 * __rec_root_dirty_update --
 *	Update the reference for an evicted, dirty root page.
 */
static int
__rec_root_dirty_update(WT_SESSION_IMPL *session, WT_PAGE *page)
{
	WT_BTREE *btree;
	WT_PAGE *next;
	WT_PAGE_MODIFY *mod;

	btree = session->btree;
	mod = page->modify;

	next = NULL;
	switch (F_ISSET(page, WT_PAGE_REC_MASK)) {
	case WT_PAGE_REC_EMPTY:				/* Page is empty */
		WT_VERBOSE(session, evict, "root page empty");

		/* If the root page is empty, clear the root address. */
		WT_RET(__rec_root_addr_update(session, NULL, 0));
		btree->root_page = NULL;
		break;
	case WT_PAGE_REC_REPLACE: 			/* 1-for-1 page swap */
		WT_VERBOSE(session, evict, "root page replaced");

		/* Update the root to its replacement. */
		WT_RET(__rec_root_addr_update(
		    session, mod->u.replace.addr, mod->u.replace.size));
		btree->root_page = NULL;
		break;
	case WT_PAGE_REC_SPLIT:				/* Page split */
		WT_VERBOSE(session, evict,
		    "root page split %p -> %p", page, mod->u.split);

		/* Update the root to the split page. */
		next = mod->u.split;

		/* Clear the reference else discarding the page will free it. */
		mod->u.split = NULL;
		break;
	}

	/*
	 * Discard pages which were merged into this page during reconciliation,
	 * then discard the page itself.
	 */
	WT_RET(__rec_discard(session, page));

	if (next == NULL)
		return (0);

	/*
	 * Newly created internal pages are normally merged into their parent
	 * when the parent is evicted.  Newly split root pages can't be merged,
	 * they have no parent and the new root page must be written.  We also
	 * have to write the root page immediately, as the sync or close that
	 * triggered the split won't see our new root page during its traversal.
	 *
	 * Make the new root page look like a normal page that's been modified,
	 * write it out and discard it.  Keep doing that and eventually we'll
	 * perform a simple replacement (as opposed to another level of split),
	 * allowing us to can update the tree's root information and quit.  The
	 * only time we see multiple splits in here is when we've bulk-loaded
	 * something huge, and now we're evicting the index page referencing all
	 * of those leaf pages.
	 */
	WT_RET(__wt_page_modify_init(session, next));
	__wt_page_modify_set(next);
	F_CLR(next, WT_PAGE_REC_MASK);

	WT_RET(__wt_rec_write(session, next, NULL));

	return (__rec_root_dirty_update(session, next));
}

/*
 * __rec_discard --
 *	Discard any pages merged into an evicted page, then the page itself.
 */
static int
__rec_discard(WT_SESSION_IMPL *session, WT_PAGE *page)
{
	WT_REF *ref;
	uint32_t i;

	switch (page->type) {
	case WT_PAGE_COL_INT:
	case WT_PAGE_ROW_INT:
		/* For each entry in the page... */
		WT_REF_FOREACH(page, ref, i)
			if (ref->state != WT_REF_DISK)
				WT_RET(__rec_discard(session, ref->page));
		/* FALLTHROUGH */
	default:
		WT_RET(__rec_discard_page(session, page));
		break;
	}
	return (0);
}

/*
 * __rec_discard_page --
 *	Process the page's list of tracked objects, and discard it.
 */
static int
__rec_discard_page(WT_SESSION_IMPL *session, WT_PAGE *page)
{
	WT_PAGE_MODIFY *mod;

	mod = page->modify;

	/*
	 * or if the page was split and later merged, discard it.
	 */
	if (mod != NULL) {
		/*
		 * If the page has been modified and was tracking objects,
		 * resolve them.
		 */
		WT_RET(__wt_rec_track_wrapup(session, page, 1));

		/*
		 * If the page was split and eventually merged into the parent,
		 * discard the split page; if the split page was promoted into
		 * a split-merge page, then the reference must be cleared before
		 * the page is discarded.
		 */
		if (F_ISSET(page, WT_PAGE_REC_MASK) == WT_PAGE_REC_SPLIT &&
		    mod->u.split != NULL)
			__wt_page_out(session, mod->u.split, 0);
	}

	/* We should never evict the file's current eviction point. */
	WT_ASSERT(session, session->btree->evict_page != page);

	/* Discard the page itself. */
	__wt_page_out(session, page, 0);

	return (0);
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
 *	hazard reference.
 */
static int
__rec_review(WT_SESSION_IMPL *session,
    WT_REF *ref, WT_PAGE *page, uint32_t flags, int top)
{
	uint32_t i;

	/*
	 * Get exclusive access to the page if our caller doesn't have the tree
	 * locked down.
	 */
	if (!LF_ISSET(WT_REC_SINGLE))
		WT_RET(__hazard_exclusive(session, ref, top));

	/*
	 * Recurse through the page's subtree: this happens first because we
	 * have to write pages in depth-first order, otherwise we'll dirty
	 * pages after we've written them.
	 */
	if (page->type == WT_PAGE_COL_INT || page->type == WT_PAGE_ROW_INT)
		WT_REF_FOREACH(page, ref, i)
			switch (ref->state) {
			case WT_REF_DISK:		/* On-disk */
				break;
			case WT_REF_MEM:		/* In-memory */
				WT_RET(__rec_review(
				    session, ref, ref->page, flags, 0));
				break;
			case WT_REF_EVICT_WALK:		/* Walk point */
			case WT_REF_EVICTING:		/* Being evaluated */
			case WT_REF_LOCKED:		/* Being evicted */
			case WT_REF_READING:		/* Being read */
				return (EBUSY);
			}

	/*
	 * Check if this page can be evicted:
	 *
	 * Fail if the top-level page is a page expected to be removed from the
	 * tree as part of eviction (an empty page or a split-merge page).  Note
	 * "split" pages are NOT included in this test, because a split page can
	 * be separately evicted, at which point it's replaced in its parent by
	 * a reference to a split-merge page.  That's a normal part of the leaf
	 * page life-cycle if it grows too large and must be pushed out of the
	 * cache.  There is also an exception for empty pages, the root page may
	 * be empty when evicted, but that only happens when the tree is closed.
	 *
	 * Fail if any page in the top-level page's subtree can't be merged into
	 * its parent.  You can't evict a page that references such in-memory
	 * pages, they must be evicted first.  The test is necessary but should
	 * not fire much: the LRU-based eviction code is biased for leaf pages,
	 * an internal page shouldn't be selected for LRU-based eviction until
	 * its children have been evicted.  Empty, split and split-merge pages
	 * are all included in this test, they can all be merged into a parent.
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
	 *
	 * We don't do a cheap test for the top-level page: we're not called
	 * to evict split-merge pages, which means the only interesting case
	 * is an empty page.  If the eviction thread picked an "empty" page
	 * for eviction, it must have had reason, probably the empty page got
	 * really, really full and is being forced out of the cache.
	 */
	if (!top && !F_ISSET(page,
	    WT_PAGE_REC_EMPTY | WT_PAGE_REC_SPLIT | WT_PAGE_REC_SPLIT_MERGE))
		return (EBUSY);

	/* If the page is dirty, write it so we know the final state. */
	if (__wt_page_is_modified(page) &&
	    !F_ISSET(page, WT_PAGE_REC_SPLIT_MERGE))
		WT_RET(__wt_rec_write(session, page, NULL));

	/*
	 * Repeat the eviction tests.
	 *
	 * Fail if the top-level page should be merged into its parent, and it's
	 * not the root page.
	 *
	 * Fail if a page in the top-level page's subtree can't be merged into
	 * its parent.
	 */
	if (top) {
		/*
		 * We never get a top-level split-merge page to evict, they are
		 * ignored by the eviction thread.  Check out of sheer paranoia.
		 */
		if (F_ISSET(page, WT_PAGE_REC_SPLIT_MERGE))
			return (EBUSY);
		if (F_ISSET(page, WT_PAGE_REC_EMPTY) && !WT_PAGE_IS_ROOT(page))
			return (EBUSY);
	} else
		if (!F_ISSET(page, WT_PAGE_REC_EMPTY |
		    WT_PAGE_REC_SPLIT | WT_PAGE_REC_SPLIT_MERGE))
			return (EBUSY);
	return (0);
}

/*
 * __rec_excl_clear --
 *     Discard exclusive access and return a page's subtree to availability.
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
	WT_CONNECTION_IMPL *conn;
	WT_HAZARD *hp;
	uint32_t elem, i;

	/*
	 * Make sure there is space to track exclusive access so we can unlock
	 * to clean up.
	 */
	if (session->excl_next * sizeof(WT_REF *) == session->excl_allocated)
		WT_RET(__wt_realloc(session, &session->excl_allocated,
		    (session->excl_next + 50) * sizeof(WT_REF *),
		    &session->excl));

	/*
	 * Hazard references are acquired down the tree, which means we can't
	 * deadlock.
	 *
	 * Request exclusive access to the page.  It may be either in the
	 * evicting state (if this is the top-level page for this eviction
	 * operation), or a child page in memory.  If another thread already
	 * has this page, give up.
	 */
	if (!WT_ATOMIC_CAS(ref->state, WT_REF_MEM, WT_REF_LOCKED) && (!top ||
	    !WT_ATOMIC_CAS(ref->state, WT_REF_EVICTING, WT_REF_LOCKED)))
		return (EBUSY);	/* We couldn't change the state. */

	session->excl[session->excl_next++] = ref;

	/* Walk the list of hazard references to search for a match. */
	conn = S2C(session);
	elem = conn->session_size * conn->hazard_size;
	for (i = 0, hp = conn->hazard; i < elem; ++i, ++hp)
		if (hp->page == ref->page) {
			WT_BSTAT_INCR(session, rec_hazard);
			WT_CSTAT_INCR(session, cache_evict_hazard);

			WT_VERBOSE(session,
			    evict, "page %p hazard request failed", ref->page);
			return (EBUSY);
		}

	return (0);
}
