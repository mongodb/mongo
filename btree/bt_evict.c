/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008 WiredTiger Software.
 *	All rights reserved.
 *
 * $Id$
 */

#include "wt_internal.h"

static int __wt_cache_clean(WT_TOC *, u_int32_t, WT_PAGE **);
static int __wt_cache_discard(WT_TOC *, WT_CACHE *, WT_PAGE *);
static int __wt_cache_write(WT_TOC *, WT_PAGE *);

#ifdef HAVE_DIAGNOSTIC
static int __wt_cache_dump(WT_CACHE *, char *, FILE *);
#endif

/*
 * __wt_cache_create --
 *	Create an underlying cache.
 */
int
__wt_cache_create(WT_TOC *toc, WT_CACHE **cache_addr)
{
	ENV *env;
	WT_CACHE *cache;
	u_int32_t i;

	env = toc->env;

	WT_RET(__wt_calloc(env, 1, sizeof(*cache), &cache));

	/*
	 * Initialize the cache page queues.  Size for a cache filled with
	 * 16KB pages, and 8 pages per bucket (which works out to 8 buckets
	 * per MB).
	 */
	cache->hashsize = __wt_prime(env->cachesize * 8);
	WT_RET(__wt_calloc(
	    env, cache->hashsize, sizeof(cache->hqh[0]), &cache->hqh));
	for (i = 0; i < cache->hashsize; ++i)
		TAILQ_INIT(&cache->hqh[i]);
	TAILQ_INIT(&cache->lqh);

	*cache_addr = cache;
	return (0);
}

/*
 * __wt_cache_destroy --
 *	Discard an underlying cache.
 */
int
__wt_cache_destroy(WT_TOC *toc, WT_CACHE **cache_addr)
{
	ENV *env;
	WT_CACHE *cache;
	WT_PAGE *page;
	int ret;

	/*
	 * !!!
	 * The cache we're discarding may NOT be the one referenced by the
	 * WT_TOC because we may be handling server caches.  For this reason,
	 * __wt_cache_destroy and __wt_cache_sync both take cache arguments,
	 * and functions they call must also take cache arguments ignoring
	 * the WT_TOC->cache field.
	 */
	env = toc->env;
	cache = *cache_addr;
	ret = 0;

	/* Write any modified pages, discard pages. */
	while ((page = TAILQ_FIRST(&cache->lqh)) != NULL) {
		if (F_ISSET(page, WT_MODIFIED))
			WT_TRET(__wt_cache_write(toc, page));
		WT_TRET(__wt_cache_discard(toc, cache, page));
	}

	/* There shouldn't be any allocated bytes. */
	WT_ASSERT(env, cache->cache_bytes == 0);

	/* Discard allocated memory. */
	WT_FREE_AND_CLEAR(env, cache->hqh);
	WT_FREE_AND_CLEAR(env, *cache_addr);
		
	return (ret);
}

/*
 * __wt_cache_sync --
 *	Flush an underlying cache to disk.
 */
int
__wt_cache_sync(WT_TOC *toc, WT_CACHE *cache)
{
	WT_PAGE *page;

	/*
	 * !!!
	 * The cache we're discarding may NOT be the one referenced by the
	 * WT_TOC because we may be handling server caches.  For this reason,
	 * __wt_cache_destroy and __wt_cache_sync both take cache arguments,
	 * and functions they call must also take cache arguments ignoring
	 * the WT_TOC->cache field.
	 */

	/* Write any modified pages. */
	TAILQ_FOREACH(page, &cache->lqh, q)
		if (F_ISSET(page, WT_MODIFIED))
			WT_RET(__wt_cache_write(toc, page));
	return (0);
}

/*
 * __wt_cache_toc_lru --
 *	Discard cache pages if we're holding too much memory.
 */
int
__wt_cache_toc_lru(WT_TOC *toc)
{
	ENV *env;
	WT_CACHE *cache;
	WT_PAGE *page;

	env = toc->env;
	cache = toc->cache;

	/* Discard pages until we're below our threshold. */
	TAILQ_FOREACH(page, &cache->lqh, q) {
		if (cache->cache_bytes <=
		    (u_int64_t)env->cachesize * WT_MEGABYTE)
			break;
		if (page->ref == 0) {
			if (F_ISSET(page, WT_MODIFIED))
				WT_RET(__wt_cache_write(toc, page));
			WT_RET(__wt_cache_discard(toc, cache, page));
		}
	}
	return (0);
}

