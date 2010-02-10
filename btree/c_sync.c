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
static int  __wt_cache_in_serial_func(WT_TOC *);
static int  __wt_cache_drain(
		WT_TOC *, WT_DRAIN *, u_int32_t, WT_PAGE **, u_int32_t);
static void __wt_cache_hazard_check(ENV *, WT_PAGE *);
static int  __wt_cache_write(ENV *, WT_PAGE *);
static void __wt_cache_discard(ENV *, WT_PAGE *);
static int  __wt_cache_drain_compare_gen(const void *a, const void *b);
static int  __wt_cache_drain_compare_page(const void *a, const void *b);
static int  __wt_cache_hazard_compare(const void *a, const void *b);
static void __wt_hazard_clear(WT_TOC *, WT_PAGE *);
static void __wt_hazard_set(WT_TOC *, WT_PAGE *);

/*
 * WT_CACHE_DRAIN_CHECK --
 *	Macro to wait on the cache to drain.
 *
 * !!!
 * It might be better to sleep here, because it can take a long time for the
 * cache to drain -- again, once we have an I/O thread, it can wake us up.
 */
#define	WT_CACHE_DRAIN_CHECK(toc) do {					\
	while (F_ISSET((toc)->env->ienv, WT_CACHE_LOCKOUT)) {		\
		F_SET(toc, WT_CACHE_DRAIN_WAIT);			\
		__wt_toc_serialize_request(toc, NULL, NULL);		\
		F_CLR(toc, WT_CACHE_DRAIN_WAIT);			\
	}								\
} while (0)

/*
 * __wt_hazard_set --
 *	Set a hazard reference.
 */
static void
__wt_hazard_set(WT_TOC *toc, WT_PAGE *page)
{
	ENV *env;
	WT_PAGE **hp;

	env = toc->env;

	/* Set the caller's hazard pointer. */
	for (hp = toc->hazard; hp < toc->hazard + env->hazard_size; ++hp)
		if (*hp == NULL) {
			*hp = page;
			WT_MEMORY_FLUSH;
			return;
		}
	WT_ASSERT(env, hp < toc->hazard + env->hazard_size);
}

/*
 * __wt_hazard_clear --
 *	Clear a hazard reference.
 */
static void
__wt_hazard_clear(WT_TOC *toc, WT_PAGE *page)
{
	ENV *env;
	WT_PAGE **hp;

	env = toc->env;

	/* Clear the caller's hazard pointer. */
	for (hp = toc->hazard; hp < toc->hazard + env->hazard_size; ++hp)
		if (*hp == page) {
			*hp = NULL;
			/*
			 * We don't have to flush memory here for correctness,
			 * but that gives the drain thread immediate access to
			 * the buffer.
			 */
			WT_MEMORY_FLUSH;
			return;
		}
	WT_ASSERT(env, hp < toc->hazard + env->hazard_size);
}

/*
 * __wt_cache_create --
 *	Create the underlying cache.
 */
int
__wt_cache_create(ENV *env)
{
	IENV *ienv;
	WT_CACHE *cache;

	ienv = env->ienv;
	cache = &ienv->cache;

	WT_RET(__wt_mtx_init(&cache->mtx));	/* Cache server mutex */
	__wt_lock(env, &cache->mtx);		/* Blocking mutex */

	WT_STAT_SET(
	    ienv->stats, CACHE_BYTES_MAX, env->cache_size * WT_MEGABYTE);

	/*
	 * Initialize the cache page queues.  No server support needed, this is
	 * done when the environment is first opened, before there are multiple
	 * threads of control using the cache.
	 *
	 * We don't sort the hash queues in page LRU order because that requires
	 * manipulating the linked list as part of each read operation.  As a
	 * result, WiredTiger is much more sensitive to long bucket chains than
	 * Berkeley DB, and the bucket chains need to be short to avoid spending
	 * all our time walking the linked list.  To help, we do put the bucket
	 * into LRU order when looking for pages to evict.
	 *
	 * By default, size for a cache filled with 8KB pages, and 4 pages per
	 * bucket (or, 32 buckets per MB).
	 */
	cache->hash_size = env->cache_hash_size;
	if (cache->hash_size == WT_CACHE_HASH_SIZE_DEFAULT)
		cache->hash_size = __wt_prime(env->cache_size * 32);
	WT_STAT_SET(ienv->stats, HASH_BUCKETS, cache->hash_size);

	WT_RET(
	    __wt_calloc(env, cache->hash_size, sizeof(WT_PAGE *), &cache->hb));

	F_SET(cache, WT_INITIALIZED);
	return (0);
}

