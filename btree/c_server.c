/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008 WiredTiger Software.
 *	All rights reserved.
 *
 * $Id$
 */

#include "wt_internal.h"

static int  __wt_cache_discard_serial_func(WT_TOC *);
static int  __wt_cache_drain(
		WT_TOC *, WT_REF *, u_int32_t, WT_PAGE **, u_int32_t);
static int  __wt_cache_drain_compare_gen(const void *a, const void *b);
static int  __wt_cache_drain_compare_page(const void *a, const void *b);
static int  __wt_cache_hazard_compare(const void *a, const void *b);

#ifdef HAVE_DIAGNOSTIC
static void __wt_cache_hazard_check(ENV *, WT_PAGE *);
#endif

/*
 * __wt_cache_size_check --
 *	Check for the cache filling up.
 */
void
__wt_cache_size_check(ENV *env)
{
	IENV *ienv;
	WT_CACHE *cache;
	u_int64_t bytes_inuse, bytes_max;

	ienv = env->ienv;
	cache = &ienv->cache;

	bytes_inuse = WT_STAT(ienv->stats, CACHE_BYTES_INUSE);
	bytes_max = WT_STAT(ienv->stats, CACHE_BYTES_MAX);

	/* Wake the server if it's sleeping and we need it to run. */
	if (F_ISSET(cache, WT_SERVER_SLEEPING) && bytes_inuse > bytes_max) {
		F_CLR(cache, WT_SERVER_SLEEPING);
		WT_MEMORY_FLUSH;
		__wt_unlock(&cache->mtx);
	}

	/*
	 * If we're 10% over the maximum cache, shut out page allocations until
	 * we drain to at least 5% under the maximum cache.
	 */
	if (F_ISSET(ienv, WT_CACHE_LOCKOUT)) {
		if (bytes_inuse <= bytes_max - (bytes_max / 20)) {
			F_CLR(ienv, WT_CACHE_LOCKOUT);
			WT_MEMORY_FLUSH;
		}
	} else {
		if (bytes_inuse > bytes_max + (bytes_max / 10)) {
			F_SET(ienv, WT_CACHE_LOCKOUT);
			WT_MEMORY_FLUSH;
			WT_STAT_INCR(ienv->stats, CACHE_LOCKOUT);
		}
	}
}

static int
__wt_cache_drain_compare_page(const void *a, const void *b)
{
	WT_PAGE *a_page, *b_page;

	a_page = ((WT_REF *)a)->ref;
	b_page = ((WT_REF *)b)->ref;

	return (a_page > b_page ? 1 : (a_page < b_page ? -1 : 0));
}

static int
__wt_cache_drain_compare_gen(const void *a, const void *b)
{
	u_int32_t a_gen, b_gen;

	a_gen = ((WT_REF *)a)->gen;
	b_gen = ((WT_REF *)b)->gen;

	return (a_gen > b_gen ? 1 : (a_gen < b_gen ? -1 : 0));
}

static int
__wt_cache_hazard_compare(const void *a, const void *b)
{
	WT_PAGE *a_page, *b_page;

	a_page = *(WT_PAGE **)a;
	b_page = *(WT_PAGE **)b;

	return (a_page > b_page ? 1 : (a_page < b_page ? -1 : 0));
}

/*
 * __wt_cache_srvr --
 *	Server routine to drain the cache.
 */
