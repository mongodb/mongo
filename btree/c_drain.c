/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008 WiredTiger Software.
 *	All rights reserved.
 *
 * $Id$
 */

#include "wt_internal.h"

static int  __wt_drain_compare_lru(const void *a, const void *b);
static int  __wt_drain_compare_page(const void *a, const void *b);
static void __wt_drain_evict(ENV *);
static void __wt_drain_hazard_check(ENV *);
static int  __wt_drain_hazard_compare(const void *a, const void *b);
static void __wt_drain_set(ENV *);
static int  __wt_drain_trickle(WT_TOC *);
static void __wt_drain_write(WT_TOC *);

#ifdef HAVE_DIAGNOSTIC
static void __wt_drain_hazard_validate(ENV *, WT_CACHE_ENTRY *);
#endif

/*
 * __wt_workq_drain_server --
 *	See if the drain server thread needs to be awakened.
 */
void
__wt_workq_drain_server(ENV *env)
{
	WT_CACHE *cache;
	u_int64_t bytes_inuse, bytes_max;

	cache = env->ienv->cache;

	/* If the cache drain server is running, there's nothing to do. */
	if (!cache->drain_sleeping)
		return;

	/*
	 * If we're locking out reads, or we're over our cache limit, cycle
	 * some of the cache content.
	 */
	bytes_inuse = __wt_cache_bytes_inuse(cache);
	bytes_max = WT_STAT(cache->stats, CACHE_BYTES_MAX);
	if (!cache->read_lockout && bytes_inuse < bytes_max)
		return;

	WT_VERBOSE(env, WT_VERB_SERVERS, (env,
	    "cache drain server running trickle: read lockout %sset, "
	    "bytes inuse > max (%llu > %llu), ",
	    cache->read_lockout ? "" : "not ",
	    (u_quad)bytes_inuse, (u_quad)bytes_max));

	/*
	 * Wake the cache drain thread; no memory flush needed, the
	 * drain_sleeping field is declared volatile.
	 */
	cache->drain_sleeping = 0;
	__wt_unlock(env, cache->mtx_drain);
}

/*
 * __wt_cache_drain_server --
 *	Thread to drain the cache.
 */
void *
__wt_cache_drain_server(void *arg)
{
	ENV *env;
	IENV *ienv;
	WT_CACHE *cache;
	WT_TOC *toc;
	u_int64_t bytes_inuse, bytes_max;
	int ret;

	env = arg;
	ienv = env->ienv;
	cache = ienv->cache;
	ret = 0;

	/* We need a thread of control because we're reading/writing pages. */
	toc = NULL;
	WT_ERR(__wt_toc_api_set(env, "CacheReconciliation", NULL, &toc));

	/*
	 * Allocate memory for a copy of the hazard references -- it's a fixed
	 * size so doesn't need run-time adjustments.
	 */
	cache->hazard_elem = env->toc_size * env->hazard_size;
	WT_ERR(__wt_calloc(
	    env, cache->hazard_elem, sizeof(WT_PAGE *), &cache->hazard));
	cache->hazard_len = cache->hazard_elem * sizeof(WT_PAGE *);

	while (F_ISSET(ienv, WT_SERVER_RUN)) {
		WT_VERBOSE(env,
		    WT_VERB_SERVERS, (env, "cache drain server sleeping"));
		cache->drain_sleeping = 1;
		__wt_lock(env, cache->mtx_drain);
		WT_VERBOSE(env,
		    WT_VERB_SERVERS, (env, "cache drain server waking"));

		for (;;) {
			/*
			 * The cache drain server is a long-running thread; its
			 * TOC must "enter" and "leave" the library periodically
			 * in order to be a good thread citizen.
			 */
			WT_TOC_GEN_SET(toc);

			/* Single-thread reconciliation. */
			__wt_lock(env, cache->mtx_reconcile);
			ret = __wt_drain_trickle(toc);
			__wt_unlock(env, cache->mtx_reconcile);

			WT_ERR(ret);
			WT_TOC_GEN_CLR(toc);

			/*
			 * If we've locked out reads, keep draining until we
			 * get to at least 5% under the maximum cache.  Else,
			 * quit draining as soon as we get under the maximum
			 * cache.
			 */
			bytes_inuse = __wt_cache_bytes_inuse(cache);
			bytes_max = WT_STAT(cache->stats, CACHE_BYTES_MAX);
			if (cache->read_lockout) {
				if (bytes_inuse <= bytes_max - (bytes_max / 20))
					break;
			} else if (bytes_inuse < bytes_max)
				break;
		}
	}
err:	if (ret != 0)
		__wt_api_env_err(env, ret, "cache drain server failure");

	if (cache->drain != NULL)
		__wt_free(env, cache->drain, cache->drain_len);
	if (cache->hazard != NULL)
		__wt_free(env, cache->hazard, cache->hazard_len);
	if (toc != NULL)
		WT_TRET(toc->close(toc, 0));

	return (NULL);
}

