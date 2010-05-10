/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008 WiredTiger Software.
 *	All rights reserved.
 *
 * $Id$
 */

#include "wt_internal.h"

static int  __wt_drain_compare_gen(const void *a, const void *b);
static int  __wt_drain_compare_level(const void *a, const void *b);
static int  __wt_drain_compare_page(const void *a, const void *b);
static void __wt_drain_evict(ENV *);
static void __wt_drain_hazard_check(ENV *);
static int  __wt_drain_hazard_compare(const void *a, const void *b);
static void __wt_drain_set(ENV *);
static void __wt_drain_write(WT_TOC *, void (*)(const char *, u_int64_t));

#ifdef HAVE_DIAGNOSTIC
static void __wt_drain_hazard_validate(ENV *, WT_CACHE_ENTRY *);
#endif

/*
 * __wt_drain_compare_level --
 *	Qsort function: sort WT_CACHE_ENTRY list based on the page's level.
 */
static int
__wt_drain_compare_level(const void *a, const void *b)
{
	WT_CACHE_ENTRY *a_entry, *b_entry;
	u_int a_level, b_level;

	/*
	 * The array may contain entries set to NULL for various reasons (for
	 * example, a matching hazard reference), and so can't be discarded.
	 * Ignore them.
	 */
	a_entry = *(WT_CACHE_ENTRY **)a;
	b_entry = *(WT_CACHE_ENTRY **)b;
	if (a_entry == NULL || b_entry == NULL)
		return (a_entry == NULL &&
		    b_entry == NULL ? 0 : a_entry == NULL ? -1 : 1);

	a_level = a_entry->page->hdr->level;
	b_level = b_entry->page->hdr->level;

	/*
	 * Sort in descending order, the bigger a page's level, the sooner
	 * we want to write it.
	 */
	return (a_level > b_level ? 1 : (a_level < b_level ? -1 : 0));
}

/*
 * __wt_drain_compare_gen --
 *	Qsort function: sort WT_CACHE_ENTRY list based on the page's generation.
 */
static int
__wt_drain_compare_gen(const void *a, const void *b)
{
	u_int32_t a_gen, b_gen;

	a_gen = (*(WT_CACHE_ENTRY **)a)->read_gen;
	b_gen = (*(WT_CACHE_ENTRY **)b)->read_gen;

	return (a_gen > b_gen ? 1 : (a_gen < b_gen ? -1 : 0));
}

/*
 * __wt_drain_compare_page --
 *	Qsort function: sort WT_CACHE_ENTRY list based on the page's address.
 */
