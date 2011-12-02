/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wt_internal.h"

static int  __hazard_bsearch_cmp(const void *, const void *);
static void __hazard_copy(WT_SESSION_IMPL *);
static int  __hazard_exclusive(WT_SESSION_IMPL *, WT_REF *, uint32_t);
static int  __hazard_qsort_cmp(const void *, const void *);
static int  __rec_discard_page(WT_SESSION_IMPL *, WT_PAGE *);
static int  __rec_parent_clean_update(WT_SESSION_IMPL *, WT_PAGE *);
static int  __rec_parent_dirty_update(WT_SESSION_IMPL *, WT_PAGE *, uint32_t);
static int  __rec_sub_discard(WT_SESSION_IMPL *, WT_PAGE *);
static int  __rec_sub_discard_col(WT_SESSION_IMPL *, WT_PAGE *);
static int  __rec_sub_discard_row(WT_SESSION_IMPL *, WT_PAGE *);
static int  __rec_sub_excl(WT_SESSION_IMPL *, WT_PAGE *, uint32_t);
static void __rec_sub_excl_clear(WT_SESSION_IMPL *, WT_PAGE *, uint32_t);
static int  __rec_sub_excl_col(WT_SESSION_IMPL *, WT_PAGE *, uint32_t);
static void __rec_sub_excl_col_clear(WT_SESSION_IMPL *, WT_PAGE *);
static int  __rec_sub_excl_page(
		WT_SESSION_IMPL *, WT_REF *, WT_PAGE *, uint32_t);
static int  __rec_sub_excl_row(WT_SESSION_IMPL *, WT_PAGE *, uint32_t);
static void __rec_sub_excl_row_clear(WT_SESSION_IMPL *, WT_PAGE *);

/*
 * __wt_rec_evict --
 *	Reconciliation plus eviction.
 */
int
__wt_rec_evict(WT_SESSION_IMPL *session, WT_PAGE *page, uint32_t flags)
{
	WT_CONNECTION_IMPL *conn;

	conn = S2C(session);

	WT_VERBOSE(session, evict,
	    "page %p (%s)", page, __wt_page_type_string(page->type));

	/*
	 * Get exclusive access to the page and review the page's subtree for
	 * in-memory pages that would block our eviction of the page.  If the
	 * check fails (for example, we find a child page that can't be merged),
	 * we're done.  We have to make this check for clean pages, too: while
	 * unlikely eviction would choose an internal page with children, it's
	 * possible.
	 */
	WT_RET(__rec_sub_excl(session, page, flags));

	/* If the page is dirty, write it. */
	if (__wt_page_is_modified(page))
		WT_RET(__wt_rec_write(session, page, NULL));

	/* Update the parent and discard the page. */
	if (F_ISSET(page, WT_PAGE_REC_MASK) == 0) {
		WT_STAT_INCR(conn->stats, cache_evict_unmodified);
		WT_RET(__rec_parent_clean_update(session, page));
	} else {
		WT_STAT_INCR(conn->stats, cache_evict_modified);
		WT_RET(__rec_parent_dirty_update(session, page, flags));
	}
	return (0);
}

/*
 * __rec_parent_clean_update  --
 *	Update a parent page's reference for an evicted, clean page.
 */
static int
__rec_parent_clean_update(WT_SESSION_IMPL *session, WT_PAGE *page)
{
	/*
	 * Update the relevant WT_REF structure; no memory flush is needed,
	 * the state field is declared volatile.
	 */
	page->parent_ref->page = NULL;
	page->parent_ref->state = WT_REF_DISK;

	/* Discard the page. */
	return (__rec_discard_page(session, page));
}

/*
 * __rec_parent_dirty_update --
 *	Update a parent page's reference for an evicted, dirty page.
 */
