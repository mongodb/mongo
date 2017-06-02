/*-
 * Copyright (c) 2014-2017 MongoDB, Inc.
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
__compact_rewrite(WT_SESSION_IMPL *session, WT_REF *ref, bool *skipp)
{
	WT_BM *bm;
	WT_DECL_RET;
	WT_MULTI *multi;
	WT_PAGE *page;
	WT_PAGE_MODIFY *mod;
	size_t addr_size;
	uint32_t i;
	const uint8_t *addr;

	*skipp = true;					/* Default skip. */

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
	 * If the page is a replacement, test the replacement addresses.
	 * Ignore empty pages, they get merged into the parent.
	 */
	if (mod == NULL || mod->rec_result == 0) {
		__wt_ref_info(ref, &addr, &addr_size, NULL);
		if (addr == NULL)
			return (0);
		return (
		    bm->compact_page_skip(bm, session, addr, addr_size, skipp));
	}

	/*
	 * The page's modification information can change underfoot if the page
	 * is being reconciled, serialize with reconciliation.
	 */
	if (mod->rec_result == WT_PM_REC_REPLACE ||
	    mod->rec_result == WT_PM_REC_MULTIBLOCK)
		WT_PAGE_LOCK(session, page);

	if (mod->rec_result == WT_PM_REC_REPLACE)
		ret = bm->compact_page_skip(bm, session,
		    mod->mod_replace.addr, mod->mod_replace.size, skipp);

	if (mod->rec_result == WT_PM_REC_MULTIBLOCK)
		for (multi = mod->mod_multi,
		    i = 0; i < mod->mod_multi_entries; ++multi, ++i) {
			if (multi->disk_image != NULL)
				continue;
			if ((ret = bm->compact_page_skip(bm, session,
			    multi->addr.addr, multi->addr.size, skipp)) != 0)
				break;
			if (!*skipp)
				break;
		}

	if (mod->rec_result == WT_PM_REC_REPLACE ||
	    mod->rec_result == WT_PM_REC_MULTIBLOCK)
		WT_PAGE_UNLOCK(session, page);

	return (ret);
}

/*
 * __wt_compact --
 *	Compact a file.
 */
int
__wt_compact(WT_SESSION_IMPL *session)
{
	WT_BM *bm;
	WT_BTREE *btree;
	WT_DECL_RET;
	WT_REF *ref;
	u_int i;
	bool skip;

	btree = S2BT(session);
	bm = btree->bm;
	ref = NULL;

	WT_STAT_DATA_INCR(session, session_compact);

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
	 * There are two ways we call reconciliation: checkpoints and eviction.
	 * Get the tree's flush lock which blocks threads writing pages for
	 * checkpoints.
	 */
	__wt_spin_lock(session, &btree->flush_lock);

	/* Walk the tree reviewing pages to see if they should be re-written. */
	for (i = 0;;) {
		/* Periodically check if we've run out of time. */
		if (++i > 100) {
			WT_ERR(__wt_session_compact_check_timeout(session));
			i = 0;
		}

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

		session->compact_state = WT_COMPACT_SUCCESS;

		/* Rewrite the page: mark the page and tree dirty. */
		WT_ERR(__wt_page_modify_init(session, ref->page));
		__wt_page_modify_set(session, ref->page);

		WT_STAT_DATA_INCR(session, btree_compact_rewrite);
	}

err:	if (ref != NULL)
		WT_TRET(__wt_page_release(session, ref, 0));

	/* Unblock threads writing leaf pages. */
	__wt_spin_unlock(session, &btree->flush_lock);

	return (ret);
}

/*
 * __wt_compact_page_skip --
 *	Return if compaction requires we read this page.
 */
int
__wt_compact_page_skip(WT_SESSION_IMPL *session, WT_REF *ref, bool *skipp)
{
	WT_BM *bm;
	WT_DECL_RET;
	size_t addr_size;
	u_int type;
	const uint8_t *addr;

	/*
	 * Skip deleted pages, rewriting them doesn't seem useful; in a better
	 * world we'd write the parent to delete the page.
	 */
	if (ref->state == WT_REF_DELETED) {
		*skipp = true;
		return (0);
	}

	*skipp = false;				/* Default to reading */

	/*
	 * If the page is in-memory, we want to look at it (it may have been
	 * modified and written, and the current location is the interesting
	 * one in terms of compaction, not the original location).
	 *
	 * This test could be combined with the next one, but this is a cheap
	 * test and the next one is expensive.
	 */
	if (ref->state != WT_REF_DISK)
		return (0);

	/*
	 * There's nothing to prevent the WT_REF state from changing underfoot,
	 * which can change its address. For example, the WT_REF address might
	 * reference an on-page cell, and page eviction can free that memory.
	 * Lock the WT_REF so we can look at its address.
	 */
	if (!__wt_atomic_casv32(&ref->state, WT_REF_DISK, WT_REF_LOCKED))
		return (0);

	/*
	 * The page is on disk, so there had better be an address; assert that
	 * fact, test at run-time to avoid the core dump.
	 *
	 * Internal pages must be read to walk the tree; ask the block-manager
	 * if it's useful to rewrite leaf pages, don't do the I/O if a rewrite
	 * won't help.
	 */
	__wt_ref_info(ref, &addr, &addr_size, &type);
	WT_ASSERT(session, addr != NULL);
	if (addr != NULL && type != WT_CELL_ADDR_INT) {
		bm = S2BT(session)->bm;
		ret = bm->compact_page_skip(
		    bm, session, addr, addr_size, skipp);
	}

	/* Reset the WT_REF state. */
	ref->state = WT_REF_DISK;

	return (ret);
}