/*
 * __wt_drain_trickle --
 *	Select a group of pages to trickle out.
 */
static int
__wt_drain_trickle(WT_TOC *toc)
{
	ENV *env;
	WT_CACHE *cache;
	WT_CACHE_ENTRY *e, **drain;
	u_int64_t cache_pages;
	u_int32_t i, review_cnt;

	env = toc->env;
	cache = env->ienv->cache;

	/*
	 * Review 2% of the pages in the cache, but not less than 20 pages and
	 * not more than 100 pages.
	 */
	cache_pages = __wt_cache_pages_inuse(cache);
	if (cache_pages <= 20)
		review_cnt = cache_pages;
	else {
		review_cnt = cache_pages / 20;
		if (review_cnt < 20)
			review_cnt = 20;
		else if (review_cnt > 100)
			review_cnt = 100;
	}

	WT_VERBOSE(env, WT_VERB_CACHE, (env,
	    "cache drain: bucket %lu: review %lu of %llu total pages",
	    (u_long)cache->bucket_cnt,
	    (u_long)review_cnt, (u_quad)cache_pages));

	/* Allocate more space in the drain list as necessary. */
	if (review_cnt > cache->drain_len / sizeof(WT_CACHE_ENTRY *))
		WT_RET(__wt_realloc(env, &cache->drain_len,
		    (review_cnt + 20) * sizeof(WT_CACHE_ENTRY *),
		    &cache->drain));

	/*
	 * Get the next review_cnt pages available to be discarded.  There's
	 * no defense against a cache filled with pinned pages, but that's
	 * not an issue, only the root page of each database is pinned.
	 */
	for (drain = cache->drain; review_cnt > 0;) {
		if (++cache->bucket_cnt == cache->hb_size)
			cache->bucket_cnt = 0;
		for (i = WT_CACHE_ENTRY_CHUNK,
		    e = cache->hb[cache->bucket_cnt];;) {
			/* Skip empty slots and ignore pinned pages. */
			if (e->state != WT_EMPTY &&
			     !F_ISSET(e->page, WT_PINNED)) {
				*drain++ = e;
				if (--review_cnt == 0)
					break;
			}

			WT_CACHE_ENTRY_NEXT(e, i);
		}
	}
	cache->drain_elem = drain - cache->drain;

	/*
	 * Sort the pages by ascending LRU generation number, then review the
	 * WT_CACHE_DRAIN_CNT pages with the lowest generation numbers.  (I
	 * have no evidence WT_CACHE_DRAIN_CNT is the right choice, but I'm
	 * trying to amortize the cost of building and sorting the drain and
	 * hazard arrays.)
	 */
	if (cache->drain_elem > env->cache_drain_cnt) {
		qsort(cache->drain, (size_t)cache->drain_elem,
		    sizeof(WT_CACHE_ENTRY *), __wt_drain_compare_lru);
		cache->drain_elem = env->cache_drain_cnt;
	}

	/*
	 * We have the WT_CACHE_DRAIN_CNT pages we think are the least useful.
	 * Discarding pages is done in 4 steps:
	 *	Set the WT_DRAIN state
	 *	Check for any hazard references
	 *	Write any dirty pages
	 *	Discard the memory
	 */
	__wt_drain_set(env);
	__wt_drain_hazard_check(env);
	__wt_drain_write(toc);
	__wt_drain_evict(env);

	return (0);
}

/*
 * __wt_drain_set --
 *	Set the WT_DRAIN flag on a set of pages.
 */
