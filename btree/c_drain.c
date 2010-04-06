/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008 WiredTiger Software.
 *	All rights reserved.
 *
 * $Id$
 */

#include "wt_internal.h"

static int __wt_cache_compare_gen(const void *a, const void *b);
static int __wt_cache_compare_page(const void *a, const void *b);
static int __wt_cache_hazard_compare(const void *a, const void *b);
static int __wt_cache_hazchk(
	       ENV *, WT_CACHE_ENTRY **, u_int32_t, WT_PAGE **, u_int32_t);
static int __wt_cache_recycle(ENV *, WT_CACHE_ENTRY **, u_int32_t);
static int __wt_cache_wmod(ENV *, WT_CACHE_ENTRY **, u_int32_t);

#ifdef HAVE_DIAGNOSTIC
static void __wt_cache_hazard_check(ENV *, WT_CACHE_ENTRY *);
#endif

/*
 * __wt_cache_compare_gen --
 *	Qsort function: sort WT_CACHE_ENTRY list based on the page's generation.
 */
static int
__wt_cache_compare_gen(const void *a, const void *b)
{
	u_int32_t a_gen, b_gen;

	a_gen = (*(WT_CACHE_ENTRY **)a)->gen;
	b_gen = (*(WT_CACHE_ENTRY **)b)->gen;

	return (a_gen > b_gen ? 1 : (a_gen < b_gen ? -1 : 0));
}

/*
 * __wt_cache_compare_page --
 *	Qsort function: sort WT_CACHE_ENTRY list based on the page's address.
 */
static int
__wt_cache_compare_page(const void *a, const void *b)
{
	WT_PAGE *a_page, *b_page;

	a_page = (*(WT_CACHE_ENTRY **)a)->page;
	b_page = (*(WT_CACHE_ENTRY **)b)->page;

	return (a_page > b_page ? 1 : (a_page < b_page ? -1 : 0));
}

/*
 * __wt_cache_hazard_compare --
 *	Qsort function: sort hazard list based on the page's address.
 */
static int
__wt_cache_hazard_compare(const void *a, const void *b)
{
	WT_PAGE *a_page, *b_page;

	a_page = *(WT_PAGE **)a;
	b_page = *(WT_PAGE **)b;

	return (a_page > b_page ? 1 : (a_page < b_page ? -1 : 0));
}

/*
 * __wt_cache_drain --
 *	Server thread to drain the cache.
 */
