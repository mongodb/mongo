/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wt_internal.h"

static int  __hazard_bsearch_cmp(const void *, const void *);
static void __hazard_copy(WT_SESSION_IMPL *);
static int  __hazard_exclusive(WT_SESSION_IMPL *, WT_REF *, int);
static int  __hazard_qsort_cmp(const void *, const void *);
static int  __rec_discard(WT_SESSION_IMPL *, WT_PAGE *);
static int  __rec_discard_page(WT_SESSION_IMPL *, WT_PAGE *);
static int  __rec_page_clean_update(WT_SESSION_IMPL *, WT_PAGE *);
static int  __rec_page_dirty_update(WT_SESSION_IMPL *, WT_PAGE *, uint32_t);
static int  __rec_review(WT_SESSION_IMPL *, WT_PAGE *, uint32_t);
static int  __rec_root_addr_update(WT_SESSION_IMPL *, uint8_t *, uint32_t);
static int  __rec_root_clean_update(WT_SESSION_IMPL *, WT_PAGE *);
static int  __rec_root_dirty_update(WT_SESSION_IMPL *, WT_PAGE *);
static void __rec_sub_excl_clear(
		WT_SESSION_IMPL *, WT_PAGE *, WT_PAGE *, uint32_t);
static int  __rec_sub_excl_col(
		WT_SESSION_IMPL *, WT_PAGE *, WT_PAGE **, uint32_t);
static int  __rec_sub_excl_col_clear(WT_SESSION_IMPL *, WT_PAGE *, WT_PAGE *);
static int  __rec_sub_excl_page(
		WT_SESSION_IMPL *, WT_REF *, WT_PAGE *, uint32_t);
static int  __rec_sub_excl_row(
		WT_SESSION_IMPL *, WT_PAGE *, WT_PAGE **, uint32_t);
static int __rec_sub_excl_row_clear(WT_SESSION_IMPL *, WT_PAGE *, WT_PAGE *);

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

	WT_VERBOSE(session, evict,
	    "page %p (%s)", page, __wt_page_type_string(page->type));

	/*
	 * You cannot evict pages merge-split pages (that is, internal pages
	 * that are a result of a split of another page).  They can only be
	 * evicted as a result of evicting their parents, else we would lose
	 * the merge flag and they would be written separately, permanently
	 * deepening the tree.  Should the eviction server request eviction
	 * of a merge-split page, ignore the request (but unlock the page and
	 * bump the read generation to ensure it isn't selected again).
	 */
	if (F_ISSET(page, WT_PAGE_REC_SPLIT_MERGE)) {
		page->read_gen = __wt_cache_read_gen(session);
		page->ref->state = WT_REF_MEM;
		return (0);
	}

	/*
	 * Get exclusive access to the page and review the page and its subtree
	 * for conditions that would block our eviction of the page.  If the
	 * check fails (for example, we find a child page that can't be merged),
	 * we're done.  We have to make this check for clean pages, too: while
	 * unlikely eviction would choose an internal page with children, it's
	 * not disallowed anywhere.
	 */
	WT_RET(__rec_review(session, page, flags));

	/* If the page is dirty, write it. */
	if (__wt_page_is_modified(page))
		WT_ERR(__wt_rec_write(session, page, NULL));

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
			WT_ERR(__rec_page_dirty_update(session, page, flags));
	}

	return (0);

err:	__rec_sub_excl_clear(session, page, NULL, flags);
	return (ret);
}

/*
 * __rec_page_clean_update  --
 *	Update a page's reference for an evicted, clean page.
 */