static void
__wt_drain_set(ENV *env)
{
	WT_CACHE *cache;
	WT_CACHE_ENTRY *e, **drain;
	u_int32_t drain_elem;

	cache = env->ienv->cache;

	/*
	 * Set the entry state so readers don't try and use the pages.   Once
	 * that's done, any thread searching for a page will either see our
	 * state value, or will have already set a hazard reference to the page.
	 * We don't drain a page with a hazard reference set, so we can't race.
	 *
	 * No memory flush needed, the state field is declared volatile.
	 */
	for (drain_elem = 0, drain = cache->drain;
	    drain_elem < cache->drain_elem; ++drain, ++drain_elem) {
		if ((e = *drain) == NULL)
			continue;
		/*
		 * If we're reviewing a small cache, it's possible we entered
		 * a page onto the list twice -- catch it here, by discarding
		 * our reference to any page that's not in an "OK" state.
		 */
		if (e->state == WT_OK)
			e->state = WT_DRAIN;
		else
			*drain = NULL;
	}
}

/*
 * __wt_drain_hazard_check --
 *	Compare the list of hazard references to the list of pages to be
 *	discarded.
 */
static void
__wt_drain_hazard_check(ENV *env)
{
	IENV *ienv;
	WT_CACHE *cache;
	WT_CACHE_ENTRY **drain, *e;
	WT_PAGE **hazard, **end_hazard, *page;
	WT_STATS *stats;
	u_int32_t drain_elem;

	ienv = env->ienv;
	cache = ienv->cache;
	stats = cache->stats;

	/* Sort the drain pages by WT_PAGE address. */
	qsort(cache->drain, (size_t)cache->drain_elem,
	    sizeof(WT_CACHE_ENTRY *), __wt_drain_compare_page);

	/* Copy the hazard reference array and sort it by WT_PAGE address. */
	hazard = cache->hazard;
	end_hazard = hazard + cache->hazard_elem;
	memcpy(hazard, ienv->hazard, cache->hazard_elem * sizeof(WT_PAGE *));
	qsort(hazard, (size_t)cache->hazard_elem,
	    sizeof(WT_PAGE *), __wt_drain_hazard_compare);

	/* Walk the lists in parallel and look for matches. */
	for (drain = cache->drain, drain_elem = 0;
	    drain_elem < cache->drain_elem; ++drain, ++drain_elem) {
		if ((e = *drain) == NULL)
			continue;

		/*
		 * Look for the page in the hazard list until we reach the end
		 * of the list or find a hazard pointer larger than the page.
		 */
		for (page = e->page;
		    hazard < end_hazard && *hazard < page; ++hazard)
			;
		if (hazard == end_hazard)
			break;

		/*
		 * If we find a matching hazard reference, the page is in use:
		 * remove it from the drain list.
		 */
		if (*hazard == page) {
			WT_VERBOSE(env, WT_VERB_CACHE, (env,
			    "cache skip hazard referenced element/page/addr "
			    "%p/%p/%lu",
			    e, e->page, (u_long)e->addr));
			WT_STAT_INCR(stats, CACHE_EVICT_HAZARD);

			e->state = WT_OK;
			*drain = NULL;
		}
	}
}

/*
 * __wt_drain_write --
 *	Write any modified pages.
 */
static void
__wt_drain_write(WT_TOC *toc)
{
	ENV *env;
	WT_CACHE *cache;
	WT_CACHE_ENTRY *e, **drain;
	WT_PAGE *page;
	WT_STATS *stats;
	u_int32_t drain_elem;

	env = toc->env;
	cache = env->ienv->cache;
	stats = cache->stats;

	for (drain_elem = 0, drain = cache->drain;
	    drain_elem < cache->drain_elem; ++drain, ++drain_elem) {
		if ((e = *drain) == NULL)
			continue;

		page = e->page;
		if (!WT_PAGE_MODIFY_ISSET(page)) {
			WT_STAT_INCR(stats, CACHE_EVICT_UNMODIFIED);
			continue;
		}
		WT_STAT_INCR(stats, CACHE_EVICT_MODIFIED);

		/*
		 * We're using our WT_TOC handle, it needs to reference the
		 * correct DB handle.
		 */
		toc->db = e->db;
		(void)__wt_bt_rec_page(toc, page);
	}
}

/*
 * __wt_drain_evict --
 *	Recycle cache pages.
 */