/*
 * __wt_cache_destroy --
 *	Discard the underlying cache.
 */
int
__wt_cache_destroy(ENV *env)
{
	IENV *ienv;
	WT_CACHE *cache;
	WT_PAGE *page;
	u_int i;
	int ret;

	ienv = env->ienv;
	cache = &ienv->cache;
	ret = 0;

	if (!F_ISSET(cache, WT_INITIALIZED))
		return (0);

	/*
	 * Discard all pages.  No server support needed, this is done when the
	 * environment is closed, after all threads of control have exited the
	 * cache.
	 *
	 * There shouldn't be any modified pages, because all of the databases
	 * have been closed.
	 */
	for (i = 0; i < cache->hash_size; ++i)
		while ((page = cache->hb[i]) != NULL) {
			__wt_cache_discard(env, page);
			__wt_bt_page_recycle(env, page);
		}

	/* There shouldn't be any allocated bytes. */
	WT_ASSERT(env, WT_STAT(ienv->stats, CACHE_BYTES_INUSE) == 0);

	/* Discard allocated memory, and clear. */
	__wt_free(env, cache->hb, cache->hash_size * sizeof(WT_PAGE *));
	memset(cache, 0, sizeof(cache));

	return (ret);
}

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

/*
 * __wt_cache_alloc --
 *	Allocate bytes from a file.
 */
int
__wt_cache_alloc(WT_TOC *toc, u_int32_t bytes, WT_PAGE **pagep)
{
	DB *db;
	ENV *env;
	IDB *idb;
	WT_PAGE *page;
	int ret;

	*pagep = NULL;

	db = toc->db;
	env = toc->env;
	idb = db->idb;

	WT_ASSERT(env, bytes % WT_FRAGMENT == 0);

	/* Check cache size before any allocation. */
	WT_CACHE_DRAIN_CHECK(toc);

	WT_STAT_INCR(env->ienv->stats, CACHE_ALLOC);
	WT_STAT_INCR(idb->stats, DB_CACHE_ALLOC);

	/*
	 * Allocate memory for the in-memory page information and for the page
	 * itself. They're two separate allocation calls so we (hopefully) get
	 * better alignment from the underlying heap memory allocator.
	 * Clear the memory because code depends on initial values of 0.
	 */
	WT_RET(__wt_calloc(env, 1, sizeof(WT_PAGE), &page));
	WT_ERR(__wt_calloc(env, 1, (size_t)bytes, &page->hdr));

	/*
	 * Allocate "bytes" bytes from the end of the file; we must serialize
	 * the allocation of bytes from the file, the change of total bytes
	 * in the cache, and the insert onto the hash queue.
	 */
	__wt_cache_in_serial(toc, page, WT_ADDR_INVALID, bytes);
	if ((ret = toc->serial_ret) != 0)
		goto err;

	*pagep = page;
	return (0);

err:	if (page->hdr != NULL)
		__wt_free(env, page->hdr, bytes);
	__wt_free(env, page, sizeof(WT_PAGE));
	return (ret);
}

/*
 * __wt_cache_in --
 *	Return a database page, reading as necessary.
 */