static int
__rec_parent_dirty_update(
    WT_SESSION_IMPL *session, WT_PAGE *page, uint32_t flags)
{
	WT_BTREE *btree;
	WT_PAGE *root_split;
	WT_PAGE_MODIFY *mod;
	WT_REF *parent_ref;

	btree = session->btree;
	mod = page->modify;
	parent_ref = page->parent_ref;
	root_split = NULL;

	switch (F_ISSET(page, WT_PAGE_REC_MASK)) {
	case WT_PAGE_REC_EMPTY:				/* Page is empty */
		/*
		 * Special case the root page: if the root page is empty, we
		 * reset the root address and discard the tree.
		 */
		if (WT_PAGE_IS_ROOT(page)) {
			parent_ref->addr = WT_ADDR_INVALID;
			parent_ref->page = NULL;
			/*
			 * Publish: a barrier to ensure the structure fields are
			 * set before the state change makes the page available
			 * to readers.
			 */
			WT_PUBLISH(parent_ref->state, WT_REF_DISK);
			break;
		}
		/* FALLTHROUGH */
	case WT_PAGE_REC_SPLIT_MERGE:			/* Page split */
		/*
		 * Empty pages or pages which split and were then evicted, that
		 * is, pages now waiting to be merged into their parents. We're
		 * not going to evict this page after all, instead we'll merge
		 * it into its parent when that page is evicted.  Release our
		 * exclusive reference to it, as well as any pages below it we
		 * locked down, and return it into use.
		 */
		__rec_sub_excl_clear(session, page, flags);
		return (0);
	case WT_PAGE_REC_REPLACE: 			/* 1-for-1 page swap */
		/*
		 * Special case the root page: none, we just wrote a new root
		 * page, updating the parent is all that's necessary.
		 *
		 * Update the parent to reference the replacement page.
		 */
		parent_ref->addr = mod->u.write_off.addr;
		parent_ref->size = mod->u.write_off.size;
		parent_ref->page = NULL;

		/*
		 * Publish: a barrier to ensure the structure fields are set
		 * before the state change makes the page available to readers.
		 */
		WT_PUBLISH(parent_ref->state, WT_REF_DISK);
		break;
	case WT_PAGE_REC_SPLIT:				/* Page split */
		/* Special case the root page: see below. */
		if (WT_PAGE_IS_ROOT(page)) {
			root_split = mod->u.write_split;
			break;
		}

		/*
		 * Update the parent to reference new internal page(s).
		 *
		 * Publish: a barrier to ensure the structure fields are set
		 * before the state change makes the page available to readers.
		 */
		parent_ref->page = mod->u.write_split;
		WT_PUBLISH(parent_ref->state, WT_REF_MEM);
		break;
	WT_ILLEGAL_FORMAT(session);
	}

	/*
	 * Eviction:
	 *
	 * Discard pages which were merged into this page during reconciliation,
	 * then discard the page itself.
	 */
	WT_RET(__rec_sub_discard(session, page));
	WT_RET(__rec_discard_page(session, page));

	/*
	 * Newly created internal pages are normally merged into their parent
	 * when the parent is evicted.  Newly split root pages can't be merged,
	 * they have no parent and the new root page must be written.  We also
	 * have to write the root page immediately, as the sync or close that
	 * triggered the split won't see our new root page during its traversal.
	 *
	 * We left the old root page locked and we've discarded the old root
	 * page.  Now, make the new root page look like a normal page that has
	 * been modified, write it out, update the tree's root information,
	 * and discard it.
	 */
	if (root_split != NULL) {
		WT_VERBOSE(session, evict, "split root page %p", page);

		WT_RET(__wt_page_set_modified(session, root_split));
		mod = root_split->modify;
		F_CLR(root_split, WT_PAGE_REC_MASK);

		WT_RET(__wt_rec_write(session, root_split, NULL));

		WT_ASSERT(session, F_ISSET(root_split,
		    WT_PAGE_REC_MASK) == WT_PAGE_REC_REPLACE);
		btree->root_page.addr = mod->u.write_off.addr;
		btree->root_page.size = mod->u.write_off.size;
		btree->root_page.page = NULL;

		/*
		 * Publish: a barrier to ensure the structure fields are set
		 * before the state change makes the page available to readers.
		 */
		WT_PUBLISH(parent_ref->state, WT_REF_DISK);

		WT_RET(__rec_discard_page(session, root_split));
	}

	return (0);
}

/*
 * __rec_sub_excl --
 *	Get exclusive access to a subtree for reconciliation.
 */
static int
__rec_sub_excl(WT_SESSION_IMPL *session, WT_PAGE *page, uint32_t flags)
{
	int ret;

	ret = 0;

	/*
	 * Attempt exclusive access to the page if our caller doesn't have the
	 * tree locked down.
	 */
	if (!LF_ISSET(WT_REC_SINGLE))
		WT_RET(__hazard_exclusive(session, page->parent_ref, flags));

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
	 * pages we locked while we were looking.  (I'm re-walking the tree
	 * rather than keeping track of the locked pages on purpose -- the
	 * only time this should ever fail is if eviction's LRU-based choice
	 * is very unlucky.)
	 */
	switch (page->type) {
	case WT_PAGE_COL_INT:
		ret = __rec_sub_excl_col(session, page, flags);
		break;
	case WT_PAGE_ROW_INT:
		ret = __rec_sub_excl_row(session, page, flags);
		break;
	default:
		break;
	}

	/* If can't evict this page, release our exclusive reference. */
	if (ret != 0)
		__rec_sub_excl_clear(session, page, flags);

	return (ret);
}

