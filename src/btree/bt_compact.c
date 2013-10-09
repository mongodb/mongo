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
	WT_CONFIG_ITEM cval;
	WT_DECL_RET;
	WT_PAGE *page;
	int trigger, skip;

	bm = S2BT(session)->bm;

	WT_DSTAT_INCR(session, session_compact);

	WT_RET(__wt_config_gets(session, cfg, "trigger", &cval));
	trigger = (int)cval.val;

	/* Check if compaction might be useful. */
	WT_RET(bm->compact_skip(bm, session, trigger, &skip));
	if (skip)
		return (0);

	/*
	 * Walk the cache reviewing in-memory pages to see if they need to be
	 * re-written.  This requires looking at page reconciliation results,
	 * which means the page cannot be reconciled at the same time as it's
	 * being reviewed for compaction.  The underlying functions ensure we
	 * don't collide with page eviction, but we need to make sure we don't
	 * collide with checkpoints either, they are the other operation that
	 * can reconcile a page.
	 */
	__wt_spin_lock(session, &S2C(session)->checkpoint_lock);
	WT_RET(__wt_bt_cache_op(session, NULL, WT_SYNC_COMPACT));
	__wt_spin_unlock(session, &S2C(session)->checkpoint_lock);

	/*
	 * Walk the tree, reviewing on-disk pages to see if they need to be
	 * re-written.
	 */
	for (page = NULL;;) {
		WT_RET(__wt_tree_walk(session, &page, WT_TREE_COMPACT));
		if (page == NULL)
			break;

		/*
		 * The only pages returned by the tree walk function are pages
		 * we want to re-write; mark the page and tree dirty.
		 */
		if ((ret = __wt_page_modify_init(session, page)) != 0) {
			WT_TRET(__wt_page_release(session, page));
			WT_RET(ret);
		}
		__wt_page_modify_set(session, page);

		WT_DSTAT_INCR(session, btree_compact_rewrite);
	}

	return (0);
}

/*
 * __wt_compact_page_skip --
 *	Return if the block-manager wants us to re-write this page.
 */
int
__wt_compact_page_skip(
    WT_SESSION_IMPL *session, WT_PAGE *parent, WT_REF *ref, int *skipp)
{
	WT_BM *bm;
	uint32_t addr_size;
	const uint8_t *addr;

	bm = S2BT(session)->bm;

	/*
	 * There's one compaction test we do before we read the page, to see
	 * if the block-manager thinks it useful to rewrite the page.  If a
	 * rewrite won't help, we don't want to do I/O for nothing.  For that
	 * reason, this check is done in a call from inside the tree-walking
	 * routine.
	 *
	 * Ignore everything but on-disk pages, we've already done a pass over
	 * the in-memory pages.
	 */
	if (ref->state != WT_REF_DISK) {
		*skipp = 1;
		return (0);
	}

	__wt_get_addr(parent, ref, &addr, &addr_size);
	if (addr == NULL) {
		*skipp = 1;
		return (0);
	}

	return (bm->compact_page_skip(bm, session, addr, addr_size, skipp));
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
	int skip;
	uint32_t addr_size;
	const uint8_t *addr;

	bm = S2BT(session)->bm;
	mod = page->modify;

	/*
	 * We have to review page reconciliation information as an in-memory
	 * page's original disk addresses might have been fine for compaction
	 * but its replacement addresses might be a problem.  To review page
	 * reconciliation information, we have to lock out both eviction and
	 * checkpoints, as those are the other two operations that can write
	 * a page.
	 *
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
	 * If the page is a split, ignore it, it will be merged into the parent.
	 */
	if (mod == NULL)
		goto disk;

	switch (F_ISSET(mod, WT_PM_REC_MASK)) {
	case 0:
disk:		__wt_get_addr(page->parent, page->ref, &addr, &addr_size);
		if (addr == NULL)
			return (0);
		WT_RET(
		    bm->compact_page_skip(bm, session, addr, addr_size, &skip));
		if (skip)
			return (0);
		break;
	case WT_PM_REC_EMPTY:
		return (0);
	case WT_PM_REC_REPLACE:
		WT_RET(bm->compact_page_skip(bm,
		    session, mod->u.replace.addr, mod->u.replace.size, &skip));
		if (skip)
			return (0);
		break;
	case WT_PM_REC_SPLIT:
	case WT_PM_REC_SPLIT_MERGE:
		return (0);
	}

	/* Mark the page and tree dirty, we want to write this page. */
	WT_RET(__wt_page_modify_init(session, page));
	__wt_page_modify_set(session, page);

	WT_DSTAT_INCR(session, btree_compact_rewrite);
	return (0);
}