int
__wt_cache_in(WT_TOC *toc,
    u_int32_t addr, u_int32_t bytes, u_int32_t flags, WT_PAGE **pagep)
{
	DB *db;
	ENV *env;
	IDB *idb;
	IENV *ienv;
	WT_CACHE *cache;
	WT_PAGE *page;
	WT_PAGE_HDR *hdr;
	off_t offset;
	u_int32_t checksum;
	u_int bucket_cnt;
	int ret;

	*pagep = NULL;

	db = toc->db;
	env = toc->env;
	idb = db->idb;
	ienv = env->ienv;
	cache = &ienv->cache;

	WT_ASSERT(env, bytes % WT_FRAGMENT == 0);

retry:	/* Search for the page in the cache. */
	for (bucket_cnt = 0,
	    page = cache->hb[WT_HASH(cache, addr)];
	    page != NULL; page = page->next) {
		++bucket_cnt;
		if (page->addr == addr && page->db == db)
			break;
	}
	if (bucket_cnt > WT_STAT(ienv->stats, LONGEST_BUCKET))
		WT_STAT_SET(ienv->stats, LONGEST_BUCKET, bucket_cnt);
	if (page != NULL) {
		/*
		 * Add the page to the WT_TOC's hazard list and flush the write,
		 * then see if the page is being discarded by the cache server.
		 * That shouldn't happen often --  we select pages based on LRU
		 * generation.  If the page is being discarded, we can't use it,
		 * sleep awhile and retry.
		 *
		 * !!!
		 * When we have an I/O thread, we could schedule work for it,
		 * and sleep, and the I/O thread could wake us when the page is
		 * ready.
		 */
		__wt_hazard_set(toc, page);
		if (page->drain) {
			__wt_hazard_clear(toc, page);
			__wt_sleep(0, 100000);
			goto retry;
		}

		WT_STAT_INCR(env->ienv->stats, CACHE_HIT);
		WT_STAT_INCR(idb->stats, DB_CACHE_HIT);

		page->page_gen = ++ienv->page_gen;
		*pagep = page;
		return (0);
	}

	/* Check cache size before any allocation. */
	WT_CACHE_DRAIN_CHECK(toc);

	WT_STAT_INCR(env->ienv->stats, CACHE_MISS);
	WT_STAT_INCR(idb->stats, DB_CACHE_MISS);

	/*
	 * Allocate memory for the in-memory page information and for the page
	 * itself. They're two separate allocation calls so we (hopefully) get
	 * better alignment from the underlying heap memory allocator.
	 * Clear the memory because code depends on initial values of 0.
	 *
	 * Read the page.
	 */
	WT_RET(__wt_calloc(env, 1, sizeof(WT_PAGE), &page));
	WT_ERR(__wt_calloc(env, 1, (size_t)bytes, &page->hdr));
	offset = WT_ADDR_TO_OFF(db, addr);
	WT_ERR(__wt_read(env, idb->fh, offset, bytes, page->hdr));

	/*
	 * If this is an unformatted read, ensure we never find it again in the
	 * cache by not linking it in.
	 */
	if (LF_ISSET(WT_UNFORMATTED)) {
		F_SET(page, WT_UNFORMATTED);
		*pagep = page;
		return (0);
	}

	/* Verify the checksum. */
	hdr = page->hdr;
	checksum = hdr->checksum;
	hdr->checksum = 0;
	if (checksum != __wt_cksum(hdr, bytes)) {
		__wt_db_errx(db,
		    "file offset %llu with length %lu was read and had a "
		    "checksum error", (u_quad)offset, (u_long)bytes);
		ret = WT_ERROR;
		goto err;
	}

	/* Serialize the insert onto the hash queue. */
	__wt_cache_in_serial(toc, page, addr, bytes);
	if ((ret = toc->serial_ret) == WT_RESTART)
		goto err;

	*pagep = page;
	return (0);

err:	__wt_free(env, page->hdr, bytes);
	__wt_free(env, page, sizeof(WT_PAGE));
	if (ret == WT_RESTART)
		goto retry;
	return (ret);
}

/*
 * __wt_cache_in_serial_func --
 *	Server function to read bytes from a file.
 */
static int
__wt_cache_in_serial_func(WT_TOC *toc)
{
	DB *db;
	IENV *ienv;
	WT_CACHE *cache;
	WT_FH *fh;
	WT_PAGE **hb, *page, *tp;
	u_int32_t addr, bytes;

	__wt_cache_in_unpack(toc, page, addr, bytes);

	db = toc->db;
	ienv = toc->env->ienv;
	cache = &ienv->cache;

	/*
	 * If we're reading a page into memory, check to see if some other
	 * thread brought the page in while we were waiting to run.
	 */
	if (addr != WT_ADDR_INVALID)
		for (tp =
		    cache->hb[WT_HASH(cache, addr)]; tp != NULL; tp = tp->next)
			if (tp->addr == addr && tp->db == db)
				return (WT_RESTART);

	/* Initialize the page structure. */
	page->db = db;
	if (addr == WT_ADDR_INVALID) {
		fh = db->idb->fh;
		addr = WT_OFF_TO_ADDR(db, fh->file_size);
		fh->file_size += bytes;
	}
	page->addr = addr;
	page->bytes = bytes;
	page->page_gen = ++ienv->page_gen;

	/*
	 * Add the page to the WT_TOC's hazard list and flush the write; we
	 * don't need to do any further checking, no possible cache walk can
	 * find the page without also finding our hazard reference.
	 */
	__wt_hazard_set(toc, page);

	/*
	 * Insert as the head of the linked list.  The insert has to be thread
	 * safe, that is, other threads may be walking the linked list at the
	 * same time we're doing the insert.
	 */
	hb = &cache->hb[WT_HASH(cache, addr)];
	page->next = *hb;
	*hb = page;

	/* Increment total cache byte and page count. */
	WT_STAT_INCR(ienv->stats, CACHE_PAGES);
	WT_STAT_INCRV(ienv->stats, CACHE_BYTES_INUSE, bytes);

	return (0);
}