/*
 * WT_PAGE_ALLOC --
 *	Allocate memory for the in-memory page information and for the page
 *	itself.	 They're two separate allocation calls so we (hopefully) get
 *	better alignment from the underlying heap memory allocator.
 *
 *	Clear the memory because code depends on initial values of 0.
 */
#define	WT_PAGE_ALLOC(toc, env, cache, bytes, page) do {		\
	if ((cache)->cache_bytes >					\
	    (u_int64_t)(env)->cachesize * WT_MEGABYTE)			\
		WT_RET(__wt_cache_toc_lru(toc));			\
	WT_RET(__wt_calloc((env), 1, sizeof(WT_PAGE), &(page)));	\
	{								\
	int __ret;							\
	if (((__ret) = __wt_calloc(					\
	    (env), 1, (size_t)(bytes), &(page)->hdr)) != 0) {		\
		__wt_free((env), (page));				\
		return ((__ret));					\
	}								\
	}								\
	(cache)->cache_bytes += (bytes);				\
	WT_STAT_INCR((env)->ienv->stats, CACHE_CLEAN, NULL);		\
} while (0)

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
	WT_CACHE *cache;
	WT_PAGE *page;

	*pagep = NULL;

	db = toc->db;
	env = toc->env;
	idb = toc->db->idb;
	cache = toc->cache;

	WT_ASSERT(env, bytes % WT_FRAGMENT == 0);

	/* A multi-threaded cache can't ever miss. */
	WT_ASSERT(env, !F_ISSET(cache, WT_READONLY));

	WT_PAGE_ALLOC(toc, env, cache, bytes, page);

	WT_STAT_INCR(
	    env->ienv->stats, CACHE_ALLOC, "pages allocated in the cache");
	WT_STAT_INCR(
	    idb->stats, DB_CACHE_ALLOC, "pages allocated in the cache");

	/* Initialize the page. */
	page->offset = idb->fh->file_size;
	page->addr = WT_OFF_TO_ADDR(db, page->offset);
	page->bytes = bytes;
	page->ref = 1;
	TAILQ_INSERT_TAIL(&cache->lqh, page, q);
	TAILQ_INSERT_HEAD(&cache->hqh[WT_HASH(cache, page->offset)], page, hq);

	idb->fh->file_size += bytes;

	*pagep = page;
	return (0);
}

/*
 * __wt_cache_in --
 *	Pin bytes of a file, reading as necessary.
 */
