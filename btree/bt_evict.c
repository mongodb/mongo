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
static int  __wt_drain_hazard_compare(const void *a, const void *b);
static void __wt_drain_hazchk(ENV *);
static void __wt_drain_set(ENV *);
static int  __wt_drain_sync(ENV *, int *);
static int  __wt_drain_trickle(ENV *, int *);
static void __wt_drain_write(
		ENV *, void (*)(const char *, u_int64_t), const char *);

#ifdef HAVE_DIAGNOSTIC
static void __wt_drain_hazard_check(ENV *, WT_CACHE_ENTRY *);
#endif

/*
 * __wt_drain_compare_level --
 *	Qsort function: sort WT_CACHE_ENTRY list based on the page's level.
 */
static int
__wt_drain_compare_level(const void *a, const void *b)
{
	WT_CACHE_ENTRY *a_entry, *b_entry;
	WT_PAGE_HDR *a_hdr, *b_hdr;
	int a_level, b_level;

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

	a_hdr = a_entry->page->hdr;
	b_hdr = b_entry->page->hdr;

	/*
	 * The idea is to minimize the I/O in reconciliation, we'd like to find
	 * the pages we need in the cache.
	 */
	switch (a_hdr->type) {
	case WT_PAGE_DUP_LEAF:
		a_level = 1;
		break;
	case WT_PAGE_DUP_INT:
		a_level = 2;
		break;
	case WT_PAGE_COL_FIX:
	case WT_PAGE_COL_VAR:
	case WT_PAGE_ROW_LEAF:
		a_level = 3;
		break;
	case WT_PAGE_COL_INT:
	case WT_PAGE_ROW_INT:
		a_level = 4;
		break;
	}

	switch (b_hdr->type) {
	case WT_PAGE_DUP_LEAF:
		b_level = 1;
		break;
	case WT_PAGE_DUP_INT:
		b_level = 2;
		break;
	case WT_PAGE_COL_FIX:
	case WT_PAGE_COL_VAR:
	case WT_PAGE_ROW_LEAF:
		b_level = 3;
		break;
	case WT_PAGE_COL_INT:
	case WT_PAGE_ROW_INT:
		b_level = 4;
		break;
	}

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
 * __wt_workq_cache_sync_server --
 *	Called to check on the cache drain server thread when a sync
 *	is scheduled.
 */
void
__wt_workq_cache_sync_server(ENV *env)
{
	WT_CACHE *cache;

	cache = env->ienv->cache;

	/* Wake the cache drain thread if it's sleeping and it needs to run. */
	if (cache->drain_sleeping) {
		WT_VERBOSE(env, WT_VERB_CACHE | WT_VERB_SERVERS,
		    (env, "workQ waking cache drain server"));

		__wt_unlock(env, cache->mtx_drain);
		cache->drain_sleeping = 0;
	}
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
	int didwork, ret;

	env = arg;
	ienv = env->ienv;
	cache = ienv->cache;
	ret = 0;

	/*
	 * Allocate memory for a copy of the hazard references -- it's a fixed
	 * size so doesn't need run-time adjustments.
	 */
	cache->hazard_elem = env->toc_size * env->hazard_size;
	WT_ERR(__wt_calloc(
	    env, cache->hazard_elem, sizeof(WT_PAGE *), &cache->hazard));
	cache->hazard_len = cache->hazard_elem * sizeof(WT_PAGE *);

	while (F_ISSET(ienv, WT_SERVER_RUN)) {
		/* Check for a sync, then running out of resources. */
		didwork = 0;
		WT_ERR(__wt_drain_sync(env, &didwork));
		WT_ERR(__wt_drain_trickle(env, &didwork));

#ifdef HAVE_DIAGNOSTIC
		__wt_drain_chk(env);
#endif
		/*
		 * No memory flush needed, the drain_sleeping field is declared
		 * volatile.
		 */
		if (!didwork) {
			WT_VERBOSE(env, WT_VERB_CACHE | WT_VERB_SERVERS,
			    (env, "cache drain server sleeping"));
			cache->drain_sleeping = 1;
			__wt_lock(env, cache->mtx_drain);
		}
	}

err:	if (cache->drain != NULL)
		__wt_free(env, cache->drain, cache->drain_len);
	if (cache->hazard != NULL)
		__wt_free(env, cache->hazard, cache->hazard_len);
	if (ret != 0)
		__wt_api_env_err(env, ret, "cache server failure");

	return (NULL);
}

/*
 * __wt_drain_sync --
 *	Flush all of the pages for any specified sync calls.
 */
static int
__wt_drain_sync(ENV *env, int *didworkp)
{
	WT_CACHE *cache;
	WT_CACHE_ENTRY *e, **drain;
	WT_SYNC_REQ *sr, *sr_end;
	WT_TOC *toc;
	DB *db;
	u_int32_t drain_elem, i, j;
	void (*f)(const char *, u_int64_t);

	cache = env->ienv->cache;

	/* Check if we need to run. */
	sr = cache->sync_request;
	sr_end =
	    sr + sizeof(cache->sync_request) / sizeof(cache->sync_request[0]);
	for (toc = NULL; sr < sr_end; ++sr)
		if (sr->toc != NULL) {
			/*
			 * We only do a single sync per call; that's OK, if by
			 * some chance there is more than one sync request, we
			 * will find it on our next loop.
			 */
			toc = sr->toc;
			f = sr->f;

			/* We've got it -- clear the slot. */
			sr->toc = NULL;
			WT_MEMORY_FLUSH;
			break;
		}
	if (toc == NULL)
		return (0);
	db = toc->db;

	drain = cache->drain;
	drain_elem = 0;
	WT_CACHE_FOREACH_PAGE_ALL(cache, e, i, j) {
		/*
		 * Skip pages that aren't ours to take -- pinned pages are OK,
		 * we're not discarding the pages from memory.
		 */
		if (e->state != WT_OK)
			continue;

		/*
		 * We only want modified pages from the specified database.
		 * There's an obvious race here, where we're flushing pages
		 * while other threads are modifying them -- that's OK, sync
		 * offers no guarantees in the face of other writers.
		 */
		if (e->db != db || e->write_gen == e->page->write_gen)
			continue;

		/*
		 * If a page is marked as useless, clear its read generation
		 * number so we'll choose it.  We're rather do this in when
		 * returning a page to the cache, but we don't have a reference
		 * to the WT_CACHE_ENTRY at that point.
		 */
		if (F_ISSET(e->page, WT_DISCARD))
			e->read_gen = 0;

		/* Allocate more space as necessary. */
		if (drain_elem * sizeof(WT_CACHE_ENTRY *) >= cache->drain_len) {
			WT_RET(__wt_realloc(env, &cache->drain_len,
			    (drain_elem + 100) * sizeof(WT_CACHE_ENTRY *),
			    &cache->drain));
			drain = cache->drain + drain_elem;
		}

		*drain++ = e;
		++drain_elem;
	}
	cache->drain_elem = drain_elem;

	if (drain_elem != 0) {
		WT_VERBOSE(env, WT_VERB_CACHE, (env,
		    "cache drain server flushing file %s, %lu of %llu total "
		    "cache pages",
		    db->idb->name, (u_long)drain_elem,
		    (u_quad)WT_CACHE_PAGES_INUSE(cache)));

		__wt_drain_write(env, f, toc->name);

		cache->drain_elem = 0;
		*didworkp = 1;
	}

	/* Wake the caller. */
	__wt_unlock(env, toc->mtx);

	return (0);
}

/*
 * __wt_drain_trickle --
 *	Select a group of pages to trickle out.
 */
static int
__wt_drain_trickle(ENV *env, int *didworkp)
{
	WT_CACHE *cache;
	WT_CACHE_ENTRY *e, **drain;
	u_int64_t bytes_inuse, bytes_max, cache_pages;
	u_int32_t i, review_cnt;

	cache = env->ienv->cache;

	/*
	 * Check if we need to run.  We check the cache lockout flag because it
	 * implies being more aggressive about cleaning than only comparing the
	 * in-use bytes vs. the max bytes.
	 */
	bytes_inuse = WT_CACHE_BYTES_INUSE(cache);
	bytes_max = WT_STAT(cache->stats, CACHE_BYTES_MAX);
	if (cache->read_lockout == 0 && bytes_inuse <= bytes_max)
		return (0);

	WT_VERBOSE(env, WT_VERB_CACHE | WT_VERB_SERVERS, (env,
	    "cache drain server running: read lockout %sset, "
	    "bytes inuse > max (%llu > %llu), ",
	    cache->read_lockout ? "" : "not ",
	    (u_quad)bytes_inuse, (u_quad)bytes_max));

	/*
	 * Review 2% of the pages in the cache, but not less than 20 pages and
	 * not more than 100 pages.
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
	WT_VERBOSE(env, WT_VERB_CACHE, (env,
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
	 *	Write the modified pages (slow)
	 *	Set the WT_DRAIN state (fast)
	 *	Check for hazard references (fast)
	 *	Discard the memory (fast)
	 */
	__wt_drain_write(env, NULL, NULL);

	/*
	 * Lock the hash buckets, they can't grow while the cache drain server
	 * has them pinned (that is, has references to pages in the bucket).
	 * The problem is copying buckets with the WT_DRAIN flag set, the drain
	 * server is referencing the wrong page at some point.  The lock should
	 * not be a problem, we don't hold it during any slow operations, and
	 * the hash buckets grow briefly but then quit growing once the cache
	 * full.
	 */
	__wt_lock(env, cache->mtx_hb);

	__wt_drain_set(env);
	__wt_drain_hazchk(env);
	__wt_drain_evict(env);

	__wt_unlock(env, cache->mtx_hb);

	cache->drain_elem = 0;
	*didworkp = 1;

	return (0);
}

/*
 * __wt_drain_write --
 *	Write any modified pages.
 */
static void
__wt_drain_write(ENV *env, void (*f)(const char *, u_int64_t), const char *name)
{
	WT_CACHE *cache;
	WT_CACHE_ENTRY *e, **drain;
	WT_PAGE *page;
	WT_STATS *stats;
	u_int64_t fcnt;
	u_int32_t drain_elem;

	cache = env->ienv->cache;
	stats = cache->stats;

	/*
	 * Sort the pages for writing; we'd like to write leaf pages first so
	 * we minimize the I/O we do during reconcilation.  This is almost
	 * certainly not enough, we'll want to do much, much better planning
	 * than this, at some point.
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
			f(name, fcnt);

		/*
		 * The page is still being read and/or written by other threads,
		 * for all we know.  Copy the page's write generation to our
		 * local information and write the page.   If: (1) the workQ
		 * updates the page's write generation number before it allows
		 * page modification, (2) we copy the page's write generation
		 * number before we write the page to the backing store, (3) no
		 * thread is currently accessing the page, and (4) the page's
		 * write generation is the same as our copy, then the page is
		 * clean.
		 */
		page = e->page;
		if (e->write_gen != page->write_gen) {
			WT_VERBOSE(env, WT_VERB_CACHE, (env,
			    "cache drain server writing element/page %#lx/%lu",
			    WT_PTR_TO_ULONG(e), (u_long)e->addr));

			e->write_gen = page->write_gen;
			WT_STAT_INCR(stats, CACHE_EVICT_MODIFIED);

			/*
			 * If something bad happens, we can't discard the page,
			 * clear its reference.
			 */
			if (__wt_bt_page_reconcile(e->db, page))
				(*drain) = NULL;
		} else {
			WT_VERBOSE(env, WT_VERB_CACHE, (env,
			    "cache drain server discarding element/page "
			    "%#lx/%#lu (%lu != %lu)",
			    WT_PTR_TO_ULONG(e), (u_long)e->addr,
			    (u_long)e->write_gen, (u_long)page->write_gen));

			WT_STAT_INCR(stats, CACHE_EVICT);
		}
	}

	/* Wrap up reporting. */
	if (f != NULL)
		f(name, fcnt);
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
		if ((*drain)->state != WT_OK)
			*drain = NULL;
		else
			(*drain)->state = WT_DRAIN;
	}
}

/*
 * __wt_drain_hazchk --
 *	Compare the list of hazard references to the list of pages to be
 *	discarded.
 */
static void
__wt_drain_hazchk(ENV *env)
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
			    "cache drain server skipping hazard referenced "
			    "element/page %#lx/%lu",
			    WT_PTR_TO_ULONG(e), (u_long)e->addr));
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
		__wt_drain_hazard_check(env, e);
#endif
		/*
		 * If the page has been modified since we looked at it, keep
		 * it in the cache.
		 *
		 * XXX
		 * This is probably where we have to figure out if a page is
		 * dirty because sync is re-dirtying pages, or a different
		 * thread accessed the page.
		 */
		page = e->page;
		e->page = NULL;
		WT_CACHE_PAGE_OUT(cache, page->size);
		__wt_bt_page_discard(env, page);

		e->state = WT_EMPTY;
	}
}

#ifdef HAVE_DIAGNOSTIC
/*
 * __wt_drain_hazard_check --
 *	Return if a page is or isn't on the hazard list.
 */
static void
__wt_drain_hazard_check(ENV *env, WT_CACHE_ENTRY *e)
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
 * __wt_drain_chk --
 *	Check the cache for consistency.
 */
void
__wt_drain_chk(ENV *env)
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
