/*-
 * Copyright (c) 2008-2012 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

static int  __hazard_exclusive(WT_SESSION_IMPL *, WT_REF *, int);
static int  __rec_discard_page(WT_SESSION_IMPL *, WT_PAGE *, int);
static int  __rec_discard_tree(WT_SESSION_IMPL *, WT_PAGE *, int);
static void __rec_excl_clear(WT_SESSION_IMPL *);
static void __rec_page_clean_update(WT_SESSION_IMPL *, WT_PAGE *);
static int  __rec_page_dirty_update(WT_SESSION_IMPL *, WT_PAGE *);
static int  __rec_review(WT_SESSION_IMPL *, WT_REF *, WT_PAGE *, uint32_t, int);
static void __rec_root_update(WT_SESSION_IMPL *);

/*
 * __wt_rec_evict --
 *	Reconciliation plus eviction.
 */
int
__wt_rec_evict(WT_SESSION_IMPL *session, WT_PAGE *page, uint32_t flags)
{
	WT_CONNECTION_IMPL *conn;
	WT_DECL_RET;
	int single;

	conn = S2C(session);

	WT_VERBOSE(session, evict,
	    "page %p (%s)", page, __wt_page_type_string(page->type));

	WT_ASSERT(session, session->excl_next == 0);
	single = LF_ISSET(WT_REC_SINGLE) ? 1 : 0;

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
	if (!single &&
	    (page->type == WT_PAGE_COL_INT || page->type == WT_PAGE_ROW_INT))
		WT_STAT_INCR(conn->stats, cache_evict_internal);

	/* Update the parent and discard the page. */
	if (page->modify == NULL || !F_ISSET(page->modify, WT_PM_REC_MASK)) {
		WT_STAT_INCR(conn->stats, cache_evict_unmodified);
		WT_ASSERT(session, single || page->ref->state == WT_REF_LOCKED);

		if (WT_PAGE_IS_ROOT(page))
			__rec_root_update(session);
		else
			__rec_page_clean_update(session, page);

		/* Discard the page. */
		WT_RET(__rec_discard_page(session, page, single));
	} else {
		WT_STAT_INCR(conn->stats, cache_evict_modified);

		if (WT_PAGE_IS_ROOT(page))
			__rec_root_update(session);
		else
			WT_ERR(__rec_page_dirty_update(session, page));

		/* Discard the tree rooted in this page. */
		WT_ERR(__rec_discard_tree(session, page, single));
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
 * __rec_root_update  --
 *	Update a root page's reference on eviction (clean or dirty).
 */
static void
__rec_root_update(WT_SESSION_IMPL *session)
{
	session->btree->root_page = NULL;
}

/*
 * __rec_page_clean_update  --
 *	Update a clean page's reference on eviction.
 */
static void
__rec_page_clean_update(WT_SESSION_IMPL *session, WT_PAGE *page)
{
	/* Update the relevant WT_REF structure. */
	page->ref->page = NULL;
	WT_PUBLISH(page->ref->state, WT_REF_DISK);

	WT_UNUSED(session);
}

/*
 * __rec_page_dirty_update --
 *	Update a dirty page's reference on eviction.
 */
static int
__rec_page_dirty_update(WT_SESSION_IMPL *session, WT_PAGE *page)
{
	WT_PAGE_MODIFY *mod;
	WT_REF *parent_ref;

	mod = page->modify;
	parent_ref = page->ref;

	switch (F_ISSET(mod, WT_PM_REC_MASK)) {
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
		WT_RET(__wt_calloc(
		    session, 1, sizeof(WT_ADDR), &parent_ref->addr));
		((WT_ADDR *)parent_ref->addr)->addr = mod->u.replace.addr;
		((WT_ADDR *)parent_ref->addr)->size = mod->u.replace.size;
		parent_ref->page = NULL;
		WT_PUBLISH(parent_ref->state, WT_REF_DISK);
		break;
	case WT_PM_REC_SPLIT:				/* Page split */
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
		F_CLR(mod, WT_PM_REC_SPLIT);
		break;
	case WT_PM_REC_EMPTY:				/* Page is empty */
		/* We checked if the page was empty when we reviewed it. */
		/* FALLTHROUGH */
	WT_ILLEGAL_VALUE(session);
	}

	return (0);
}

/*
 * __rec_discard_tree --
 *	Discard the tree rooted a page (that is, any pages merged into it),
 * then the page itself.
 */
static int
__rec_discard_tree(WT_SESSION_IMPL *session, WT_PAGE *page, int single)
{
	WT_REF *ref;
	uint32_t i;

	switch (page->type) {
	case WT_PAGE_COL_INT:
	case WT_PAGE_ROW_INT:
		/* For each entry in the page... */
		WT_REF_FOREACH(page, ref, i) {
			if (ref->state == WT_REF_DISK)
				continue;
			WT_ASSERT(session,
			    single || ref->state == WT_REF_LOCKED);
			WT_RET(__rec_discard_tree(session, ref->page, single));
		}
		/* FALLTHROUGH */
	default:
		WT_RET(__rec_discard_page(session, page, single));
		break;
	}
	return (0);
}

/*
 * __rec_discard_page --
 *	Discard the page.
 */
static int
__rec_discard_page(WT_SESSION_IMPL *session, WT_PAGE *page, int single)
{
	/* We should never evict the file's current eviction point. */
	WT_ASSERT(session, session->btree->evict_page != page);

	/* Make sure a page is not in the eviction request list. */
	if (!single)
		__wt_evict_list_clr_page(session, page);

	/* Discard the page. */
	__wt_page_out(session, &page, 0);

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
	WT_PAGE_MODIFY *mod;
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
			case WT_REF_EVICT_FORCE:	/* Forced eviction */
			case WT_REF_EVICT_WALK:		/* Walk point */
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
	mod = page->modify;
	if (!top && (mod == NULL || !F_ISSET(mod,
	    WT_PM_REC_EMPTY | WT_PM_REC_SPLIT | WT_PM_REC_SPLIT_MERGE)))
		return (EBUSY);

	/* If the page is dirty, write it so we know the final state. */
	if (__wt_page_is_modified(page) &&
	    !F_ISSET(mod, WT_PM_REC_SPLIT_MERGE))
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
		if (mod != NULL) {
			if (F_ISSET(mod, WT_PM_REC_SPLIT_MERGE))
				return (EBUSY);
			if (F_ISSET(mod, WT_PM_REC_EMPTY) &&
			    !WT_PAGE_IS_ROOT(page))
				return (EBUSY);
		}
	} else if (mod == NULL || !F_ISSET(mod,
	    WT_PM_REC_EMPTY | WT_PM_REC_SPLIT | WT_PM_REC_SPLIT_MERGE))
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
	 * Request exclusive access to the page.  The top-level page should
	 * already be in the locked state, lock child pages in memory.
	 * If another thread already has this page, give up.
	 */
	if (!top && !WT_ATOMIC_CAS(ref->state, WT_REF_MEM, WT_REF_LOCKED))
		return (EBUSY);	/* We couldn't change the state. */
	WT_ASSERT(session, ref->state == WT_REF_LOCKED);

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