int
__wt_cache_in(WT_TOC *toc,
    off_t offset, u_int32_t bytes, u_int32_t flags, WT_PAGE **pagep)
{
	DB *db;
	ENV *env;
	IDB *idb;
	WT_CACHE *cache;
	WT_PAGE *page;
	WT_PAGE_HDR *hdr;
	WT_PAGE_HQH *hashq;
	int ret;

	*pagep = NULL;

	db = toc->db;
	env = toc->env;
	idb = db->idb;
	cache = toc->cache;

	WT_ASSERT(env, bytes % WT_FRAGMENT == 0);
	WT_ENV_FCHK(env, "__wt_cache_in", flags, WT_APIMASK_WT_CACHE_IN);

	/* Check for the page in the cache. */
	hashq = &cache->hqh[WT_HASH(cache, offset)];
	TAILQ_FOREACH(page, hashq, hq)
		if (page->offset == offset)
			break;
	if (page != NULL) {
		++page->ref;

		WT_STAT_INCR(env->ienv->stats,
		    CACHE_HIT, "cache hit: reads found in the cache");
		WT_STAT_INCR(idb->stats,
		    DB_CACHE_HIT, "cache hit: reads found in the cache");

		if (!F_ISSET(cache, WT_READONLY)) {
			/* Move to the head of the hash queue */
			TAILQ_REMOVE(hashq, page, hq);
			TAILQ_INSERT_HEAD(hashq, page, hq);

			/* Move to the tail of the LRU queue. */
			TAILQ_REMOVE(&cache->lqh, page, q);
			TAILQ_INSERT_TAIL(&cache->lqh, page, q);
		}

		*pagep = page;
		return (0);
	}

	/* A multi-threaded cache can't ever miss. */
	WT_ASSERT(env, !F_ISSET(cache, WT_READONLY));

	WT_PAGE_ALLOC(toc, env, cache, bytes, page);

	WT_STAT_INCR(env->ienv->stats,
	    CACHE_MISS, "cache miss: reads not found in the cache");
	WT_STAT_INCR(idb->stats,
	    DB_CACHE_MISS, "cache miss: reads not found in the cache");

	/* Initialize the page. */
	page->offset = offset;
	page->addr = WT_OFF_TO_ADDR(db, offset);
	page->bytes = bytes;
	TAILQ_INSERT_TAIL(&cache->lqh, page, q);
	TAILQ_INSERT_HEAD(hashq, page, hq);

	/* Read the page. */
	WT_ERR(__wt_read(toc, idb->fh, offset, bytes, page->hdr));

	/* Verify the checksum. */
	if (!LF_ISSET(WT_UNFORMATTED)) {
		hdr = page->hdr;
		u_int32_t checksum = hdr->checksum;
		hdr->checksum = 0;
		if (checksum != __wt_cksum(hdr, bytes)) {
			__wt_db_errx(db,
			    "file offset %llu with length %lu was read and "
			    "had a checksum error",
			    (u_quad)offset, (u_long)bytes);
			ret = WT_ERROR;
			goto err;
		}
	}

	page->ref = 1;
	*pagep = page;
	return (0);

err:	(void)__wt_cache_discard(toc, cache, page);
	return (ret);
}

/*
 * __wt_cache_out --
 *	Unpin bytes of a file, writing as necessary.
 */
int
__wt_cache_out(WT_TOC *toc, WT_PAGE *page, u_int32_t flags)
{
	ENV *env;

	env = toc->env;

	WT_ENV_FCHK(env, "__wt_cache_out", flags, WT_APIMASK_WT_CACHE_OUT);

	/* Check and decrement the reference count. */
	WT_ASSERT(env, page->ref > 0);
	--page->ref;

	/* Set modify flag, and update clean/dirty statistics. */
	if (LF_ISSET(WT_MODIFIED) && !F_ISSET(page, WT_MODIFIED)) {
		F_SET(page, WT_MODIFIED);

		WT_STAT_INCR(
		    env->ienv->stats, CACHE_DIRTY, "dirty pages in the cache");
		WT_STAT_DECR(
		    env->ienv->stats, CACHE_CLEAN, "clean pages in the cache");
	}

	/*
	 * If the page isn't to be retained in the cache, write it if it's
	 * dirty, and discard it.
	 */
	if (LF_ISSET(WT_UNFORMATTED)) {
		if (F_ISSET(page, WT_MODIFIED))
			WT_RET(__wt_cache_write(toc, page));
		WT_RET(__wt_cache_discard(toc, toc->cache, page));
	}

	return (0);
}

/*
 * __wt_cache_clean --
 *	Clear some space out of the cache.
 */
static int
__wt_cache_clean(WT_TOC *toc, u_int32_t bytes, WT_PAGE **pagep)
{
	ENV *env;
	WT_CACHE *cache;
	WT_PAGE *page;
	WT_PAGE_HDR *hdr;
	u_int64_t bytes_free, bytes_need_free;

	/*
	 * !!!
	 * Not currently used, will be needed when we start enforcing cache
	 * memory constraints.
	 */

	*pagep = NULL;

	env = toc->env;
	cache = toc->cache;
	bytes_free = 0;
	bytes_need_free = (u_int64_t)bytes * 3;

	do {
		TAILQ_FOREACH(page, &cache->lqh, q)
			if (page->ref == 0)
				break;
		if (page == NULL)
			break;

		/* Write the page if it's been modified. */
		if (F_ISSET(page, WT_MODIFIED)) {
			WT_STAT_INCR(env->ienv->stats, CACHE_WRITE_EVICT,
			    "dirty pages evicted from the cache");

			WT_RET(__wt_cache_write(toc, page));
		} else
			WT_STAT_INCR(env->ienv->stats, CACHE_EVICT,
			    "clean pages evicted from the cache");

		/* Return the page if it's the right size. */
		if (page->bytes == bytes) {
			TAILQ_REMOVE(&cache->hqh[
			    WT_HASH(cache, page->offset)], page, hq);
			TAILQ_REMOVE(&cache->lqh, page, q);

			/* Clear the page. */
			__wt_bt_page_recycle(env, page);
			hdr = page->hdr;
			memset(hdr, 0, (size_t)bytes);
			memset(page, 0, sizeof(WT_PAGE));
			page->hdr = hdr;
			page->bytes = bytes;

			*pagep = page;
			return (0);
		}

		bytes_free += page->bytes;

		/* Discard the page. */
		WT_RET(__wt_cache_discard(toc, cache, page));
	} while (bytes_free < bytes_need_free);

	return (0);
}

