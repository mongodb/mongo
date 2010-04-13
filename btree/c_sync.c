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
__wt_cache_sync(
    WT_TOC *toc, void (*f)(const char *, u_int64_t), u_int32_t flags)
{
	ENV *env;
	IDB *idb;
	int ret;

	env = toc->env;
	idb = toc->db->idb;

	/*
	 * The cache drain server is the only thread of control that's
	 * allowed to write pages from the cache.  Schedule the sync.
	 */
	__wt_sync_serial(toc, f, ret);

	/* Optionally flush the file to the backing disk. */
	if (!LF_ISSET(WT_OSWRITE))
		(void)__wt_fsync(env, idb->fh);

	return (ret);
}

/*
 * __wt_sync_serial_func --
 *	Ask the I/O thread to read a page into the cache.
 */
int
__wt_sync_serial_func(WT_TOC *toc)
{
	ENV *env;
	IENV *ienv;
	WT_CACHE *cache;
	WT_SYNC_REQ *sr, *sr_end;
	void (*f)(const char *, u_int64_t);

	__wt_sync_unpack(toc, f);

	env = toc->env;
	ienv = env->ienv;
	cache = ienv->cache;

	/* Find an empty slot and enter the sync request. */
	sr = cache->sync_request;
	sr_end =
	    sr + sizeof(cache->sync_request) / sizeof(cache->sync_request[0]);
	for (; sr < sr_end; ++sr)
		if (sr->toc == NULL) {
			/*
			 * Fill in the arguments, flush memory, then the WT_TOC
			 * field that turns the slot on.
			 */
			sr->f = f;
			WT_MEMORY_FLUSH;
			sr->toc = toc;
			WT_MEMORY_FLUSH;
			return (0);
		}
	__wt_api_env_errx(env, "cache server sync request table full");
	return (WT_RESTART);
}
