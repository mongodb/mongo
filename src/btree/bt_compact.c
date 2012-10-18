/*-
 * Copyright (c) 2008-2012 WiredTiger, Inc.
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
	WT_DECL_RET;
	WT_PAGE *page;
	int skip;

	WT_UNUSED(cfg);

	/* Check if compaction might be useful. */
	WT_RET(__wt_bm_compact_skip(session, &skip));
	if (skip)
		return (0);

	/*
	 * Invoke the eviction server to review in-memory pages to see if they
	 * need to be re-written (we must use the eviction server because it's
	 * the only thread that can safely look at page reconciliation values).
	 */
	WT_RET(__wt_sync_file_serial(session, WT_SYNC_COMPACT));
	__wt_evict_server_wake(session);
	__wt_cond_wait(session, session->cond, 0);
	WT_RET(session->syncop_ret);

	/*
	 * Walk the tree reviewing all of the on-disk pages to see if they
	 * need to be re-written.
	 */
	for (page = NULL;;) {
		WT_RET(__wt_tree_walk(session, &page, WT_TREE_COMPACT));
		if (page == NULL)
			break;

		/* Mark the page and tree dirty, we want to write this page. */
		if ((ret = __wt_page_modify_init(session, page)) != 0) {
			__wt_stack_release(session, page);
			WT_RET(ret);
		}
		__wt_page_and_tree_modify_set(session, page);

		WT_BSTAT_INCR(session, file_compact_rewrite);
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
	uint32_t addr_size;
	const uint8_t *addr;

	/*
	 * There's one compaction test we do before we read the page, to see
	 * if the block-manager thinks it useful to rewrite the page.  If a
	 * rewrite won't help, we don't want to do I/O for nothing.  For that
	 * reason, this check is done in a call from inside the tree-walking
	 * routine.
	 *
	 * Ignore everything but on-disk pages, the eviction server has already
	 * done a pass over the in-memory pages.
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

	return (__wt_bm_compact_page_skip(session, addr, addr_size, skipp));
}

/*
 * __wt_compact_evict --
 *	Helper routine for the eviction thread to decide if a file's size would
 * benefit from re-writing this page.
 */
int
__wt_compact_evict(WT_SESSION_IMPL *session, WT_PAGE *page)
{
	WT_PAGE_MODIFY *mod;
	int skip;
	uint32_t addr_size;
	const uint8_t *addr;

	mod = page->modify;

	/*
	 * We're using the eviction thread in compaction because it can safely
	 * look at page reconciliation information, no pages are being evicted
	 * if the eviction is busy here.  That's not good for performance and
	 * implies compaction will impact performance, but right now it's the
	 * only way to safely look at reconciliation information.
	 *
	 * The reason we need to look at reconciliation information is that an
	 * in-memory page's original disk addresses might have been fine for
	 * compaction, but its replacement addresses might be a problem.
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
		    __wt_bm_compact_page_skip(session, addr, addr_size, &skip));
		if (skip)
			return (0);
		break;
	case WT_PM_REC_EMPTY:
		return (0);
	case WT_PM_REC_REPLACE:
		WT_RET(__wt_bm_compact_page_skip(
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
	__wt_page_and_tree_modify_set(session, page);

	WT_BSTAT_INCR(session, file_compact_rewrite);
	return (0);
}
