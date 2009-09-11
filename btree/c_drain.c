/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008 WiredTiger Software.
 *	All rights reserved.
 *
 * $Id$
 */

#include "wt_internal.h"

static int __wt_cache_clean(WT_STOC *, u_int32_t, WT_PAGE **);
static int __wt_cache_write(WT_STOC *, WT_PAGE *);

#ifdef HAVE_DIAGNOSTIC
static int __wt_cache_dump(WT_STOC *, char *, FILE *);
#endif

/*
 * __wt_cache_open --
 *	Open an underlying database file in a cache.
 */
int
__wt_cache_open(WT_STOC *stoc)
{
	ENV *env;
	IDB *idb;
	u_int32_t i;

	env = stoc->env;
	idb = stoc->db->idb;

	/*
	 * Initialize the cache page queues.  Size for a cache filled with
	 * 16KB pages, and 8 pages per bucket (which works out to 8 buckets
	 * per MB).
	 */
	stoc->hashsize = __wt_prime(env->cachesize * 8);
	WT_RET(__wt_calloc(
	    env, stoc->hashsize, sizeof(stoc->hqh[0]), &stoc->hqh));
	for (i = 0; i < stoc->hashsize; ++i)
		TAILQ_INIT(&stoc->hqh[i]);
	TAILQ_INIT(&stoc->lqh);

	/* Open the fle. */
	return (__wt_open(env, idb->dbname, idb->mode,
	    F_ISSET(idb, WT_CREATE) ? WT_CREATE : 0, &idb->fh));
}

/*
 * __wt_cache_close --
 *	Close an underlying database file in a cache.
 */
int
__wt_cache_close(WT_STOC *stoc)
{
	ENV *env;
	IDB *idb;
	WT_PAGE *page;
	int ret;

	env = stoc->env;
	idb = stoc->db->idb;
	ret = 0;

	/*
	 * BUG:
	 * Walk the list of STOCs, find all of them associated with this
	 * DB handle and close them.
	 */

	/* Write any modified pages, discard pages. */
	while ((page = TAILQ_FIRST(&stoc->lqh)) != NULL) {
		/* There shouldn't be any pinned pages. */
		WT_ASSERT(env, page->ref == 0);

		if (F_ISSET(page, WT_MODIFIED))
			WT_TRET(__wt_cache_write(stoc, page));
		WT_TRET(__wt_cache_discard(stoc, page));
	}

	/* There shouldn't be any allocated bytes. */
	WT_ASSERT(env, stoc->cache_bytes == 0);

	/* Close the underlying file handle. */
	WT_TRET(__wt_close(env, idb->fh));
	idb->fh = NULL;

	/* Discard buckets. */
	WT_FREE_AND_CLEAR(env, stoc->hqh);
		
	return (ret);
}

/*
 * __wt_cache_db_sync --
 *	Flush an underlying database file to disk.
 */
int
__wt_cache_sync(WT_STOC *stoc)
{
	ENV *env;
	WT_PAGE *page;

	env = stoc->env;

	/* Write any modified pages. */
	TAILQ_FOREACH(page, &stoc->lqh, q) {
		/* There shouldn't be any pinned pages. */
		WT_ASSERT(env, page->ref == 0);

		if (F_ISSET(page, WT_MODIFIED))
			WT_RET(__wt_cache_write(stoc, page));
	}
	return (0);
}

/*
 * __wt_cache_stoc_lru --
 *	Discard cache pages if we're holding too much memory.
 */
