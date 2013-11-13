/*-
 * Copyright (c) 2008-2013 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

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
	uint32_t addr_size;
	int skip;
	const uint8_t *addr;

	WT_UNUSED(cfg);

	bm = S2BT(session)->bm;
	conn = S2C(session);
	page = NULL;

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

	/* Start compaction. */
	WT_RET(bm->compact_start(bm, session));

	/*
	 * Walk the cache reviewing in-memory pages to see if they need to be
	 * re-written, which requires looking at page reconciliation results.
	 * The underlying functions ensure we don't collide with page eviction,
	 * but we need to make sure we don't collide with checkpoints either,
	 * they are the other operation that can reconcile a page.
	 *	Lock out checkpoints and set a flag so reconciliation knows
	 * compaction is running.  This means checkpoints block compaction, but
	 * compaction won't block checkpoints for more than a few instructions.
	 *	Reconciliation looks for the flag: if set, reconciliation locks
	 * the page it's writing.  That's bad, because a page lock blocks work
	 * on the page, but compaction is an uncommon, heavy-weight operation,
	 * we're probably doing a ton of I/O, spinlocks aren't our big concern.
	 */
	__wt_spin_lock(session, &conn->checkpoint_lock);
	conn->compact_in_memory_pass = 1;
	__wt_spin_unlock(session, &conn->checkpoint_lock);
	ret = __wt_bt_cache_op(session, NULL, WT_SYNC_COMPACT);
	conn->compact_in_memory_pass = 0;
	WT_FULL_BARRIER();
	WT_ERR(ret);

	/*
	 * Walk the tree, reviewing on-disk pages to see if they need to be
	 * re-written.
	 */
	for (page = NULL;;) {
		WT_ERR(__wt_tree_walk(session, &page, WT_TREE_COMPACT));
		if (page == NULL)
			break;

		/*
		 * We may be visiting a page that doesn't need to be re-written.
		 * (We have to visit all of the internal pages in order to walk
		 * the tree, both those needing to be rewritten for compaction
		 * and those that don't.  Leave open the possibility a page of
		 * a different type had no address but yet wasn't in-memory, it
		 * keeps the code simpler and there's no guarantee it couldn't
		 * happen.)  If we can't find an address for the page ignore it,
		 * it can't be a factor in compaction.
		 */
		if (WT_PAGE_IS_ROOT(page))
			continue;

		WT_ERR(__wt_ref_info(session,
		    page->parent, page->ref, &addr, &addr_size, NULL));
		if (addr == NULL)
			continue;
		WT_ERR(
		    bm->compact_page_skip(bm, session, addr, addr_size, &skip));
		if (skip)
			continue;

		WT_ERR(__wt_page_modify_init(session, page));
		__wt_page_modify_set(session, page);

		WT_STAT_FAST_DATA_INCR(session, btree_compact_rewrite);
	}

err:	if (page != NULL)
		WT_TRET(__wt_page_release(session, page));

	WT_TRET(bm->compact_end(bm, session));
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
	uint32_t addr_size;
	u_int type;
	const uint8_t *addr;

	*skipp = 0;				/* Default to reading. */

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

/*
 * __wt_compact_evict --
 *	Helper routine to decide if a file's size would benefit from re-writing
 * this page.
 */
int
__wt_compact_evict(WT_SESSION_IMPL *session, WT_PAGE *page)
{
	WT_BM *bm;
	WT_PAGE_MODIFY *mod;
	uint32_t addr_size;
	int skip;
	const uint8_t *addr;

	bm = S2BT(session)->bm;
	mod = page->modify;

	/*
	 * Ignore the root: it may not have a replacement address, and besides,
	 * if anything else gets written, so will it.
	 */
	if (WT_PAGE_IS_ROOT(page))
		return (0);

	/*
	 * If the page is already dirty, skip some work, it will be written in
	 * any case.
	 */
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
		    bm->compact_page_skip(bm, session, addr, addr_size, &skip));
		if (skip)
			return (0);
	} else if (F_ISSET(mod, WT_PM_REC_MASK) == WT_PM_REC_REPLACE) {
		/*
		 * The page's modification information can change underfoot if
		 * the page is being reconciled, lock the page down.
		 */
		WT_PAGE_LOCK(session, page);
		ret = bm->compact_page_skip(bm,
		    session, mod->u.replace.addr, mod->u.replace.size, &skip));
		WT_PAGE_UNLOCK(session, page);
		WT_RET(ret):
		if (skip)
			return (0);
	}

	/* Mark the page and tree dirty, we want to write this page. */
	WT_RET(__wt_page_modify_init(session, page));
	__wt_page_modify_set(session, page);

	WT_STAT_FAST_DATA_INCR(session, btree_compact_rewrite);
	return (0);
}
