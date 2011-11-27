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
static int  __rec_ovfl_delete(WT_SESSION_IMPL *, WT_PAGE *);
static void __rec_parent_clean_update(WT_SESSION_IMPL *, WT_PAGE *);
static int  __rec_parent_dirty_update(WT_SESSION_IMPL *, WT_PAGE *, int);
static int  __rec_subtree(WT_SESSION_IMPL *, WT_PAGE *, uint32_t);
static int  __rec_subtree_col(WT_SESSION_IMPL *, WT_PAGE *, uint32_t);
static void __rec_subtree_col_clear(WT_SESSION_IMPL *, WT_PAGE *);
static int  __rec_subtree_row(WT_SESSION_IMPL *, WT_PAGE *, uint32_t);
static void __rec_subtree_row_clear(WT_SESSION_IMPL *, WT_PAGE *);

/*
 * __wt_rec_evict --
 *	Reconciliation plus eviction.
 */
int
__wt_rec_evict(WT_SESSION_IMPL *session, WT_PAGE *page, uint32_t flags)
{
	WT_CONNECTION_IMPL *conn;

	conn = S2C(session);

	WT_VERBOSE(session, RECONCILE,
	    "evict: addr %" PRIu32 " (%s)", WT_PADDR(page),
	    __wt_page_type_string(page->type));

	/*
	 * We're only interested in normal pages, except the root has to be
	 * evicted regardless.
	 */
	WT_ASSERT(session,
	    WT_PAGE_IS_ROOT(page) ||
	    !F_ISSET(page, WT_PAGE_REC_EMPTY | WT_PAGE_REC_SPLIT));

	/*
	 * Get exclusive access to the page and review the page's subtree for
	 * in-memory pages that would block our eviction of the page.  If the
	 * check fails (for example, we find child page that can't be merged),
	 * we're done.
	 */
	WT_RET(__rec_subtree(session, page, LF_ISSET(WT_REC_SINGLE) ? 1 : 0));

	if (page->modify == NULL) {
		/*
		 * If the page was never modified:
		 *	update the parent
		 */
		WT_STAT_INCR(conn->stats, cache_evict_unmodified);

		__rec_parent_clean_update(session, page);
	} else {
		/*
		 * If the page was ever modified:
		 *	write the page if it's currently dirty,
		 *	update the parent
		 */
		WT_STAT_INCR(conn->stats, cache_evict_modified);

		if (__wt_page_is_modified(page))
			WT_RET(__wt_rec_write(session, page, NULL));

		WT_RET(__rec_parent_dirty_update(
		    session, page, LF_ISSET(WT_REC_SINGLE) ? 1 : 0));
	}

	__wt_page_out(session, page, 0);

	return (0);
}

/*
 * __rec_parent_clean_update  --
 *	Update a parent page's reference for an evicted, clean page.
 */
static void
__rec_parent_clean_update(WT_SESSION_IMPL *session, WT_PAGE *page)
{
	WT_REF *parent_ref;

	parent_ref = page->parent_ref;

	/*
	 * If a page is on disk, it must have a valid disk address -- with
	 * one exception: if you create a root page and never use it, then
	 * it won't have a disk address.
	 */
	WT_ASSERT(session,
	    WT_PAGE_IS_ROOT(page) || parent_ref->addr != WT_ADDR_INVALID);

	/*
	 * Update the relevant WT_REF structure; no memory flush is needed,
	 * the state field is declared volatile.
	 */
	parent_ref->page = NULL;
	parent_ref->state = WT_REF_DISK;
}

/*
 * __rec_parent_dirty_update --
 *	Update a parent page's reference for an evicted, dirty page.
 */
