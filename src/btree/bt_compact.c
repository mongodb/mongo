/*-
 * Copyright (c) 2014-2015 MongoDB, Inc.
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
__compact_rewrite(WT_SESSION_IMPL *session, WT_REF *ref, int *skipp)
{
	WT_BM *bm;
	WT_DECL_RET;
	WT_PAGE *page;
	WT_PAGE_MODIFY *mod;
	size_t addr_size;
	const uint8_t *addr;

	*skipp = 1;					/* Default skip. */

	bm = S2BT(session)->bm;
	page = ref->page;
	mod = page->modify;

	/*
	 * Ignore the root: it may not have a replacement address, and besides,
	 * if anything else gets written, so will it.
	 */
	if (__wt_ref_is_root(ref))
		return (0);

	/* Ignore currently dirty pages, they will be written regardless. */
	if (__wt_page_is_modified(page))
		return (0);

	/*
	 * If the page is clean, test the original addresses.
	 * If the page is a 1-to-1 replacement, test the replacement addresses.
	 * Ignore empty pages, they get merged into the parent.
	 */
	if (mod == NULL || F_ISSET(mod, WT_PM_REC_MASK) == 0) {
		WT_RET(__wt_ref_info(session, ref, &addr, &addr_size, NULL));
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
		ret = bm->compact_page_skip(bm, session,
		    mod->mod_replace.addr, mod->mod_replace.size, skipp);
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
	WT_BTREE *btree;
	WT_CONNECTION_IMPL *conn;
	WT_DECL_RET;
	WT_REF *ref;
	int block_manager_begin, skip;

	WT_UNUSED(cfg);

	conn = S2C(session);
	btree = S2BT(session);
	bm = btree->bm;
	ref = NULL;
	block_manager_begin = 0;

	WT_STAT_FAST_DATA_INCR(session, session_compact);

	/*
	 * Check if compaction might be useful -- the API layer will quit trying
	 * to compact the data source if we make no progress, set a flag if the
	 * block layer thinks compaction is possible.
	 */
	WT_RET(bm->compact_skip(bm, session, &skip));
	if (skip)
		return (0);

	/*
	 * Reviewing in-memory pages requires looking at page reconciliation
	 * results, because we care about where the page is stored now, not
	 * where the page was stored when we first read it into the cache.
	 * We need to ensure we don't race with page reconciliation as it's
	 * writing the page modify information.
	 *
	 * There are three ways we call reconciliation: checkpoints, threads
	 * writing leaf pages (usually in preparation for a checkpoint), and
	 * eviction.
	 *
	 * We're holding the schema lock which serializes with checkpoints.
	 */
	WT_ASSERT(session, F_ISSET(session, WT_SESSION_SCHEMA_LOCKED));

	/*
	 * Get the tree handle's flush lock which blocks threads writing leaf
	 * pages.
	 */
	__wt_spin_lock(session, &btree->flush_lock);

	/*
	 * That leaves eviction, we don't want to block eviction.  Set a flag
	 * so reconciliation knows compaction is running.  If reconciliation
	 * sees the flag it locks the page it's writing, we acquire the same
	 * lock when reading the page's modify information, serializing access.
	 * The same page lock blocks work on the page, but compaction is an
	 * uncommon, heavy-weight operation.  If it's ever a problem, there's
	 * no reason we couldn't use an entirely separate lock than the page
	 * lock.
	 *
	 * We also need to ensure we don't race with an on-going reconciliation.
	 * After we set the flag, wait for eviction of this file to drain, and
	 * then let eviction continue;
	 */
	conn->compact_in_memory_pass = 1;
	WT_ERR(__wt_evict_file_exclusive_on(session));
	__wt_evict_file_exclusive_off(session);

	/* Start compaction. */
	WT_ERR(bm->compact_start(bm, session));
	block_manager_begin = 1;

	/* Walk the tree reviewing pages to see if they should be re-written. */
	session->compaction = 1;
	for (;;) {
		/*
		 * Pages read for compaction aren't "useful"; don't update the
		 * read generation of pages already in memory, and if a page is
		 * read, set its generation to a low value so it is evicted
		 * quickly.
		 */
		WT_ERR(__wt_tree_walk(session, &ref,
		    WT_READ_COMPACT | WT_READ_NO_GEN | WT_READ_WONT_NEED));
		if (ref == NULL)
			break;

		WT_ERR(__compact_rewrite(session, ref, &skip));
		if (skip)
			continue;

		/* Rewrite the page: mark the page and tree dirty. */
		WT_ERR(__wt_page_modify_init(session, ref->page));
		__wt_page_modify_set(session, ref->page);

		WT_STAT_FAST_DATA_INCR(session, btree_compact_rewrite);
	}

err:	if (ref != NULL)
		WT_TRET(__wt_page_release(session, ref, 0));

	if (block_manager_begin)
		WT_TRET(bm->compact_end(bm, session));

	__wt_spin_unlock(session, &btree->flush_lock);

	conn->compact_in_memory_pass = 0;
	WT_FULL_BARRIER();

	return (ret);
}

/*
 * __wt_compact_page_skip --
 *	Return if compaction requires we read this page.
 */
int
__wt_compact_page_skip(WT_SESSION_IMPL *session, WT_REF *ref, int *skipp)
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
	WT_RET(__wt_ref_info(session, ref, &addr, &addr_size, &type));
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