static int
__wt_drain_compare_page(const void *a, const void *b)
{
	WT_PAGE *a_page, *b_page;

	a_page = (*(WT_CACHE_ENTRY **)a)->page;
	b_page = (*(WT_CACHE_ENTRY **)b)->page;

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

/*
 * __wt_drain_trickle --
 *	Select a group of pages to trickle out.
 */
int
__wt_drain_trickle(WT_TOC *toc, int *didworkp)
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
	cache_pages = WT_STAT(cache->stats, CACHE_PAGES_INUSE);
	if (cache_pages <= 20)
		review_cnt = cache_pages;
	else {
		review_cnt = cache_pages / 20;
		if (review_cnt < 20)
			review_cnt = 20;
		else if (review_cnt > 100)
			review_cnt = 100;
	}
	WT_VERBOSE(env, WT_VERB_SERVERS, (env,
	    "cache drain server reviewing %lu entries of %llu total cache "
	    "pages",
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
	for (drain = cache->drain; review_cnt > 0; ++cache->bucket_cnt) {
		if (cache->bucket_cnt == cache->hb_size)
			cache->bucket_cnt = 0;
		WT_CACHE_FOREACH_PAGE(
		    cache, &cache->hb[cache->bucket_cnt], e, i) {
			/*
			 * Skip pages that aren't ours to take -- we're really
			 * going to discard these pages from memory, so ignore
			 * pinned pages.
			 */
			if (e->state != WT_OK || F_ISSET(e->page, WT_PINNED))
				continue;

			/*
			 * If a page is marked as useless, clear its generation
			 * number so we'll choose it.  We're rather do this in
			 * when returning a page to the cache, but we don't have
			 * a reference to the WT_CACHE_ENTRY at that point.
			 */
			if (F_ISSET(e->page, WT_DISCARD))
				e->read_gen = 0;

			*drain++ = e;
			if (--review_cnt == 0)
				break;
		}
	}
	cache->drain_elem = drain - cache->drain;

	/*
	 * Sort the pages by ascending generation number, then review the
	 * WT_CACHE_DRAIN_CNT pages with the lowest generation numbers.  (I
	 * have no evidence WT_CACHE_DRAIN_CNT is the right choice, but I'm
	 * trying to amortize the cost of building and sorting the drain and
	 * hazard arrays.)
	 */
	if (cache->drain_elem > WT_CACHE_DRAIN_CNT) {
		qsort(cache->drain, (size_t)cache->drain_elem,
		    sizeof(WT_CACHE_ENTRY *), __wt_drain_compare_gen);
		cache->drain_elem = WT_CACHE_DRAIN_CNT;
	}

	/*
	 * Discarding pages is done in 4 steps:
	 *	Check for hazard references (fast)
	 *	Set the WT_DRAIN state (fast)
	 *	Write any dirty pages (slow)
	 *	Discard the memory (fast)
	 */
	__wt_drain_set(env);
	__wt_drain_hazard_check(env);
	__wt_drain_write(toc, NULL);
	__wt_drain_evict(env);

	cache->drain_elem = 0;
	*didworkp = 1;

	return (0);
}

/*
 * __wt_drain_write --
 *	Write any modified pages.
 */
static void
__wt_drain_write(WT_TOC *toc, void (*f)(const char *, u_int64_t))
{
	ENV *env;
	WT_CACHE *cache;
	WT_CACHE_ENTRY *e, **drain;
	WT_PAGE *page;
	WT_STATS *stats;
	u_int64_t fcnt;
	u_int32_t drain_elem;

	env = toc->env;
	cache = env->ienv->cache;
	stats = cache->stats;

	/*
	 * Sort the pages for writing -- we write them in level order so that
	 * reconciliation updates naturally flow up the tree.
	 */
	qsort(cache->drain, (size_t)cache->drain_elem,
	    sizeof(WT_CACHE_ENTRY *), __wt_drain_compare_level);

	fcnt = 0;
	for (drain_elem = 0, drain = cache->drain;
	    drain_elem < cache->drain_elem; ++drain, ++drain_elem) {
		if ((e = *drain) == NULL)
			continue;

		/*
		 * This is the operation that takes all the time, so it's what
		 * we track for progress.
		 */
		if (f != NULL && ++fcnt % 10 == 0)
			f(toc->name, fcnt);

		page = e->page;
		if (!page->modified) {
			WT_STAT_INCR(stats, CACHE_EVICT_UNMODIFIED);
			continue;
		}

		WT_VERBOSE(env, WT_VERB_CACHE, (env,
		    "cache writing element/page/addr %p/%p/%lu",
		    e, e->page, (u_long)e->addr));

		/*
		 * Clear the page's modified value and write the page (the page
		 * can be modified while we write it).
		 */
		page->modified = 0;
		WT_STAT_INCR(stats, CACHE_EVICT_MODIFIED);

		/*
		 * We're using our WT_TOC handle, it needs to reference the
		 * correct DB handle.
		 */
		toc->db = e->db;

		/*
		 * If something bad happens when we try and write the page so
		 * that we can't discard the page, clear our reference.
		 */
		if (__wt_bt_rec_page(toc, page))
			(*drain) = NULL;
	}

	/* Wrap up reporting. */
	if (f != NULL)
		f(toc->name, fcnt);
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
		if ((*drain)->state == WT_OK)
			(*drain)->state = WT_DRAIN;
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
		 * Take a page reference, then clean up the entry.  We don't
		 * have to clear these fields (the state field is the only
		 * status any thread should be checking), but hopefully it
		 * will catch illegal references sooner rather than later.
		 */
		page = e->page;
		WT_CACHE_ENTRY_SET(e, NULL, NULL, WT_ADDR_INVALID, 0, WT_EMPTY);

		/* Free the memory. */
		WT_CACHE_PAGE_OUT(cache, page->size);
		__wt_bt_page_discard(env, page);
	}
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
#endif