static int
__rec_page_clean_update(WT_SESSION_IMPL *session, WT_PAGE *page)
{
	/*
	 * Update the relevant WT_REF structure; no memory flush is needed,
	 * the state field is declared volatile.
	 */
	page->ref->page = NULL;
	page->ref->state = WT_REF_DISK;

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
__rec_page_dirty_update(WT_SESSION_IMPL *session, WT_PAGE *page, uint32_t flags)
{
	WT_PAGE_MODIFY *mod;
	WT_REF *parent_ref;

	mod = page->modify;
	parent_ref = page->ref;

	switch (F_ISSET(page, WT_PAGE_REC_MASK)) {
	case WT_PAGE_REC_EMPTY:				/* Page is empty */
		/*
		 * We're not going to evict this page after all, instead we'll
		 * merge it into its parent when that page is evicted.  Release
		 * our exclusive reference to it, as well as any pages below it
		 * we locked down, and return it into use.
		 */
		__rec_sub_excl_clear(session, page, NULL, flags);
		return (0);
	case WT_PAGE_REC_REPLACE: 			/* 1-for-1 page swap */
		if (parent_ref->addr != NULL &&
		    __wt_off_page(page->parent, parent_ref->addr))
			__wt_free(session, parent_ref->addr);
		WT_RET(__wt_calloc(
		    session, 1, sizeof(WT_ADDR), &parent_ref->addr));

		((WT_ADDR *)parent_ref->addr)->addr = mod->u.replace.addr;
		((WT_ADDR *)parent_ref->addr)->size = mod->u.replace.size;
		parent_ref->page = NULL;

		/*
		 * Publish: a barrier to ensure the structure fields are set
		 * before the state change makes the page available to readers.
		 */
		WT_PUBLISH(parent_ref->state, WT_REF_DISK);
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
		break;
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

		next = mod->u.split;
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
	WT_RET(__wt_page_set_modified(session, next));
	F_CLR(next, WT_PAGE_REC_MASK);
	WT_RET(__wt_rec_write(session, next, NULL));
	return (__rec_root_dirty_update(session, next));
}

/*
 * __rec_review --
 *	Get exclusive access to the page and review the page and its subtree
 * for conditions that would block our eviction of the page.
 */
static int
__rec_review(WT_SESSION_IMPL *session, WT_PAGE *page, uint32_t flags)
{
	WT_PAGE *last_page;
	int ret;

	last_page = NULL;
	ret = 0;

	/*
	 * Attempt exclusive access to the page if our caller doesn't have the
	 * tree locked down.
	 */
	if (!LF_ISSET(WT_REC_SINGLE)) {
		WT_RET(__hazard_exclusive(
		    session, page->ref, LF_ISSET(WT_REC_WAIT) ? 1 : 0));

		last_page = page;
	}

	/*
	 * Walk the page's subtree and make sure we can evict this page.
	 *
	 * When evicting a page, it may reference deleted or split pages which
	 * will be merged into the evicted page.
	 *
	 * If we find an in-memory page, we're done: you can't evict a page that
	 * references other in-memory pages, those pages must be evicted first.
	 * While the test is necessary, it shouldn't happen much: reading any
	 * internal page increments its read generation, and so internal pages
	 * shouldn't be selected for eviction until after any children have been
	 * evicted.
	 *
	 * If we find a split page, get exclusive access to the page and then
	 * continue, the split page will be merged into our page.
	 *
	 * If we find a deleted page, get exclusive access to the page and then
	 * check its status.  If still deleted, we can continue, the page will
	 * be merged into our page.  However, another thread of control might
	 * have inserted new material and the page is no longer deleted, which
	 * means the reconciliation fails.
	 *
	 * If reconciliation isn't going to be possible, we have to clear any
	 * pages we locked while we were looking.  We keep track of the last
	 * page we successfully locked, and traverse the tree in the same order
	 * to clear locks, stopping when we reach the last locked page.
	 */
	switch (page->type) {
	case WT_PAGE_COL_INT:
		ret = __rec_sub_excl_col(session, page, &last_page, flags);
		break;
	case WT_PAGE_ROW_INT:
		ret = __rec_sub_excl_row(session, page, &last_page, flags);
		break;
	default:
		break;
	}

	/* If can't evict this page, release our exclusive reference(s). */
	if (ret != 0 && last_page != NULL)
		__rec_sub_excl_clear(session, page, last_page, flags);

	return (ret);
}

/*
 * __rec_sub_excl_clear --
 *     Discard exclusive access and return a page to availability.
 */
static void
__rec_sub_excl_clear(WT_SESSION_IMPL *session,
    WT_PAGE *page, WT_PAGE *last_page, uint32_t flags)
{
	if (LF_ISSET(WT_REC_SINGLE))
		return;

	WT_ASSERT(session, page->ref->state == WT_REF_LOCKED);

	/*
	 * Take care to unlock pages in the same order we locked them.
	 * Otherwise, tracking the last successfully locked page is meaningless.
	 */
	page->ref->state = WT_REF_MEM;
	if (page == last_page)
		return;

	switch (page->type) {
	case WT_PAGE_COL_INT:
		(void)__rec_sub_excl_col_clear(session, page, last_page);
		break;
	case WT_PAGE_ROW_INT:
		(void)__rec_sub_excl_row_clear(session, page, last_page);
		break;
	default:
		break;
	}
}

/*
 * __rec_sub_excl_col --
 *	Walk a column-store internal page's subtree, handling deleted and split
 *	pages.
 */
static int
__rec_sub_excl_col(WT_SESSION_IMPL *session,
    WT_PAGE *parent, WT_PAGE **last_pagep, uint32_t flags)
{
	WT_PAGE *page;
	WT_REF *ref;
	uint32_t i;

	/* For each entry in the page... */
	WT_REF_FOREACH(parent, ref, i) {
		switch (ref->state) {
		case WT_REF_DISK:			/* On-disk */
			continue;
		case WT_REF_LOCKED:			/* Eviction candidate */
		case WT_REF_READING:			/* Being read */
			return (WT_ERROR);
		case WT_REF_MEM:			/* In-memory */
			break;
		}
		page = ref->page;

		WT_RET(__rec_sub_excl_page(session, ref, page, flags));

		*last_pagep = page;

		/* Recurse down the tree. */
		if (page->type == WT_PAGE_COL_INT)
			WT_RET(__rec_sub_excl_col(
			    session, page, last_pagep, flags));
	}
	return (0);
}

/*
 * __rec_sub_excl_col_clear --
 *	Clear any pages for which we have exclusive access -- eviction isn't
 *	possible.
 */
static int
__rec_sub_excl_col_clear(
    WT_SESSION_IMPL *session, WT_PAGE *parent, WT_PAGE *last_page)
{
	WT_PAGE *page;
	WT_REF *ref;
	uint32_t i;

	/* For each entry in the page... */
	WT_REF_FOREACH(parent, ref, i) {
		WT_ASSERT(session, ref->state == WT_REF_LOCKED);
		ref->state = WT_REF_MEM;

		/* Recurse down the tree. */
		page = ref->page;
		if (page == last_page)
			return (1);
		if (page->type == WT_PAGE_COL_INT)
			if (__rec_sub_excl_col_clear(session, page, last_page))
				return (1);
	}

	return (0);
}

/*
 * __rec_sub_excl_row --
 *	Walk a row-store internal page's subtree, and acquiring exclusive access
 * as necessary and checking if the subtree can be evicted.
 */
static int
__rec_sub_excl_row(WT_SESSION_IMPL *session,
    WT_PAGE *parent, WT_PAGE **last_pagep, uint32_t flags)
{
	WT_PAGE *page;
	WT_REF *ref;
	uint32_t i;

	/* For each entry in the page... */
	WT_REF_FOREACH(parent, ref, i) {
		switch (ref->state) {
		case WT_REF_DISK:			/* On-disk */
			continue;
		case WT_REF_LOCKED:			/* Eviction candidate */
		case WT_REF_READING:			/* Being read */
			return (WT_ERROR);
		case WT_REF_MEM:			/* In-memory */
			break;
		}
		page = ref->page;

		WT_RET(__rec_sub_excl_page(session, ref, page, flags));

		*last_pagep = page;

		/* Recurse down the tree. */
		if (page->type == WT_PAGE_ROW_INT)
			WT_RET(__rec_sub_excl_row(
			    session, page, last_pagep, flags));
	}
	return (0);
}

/*
 * __rec_sub_excl_row_clear --
 *	Clear any pages for which we have exclusive access -- eviction isn't
 *	possible.
 */
static int
__rec_sub_excl_row_clear(
    WT_SESSION_IMPL *session, WT_PAGE *parent, WT_PAGE *last_page)
{
	WT_PAGE *page;
	WT_REF *ref;
	uint32_t i;

	/* For each entry in the page... */
	WT_REF_FOREACH(parent, ref, i) {
		switch (ref->state) {
		case WT_REF_DISK:			/* On-disk */
			continue;
		case WT_REF_LOCKED:			/* Eviction candidate */
			break;
		WT_ILLEGAL_VALUE(session);
		}
		ref->state = WT_REF_MEM;

		/* Recurse down the tree. */
		page = ref->page;
		if (page == last_page)
			return (1);
		if (page->type == WT_PAGE_ROW_INT)
			if (__rec_sub_excl_row_clear(session, page, last_page))
				return (1);
	}

	return (0);
}

/*
 * __rec_sub_excl_page --
 *	Acquire exclusive access to a page as necessary, and check if the page
 * can be evicted.
 */
static int
__rec_sub_excl_page(
    WT_SESSION_IMPL *session, WT_REF *ref, WT_PAGE *page, uint32_t flags)
{
	/*
	 * An in-memory page: if the page can't be merged into its parent, then
	 * we can't evict the subtree.  This is not a problem, it just means we
	 * chose badly when selecting a page for eviction.
	 *
	 * First, a cheap test: if the child page doesn't at least have a chance
	 * of a merge, we can't evict the candidate page.
	 */
	if (!F_ISSET(page,
	    WT_PAGE_REC_EMPTY | WT_PAGE_REC_SPLIT | WT_PAGE_REC_SPLIT_MERGE))
		return (1);

	/*
	 * Next, if our caller doesn't have the tree locked down, get exclusive
	 * access to the page and test again.
	 */
	if (!LF_ISSET(WT_REC_SINGLE))
		WT_RET(__hazard_exclusive(
		    session, ref, LF_ISSET(WT_REC_WAIT) ? 1 : 0));

	/*
	 * Second, a more careful test: merge-split pages are OK, no matter if
	 * they're clean or dirty, we can always merge them into the parent.
	 * Clean split or empty pages are OK too.  Dirty split or empty pages
	 * are not OK, they must be written first so we know what they're going
	 * to look like to the parent.
	 */
	if (F_ISSET(page, WT_PAGE_REC_SPLIT_MERGE))
		return (0);
	if (F_ISSET(page, WT_PAGE_REC_SPLIT | WT_PAGE_REC_EMPTY))
		if (!__wt_page_is_modified(page))
			return (0);
	return (1);
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
	/* If the page has tracked objects, resolve them. */
	if (page->modify != NULL)
		WT_RET(__wt_rec_track_wrapup(session, page, 1));

	/* Discard the page itself. */
	__wt_page_out(session, page, 0);

	return (0);
}

/*
 * __hazard_exclusive --
 *	Request exclusive access to a page.
 */
static int
__hazard_exclusive(WT_SESSION_IMPL *session, WT_REF *ref, int force)
{
	WT_CACHE *cache;

	cache = S2C(session)->cache;

	/* The page must be in memory, and we may already have it locked. */
	WT_ASSERT(session,
	    ref->state == WT_REF_MEM || ref->state == WT_REF_LOCKED);

	/*
	 * Hazard references are acquired down the tree, which means we can't
	 * deadlock.
	 *
	 * Request exclusive access to the page; no memory flush needed, the
	 * state field is declared volatile.  If another thread already has
	 * this page and we are not forcing the issue, give up.
	 */
	ref->state = WT_REF_LOCKED;

	/* Get a fresh copy of the hazard reference array. */
retry:	__hazard_copy(session);

	/* If we find a matching hazard reference, the page is still in use. */
	if (bsearch(ref->page, cache->hazard, cache->hazard_elem,
	    sizeof(WT_HAZARD), __hazard_bsearch_cmp) == NULL)
		return (0);

	WT_BSTAT_INCR(session, rec_hazard);

	/*
	 * If we have to get this hazard reference, spin and wait for it to
	 * become available.
	 */
	if (force) {
		__wt_yield();
		goto retry;
	}

	WT_CSTAT_INCR(session, cache_evict_hazard);

	WT_VERBOSE(session, evict,
	    "page %p hazard request failed", ref->page);

	/* Return the page to in-use. */
	ref->state = WT_REF_MEM;

	return (1);
}

/*
 * __hazard_qsort_cmp --
 *	Qsort function: sort hazard list based on the page's address.
 */
static int
__hazard_qsort_cmp(const void *a, const void *b)
{
	WT_PAGE *a_page, *b_page;

	a_page = ((WT_HAZARD *)a)->page;
	b_page = ((WT_HAZARD *)b)->page;

	return (a_page > b_page ? 1 : (a_page < b_page ? -1 : 0));
}

/*
 * __hazard_copy --
 *	Copy the hazard array and prepare it for searching.
 */
static void
__hazard_copy(WT_SESSION_IMPL *session)
{
	WT_CACHE *cache;
	WT_CONNECTION_IMPL *conn;
	uint32_t elem, i, j;

	conn = S2C(session);
	cache = conn->cache;

	/* Copy the list of hazard references, compacting it as we go. */
	elem = conn->session_size * conn->hazard_size;
	for (i = j = 0; j < elem; ++j) {
		if (conn->hazard[j].page == NULL)
			continue;
		cache->hazard[i] = conn->hazard[j];
		++i;
	}
	elem = i;

	/* Sort the list by page address. */
	qsort(
	    cache->hazard, (size_t)elem, sizeof(WT_HAZARD), __hazard_qsort_cmp);
	cache->hazard_elem = elem;
}

/*
 * __hazard_bsearch_cmp --
 *	Bsearch function: search sorted hazard list.
 */
static int
__hazard_bsearch_cmp(const void *search, const void *b)
{
	void *entry;

	entry = ((WT_HAZARD *)b)->page;

	return (search > entry ? 1 : ((search < entry) ? -1 : 0));
}
