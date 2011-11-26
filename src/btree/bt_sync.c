/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wt_internal.h"

/*
 * __wt_btree_sync --
 *	Sync the tree.
 */
int
__wt_btree_sync(WT_SESSION_IMPL *session, const char *cfg[])
{
	WT_UNUSED(cfg);

	/*
	 * Ask the eviction thread to flush any dirty pages.
	 *
	 * Reconciliation is just another reader of the page, so it's probably
	 * possible to do this work in the current thread, rather than poking
	 * the eviction thread.  The trick is for the sync thread to acquire
	 * hazard references on each dirty page, which would block the eviction
	 * thread from attempting to evict the pages.
	 *
	 * I'm not doing it for a few reasons: (1) I don't want to update the
	 * page's read-generation number, and eviction already knows how to do
	 * that, (2) I don't want more than a single sync running at a time,
	 * and using eviction makes it work that way without additional work,
	 * (3) I don't want sync and eviction operating in the same file at
	 * the same time because I don't want sync to set the page-empty flag,
	 * have the page modified, and then have eviction see the page-empty
	 * flag, and using eviction makes it work that way without additional
	 * synchronization, (4) eventually we'll have async write I/O, and this
	 * approach results in one less page writer in the system, (5) the code
	 * already works that way.   None of these problems can't be fixed, but
	 * I don't see a reason to change at this time, either.
	 */
	return (__wt_evict_file_serial(session, 0));
}