void *
__wt_cache_drain(void *arg)
{
	ENV *env;
	IENV *ienv;
	WT_CACHE *cache;
	WT_CACHE_ENTRY *e, **drain, **drainp;
	WT_PAGE **hazard;
	u_int64_t bytes_inuse, bytes_max, cache_pages;
	u_int32_t bucket_cnt, drain_len, drain_cnt, drain_elem;
	u_int32_t hazard_elem, i, review_cnt;
	int ret;

	env = arg;
	ienv = env->ienv;
	cache = ienv->cache;

	/* Allocate memory for a copy of the hazard references. */
	hazard_elem = env->toc_size * env->hazard_size;
	WT_ERR(__wt_calloc(env, hazard_elem, sizeof(WT_PAGE *), &hazard));

	/* Initialize the drain array (which is reallocated on demand). */
	drain = NULL;
	drain_len = 0;

	/* Initialize the bucket count, it cycles through the hash buckets. */
	bucket_cnt = 0;

	while (F_ISSET(ienv, WT_SERVER_RUN)) {
		/*
		 * If there's no work to do, go to sleep.  We check the cache
		 * lockout flag because it implies being more aggressive about
		 * cleaning than only comparing the in-use bytes vs. the max
		 * bytes.
		 */
		bytes_inuse = WT_CACHE_BYTES_INUSE(cache);
		bytes_max = WT_STAT(ienv->stats, CACHE_BYTES_MAX);
		if (cache->read_lockout == 0 && bytes_inuse <= bytes_max) {
			/*
			 * No memory flush needed, the drain_sleeping field is
			 * declared volatile.
			 */
			cache->drain_sleeping = 1;
			__wt_lock(env, cache->mtx_drain);
			cache->drain_sleeping = 0;
			continue;
		}

		/*
		 * Lock the hash buckets, we can't grow them while the cache
		 * drain server has them pinned (that is, has references to
		 * pages in the bucket).
		 */
		__wt_lock(env, cache->mtx_hb);

		WT_VERBOSE(env,
		    WT_VERB_CACHE | WT_VERB_SERVERS, (env,
		        "cache drain server running: bytes inuse > max "
			"(%llu > %llu), read lockout %sset",
			bytes_inuse, bytes_max,
			cache->read_lockout ? "" : "not "));

		/*
		 * Review 2% of the pages in the cache, but not less than 20
		 * pages and not more than 100 pages.  (Not sure this is an OK
		 * configuration; the only hard rule is we can't review more
		 * pages than there are in the cache, because that would result
		 * in duplicate entries in the drain array, and that will fail.)
		 */
		cache_pages = WT_CACHE_PAGES_INUSE(cache);
		if (cache_pages <= 20)
			review_cnt = cache_pages;
		else {
			review_cnt = cache_pages / 20;
			if (review_cnt < 20)
				review_cnt = 20;
			else if (review_cnt > 100)
				review_cnt = 100;
		}
		if (review_cnt * sizeof(WT_CACHE_ENTRY *) > drain_len)
			WT_ERR(__wt_realloc(env, &drain_len,
			    (review_cnt + 20) * sizeof(WT_CACHE_ENTRY *),
			    &drain));

		WT_VERBOSE(env, WT_VERB_CACHE, (env,
		    "cache drain server reviewing %lu entries of total "
		    "%lu cache pages",
		    (u_long)review_cnt, (u_long)cache_pages));

		/*
		 * Get the next review_cnt pages available to be discarded.
		 * There's no defense against a cache filled with pinned pages,
		 * but that's not an issue, only the root page of each database
		 * is pinned.
		 */
		for (drainp = drain; review_cnt > 0; ++bucket_cnt) {
			if (bucket_cnt == cache->hb_size)
				bucket_cnt = 0;
			WT_CACHE_FOREACH_PAGE(
			    cache, &cache->hb[bucket_cnt], e, i) {
				/* Skip pages that aren't ours to take. */
				if (e->state != WT_OK ||
				    F_ISSET(e->page, WT_PINNED))
					continue;
				/*
				 * If the page is marked as useless, clear its
				 * generation number so we'll take it.
				 */
				if (F_ISSET(e->page, WT_DISCARD))
					e->gen = 0;
				*drainp++ = e;
				if (--review_cnt == 0)
					break;
			}
		}

		/* No pages to drain: confused but done. */
		drain_elem = (u_int32_t)(drainp - drain);
		if (drain_elem == 0)
			goto done;

		/* Sort the drain pages by ascending generation number. */
		qsort(drain, (size_t)drain_elem,
		    sizeof(WT_CACHE_ENTRY *), __wt_cache_compare_gen);

		/*
		 * Try and drain 10 pages, knowing we may not be able to drain
		 * them all.  (I have no evidence 10 is the right choice, I'm
		 * just amortizing the cost of building and sorting the drain
		 * and hazard arrays.)
		 */
#define	WT_DRAIN_CNT	10
		if (drain_elem > WT_DRAIN_CNT)
			drain_elem = WT_DRAIN_CNT;

		/* Re-sort the drain pages by their WT_PAGE addresses. */
		qsort(drain, (size_t)drain_elem,
		    sizeof(WT_CACHE_ENTRY *), __wt_cache_compare_page);

		/*
		 * Set the entry state so readers don't try and use the pages.
		 * Now, any thread searching for a page will either see our
		 * state value, or will have already set a hazard reference to
		 * the page.  We don't drain a page with a hazard reference
		 * set, so we won't race.
		 *
		 * No memory flush needed, the state field is declared volatile.
		 */
		for (drainp = drain, drain_cnt = 0;
		    drain_cnt < drain_elem; ++drainp, ++drain_cnt) {
			/*
			 * If we're reviewing a small cache, it's possible that
			 * we entered a page onto the list twice -- we catch it
			 * here, by discarding any reference to a page that's
			 * not in an "OK" state.
			 */
			if ((*drainp)->state != WT_OK) {
				*drainp = NULL;
				continue;
			}

			(*drainp)->state = WT_DRAIN;
			WT_VERBOSE(env, WT_VERB_CACHE, (env,
			    "cache drain server draining element/page "
			    "%#lx/%#lu",
			    WT_PTR_TO_ULONG(*drainp), (u_long)(*drainp)->addr));
		}

		/* Copy and sort the hazard references. */
		memcpy(hazard, ienv->hazard, hazard_elem * sizeof(WT_PAGE *));
		qsort(hazard, (size_t)hazard_elem,
		    sizeof(WT_PAGE *), __wt_cache_hazard_compare);

		/*
		 * Compare the list of hazard references to the list of pages
		 * to be discarded.   Any matches will set the WT_CACHE_ENTRY
		 * reference to NULL.
		 */
		WT_ERR(__wt_cache_hazchk(
		    env, drain, drain_elem, hazard, hazard_elem));

		/* Flush any modified pages. */
		WT_ERR(__wt_cache_wmod(env, drain, drain_elem));

		/* Recycle the  memory. */
		WT_ERR(__wt_cache_recycle(env, drain, drain_elem));

done:		__wt_unlock(cache->mtx_hb);

#ifdef HAVE_DIAGNOSTIC
		__wt_cache_chk(env);
#endif
	}

err:	if (drain != NULL) {
		__wt_free(env, drain, drain_len);
		drain = NULL;
	}
	if (hazard != NULL) {
		__wt_free(env, hazard, hazard_elem * sizeof(WT_PAGE *));
		hazard = NULL;
	}
	if (ret != 0)
		__wt_api_env_err(env, ret, "cache server failure");
	return (NULL);
}