int
__wt_cache_stoc_lru(WT_STOC *stoc, ENV *env)
{
	WT_PAGE *page;

	/* Discard pages until we're below our threshold. */
	TAILQ_FOREACH(page, &stoc->lqh, q) {
		if (stoc->cache_bytes <= env->cachesize * WT_MEGABYTE)
			break;
		if (page->ref == 0) {
			if (F_ISSET(page, WT_MODIFIED))
				WT_RET(__wt_cache_write(stoc, page));
			WT_RET(__wt_cache_discard(stoc, page));
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
#define	WT_PAGE_ALLOC(env, stoc, bytes, page) do {			\
	if ((stoc)->cache_bytes > (env)->cachesize * WT_MEGABYTE)	\
		WT_RET(__wt_cache_stoc_lru(stoc, env));			\
	WT_RET(__wt_calloc((env), 1, sizeof(WT_PAGE), &(page)));	\
	{								\
	int __ret;							\
	if (((__ret) = __wt_calloc(					\
	    (env), 1, (size_t)(bytes), &(page)->hdr)) != 0) {		\
		__wt_free((env), (page));				\
		return ((__ret));					\
	}								\
	}								\
	(stoc)->cache_bytes += (bytes);					\
	WT_STAT_INCR((env)->ienv->stats, CACHE_CLEAN, NULL);		\
} while (0)

/*
 * __wt_cache_alloc --
 *	Allocate bytes from a file.
 */
int
__wt_cache_alloc(WT_STOC *stoc, u_int32_t bytes, WT_PAGE **pagep)
{
	DB *db;
	ENV *env;
	IDB *idb;
	IENV *ienv;
	WT_PAGE *page;

	*pagep = NULL;

	db = stoc->db;
	env = stoc->env;
	idb = stoc->db->idb;
	ienv = env->ienv;

	WT_ASSERT(env, bytes % WT_FRAGMENT == 0);

	WT_PAGE_ALLOC(env, stoc, bytes, page);

	WT_STAT_INCR(ienv->stats, CACHE_ALLOC, "pages allocated in the cache");
	WT_STAT_INCR(
	    idb->stats, DB_CACHE_ALLOC, "pages allocated in the cache");

	/* Initialize the page. */
	page->offset = idb->fh->file_size;
	page->addr = WT_OFF_TO_ADDR(db, page->offset);
	page->bytes = bytes;
	page->ref = 1;
	TAILQ_INSERT_TAIL(&stoc->lqh, page, q);
	TAILQ_INSERT_HEAD(&stoc->hqh[WT_HASH(stoc, page->offset)], page, hq);

	idb->fh->file_size += bytes;

	*pagep = page;
	return (0);
}

/*
 * __wt_cache_in --
 *	Pin bytes of a file, reading as necessary.
 */
int
__wt_cache_in(WT_STOC *stoc,
    off_t offset, u_int32_t bytes, u_int32_t flags, WT_PAGE **pagep)
{
	DB *db;
	ENV *env;
	IDB *idb;
	IENV *ienv;
	WT_PAGE *page;
	WT_PAGE_HDR *hdr;
	WT_PAGE_HQH *hashq;
	int ret;

	*pagep = NULL;

	db = stoc->db;
	env = stoc->env;
	idb = db->idb;
	ienv = env->ienv;

	WT_ASSERT(env, bytes % WT_FRAGMENT == 0);
	WT_ENV_FCHK(env, "__wt_cache_in", flags, WT_APIMASK_WT_CACHE_IN);

	/* Check for the page in the cache. */
	hashq = &stoc->hqh[WT_HASH(stoc, offset)];
	TAILQ_FOREACH(page, hashq, hq)
		if (page->offset == offset)
			break;
	if (page != NULL) {
		++page->ref;

		/* Move to the head of the hash queue */
		TAILQ_REMOVE(hashq, page, hq);
		TAILQ_INSERT_HEAD(hashq, page, hq);

		/* Move to the tail of the LRU queue. */
		TAILQ_REMOVE(&stoc->lqh, page, q);
		TAILQ_INSERT_TAIL(&stoc->lqh, page, q);

		WT_STAT_INCR(ienv->stats,
		    CACHE_HIT, "cache hit: reads found in the cache");
		WT_STAT_INCR(idb->stats,
		    DB_CACHE_HIT, "cache hit: reads found in the cache");

		*pagep = page;
		return (0);
	}

	WT_STAT_INCR(ienv->stats,
	    CACHE_MISS, "cache miss: reads not found in the cache");
	WT_STAT_INCR(idb->stats,
	    DB_CACHE_MISS, "cache miss: reads not found in the cache");

	WT_PAGE_ALLOC(env, stoc, bytes, page);

	/* Initialize the page. */
	page->offset = offset;
	page->addr = WT_OFF_TO_ADDR(db, offset);
	page->bytes = bytes;
	page->ref = 1;
	TAILQ_INSERT_TAIL(&stoc->lqh, page, q);
	TAILQ_INSERT_HEAD(hashq, page, hq);

	/* Read the page. */
	WT_ERR(__wt_read(env, idb->fh, offset, bytes, page->hdr));

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

	*pagep = page;
	return (0);

err:	(void)__wt_cache_discard(stoc, page);
	return (ret);
}

/*
 * __wt_cache_out --
 *	Unpin bytes of a file, writing as necessary.
 */
int
__wt_cache_out(WT_STOC *stoc, WT_PAGE *page, u_int32_t flags)
{
	ENV *env;
	IENV *ienv;

	env = stoc->env;
	ienv = env->ienv;

	WT_ENV_FCHK(env, "__wt_cache_out", flags, WT_APIMASK_WT_CACHE_OUT);

	/* Check and decrement the reference count. */
	WT_ASSERT(env, page->ref > 0);
	--page->ref;

	/* Set modify flag, and update clean/dirty statistics. */
	if (LF_ISSET(WT_MODIFIED) && !F_ISSET(page, WT_MODIFIED)) {
		F_SET(page, WT_MODIFIED);

		WT_STAT_INCR(
		    ienv->stats, CACHE_DIRTY, "dirty pages in the cache");
		WT_STAT_DECR(
		    ienv->stats, CACHE_CLEAN, "clean pages in the cache");
	}

	/*
	 * If the page isn't to be retained in the cache, write it if it's
	 * dirty, and discard it.
	 */
	if (LF_ISSET(WT_UNFORMATTED)) {
		if (F_ISSET(page, WT_MODIFIED))
			WT_RET(__wt_cache_write(stoc, page));
		WT_RET(__wt_cache_discard(stoc, page));
	}

	return (0);
}

/*
 * __wt_cache_clean --
 *	Clear some space out of the cache.
 */
static int
__wt_cache_clean(WT_STOC *stoc, u_int32_t bytes, WT_PAGE **pagep)
{
	ENV *env;
	IENV *ienv;
	WT_PAGE *page;
	WT_PAGE_HDR *hdr;
	u_int64_t bytes_free, bytes_need_free;

	*pagep = NULL;

	env = stoc->env;
	ienv = env->ienv;
	bytes_free = 0;
	bytes_need_free = bytes * 3;

	do {
		TAILQ_FOREACH(page, &stoc->lqh, q)
			if (page->ref == 0)
				break;
		if (page == NULL)
			break;

		/* Write the page if it's been modified. */
		if (F_ISSET(page, WT_MODIFIED)) {
			WT_STAT_INCR(ienv->stats, CACHE_WRITE_EVICT,
			    "dirty pages evicted from the cache");

			WT_RET(__wt_cache_write(stoc, page));
		} else
			WT_STAT_INCR(ienv->stats, CACHE_EVICT,
			    "clean pages evicted from the cache");

		/* Return the page if it's the right size. */
		if (page->bytes == bytes) {
			TAILQ_REMOVE(
			    &stoc->hqh[WT_HASH(stoc, page->offset)], page, hq);
			TAILQ_REMOVE(&stoc->lqh, page, q);

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
		WT_RET(__wt_cache_discard(stoc, page));
	} while (bytes_free < bytes_need_free);

	return (0);
}

/*
 * __wt_cache_write --
 *	Write a page to the backing database file.
 */
static int
__wt_cache_write(WT_STOC *stoc, WT_PAGE *page)
{
	ENV *env;
	IDB *idb;
	IENV *ienv;
	WT_PAGE_HDR *hdr;

	env = stoc->env;
	idb = stoc->db->idb;
	ienv = env->ienv;

	WT_STAT_INCR(ienv->stats, CACHE_WRITE, "writes from the cache");

	/* Update the checksum. */
	hdr = page->hdr;
	hdr->checksum = 0;
	hdr->checksum = __wt_cksum(hdr, page->bytes);

	/* Write, and if successful, clear the modified flag. */
	WT_RET(__wt_write(env, idb->fh, page->offset, page->bytes, hdr));

	F_CLR(page, WT_MODIFIED);

	WT_STAT_DECR(ienv->stats, CACHE_DIRTY, NULL);
	WT_STAT_INCR(ienv->stats, CACHE_CLEAN, NULL);

	return (0);
}

/*
 * __wt_cache_discard --
 *	Discard a page of a file.
 */
int
__wt_cache_discard(WT_STOC *stoc, WT_PAGE *page)
{
	ENV *env;
	IENV *ienv;

	env = stoc->env;
	ienv = env->ienv;

	WT_ASSERT(env, page->ref == 0);

	WT_ASSERT(env, stoc->cache_bytes >= page->bytes);
	stoc->cache_bytes -= page->bytes;

	TAILQ_REMOVE(&stoc->hqh[WT_HASH(stoc, page->addr)], page, hq);
	TAILQ_REMOVE(&stoc->lqh, page, q);

	__wt_bt_page_recycle(env, page);

	WT_STAT_DECR(ienv->stats, CACHE_CLEAN, NULL);

	__wt_free(env, page->hdr);
	__wt_free(env, page);
	return (0);
}

#ifdef HAVE_DIAGNOSTIC
/*
 * __wt_cache_dump --
 *	Dump a STOC's hash and LRU queues.
 */
static int
__wt_cache_dump(WT_STOC *stoc, char *ofile, FILE *fp)
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
	TAILQ_FOREACH(page, &stoc->lqh, q) {
		fprintf(fp, "%s%lu", sep, (u_long)page->addr);
		sep = ", ";
	}
	fprintf(fp, "\n");
	for (i = 0; i < stoc->hashsize; ++i) {
		sep = "";
		bucket_empty = 1;
		hashq = &stoc->hqh[i];
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