static int
__rec_parent_dirty_update(WT_SESSION_IMPL *session, WT_PAGE *page, int single)
{
	WT_PAGE_MODIFY *mod;
	WT_REF *parent_ref;

	mod = page->modify;
	parent_ref = page->parent_ref;

	switch (F_ISSET(page, WT_PAGE_REC_MASK)) {
	case WT_PAGE_REC_EMPTY:				/* Page is empty */
		/*
		 * If the tree is empty, we will end up here with an empty root
		 * page: discard it.
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
			return (0);
		}

		/*
		 * We're not going to evict this page after all, instead we'll
		 * merge it into its parent when that page is evicted.  Release
		 * our exclusive reference to it, as well as any pages below it
		 * we locked down, and return it into use.
		 */
		if (!single) {
			switch (page->type) {
			case WT_PAGE_COL_INT:
				__rec_subtree_col_clear(session, page);
				break;
			case WT_PAGE_ROW_INT:
				__rec_subtree_row_clear(session, page);
				break;
			default:
				break;
			}
		}
		/*
		 * Publish: a barrier to ensure the structure fields are set
		 * before the state change makes the page available to readers.
		 */
		WT_PUBLISH(parent_ref->state, WT_REF_MEM);
		break;
	case WT_PAGE_REC_REPLACE: 			/* 1-for-1 page swap */
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
	default:
		/*
		 * Update the parent to reference the created internal page(s).
		 * If we're evicting the root page and there was a split of the
		 * root page, we end up here: when we wrote the internal root
		 * page, we also wrote out the created internal pages as well,
		 * and updated the root page information, which means there's
		 * no internal page, we're done.
		 *
		 * Publish: a barrier to ensure the structure fields are set
		 * before the state change makes the page available to readers.
		 */
		if (WT_PAGE_IS_ROOT(page)) {
			parent_ref->page = NULL;
			WT_PUBLISH(parent_ref->state, WT_REF_DISK);
		} else {
			parent_ref->page = mod->u.write_split;
			WT_PUBLISH(parent_ref->state, WT_REF_MEM);
		}
		break;
	}

	return (0);
}

/*
 * __rec_subtree --
 *	Get exclusive access to a subtree for reconciliation.
 */
static int
__rec_subtree(WT_SESSION_IMPL *session, WT_PAGE *page, uint32_t flags)
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
	 * Walk the page's subtree, and make sure we can reconcile this page.
	 *
	 * When reconciling a page, it may reference deleted or split pages
	 * which will be merged into the reconciled page.
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
	 *
	 * We could skip in-memory page checks when called as part of closing
	 * a file, but we have to do the rest anyway, it's not worth doing.
	 *
	 * Finally, we do some additional cleanup work during this pass: first,
	 * add any deleted or split pages to our list of pages to discard when
	 * we discard the page being reconciled; second, review deleted pages
	 * for overflow items and schedule them for deletion as well.
	 */
	switch (page->type) {
	case WT_PAGE_COL_INT:
		if ((ret = __rec_subtree_col(session, page, flags)) != 0)
			if (!LF_ISSET(WT_REC_SINGLE))
				__rec_subtree_col_clear(session, page);
		break;
	case WT_PAGE_ROW_INT:
		if ((ret = __rec_subtree_row(session, page, flags)) != 0)
			if (!LF_ISSET(WT_REC_SINGLE))
		break;
	WT_ILLEGAL_FORMAT(session);
		break;
	}

	/*
	 * If we're not going to reconcile this page, release our exclusive
	 * reference.
	 */
	if (ret != 0 && !LF_ISSET(WT_REC_SINGLE))
		page->parent_ref->state = WT_REF_MEM;

	return (ret);
}

/*
 * __rec_subtree_col --
 *	Walk a column-store internal page's subtree, handling deleted and split
 *	pages.
 */
static int
__rec_subtree_col(WT_SESSION_IMPL *session, WT_PAGE *parent, uint32_t flags)
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

		/*
		 * We found an in-memory page: if the page won't be merged into
		 * its parent, then we can't evict the top-level page.  This is
		 * not a problem, it just means we chose badly when selecting a
		 * page for eviction.
		 */
		if (!F_ISSET(page, WT_PAGE_REC_EMPTY | WT_PAGE_REC_SPLIT))
			return (1);

		/*
		 * Another thread of control could be running in this page: get
		 * exclusive access to the page if our caller doesn't have the
		 * tree locked down.
		 */
		if (!LF_ISSET(WT_REC_SINGLE))
			WT_RET(__hazard_exclusive(session, &cref->ref, flags));

		/*
		 * Split pages are always merged into the parent regardless of
		 * their contents, but a deleted page might have changed state
		 * while we waited for exclusive access.   In other words, we
		 * had exclusive access to the parent, but another thread had
		 * a hazard reference to the deleted page in the parent's tree:
		 * while we waited, that thread inserted new material, and the
		 * deleted page became an in-memory page we can't merge, it has
		 * to be reconciled on its own.
		 *
		 * We fail if this happens, we can't evict the original page.
		 */
		if (!F_ISSET(page, WT_PAGE_REC_EMPTY | WT_PAGE_REC_SPLIT))
			return (1);

		/*
		 * Overflow items on deleted pages are also discarded when
		 * reconciliation completes.
		 */
		if (F_ISSET(page, WT_PAGE_REC_EMPTY))
			switch (page->type) {
			case WT_PAGE_COL_FIX:
			case WT_PAGE_COL_INT:
				break;
			case WT_PAGE_COL_VAR:
				WT_RET(__rec_ovfl_delete(session, page));
				break;
			WT_ILLEGAL_FORMAT(session);
			}

		/* Recurse down the tree. */
		if (page->type == WT_PAGE_COL_INT)
			WT_RET(__rec_subtree_col(session, page, flags));
	}
	return (0);
}