/*
 * __wt_cache_hazchk --
 *	Compare the list of hazard references to the list of pages to be
 *	discarded.
 */
static int
__wt_cache_hazchk(ENV *env, WT_CACHE_ENTRY **drainp,
    u_int32_t drain_elem, WT_PAGE **hazard, u_int32_t hazard_elem)
{
	WT_PAGE **end_hazard, *page;
	WT_STATS *stats;

	stats = env->ienv->stats;
	end_hazard = hazard + hazard_elem;

	/*
	 * Both the drain and hazard lists are sorted by the page address in
	 * ascending order, so we don't have to search anything, just walk
	 * the lists in parallel and compare them.
	 */
	for (; drain_elem > 0; ++drainp, --drain_elem) {
		if (*drainp == NULL)
			continue;

		/*
		 * Look for the page in the hazard list until we reach the end
		 * of the list or find a hazard pointer larger than the page.
		 */
		for (page = (*drainp)->page;
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
			    "cache drain server skipping hazard referenced "
			    "element/page %#lx/%lu",
			    WT_PTR_TO_ULONG(*drainp), (u_long)(*drainp)->addr));
			WT_STAT_INCR(stats, CACHE_HAZARD_EVICT);

			(*drainp)->state = WT_OK;
			*drainp = NULL;
		}
	}
	return (0);
}

/*
 * __wt_cache_wmod --
 *	Flush modified pages.
 */