/*
 * __rec_sub_excl_clear --
 *     Discard exclusive access and return a page to availability.
 */
static void
__rec_sub_excl_clear(WT_SESSION_IMPL *session, WT_PAGE *page, uint32_t flags)
{
	if (LF_ISSET(WT_REC_SINGLE))
		return;

	switch (page->type) {
	case WT_PAGE_COL_INT:
		__rec_sub_excl_col_clear(session, page);
		break;
	case WT_PAGE_ROW_INT:
		__rec_sub_excl_row_clear(session, page);
		break;
	default:
		break;
	}
	page->parent_ref->state = WT_REF_MEM;
}

/*
 * __rec_sub_excl_col --
 *	Walk a column-store internal page's subtree, handling deleted and split
 *	pages.
 */
static int
__rec_sub_excl_col(WT_SESSION_IMPL *session, WT_PAGE *parent, uint32_t flags)
{
	WT_COL_REF *cref;
	WT_PAGE *page;
	uint32_t i;

	/* For each entry in the page... */
	WT_COL_REF_FOREACH(parent, cref, i) {
		switch (WT_COL_REF_STATE(cref)) {
		case WT_REF_DISK:			/* On-disk */
			continue;
		case WT_REF_LOCKED:			/* Eviction candidate */
			__wt_errx(session,
			    "subtree page locked during eviction");
			return (WT_ERROR);
		case WT_REF_MEM:			/* In-memory */
			break;
		}
		page = WT_COL_REF_PAGE(cref);

		WT_RET(__rec_sub_excl_page(session, &cref->ref, page, flags));

		/* Recurse down the tree. */
		if (page->type == WT_PAGE_COL_INT)
			WT_RET(__rec_sub_excl_col(session, page, flags));
	}
	return (0);
}

/*
 * __rec_sub_excl_col_clear --
 *	Clear any pages for which we have exclusive access -- eviction isn't
 *	possible.
 */
static void
__rec_sub_excl_col_clear(WT_SESSION_IMPL *session, WT_PAGE *parent)
{
	WT_COL_REF *cref;
	WT_PAGE *page;
	uint32_t i;

	/* For each entry in the page... */
	WT_COL_REF_FOREACH(parent, cref, i)
		if (WT_COL_REF_STATE(cref) == WT_REF_LOCKED) {
			WT_COL_REF_STATE(cref) = WT_REF_MEM;

			/* Recurse down the tree. */
			page = WT_COL_REF_PAGE(cref);
			if (page->type == WT_PAGE_COL_INT)
				__rec_sub_excl_col_clear(session, page);
		}
}

/*
 * __rec_sub_excl_row --
 *	Walk a row-store internal page's subtree, and acquiring exclusive access
 * as necessary and checking if the subtree can be evicted.
 */
static int
__rec_sub_excl_row(WT_SESSION_IMPL *session, WT_PAGE *parent, uint32_t flags)
{
	WT_PAGE *page;
	WT_ROW_REF *rref;
	uint32_t i;

	/* For each entry in the page... */
	WT_ROW_REF_FOREACH(parent, rref, i) {
		switch (WT_ROW_REF_STATE(rref)) {
		case WT_REF_DISK:			/* On-disk */
			continue;
		case WT_REF_LOCKED:			/* Eviction candidate */
			__wt_errx(session,
			    "subtree page locked during eviction");
			return (WT_ERROR);
		case WT_REF_MEM:			/* In-memory */
			break;
		}
		page = WT_ROW_REF_PAGE(rref);

		WT_RET(__rec_sub_excl_page(session, &rref->ref, page, flags));

		/* Recurse down the tree. */
		if (page->type == WT_PAGE_ROW_INT)
			WT_RET(__rec_sub_excl_row(session, page, flags));
	}
	return (0);
}

/*
 * __rec_sub_excl_row_clear --
 *	Clear any pages for which we have exclusive access -- eviction isn't
 *	possible.
 */