static void
__wt_drain_evict(ENV *env)
{
	WT_CACHE *cache;
	WT_CACHE_ENTRY **drain, *e;
	WT_PAGE *page;
	u_int32_t drain_elem;

	cache = env->ienv->cache;

	for (drain = cache->drain, drain_elem = 0;
	    drain_elem < cache->drain_elem; ++drain, ++drain_elem) {
		if ((e = *drain) == NULL)
			continue;

#ifdef HAVE_DIAGNOSTIC
		__wt_drain_hazard_validate(env, e);
#endif
		WT_VERBOSE(env, WT_VERB_CACHE, (env,
		    "cache evicting element/page/addr %p/%p/%lu",
		    e, e->page, (u_long)e->addr));

		/*
		 * Copy a page reference, then make the cache entry available
		 * for re-use.
		 */
		page = e->page;
		WT_CACHE_ENTRY_CLR(e);

		WT_CACHE_PAGE_OUT(cache, page->size);

		/* The page can no longer be found, free the memory. */
		__wt_bt_page_discard(env, page);
	}
}

/*
 * __wt_drain_compare_lru --
 *	Qsort function: sort WT_CACHE_ENTRY list based on the page's generation.
 */
static int
__wt_drain_compare_lru(const void *a, const void *b)
{
	u_int32_t a_lru, b_lru;

	a_lru = (*(WT_CACHE_ENTRY **)a)->page->lru;
	b_lru = (*(WT_CACHE_ENTRY **)b)->page->lru;

	return (a_lru > b_lru ? 1 : (a_lru < b_lru ? -1 : 0));
}

/*
 * __wt_drain_compare_page --
 *	Qsort function: sort WT_CACHE_ENTRY list based on the page's address.
 */
static int
__wt_drain_compare_page(const void *a, const void *b)
{
	WT_CACHE_ENTRY *a_entry, *b_entry;
	WT_PAGE *a_page, *b_page;

	/*
	 * The array may contain entries set to NULL (for example, we found a
	 * matching hazard reference), and so can't be discarded.  Ignore them.
	 */
	a_entry = *(WT_CACHE_ENTRY **)a;
	b_entry = *(WT_CACHE_ENTRY **)b;
	if (a_entry == NULL || b_entry == NULL)
		return (a_entry == NULL &&
		    b_entry == NULL ? 0 : a_entry == NULL ? -1 : 1);


	a_page = a_entry->page;
	b_page = b_entry->page;

	return (a_page > b_page ? 1 : (a_page < b_page ? -1 : 0));
}

/*
 * __wt_drain_hazard_compare --
 *	Qsort function: sort hazard list based on the page's address.
 */
static int
__wt_drain_hazard_compare(const void *a, const void *b)
{
	WT_PAGE *a_page, *b_page;

	a_page = *(WT_PAGE **)a;
	b_page = *(WT_PAGE **)b;

	return (a_page > b_page ? 1 : (a_page < b_page ? -1 : 0));
}

#ifdef HAVE_DIAGNOSTIC
/*
 * __wt_drain_hazard_validate --
 *	Return if a page is or isn't on the hazard list.
 */
static void
__wt_drain_hazard_validate(ENV *env, WT_CACHE_ENTRY *e)
{
	IENV *ienv;
	WT_PAGE **hp;
	WT_TOC **tp, *toc;

	ienv = env->ienv;

	for (tp = ienv->toc; (toc = *tp) != NULL; ++tp)
		for (hp = toc->hazard;
		    hp < toc->hazard + toc->env->hazard_size; ++hp)
			if (*hp == e->page) {
				__wt_api_env_errx(env,
				    "hazard check for drained page %#lx/%lu "
				    "failed",
				    (u_long)e->page, (u_long)e->addr);
				__wt_abort(env);
			}
}

/*
 * __wt_drain_dump --
 *	Display the drain list.
 */
void
__wt_drain_dump(ENV *env, const char *tag)
{
	WT_CACHE *cache;
	WT_CACHE_ENTRY **drain;
	WT_MBUF mb;
	u_int32_t drain_elem;
	int sep;

	cache = env->ienv->cache;
	__wt_mb_init(env, &mb);
	__wt_mb_add(&mb, "%s", tag);

	sep = ':';
	for (drain_elem = 0, drain = cache->drain;
	    drain_elem < cache->drain_elem; ++drain, ++drain_elem) {
		__wt_mb_add(&mb, "%c %p", sep, *drain);
		sep = ',';
	}
	__wt_mb_discard(&mb);
}
#endif
