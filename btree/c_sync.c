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
	WT_PAGE *page;
	u_int64_t fcnt;
	u_int i;

	db = toc->db;
	env = toc->env;
	cache = &env->ienv->cache;

	/*
	 * Write any modified pages -- if the handle is set, write only pages
	 * belonging to the specified file.
	 *
	 * We only report progress on every 10 writes, to minimize callbacks.
	 */
	for (i = 0, fcnt = 0; i < cache->hash_size; ++i) {
retry:		for (page = cache->hb[i]; page != NULL; page = page->next) {
			if (page->db != db || !F_ISSET(page, WT_MODIFIED))
				continue;

			/*
			 * Get a hazard reference so the page can't be discarded
			 * underfoot.
			 */
			__wt_hazard_set(toc, page);
			if (page->drain) {
				__wt_hazard_clear(toc, page);
				__wt_sleep(0, 100000);
				goto retry;
			}

			WT_RET(__wt_cache_write(env, page));
			__wt_hazard_clear(toc, page);

			if (f != NULL && ++fcnt % 10 == 0)
				f("Db.sync", fcnt);
		}
	}
	return (0);
}