static void
__rec_sub_excl_row_clear(WT_SESSION_IMPL *session, WT_PAGE *parent)
{
	WT_PAGE *page;
	WT_ROW_REF *rref;
	uint32_t i;

	/* For each entry in the page... */
	WT_ROW_REF_FOREACH(parent, rref, i)
		if (WT_ROW_REF_STATE(rref) == WT_REF_LOCKED) {
			WT_ROW_REF_STATE(rref) = WT_REF_MEM;

			/* Recurse down the tree. */
			page = WT_ROW_REF_PAGE(rref);
			if (page->type == WT_PAGE_ROW_INT)
				__rec_sub_excl_row_clear(session, page);
		}
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
	 * The test is for dirty pages (which must be written before we can know
	 * what they will look like during their parent eviction), and for empty
	 * or split pages.
	 *
	 * The test is cheap, do it first.  If it passes, get exclusive access
	 * to the page if our caller doesn't have the tree locked down, and test
	 * again.
	 */
	if (__wt_page_is_modified(page) ||
	    !F_ISSET(page,
	    WT_PAGE_REC_EMPTY | WT_PAGE_REC_SPLIT | WT_PAGE_REC_SPLIT_MERGE))
		return (1);

	if (LF_ISSET(WT_REC_SINGLE))
		return (0);

	WT_RET(__hazard_exclusive(session, ref, flags));

	if (__wt_page_is_modified(page) ||
	    !F_ISSET(page,
	    WT_PAGE_REC_EMPTY | WT_PAGE_REC_SPLIT | WT_PAGE_REC_SPLIT_MERGE))
		return (1);

	return (0);
}

/*
 * __rec_sub_discard --
 *	Discard any pages merged into the evicted page.
 */
static int
__rec_sub_discard(WT_SESSION_IMPL *session, WT_PAGE *page)
{
	switch (page->type) {
	case WT_PAGE_COL_INT:
		WT_RET(__rec_sub_discard_col(session, page));
		break;
	case WT_PAGE_ROW_INT:
		WT_RET(__rec_sub_discard_row(session, page));
		break;
	default:
		break;
	}
	return (0);
}

/*
 * __rec_sub_discard_col --
 *	Discard any column-store pages we merged.
 */
static int
__rec_sub_discard_col(WT_SESSION_IMPL *session, WT_PAGE *parent)
{
	WT_COL_REF *cref;
	WT_PAGE *page;
	uint32_t i;

	/* For each entry in the page... */
	WT_COL_REF_FOREACH(parent, cref, i)
		if (WT_COL_REF_STATE(cref) != WT_REF_DISK) {
			page = WT_ROW_REF_PAGE(cref);

			/* Recurse down the tree. */
			if (page->type == WT_PAGE_COL_INT)
				WT_RET(__rec_sub_discard_col(session, page));

			WT_RET(__rec_discard_page(session, page));
		}
	return (0);
}

/*
 * __rec_sub_discard_row --
 *	Discard any row-store pages we merged.
 */
static int
__rec_sub_discard_row(WT_SESSION_IMPL *session, WT_PAGE *parent)
{
	WT_PAGE *page;
	WT_ROW_REF *rref;
	uint32_t i;

	/* For each entry in the page... */
	WT_ROW_REF_FOREACH(parent, rref, i)
		if (WT_ROW_REF_STATE(rref) != WT_REF_DISK) {
			page = WT_ROW_REF_PAGE(rref);

			/* Recurse down the tree. */
			if (page->type == WT_PAGE_ROW_INT)
				WT_RET(__rec_sub_discard_row(session, page));

			WT_RET(__rec_discard_page(session, page));
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
		WT_RET(__wt_rec_track_discard(session, page, 1));

	/* Discard the page itself. */
	__wt_page_out(session, page, 0);

	return (0);
}

/*
 * __hazard_exclusive --
 *	Request exclusive access to a page.
 */
static int
__hazard_exclusive(WT_SESSION_IMPL *session, WT_REF *ref, uint32_t flags)
{
	WT_CACHE *cache;

	cache = S2C(session)->cache;

	/*
	 * Hazard references are acquired down the tree, which means we can't
	 * deadlock.
	 *
	 * Request exclusive access to the page; no memory flush needed, the
	 * state field is declared volatile.
	 */
	ref->state = WT_REF_LOCKED;

	/* Publish the page's state before reading the hazard references. */
	WT_READ_BARRIER();
	WT_WRITE_BARRIER();

	/* Get a fresh copy of the hazard reference array. */
retry:	__hazard_copy(session);

	/* If we find a matching hazard reference, the page is still in use. */
	if (bsearch(ref->page, cache->hazard, cache->hazard_elem,
	    sizeof(WT_HAZARD), __hazard_bsearch_cmp) == NULL)
		return (0);

	WT_BSTAT_INCR(session, rec_hazard);
	if (LF_ISSET(WT_REC_WAIT)) {
		__wt_yield();
		goto retry;
	}

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
