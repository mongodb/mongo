/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008 WiredTiger Software.
 *	All rights reserved.
 *
 * $Id$
 */

#include "wt_internal.h"

#ifdef HAVE_DIAGNOSTIC
static void __wt_cache_validate(ENV *);
#endif

/*
 * __wt_workq_cache_server --
 *	Called to check on the cache server thread when it needs to run.
 */
void
__wt_workq_cache_server(ENV *env)
{
	WT_CACHE *cache;

	cache = env->ienv->cache;

	/*
	 * Wake the cache drain thread if it's sleeping and it needs to run; no
	 * memory flush needed, the server_sleeping field is declared volatile.
	 */
	if (cache->server_sleeping) {
		WT_VERBOSE(env,
		    WT_VERB_SERVERS, (env, "workQ waking cache server"));
		cache->server_sleeping = 0;
		__wt_unlock(env, cache->mtx_server);
	}
}

/*
 * __wt_cache_server --
 *	Server thread to manage the cache.
 */
void *
__wt_cache_server(void *arg)
{
	ENV *env;
	IENV *ienv;
	WT_CACHE *cache;
	WT_TOC *toc;
	u_int64_t bytes_inuse, bytes_max;
	int didwork, read_lockout, ret;

	env = arg;
	ienv = env->ienv;
	cache = ienv->cache;
	toc = NULL;
	ret = 0;

	/*
	 * Allocate memory for a copy of the hazard references -- it's a fixed
	 * size so doesn't need run-time adjustments.
	 */
	cache->hazard_elem = env->toc_size * env->hazard_size;
	WT_ERR(__wt_calloc(
	    env, cache->hazard_elem, sizeof(WT_PAGE *), &cache->hazard));
	cache->hazard_len = cache->hazard_elem * sizeof(WT_PAGE *);

	/* We need a thread of control because we're reading/writing pages. */
	WT_ERR(__wt_toc_api_set(env, "CacheServer", NULL, &toc));

	while (F_ISSET(ienv, WT_SERVER_RUN)) {
		didwork = 0;

		/*
		 * If we're 10% over the maximum cache, shut out reads (which
		 * include page allocations) until we drain to at least 5%
		 * under the maximum cache.
		 */
		bytes_inuse = WT_STAT(cache->stats, CACHE_BYTES_INUSE);
		bytes_max = WT_STAT(cache->stats, CACHE_BYTES_MAX);
		if (read_lockout) {
			if (bytes_inuse <= bytes_max - (bytes_max / 20))
				read_lockout = 0;
		} else if (bytes_inuse > bytes_max + (bytes_max / 10)) {
			WT_VERBOSE(env, WT_VERB_CACHE, (env,
			    "bytes-inuse %llu of bytes-max %llu",
			    (u_quad)bytes_inuse, (u_quad)bytes_max));
			read_lockout = 1;
		}
		if (!read_lockout)
			WT_ERR(__wt_cache_server_read(env, &didwork));

		if (read_lockout || bytes_inuse > bytes_max) {
			WT_VERBOSE(env, WT_VERB_SERVERS, (env,
			    "cache trickle running: read lockout %sset, "
			    "bytes inuse > max (%llu > %llu), ",
			    read_lockout ? "" : "not ",
			    (u_quad)bytes_inuse, (u_quad)bytes_max));

			/*
			 * The cache server is a long-running thread; its TOC
			 * must "enter" and "leave" the library periodically
			 * in order to be a good thread citizen.
			 */
			WT_TOC_GEN_SET(toc);
			WT_ERR(__wt_drain_trickle(toc, &didwork));
			WT_TOC_GEN_CLR(toc);
		}

#ifdef HAVE_DIAGNOSTIC
		__wt_cache_validate(env);
#endif
		/*
		 * Go to sleep; no memory flush, the server_sleeping field is
		 * declared volatile.
		 */
		if (!didwork) {
			WT_VERBOSE(env, WT_VERB_SERVERS,
			    (env, "cache server sleeping"));
			cache->server_sleeping = 1;
			__wt_lock(env, cache->mtx_server);
		}
	}

err:	if (cache->drain != NULL)
		__wt_free(env, cache->drain, cache->drain_len);
	if (cache->hazard != NULL)
		__wt_free(env, cache->hazard, cache->hazard_len);
	if (toc != NULL)
		WT_TRET(toc->close(toc, 0));
	if (ret != 0)
		__wt_api_env_err(env, ret, "cache server failure");

	return (NULL);
}

#ifdef HAVE_DIAGNOSTIC
/*
 * __wt_cache_validate --
 *	Check the cache for consistency.
 */
static void
__wt_cache_validate(ENV *env)
{
	WT_CACHE *cache;
	WT_CACHE_ENTRY *e;
	uint32_t i, j;

	cache = env->ienv->cache;
	WT_CACHE_FOREACH_PAGE_ALL(cache, e, i, j)
		switch (e->state) {
		case WT_DRAIN:
			__wt_api_env_errx(env,
			    "element/page/addr %p/%p/%lu: state == WT_DRAIN",
			    e, e->page, (u_long)e->addr);
			__wt_abort(env);
			break;
		case WT_OK:
			if (e->addr == e->page->addr)
				break;
			__wt_api_env_errx(env,
			    "element/page %p/%p: e->addr != page->addr "
			    "(%lu != %lu)",
			    e, e->page, (u_long)e->addr, (u_long)e->page->addr);
			__wt_abort(env);
			/* NOTREACHED */
		}
}
#endif