void *
__wt_cache_srvr(void *arg)
{
	ENV *env;
	IENV *ienv;
	WT_CACHE *cache;
	WT_PAGE **hazard, *page;
	WT_REF *drain, *drainp;
	WT_TOC *toc;
	u_int64_t cache_pages;
	u_int32_t bucket_cnt, drain_len, drain_cnt, drain_elem, hazard_elem;
	u_int32_t review_cnt;
	int ret;

	env = arg;
	ienv = env->ienv;
	cache = &env->ienv->cache;

	hazard = NULL;
	hazard_elem = env->toc_size * env->hazard_size;
	drain = NULL;
	drain_len = 0;

	/* Create a WT_TOC so we can make serialization requests. */
	if (env->toc(env, 0, &toc) != 0)
		return (NULL);
	toc->name = "cache server";

	/* Allocate memory for a copy of the hazard references. */
	WT_ERR(__wt_calloc(env, hazard_elem, sizeof(WT_PAGE *), &hazard));

	bucket_cnt = 0;
	while (F_ISSET(ienv, WT_SERVER_RUN)) {
		/*
		 * If there's no work to do, go to sleep.  We check the workQ's
		 * cache_lockout field because the workQ wants us to be more
		 * agressive about cleaning up than just comparing the inuse
		 * bytes vs. the max bytes.
		 */
		if (!F_ISSET(ienv, WT_CACHE_LOCKOUT) &&
		    WT_STAT(ienv->stats, CACHE_BYTES_INUSE) <=
		    WT_STAT(ienv->stats, CACHE_BYTES_MAX)) {
			F_SET(cache, WT_SERVER_SLEEPING);
			WT_MEMORY_FLUSH;
			__wt_lock(env, &cache->mtx);
			continue;
		}

		/*
		 * Review 2% of the pages in the cache, with a minimum of 20
		 * pages and a maximum of 100 pages.  (I don't know if this
		 * is a reasonable configuration; the only hard rule is we
		 * can't review more pages than there are in the cache, because
		 * that could result in duplicate entries in the drain array,
		 * and that will fail.
		 */
		cache_pages = WT_STAT(ienv->stats, CACHE_PAGES);
		if (cache_pages <= 20)
			review_cnt = cache_pages;
		else {
			review_cnt = cache_pages / 20;
			if (review_cnt < 20)
				review_cnt = 20;
			else if (review_cnt > 100)
				review_cnt = 100;
		}
		if (review_cnt * sizeof(WT_REF) > drain_len)
			WT_ERR(__wt_realloc(env, &drain_len,
			    (review_cnt + 20) * sizeof(WT_REF), &drain));

		/*
		 * Copy out review_cnt pages with their generation numbers.  We
		 * copy the generation number because we're going to sort based
		 * on that value, and we don't want it changing underfoot.
		 *
		 * There's no defense against a cache filled with pinned pages,
		 * but that's not an issue, only the root page of each database
		 * is pinned.
		 */
		for (drainp = drain; review_cnt > 0; ++bucket_cnt) {
			if (bucket_cnt == cache->hash_size)
				bucket_cnt = 0;

			for (page = cache->hb[bucket_cnt];
			    page != NULL; page = page->next)
				if (!F_ISSET(page, WT_PINNED)) {
					drainp->ref = page;
					drainp->gen = page->page_gen;
					++drainp;
					if (--review_cnt == 0)
						break;
				}
		}

		/* No pages to drain: confused but done. */
		drain_elem = (u_int32_t)(drainp - drain);
		if (drain_elem == 0)
			continue;

		/* Sort the list of drain pages by their generation number. */
		qsort(drain, (size_t)drain_elem,
		    sizeof(WT_REF), __wt_cache_drain_compare_gen);

		/*
		 * Try and drain 10 pages, knowing we may not be able to drain
		 * them all.  (I have no evidence 10 is the right choice, I'm
		 * just amortizing the cost of building and sorting the drain
		 * and hazard arrays.)
		 */
#define	WT_DRAIN_CNT	10
		if (drain_elem > WT_DRAIN_CNT)
			drain_elem = WT_DRAIN_CNT;

		/* Re-sort the drain pages by their addressess. */
		qsort(drain, (size_t)drain_elem,
		    sizeof(WT_REF), __wt_cache_drain_compare_page);

		/*
		 * Mark the pages as being drained, and flush memory.
		 *
		 * Basically, any thread acquiring a page will either see our
		 * drain flags or will have already set a hazard pointer to
		 * reference the page.  Since we don't drain a page with a
		 * hazard pointer set, we won't race.  This is the core of the
		 * page draining algorithm.
		 */
		for (drain_cnt = 0; drain_cnt < drain_elem; ++drain_cnt)
			((WT_PAGE *)drain[drain_cnt].ref)->drain = 1;
		WT_MEMORY_FLUSH;

		/* Copy and sort the hazard references. */
		memcpy(hazard, ienv->hazard, hazard_elem * sizeof(WT_PAGE *));
		qsort(hazard, (size_t)hazard_elem,
		    sizeof(WT_PAGE *), __wt_cache_hazard_compare);

		/* Drain the cache. */
		if (__wt_cache_drain(
		    toc, drain, drain_elem, hazard, hazard_elem) != 0)
			break;
	}

err:	if (drain != NULL)
		__wt_free(env, drain, drain_len);
	if (hazard != NULL)
		__wt_free(env, hazard, hazard_elem * sizeof(WT_PAGE *));

	(void)toc->close(toc, 0);

	return (NULL);
}