/*
 * __wt_cache_out --
 *	Discard a database page, writing as necessary.
 */
int
__wt_cache_out(WT_TOC *toc, WT_PAGE *page, u_int32_t flags)
{
	ENV *env;

	env = toc->env;

	/* Unformatted pages may not be modified. */
	WT_ASSERT(env,
	    !(LF_ISSET(WT_MODIFIED) && F_ISSET(page, WT_UNFORMATTED)));

	/* If the page has been modified, set the local flag. */
	if (LF_ISSET(WT_MODIFIED))
		F_SET(page, WT_MODIFIED);

	/*
	 * Unformatted pages were never linked on the cache chains, and are
	 * discarded as soon as they're returned.
	 */
	if (F_ISSET(page, WT_UNFORMATTED))
		__wt_bt_page_recycle(env, page);
	else
		__wt_hazard_clear(toc, page);

	return (0);
}

static int
__wt_cache_drain_compare_page(const void *a, const void *b)
{
	WT_PAGE *a_page, *b_page;

	a_page = ((WT_DRAIN *)a)->page;
	b_page = ((WT_DRAIN *)b)->page;

	return (a_page > b_page ? 1 : (a_page < b_page ? -1 : 0));
}

static int
__wt_cache_drain_compare_gen(const void *a, const void *b)
{
	u_int32_t a_gen, b_gen;

	a_gen = ((WT_DRAIN *)a)->gen;
	b_gen = ((WT_DRAIN *)b)->gen;

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
	WT_DRAIN *drain, *drainp;
	WT_PAGE **hazard, *page;
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
		if (review_cnt * sizeof(WT_DRAIN) > drain_len)
			WT_ERR(__wt_realloc(env, &drain_len,
			    (review_cnt + 20) * sizeof(WT_DRAIN), &drain));

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
					drainp->page = page;
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
		    sizeof(WT_DRAIN), __wt_cache_drain_compare_gen);

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
		    sizeof(WT_DRAIN), __wt_cache_drain_compare_page);

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
			drain[drain_cnt].page->drain = 1;
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
__wt_cache_drain(WT_TOC *toc, WT_DRAIN *drain,
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
		for (page = drain[drain_cnt].page;
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

			drain[drain_cnt].page = NULL;
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
		if ((page = drain->page) != NULL)
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
	WT_DRAIN *drain;
	WT_PAGE *page;
	u_int32_t drain_cnt, drain_elem;

	env = toc->env;

	__wt_cache_discard_unpack(toc, drain, drain_elem);

	for (drain_cnt = 0; drain_cnt < drain_elem; ++drain_cnt, ++drain)
		if ((page = drain->page) != NULL) {
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
static void
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

/*
 * __wt_cache_write --
 *	Write a page to the backing database file.
 */
static int
__wt_cache_write(ENV *env, WT_PAGE *page)
{
	DB *db;
	WT_PAGE_HDR *hdr;

	db = page->db;

	/* Update the checksum. */
	hdr = page->hdr;
	hdr->checksum = 0;
	hdr->checksum = __wt_cksum(hdr, page->bytes);

	/* Write, and if successful, clear the modified flag. */
	WT_RET(__wt_write(env, db->idb->fh,
	    WT_ADDR_TO_OFF(page->db, page->addr), page->bytes, hdr));

	F_CLR(page, WT_MODIFIED);

	WT_STAT_INCR(env->ienv->stats, CACHE_WRITE);

	return (0);
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

/*
 * __wt_cache_dump --
 *	Dump a cache.
 */
int
__wt_cache_dump(ENV *env)
{
	IENV *ienv;
	WT_CACHE *cache;
	WT_PAGE *page;
	u_int32_t i;

	ienv = env->ienv;
	cache = &ienv->cache;

	__wt_msg(env, "cache dump (%llu pages): ==================",
	    WT_STAT(ienv->stats, CACHE_PAGES));

	for (i = 0; i < cache->hash_size; ++i) {
		__wt_msg(env, "hash bucket %d:", i);
		for (page = cache->hb[i]; page != NULL; page = page->next)
			__wt_msg(env, "\t%#lx {addr: %lu, bytes: %lu}",
			    WT_ADDR_TO_ULONG(page), (u_long)page->addr,
			    (u_long)page->bytes);
	}

	return (0);
}
#endif
