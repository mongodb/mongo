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
	WT_PAGE *page;
	u_int64_t fcnt;
	u_int32_t i, j;
	int ret;

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
		if (f != NULL && ++fcnt % 10 == 0)
			f("Db.sync", fcnt);

		if (e->db != db || e->state != WT_OK)
			continue;

		WT_VERBOSE(env, WT_VERB_CACHE,
		    (env, "cache sync flushing page %lu", (u_long)e->addr));

		/*
		 * Get a hazard reference so the page can't be discarded
		 * underfoot.
		 */
		page = e->page;
		__wt_hazard_set(toc, page);
		ret = e->db != db ||
		    e->state != WT_OK || !F_ISSET(page, WT_MODIFIED) ?
			0 : __wt_cache_write(env, db, page);
		__wt_hazard_clear(toc, page);
		if (ret != 0)
			return (ret);
	}

	return (0);
}