static int
__wt_cache_drain(WT_TOC *toc, WT_REF *drain,
    u_int32_t drain_elem, WT_PAGE **hazard, u_int32_t hazard_elem)
{
	ENV *env;
	WT_PAGE **hp, *page;
	u_int32_t drain_cnt;
	int work, ret;

	env = toc->env;

	/*
	 * Both the drain and hazard lists are sorted by the page address, so
	 * we don't have to search anything, just walk the lists in parallel.
	 */
	for (work = 0,
	    hp = hazard, drain_cnt = 0; drain_cnt < drain_elem; ++drain_cnt) {
		/*
		 * Look for the page in the hazard list until we reach the end
		 * of the list or find a hazard pointer larger than the page.
		 */
		for (page = drain[drain_cnt].ref;
		    hp < hazard + hazard_elem && *hp < page; ++hp)
			;

		/*
		 * If we find a matching hazard reference, the page is in use,
		 * put it back into rotation, and remove from the drain list.
		 */
		if (hp < hazard + hazard_elem && *hp == page) {
			WT_STAT_INCR(env->ienv->stats, CACHE_HAZARD_EVICT);
			page->drain = 0;
			WT_MEMORY_FLUSH;

			drain[drain_cnt].ref = NULL;
			continue;
		}
		work = 1;

		/*
		 * Write the page if it's been modified.
		 *
		 * XXX
		 * This isn't really correct, a put can be modifying the page
		 * at the same time we're writing it.   We can allow reads, at
		 * this point, but we can't allow writes.   I intend to handle
		 * this with MVCC, so I'm leaving it alone for now.
		 */
		if (F_ISSET(page, WT_MODIFIED)) {
			WT_STAT_INCR(env->ienv->stats, CACHE_WRITE_EVICT);

			if ((ret = __wt_cache_write(env, page)) != 0) {
				__wt_api_env_err(env, ret,
				    "cache server thread unable to write page");
				return (WT_ERROR);
			}
		} else
			WT_STAT_INCR(env->ienv->stats, CACHE_EVICT);
	}

	/* No pages to drain: confused but done. */
	if (!work)
		return (0);

	/*
	 * Discard the pages (modifying the linked list requires serialization
	 * on the hash bucket), and then free the memory.  The underlying code
	 * can't fail, so we don't have any reason to track which of the pages
	 * were removed from the list and therefore should be freed, they are
	 * all freed.
	 */
	__wt_cache_discard_serial(toc, drain, drain_cnt);

	for (drain_cnt = 0; drain_cnt < drain_elem; ++drain_cnt, ++drain)
		if ((page = drain->ref) != NULL)
			__wt_bt_page_recycle(env, page);

	return (0);
}

/*
 * __wt_cache_discard_serial_func --
 *	Server version: discard a page of a file.
 */
static int
__wt_cache_discard_serial_func(WT_TOC *toc)
{
	ENV *env;
	WT_PAGE *page;
	WT_REF *drain;
	u_int32_t drain_cnt, drain_elem;

	env = toc->env;

	__wt_cache_discard_unpack(toc, drain, drain_elem);

	for (drain_cnt = 0; drain_cnt < drain_elem; ++drain_cnt, ++drain)
		if ((page = drain->ref) != NULL) {
#ifdef HAVE_DIAGNOSTIC
			__wt_cache_hazard_check(env, page);
#endif
			__wt_cache_discard(env, page);
		}
	return (0);
}

/*
 * __wt_cache_discard --
 *	Remove a page from its hash bucket.
 */
void
__wt_cache_discard(ENV *env, WT_PAGE *page)
{
	IENV *ienv;
	WT_CACHE *cache;
	WT_PAGE **hb, *tpage;

	ienv = env->ienv;

	WT_ASSERT(env, page->next != (WT_PAGE *)WT_DEBUG_BYTE);

	cache = &ienv->cache;
	WT_ASSERT(env, WT_STAT(ienv->stats, CACHE_BYTES_INUSE) >= page->bytes);
	WT_STAT_DECR(ienv->stats, CACHE_PAGES);
	WT_STAT_DECRV(ienv->stats, CACHE_BYTES_INUSE, page->bytes);

	/*
	 * Remove the page in a safe fashion, that is, without causing problems
	 * for threads walking the linked list.
	 */
	hb = &cache->hb[WT_HASH(cache, page->addr)];
	if (*hb == page)
		*hb = page->next;
	else
		for (tpage = *hb;; tpage = tpage->next)
			if (tpage->next == page) {
				tpage->next = page->next;
				break;
			}

	/*
	 * Killing the page's next pointer isn't required, because no thread
	 * should be referencing this page but us, and we're about to free
	 * the memory.   It's here to support the assert at the beginning of
	 * this function, which ensures we never discard the same page twice:
	 * that has happened in the past where there's a bug and a page gets
	 * entered into the cache more than once, or gets entered onto the
	 * drain list more than once.
	 */
	page->next = (WT_PAGE *)WT_DEBUG_BYTE;
}

#ifdef HAVE_DIAGNOSTIC
/*
 * __wt_cache_hazard_check --
 *	Return if a page is or isn't on the hazard list.
 */
static void
__wt_cache_hazard_check(ENV *env, WT_PAGE *page)
{
	IENV *ienv;
	WT_PAGE **hp;
	WT_TOC **tp, *toc;

	ienv = env->ienv;

	for (tp = ienv->toc; (toc = *tp) != NULL; ++tp)
		for (hp = toc->hazard;
		    hp < toc->hazard + toc->env->hazard_size; ++hp)
			WT_ASSERT(env, *hp != page);
}
#endif
