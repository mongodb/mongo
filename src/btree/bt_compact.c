/*-
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __compact_rewrite --
 *	Return if a page needs to be re-written.
 */
static int
__compact_rewrite(WT_SESSION_IMPL *session, WT_PAGE *page, int *skipp)
{
	WT_BM *bm;
	WT_DECL_RET;
	WT_PAGE_MODIFY *mod;
	size_t addr_size;
	const uint8_t *addr;

	*skipp = 1;					/* Default skip. */

	bm = S2BT(session)->bm;
	mod = page->modify;

	/*
	 * Ignore the root: it may not have a replacement address, and besides,
	 * if anything else gets written, so will it.
	 */
	if (WT_PAGE_IS_ROOT(page))
		return (0);

	/* Ignore currently dirty pages, they will be written regardless. */
	if (__wt_page_is_modified(page))
		return (0);

	/*
	 * If the page is clean, test the original addresses.
	 * If the page is a 1-to-1 replacement, test the replacement addresses.
	 * Ignore split and empty pages, they get merged into the parent.
	 */
	if (mod == NULL || F_ISSET(mod, WT_PM_REC_MASK) == 0) {
		WT_RET(__wt_ref_info(session,
		    page->parent, page->ref, &addr, &addr_size, NULL));
		if (addr == NULL)
			return (0);
		WT_RET(
		    bm->compact_page_skip(bm, session, addr, addr_size, skipp));
	} else if (F_ISSET(mod, WT_PM_REC_MASK) == WT_PM_REC_REPLACE) {
		/*
		 * The page's modification information can change underfoot if
		 * the page is being reconciled, lock the page down.
		 */
		WT_PAGE_LOCK(session, page);
		ret = bm->compact_page_skip(bm,
		    session, mod->u.replace.addr, mod->u.replace.size, skipp);
		WT_PAGE_UNLOCK(session, page);
		WT_RET(ret);
	}
	return (0);
}

/*
 * __wt_compact --
 *	Compact a file.
 */
int
__wt_compact(WT_SESSION_IMPL *session, const char *cfg[])
{
	WT_BM *bm;
	WT_CONNECTION_IMPL *conn;
	WT_DECL_RET;
	WT_PAGE *page;
	int skip;

	WT_UNUSED(cfg);

	bm = S2BT(session)->bm;
	conn = S2C(session);

	WT_STAT_FAST_DATA_INCR(session, session_compact);

	/*
	 * Check if compaction might be useful -- the API layer will quit trying
	 * to compact the data source if we make no progress, set a flag if the
	 * block layer thinks compaction is possible.
	 */
	WT_RET(bm->compact_skip(bm, session, &skip));
	if (skip)
		return (0);
	session->compaction = 1;

	/*
	 * Walk the tree reviewing pages to see if they need to be re-written.
	 * Reviewing in-memory pages requires looking at page reconciliation
	 * results, because we care about where the page is stored now, not
	 * where the page was stored when we first read it into the cache.
	 *	We need to ensure we don't collide with page reconciliation as
	 * it writes the page modify information, which means ensuring we don't
	 * collide with page eviction or checkpoints, they are the operations
	 * that reconcile pages.
	 *	Lock out checkpoints and set a flag so reconciliation knows
	 * compaction is running.  This means checkpoints block compaction, but
	 * compaction won't block checkpoints for more than a few instructions.
	 *	Reconciliation looks for the flag: if set, reconciliation locks
	 * the page it's writing.  That's bad, because a page lock blocks work
	 * on the page, but compaction is an uncommon, heavy-weight operation,
	 * we're doing a ton of I/O, spinlocks aren't our primary concern.
	 *	After we set the flag, wait for eviction of this file to drain,
	 * and then let eviction continue; we don't need to block eviction, we
	 * just need to make sure we don't race with reconciliation.
	 */
	__wt_spin_lock(session, &conn->checkpoint_lock);
	conn->compact_in_memory_pass = 1;
	__wt_spin_unlock(session, &conn->checkpoint_lock);
	__wt_evict_file_exclusive_on(session);
	__wt_evict_file_exclusive_off(session);

	/* Start compaction. */
	WT_RET(bm->compact_start(bm, session));

	/* Walk the tree. */
	for (page = NULL;;) {
		WT_ERR(__wt_tree_walk(session, &page, WT_TREE_COMPACT));
		if (page == NULL)
			break;

		WT_ERR(__compact_rewrite(session, page, &skip));
		if (skip)
			continue;

		/* Rewrite the page: mark the page and tree dirty. */
		WT_ERR(__wt_page_modify_init(session, page));
		__wt_page_modify_set(session, page);

		WT_STAT_FAST_DATA_INCR(session, btree_compact_rewrite);
	}

err:	if (page != NULL)
		WT_TRET(__wt_page_release(session, page));

	WT_TRET(bm->compact_end(bm, session));

	conn->compact_in_memory_pass = 0;
	WT_FULL_BARRIER();

	return (0);
}

/*
 * __wt_compact_page_skip --
 *	Return if compaction requires we read this page.
 */
int
__wt_compact_page_skip(
    WT_SESSION_IMPL *session, WT_PAGE *parent, WT_REF *ref, int *skipp)
{
	WT_BM *bm;
	size_t addr_size;
	u_int type;
	const uint8_t *addr;

	*skipp = 0;				/* Default to reading. */
	type = 0;				/* Keep compiler quiet. */

	bm = S2BT(session)->bm;

	/*
	 * We aren't holding a hazard pointer, so we can't look at the page
	 * itself, all we can look at is the WT_REF information.  If there's no
	 * address, the page isn't on disk, but we have to read internal pages
	 * to walk the tree regardless; throw up our hands and read it.
	 */
	WT_RET(__wt_ref_info(session, parent, ref, &addr, &addr_size, &type));
	if (addr == NULL)
		return (0);

	/*
	 * Internal pages must be read to walk the tree; ask the block-manager
	 * if it's useful to rewrite leaf pages, don't do the I/O if a rewrite
	 * won't help.
	 */
	return (type == WT_CELL_ADDR_INT ? 0 :
	    bm->compact_page_skip(bm, session, addr, addr_size, skipp));
}
