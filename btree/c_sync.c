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
__wt_cache_open(DB *db)
{
	WT_STOC *stoc;
	ENV *env;
	IDB *idb;
	u_int32_t i;
	int ret;

	env = db->env;
	idb = db->idb;
	stoc = idb->stoc;

	/*
	 * Initialize the cache page queues.  Size for a cache filled with
	 * 16KB pages, and 8 pages per bucket (which works out to 8 buckets
	 * per MB).
	 */
	stoc->hashsize = __wt_prime(env->cachesize * 8);
	if ((ret = __wt_calloc(env,
	    stoc->hashsize, sizeof(stoc->hqh[0]), &stoc->hqh)) != 0)
		return (ret);
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
__wt_cache_close(DB *db)
{
	ENV *env;
	IDB *idb;
	WT_PAGE *page;
	WT_STOC *stoc;
	int ret, tret;

	env = db->env;
	idb = db->idb;
	stoc = idb->stoc; 
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

		if (F_ISSET(page, WT_MODIFIED) &&
		    (tret = __wt_cache_write(stoc, page)) != 0 && ret == 0)
			ret = tret;
		if ((tret = __wt_cache_discard(stoc, page)) != 0 && ret == 0)
			ret = tret;
	}

	/* There shouldn't be any allocated bytes. */
	WT_ASSERT(env, stoc->cache_bytes == 0);

	/* Close the underlying file handle. */
	if ((tret = __wt_close(env, idb->fh)) != 0 && ret == 0)
		ret = tret;
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
__wt_cache_sync(DB *db)
{
	ENV *env;
	WT_PAGE *page;
	WT_STOC *stoc;
	int ret;

	env = db->env;
	stoc = db->idb->stoc;

	/*
	 * BUG:
	 * Walk the list of STOCs, find all of them associated with this
	 * DB handle and close them.
	 */

	/* Write any modified pages. */
	TAILQ_FOREACH(page, &stoc->lqh, q) {
		/* There shouldn't be any pinned pages. */
		WT_ASSERT(env, page->ref == 0);

		if (F_ISSET(page, WT_MODIFIED) &&
		    (ret = __wt_cache_write(stoc, page)) != 0)
			return (ret);
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
#define	WT_PAGE_ALLOC(env, stoc, bytes, page, ret) do {			\
	if (((ret) = __wt_calloc(					\
	    (env), 1, sizeof(WT_PAGE), &(page))) != 0)			\
		return ((ret));						\
	if (((ret) = __wt_calloc(					\
	    (env), 1, (size_t)(bytes), &(page)->hdr)) != 0) {		\
		__wt_free((env), (page));				\
		return ((ret));						\
	}								\
	(stoc)->cache_bytes += (bytes);					\
	WT_STAT_INCR((env)->hstats, CACHE_CLEAN, NULL);			\
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
	WT_PAGE *page;
	int ret;

	*pagep = NULL;

	idb = stoc->idb;
	db = idb->db;
	env = db->env;

	WT_ASSERT(env, bytes % WT_FRAGMENT == 0);

	WT_STAT_INCR(env->hstats, CACHE_ALLOC, "pages allocated in the cache");
	WT_STAT_INCR(
	    db->hstats, DB_CACHE_ALLOC, "pages allocated in the cache");

	WT_PAGE_ALLOC(env, stoc, bytes, page, ret);

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
	WT_PAGE *page;
	WT_PAGE_HDR *hdr;
	WT_PAGE_HQH *hashq;
	int ret;

	*pagep = NULL;

	idb = stoc->idb;
	db = idb->db;
	env = db->env;

	WT_ASSERT(env, bytes % WT_FRAGMENT == 0);
	WT_ENV_FCHK(env, "__wt_cache_in", flags, WT_APIMASK_WT_CACHE_DB_IN);

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

		WT_STAT_INCR(env->hstats,
		    CACHE_HIT, "cache hit: reads found in the cache");
		WT_STAT_INCR(db->hstats,
		    DB_CACHE_HIT, "cache hit: reads found in the cache");

		*pagep = page;
		return (0);
	}

	WT_STAT_INCR(env->hstats,
	    CACHE_MISS, "cache miss: reads not found in the cache");
	WT_STAT_INCR(db->hstats,
	    DB_CACHE_MISS, "cache miss: reads not found in the cache");

	WT_PAGE_ALLOC(env, stoc, bytes, page, ret);

	/* Initialize the page. */
	page->offset = offset;
	page->addr = WT_OFF_TO_ADDR(db, offset);
	page->bytes = bytes;
	page->ref = 1;
	TAILQ_INSERT_TAIL(&stoc->lqh, page, q);
	TAILQ_INSERT_HEAD(hashq, page, hq);

	/* Read the page. */
	if ((ret = __wt_read(env, idb->fh, offset, bytes, page->hdr)) != 0)
		goto err;

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
	int ret;

	env = stoc->idb->db->env;

	WT_ENV_FCHK(env, "__wt_cache_out", flags, WT_APIMASK_WT_CACHE_DB_OUT);

	/* Check and decrement the reference count. */
	WT_ASSERT(env, page->ref > 0);
	--page->ref;

	/* Set modify flag, and update clean/dirty statistics. */
	if (LF_ISSET(WT_MODIFIED) && !F_ISSET(page, WT_MODIFIED)) {
		F_SET(page, WT_MODIFIED);

		WT_STAT_INCR(
		    env->hstats, CACHE_DIRTY, "dirty pages in the cache");
		WT_STAT_DECR(
		    env->hstats, CACHE_CLEAN, "clean pages in the cache");
	}

	/*
	 * If the page isn't to be retained in the cache, write it if it's
	 * dirty, and discard it.
	 */
	if (LF_ISSET(WT_UNFORMATTED)) {
		if (F_ISSET(page, WT_MODIFIED) &&
		    (ret = __wt_cache_write(stoc, page)) != 0)
			return (ret);
		if ((ret = __wt_cache_discard(stoc, page)) != 0)
			return (ret);
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
	WT_PAGE *page;
	WT_PAGE_HDR *hdr;
	u_int64_t bytes_free, bytes_need_free;
	int ret;

	*pagep = NULL;

	env = stoc->idb->db->env;
	bytes_free = 0;
	bytes_need_free = bytes * 3;
	ret = 0;

	do {
		TAILQ_FOREACH(page, &stoc->lqh, q)
			if (page->ref == 0)
				break;
		if (page == NULL)
			break;

		/* Write the page if it's been modified. */
		if (F_ISSET(page, WT_MODIFIED)) {
			WT_STAT_INCR(env->hstats, CACHE_WRITE_EVICT,
			    "dirty pages evicted from the cache");

			if ((ret = __wt_cache_write(stoc, page)) != 0)
				return (ret);
		} else
			WT_STAT_INCR(env->hstats, CACHE_EVICT,
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
		if ((ret = __wt_cache_discard(stoc, page)) != 0)
			break;
	} while (bytes_free < bytes_need_free);

	return (ret);
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
	WT_PAGE_HDR *hdr;
	int ret;

	idb = stoc->idb;
	env = idb->db->env;

	WT_STAT_INCR(env->hstats, CACHE_WRITE, "writes from the cache");

	/* Update the checksum. */
	hdr = page->hdr;
	hdr->checksum = 0;
	hdr->checksum = __wt_cksum(hdr, page->bytes);

	/* Write, and if successful, clear the modified flag. */
	if ((ret = __wt_write(
	    env, idb->fh, page->offset, page->bytes, hdr)) == 0) {
		F_CLR(page, WT_MODIFIED);

		WT_STAT_DECR(env->hstats, CACHE_DIRTY, NULL);
		WT_STAT_INCR(env->hstats, CACHE_CLEAN, NULL);
	}

	return (ret);
}

/*
 * __wt_cache_discard --
 *	Discard a page of a file.
 */
int
__wt_cache_discard(WT_STOC *stoc, WT_PAGE *page)
{
	ENV *env;

	env = stoc->idb->db->env;

	WT_ASSERT(env, page->ref == 0);

	WT_ASSERT(env, stoc->cache_bytes >= page->bytes);
	stoc->cache_bytes -= page->bytes;

	TAILQ_REMOVE(&stoc->hqh[WT_HASH(stoc, page->addr)], page, hq);
	TAILQ_REMOVE(&stoc->lqh, page, q);

	__wt_bt_page_recycle(env, page);

	WT_STAT_DECR(env->hstats, CACHE_CLEAN, NULL);

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
	int bucket_empty, do_close, i;
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
