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

	WT_RET(__wt_bm_compact_skip(session, &skip));
	if (skip)
		return (0);

	for (page = NULL;;) {
		WT_RET(__wt_tree_walk(session, &page, WT_TREE_COMPACT));
		if (page == NULL)
			break;

		/* If the page is already dirty, skip some work. */
		if (__wt_page_is_modified(page))
			continue;

		/*
		 * Reconciliation structures can be modified while the page is
		 * being read, and we need to be careful about looking inside
		 * them: for example, if the page is being replaced, we'd look
		 * at the replacement address to decide if the page needs to be
		 * re-written, not the original disk address.  However, if the
		 * page were splitting, the replacement address is changing,
		 * we don't have any kind of lock on that information.  We do
		 * ignore pages we're going to discard anyway (expected after
		 * a large delete), nothing involved in that test can move
		 * underfoot, and the worst a race can cause is we incorrectly
		 * re-write (or don't re-write), a page.
		 */
		if (page->modify != NULL &&
		    F_ISSET(page->modify, WT_PM_REC_EMPTY))
			continue;

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
	 *	Test using the original disk address of the page.  That isn't
	 * necessarily the best choice (if the page has been reconciled and
	 * replaced, the replacement address is a better choice), but we don't
	 * even have the page pinned, the original address is all we have).
	 */
	__wt_get_addr(parent, ref, &addr, &addr_size);
	if (addr == NULL) {
		*skipp = 1;
		return (0);
	}

	return (__wt_bm_compact_page_skip(session, addr, addr_size, skipp));
}