/*
 * __wt_cache_write --
 *	Write a page to the backing database file.
 */
static int
__wt_cache_write(WT_TOC *toc, WT_PAGE *page)
{
	IDB *idb;
	WT_PAGE_HDR *hdr;

	idb = toc->db->idb;

	WT_STAT_INCR(
	    toc->env->ienv->stats, CACHE_WRITE, "writes from the cache");

	/* Update the checksum. */
	hdr = page->hdr;
	hdr->checksum = 0;
	hdr->checksum = __wt_cksum(hdr, page->bytes);

	/* Write, and if successful, clear the modified flag. */
	WT_RET(__wt_write(toc, idb->fh, page->offset, page->bytes, hdr));

	F_CLR(page, WT_MODIFIED);

	WT_STAT_DECR(toc->env->ienv->stats, CACHE_DIRTY, NULL);
	WT_STAT_INCR(toc->env->ienv->stats, CACHE_CLEAN, NULL);

	return (0);
}

/*
 * __wt_cache_discard --
 *	Discard a page of a file.
 */
static int
__wt_cache_discard(WT_TOC *toc, WT_CACHE *cache, WT_PAGE *page)
{
	ENV *env;

	env = toc->env;

	WT_ASSERT(env, cache->cache_bytes >= page->bytes);
	cache->cache_bytes -= page->bytes;

	TAILQ_REMOVE(&cache->hqh[WT_HASH(cache, page->addr)], page, hq);
	TAILQ_REMOVE(&cache->lqh, page, q);

	__wt_bt_page_recycle(env, page);

	WT_STAT_DECR(env->ienv->stats, CACHE_CLEAN, NULL);

	__wt_free(env, page->hdr);
	__wt_free(env, page);
	return (0);
}

#ifdef HAVE_DIAGNOSTIC
/*
 * __wt_cache_dump --
 *	Dump a cache's hash and LRU queues.
 */
static int
__wt_cache_dump(WT_CACHE *cache, char *ofile, FILE *fp)
{
	WT_PAGE *page;
	WT_PAGE_HQH *hashq;
	u_int32_t i;
	int bucket_empty, do_close;
	char *sep;

	/* Optionally dump to a file, else to a stream, default to stdout. */
	do_close = 0;
	if (ofile != NULL) {
		if ((fp = fopen(ofile, "w")) == NULL)
			return (WT_ERROR);
		do_close = 1;
	} else if (fp == NULL)
		fp = stdout;

	fprintf(fp, "LRU: ");
	sep = "";
	TAILQ_FOREACH(page, &cache->lqh, q) {
		fprintf(fp, "%s%lu", sep, (u_long)page->addr);
		sep = ", ";
	}
	fprintf(fp, "\n");
	for (i = 0; i < cache->hashsize; ++i) {
		sep = "";
		bucket_empty = 1;
		hashq = &cache->hqh[i];
		TAILQ_FOREACH(page, hashq, hq) {
			if (bucket_empty) {
				fprintf(fp, "hash bucket %d: ", i);
				bucket_empty = 0;
			}
			fprintf(fp, "%s%lu", sep, (u_long)page->addr);
			sep = ", ";
		}
		if (!bucket_empty)
			fprintf(fp, "\n");
	}

	if (do_close)
		(void)fclose(fp);

	return (0);
}
#endif