static int
__wt_cache_wmod(ENV *env, WT_CACHE_ENTRY **drainp, u_int32_t drain_elem)
{
	WT_PAGE *page;
	WT_STATS *stats;
	int tret, ret;

	stats = env->ienv->stats;
	ret = 0;

	for (; drain_elem > 0; ++drainp, --drain_elem) {
		if (*drainp == NULL)
			continue;
		page = (*drainp)->page;

		/* Write the page if it's been modified. */
		if (F_ISSET(page, WT_MODIFIED)) {
			WT_VERBOSE(env, WT_VERB_CACHE, (env,
			    "cache drain server writing element/page %#lx/%lu",
			    WT_PTR_TO_ULONG(*drainp), (u_long)(*drainp)->addr));
			WT_STAT_INCR(stats, CACHE_WRITE_EVICT);

			if ((tret =
			    __wt_cache_write(env, (*drainp)->db, page)) != 0) {
				if (ret == 0)
					ret = tret;
				(*drainp)->state = WT_OK;
				*drainp = NULL;
			}
		} else
			WT_STAT_INCR(stats, CACHE_EVICT);
	}
	return (ret);
}

/*
 * __wt_cache_recycle --
 *	Recycle cache pages.
 */
static int
__wt_cache_recycle(ENV *env, WT_CACHE_ENTRY **drainp, u_int32_t drain_elem)
{
	WT_PAGE *page;
	WT_CACHE *cache;

	cache = env->ienv->cache;

	for (; drain_elem > 0; ++drainp, --drain_elem) {
		if (*drainp == NULL)
			continue;
#ifdef HAVE_DIAGNOSTIC
		__wt_cache_hazard_check(env, *(drainp));
#endif
		WT_VERBOSE(env, WT_VERB_CACHE, (env,
		    "cache drain server recycling element/page %#lx/%lu",
		    WT_PTR_TO_ULONG(*drainp), (u_long)(*drainp)->addr));

		/* Copy the page ref, and give the slot to the I/O server. */
		page = (*drainp)->page;
		(*drainp)->state = WT_EMPTY;

		WT_CACHE_PAGE_OUT(cache, page->bytes);

		__wt_bt_page_recycle(env, page);
	}
	return (0);
}

/*
 * __wt_cache_write --
 *	Write a page to the backing database file.
 */
int
__wt_cache_write(ENV *env, DB *db, WT_PAGE *page)
{
	WT_CACHE *cache;
	WT_FH *fh;
	WT_PAGE_HDR *hdr;
	int ret;

	cache = env->ienv->cache;
	fh = db->idb->fh;

	/* Update the checksum. */
	hdr = page->hdr;
	hdr->checksum = 0;
	hdr->checksum = __wt_cksum(hdr, page->bytes);

	/* Write, and if successful, clear the modified flag. */
	if ((ret = __wt_write(
	    env, fh, WT_ADDR_TO_OFF(db, page->addr), page->bytes, hdr)) != 0) {
		__wt_api_env_err(env, ret, "cache unable to write page");
		return (ret);
	}

	F_CLR(page, WT_MODIFIED);

	return (0);
}

#ifdef HAVE_DIAGNOSTIC
/*
 * __wt_cache_hazard_check --
 *	Return if a page is or isn't on the hazard list.
 */
static void
__wt_cache_hazard_check(ENV *env, WT_CACHE_ENTRY *e)
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
 * __wt_cache_chk --
 *	Check the cache for consistency.
 */
void
__wt_cache_chk(ENV *env)
{
	WT_CACHE *cache;
	WT_CACHE_ENTRY *e;
	uint32_t i, j;

	cache = env->ienv->cache;
	WT_CACHE_FOREACH_PAGE_ALL(cache, e, i, j)
		switch (e->state) {
		case WT_DRAIN:
			__wt_api_env_errx(env,
			    "e->state == WT_DRAIN (e: %#lx, addr: %lu)",
			    (u_long)e, (u_long)e->addr);
			__wt_abort(env);
			break;
		case WT_OK:
			if (e->addr != e->page->addr) {
				__wt_api_env_errx(env, "e->addr != page->addr");
				__wt_abort(env);
			}
			break;
		}
}
#endif
