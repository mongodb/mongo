/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008 WiredTiger Software.
 *	All rights reserved.
 *
 * $Id$
 */

#include "wt_internal.h"

/*
 * __wt_cache_sync --
 *	Flush a database's underlying cache to disk.
 */
int
__wt_cache_sync(WT_TOC *toc, void (*f)(const char *, u_int64_t))
{
	DB *db;
	ENV *env;
	WT_CACHE *cache;
	WT_CACHE_ENTRY *e;
	u_int64_t fcnt;
	u_int32_t i, j;

	db = toc->db;
	env = toc->env;
	cache = env->ienv->cache;

	/*
	 * Write any modified pages -- if the handle is set, write only pages
	 * belonging to the specified file.
	 *
	 * We only report progress on every 10 writes, to minimize callbacks.
	 */
	WT_CACHE_FOREACH_PAGE_ALL(cache, e, i, j) {
		if (e->db != db ||
		    e->state != WT_OK || !F_ISSET(e->page, WT_MODIFIED))
			continue;

		if (f != NULL && ++fcnt % 10 == 0)
			f(toc->name, fcnt);

		WT_VERBOSE(env, WT_VERB_CACHE,
		    (env, "cache sync flushing element/page %#lx/%lu",
		     WT_PTR_TO_ULONG(e), (u_long)e->addr));

		WT_RET(__wt_bt_page_reconcile(db, e->page));
		e->state = WT_EMPTY;
	}

	/* Wrap up reporting. */
	if (f != NULL)
		f(toc->name, fcnt);

	return (0);
}