/*
 * __rec_subtree_col_clear --
 *	Clear any pages for which we have exclusive access -- eviction isn't
 *	possible.
 */
static void
__rec_subtree_col_clear(WT_SESSION_IMPL *session, WT_PAGE *parent)
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
				__rec_subtree_col_clear(session, page);
		}
}

/*
 * __rec_subtree_row --
 *	Walk a row-store internal page's subtree, handle deleted and split
 *	pages.
 */
static int
__rec_subtree_row(WT_SESSION_IMPL *session, WT_PAGE *parent, uint32_t flags)
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

		/*
		 * We found an in-memory page: if the page won't be merged into
		 * its parent, then we can't evict the top-level page.  This is
		 * not a problem, it just means we chose badly when selecting a
		 * page for eviction.
		 */
		if (!F_ISSET(page, WT_PAGE_REC_EMPTY | WT_PAGE_REC_SPLIT))
			return (1);

		/*
		 * Another thread of control could be running in this page: get
		 * exclusive access to the page if our caller doesn't have the
		 * tree locked down.
		 */
		if (!LF_ISSET(WT_REC_SINGLE))
			WT_RET(__hazard_exclusive(session, &rref->ref, flags));

		/*
		 * Split pages are always merged into the parent regardless of
		 * their contents, but a deleted page might have changed state
		 * while we waited for exclusive access.   In other words, we
		 * had exclusive access to the parent, but another thread had
		 * a hazard reference to the deleted page in the parent's tree:
		 * while we waited, that thread inserted new material, and the
		 * deleted page became an in-memory page we can't merge, it has
		 * to be reconciled on its own.
		 *
		 * We fail if this happens, we can't evict the original page.
		 */
		if (!F_ISSET(page, WT_PAGE_REC_EMPTY | WT_PAGE_REC_SPLIT))
			return (1);

		/*
		 * Overflow items on deleted pages are also discarded when
		 * reconciliation completes.
		 */
		if (F_ISSET(page, WT_PAGE_REC_EMPTY))
			switch (page->type) {
			case WT_PAGE_ROW_INT:
			case WT_PAGE_ROW_LEAF:
				WT_RET(__rec_ovfl_delete(session, page));
				break;
			WT_ILLEGAL_FORMAT(session);
			}

		/* Recurse down the tree. */
		if (page->type == WT_PAGE_ROW_INT)
			WT_RET(__rec_subtree_row(session, page, flags));
	}
	return (0);
}

/*
 * __rec_subtree_row_clear --
 *	Clear any pages for which we have exclusive access -- eviction isn't
 *	possible.
 */
static void
__rec_subtree_row_clear(WT_SESSION_IMPL *session, WT_PAGE *parent)
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
				__rec_subtree_row_clear(session, page);
		}
}

/*
 * __rec_ovfl_delete --
 *	Walk the cells of a deleted disk page and schedule any overflow items
 * for eventual discard.
 */
static int
__rec_ovfl_delete(WT_SESSION_IMPL *session, WT_PAGE *page)
{
	WT_CELL *cell;
	WT_CELL_UNPACK *unpack, _unpack;
	WT_PAGE_DISK *dsk;
	uint32_t i;

	dsk = page->dsk;
	unpack = &_unpack;

	/*
	 * For row-internal pages, the disk image was discarded because there
	 * were no overflow items.
	 */
	if (dsk == NULL)
		return (0);

	/*
	 * We're deleting the page, which means any overflow item we ever had
	 * is deleted as well.
	 */
	WT_CELL_FOREACH(dsk, cell, unpack, i) {
		__wt_cell_unpack(cell, unpack);
		if (unpack->ovfl)
			WT_RET(__wt_rec_track(session, page, WT_PT_BLOCK,
			    NULL, unpack->off.addr, unpack->off.size));
	}

	return (0);
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

	WT_VERBOSE(session, EVICT,
	    "evict: addr %" PRIu32 " hazard request failed", ref->addr);

	/* Return the page to in-use. */
	ref->state = WT_REF_MEM;

	return (1);
}
