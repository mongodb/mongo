/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008 WiredTiger Software.
 *	All rights reserved.
 *
 * $Id$
 */

#include "wt_internal.h"

static int __wt_cache_clean(ENV *, u_int32_t, WT_PAGE **);
static int __wt_cache_discard(ENV *, WT_PAGE *);
static int __wt_cache_write(ENV *, DB *, WT_PAGE *);

#ifdef HAVE_DIAGNOSTIC
static int __wt_cache_dump(ENV *, char *, FILE *);
#endif

/*
 * __wt_cache_open --
 *	Open the cache.
 */
int
__wt_cache_open(ENV *env)
{
	IENV *ienv;
	u_int32_t i;
	int ret;

	ienv = env->ienv;

	/*
	 * Initialize the cache page queues.  Size for a cache filled with
	 * 16KB pages, and 8 pages per bucket (which works out to 8 buckets
	 * per MB).
	 */
	ienv->hashsize = __wt_prime(env->cachesize * 8);
	if ((ret = __wt_calloc(env,
	    ienv->hashsize, sizeof(ienv->hqh[0]), &ienv->hqh)) != 0)
		return (ret);
	for (i = 0; i < ienv->hashsize; ++i)
		TAILQ_INIT(&ienv->hqh[i]);
	TAILQ_INIT(&ienv->lqh);

	ienv->cache_bytes_max = env->cachesize * WT_MEGABYTE;

	return (0);
}

/*
 * __wt_cache_close --
 *	Close the cache.
 */
int
__wt_cache_close(ENV *env)
{
	IENV *ienv;
	WT_PAGE *page;
	int ret, tret;

	ienv = env->ienv;
	ret = 0;

	/* Discard pages. */
	while ((page = TAILQ_FIRST(&ienv->lqh)) != NULL) {
		/* There shouldn't be any pinned pages. */
		WT_ASSERT(env, page->ref == 0);

		if ((tret = __wt_cache_discard(env, page)) != 0 && ret == 0)
			ret = tret;
	}

	/* Discard buckets. */
	__wt_free(env, ienv->hqh);

	/* There shouldn't be any allocated bytes. */
	WT_ASSERT(env, ienv->cache_bytes == 0);

	return (ret);
}

/*
 * __wt_cache_db_open --
 *	Open an underlying database file in the cache.
 */
int
__wt_cache_db_open(DB *db)
{
	WT_FH *fh;
	DB *tdb;
	ENV *env;
	IDB *idb;

	env = db->env;
	idb = db->idb;

	/* Increment the reference count if we already have the file open. */
	TAILQ_FOREACH(tdb, &env->dbqh, q)
		if ((fh = tdb->idb->fh) != NULL &&
		    strcmp(fh->name, idb->dbname) == 0) {
			++fh->refcnt;
			idb->fh = fh;
			return (0);
		}

	/* Open the fle. */
	return (__wt_open(env, idb->dbname, idb->mode,
	    F_ISSET(idb, WT_CREATE) ? WT_CREATE : 0, &idb->fh));
}

/*
 * __wt_cache_db_close --
 *	Close an underlying database file in the cache.
 */
int
__wt_cache_db_close(DB *db)
{
	ENV *env;
	IDB *idb;
	IENV *ienv;
	WT_PAGE *page;
	int ret, tret;

	env = db->env;
	ienv = db->ienv;
	idb = db->idb;
	ret = 0;

	/* Write any modified pages, discard pages. */
	while ((page = TAILQ_FIRST(&ienv->lqh)) != NULL) {
		/* Ignore other files */
		if (page->file_id != idb->file_id)
			continue;

		/* There shouldn't be any pinned pages. */
		WT_ASSERT(env, page->ref == 0);

		if (F_ISSET(page, WT_MODIFIED) &&
		    (tret = __wt_cache_write(env, db, page)) != 0 && ret == 0)
			ret = tret;
		if ((tret = __wt_cache_discard(env, page)) != 0 && ret == 0)
			ret = tret;
	}

	if ((tret = __wt_close(env, idb->fh)) != 0 && ret == 0)
		ret = tret;
		
	idb->fh = NULL;
	return (ret);
}

/*
 * __wt_cache_db_sync --
 *	Flush an underlying database file to disk.
 */
int
__wt_cache_db_sync(DB *db)
{
	IDB *idb;
	IENV *ienv;
	ENV *env;
	WT_PAGE *page;
	int ret;

	env = db->env;
	ienv = db->ienv;
	idb = db->idb;

	/* Write any modified pages. */
	TAILQ_FOREACH(page, &ienv->lqh, q) {
		/* Ignore other files */
		if (page->file_id != idb->file_id)
			continue;

		/* There shouldn't be any pinned pages. */
		WT_ASSERT(env, page->ref == 0);

		if (F_ISSET(page, WT_MODIFIED) &&
		    (ret = __wt_cache_write(env, db, page)) != 0)
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
#define	WT_PAGE_ALLOC(env, bytes, page, ret) do {			\
	if (((ret) = __wt_calloc(					\
	    (env), 1, sizeof(WT_PAGE), &(page))) != 0)			\
		return ((ret));						\
	if (((ret) = __wt_calloc(					\
	    (env), 1, (size_t)(bytes), &(page)->hdr)) != 0) {		\
		__wt_free((env), (page));				\
		return ((ret));						\
	}								\
	(env)->ienv->cache_bytes += (bytes);				\
	WT_STAT_INCR((env)->hstats, CACHE_CLEAN, NULL);			\
} while (0)

/*
 * __wt_cache_db_alloc --
 *	Allocate bytes from a file.
 */
int
__wt_cache_db_alloc(DB *db, u_int32_t bytes, WT_PAGE **pagep)
{
	ENV *env;
	IDB *idb;
	IENV *ienv;
	WT_PAGE *page;
	int ret;

	*pagep = NULL;

	env = db->env;
	ienv = db->ienv;
	idb = db->idb;

	WT_ASSERT(env, bytes % WT_FRAGMENT == 0);

	WT_STAT_INCR(env->hstats, CACHE_ALLOC, "pages allocated in the cache");
	WT_STAT_INCR(
	    db->hstats, DB_CACHE_ALLOC, "pages allocated in the cache");

	/* Check for exceeding the size of the cache. */
	if (ienv->cache_bytes > ienv->cache_bytes_max) {
		/*
		 * __wt_cache_clean will pass us back a page it free'd, if
		 * it's of the correct size.
		 */
		if ((ret = __wt_cache_clean(env, bytes, &page)) != 0)
			return (ret);
	} else
		page = NULL;

	if (page == NULL)
		WT_PAGE_ALLOC(env, bytes, page, ret);

	/* Initialize the page. */
	page->offset = idb->fh->file_size;
	page->addr = WT_OFF_TO_ADDR(db, page->offset);
	page->bytes = bytes;
	page->file_id = idb->file_id;
	page->ref = 1;
	TAILQ_INSERT_TAIL(&ienv->lqh, page, q);
	TAILQ_INSERT_HEAD(&ienv->hqh[WT_HASH(ienv, page->offset)], page, hq);

	idb->fh->file_size += bytes;

	*pagep = page;
	return (0);
}

/*
 * __wt_cache_db_in --
 *	Pin bytes of a file, reading as necessary.
 */
int
__wt_cache_db_in(DB *db,
    off_t offset, u_int32_t bytes, u_int32_t flags, WT_PAGE **pagep)
{
	ENV *env;
	IDB *idb;
	IENV *ienv;
	WT_PAGE *page;
	WT_PAGE_HDR *hdr;
	WT_PAGE_HQH *hashq;
	int ret;

	*pagep = NULL;

	WT_DB_FCHK(db, "__wt_cache_in", flags, WT_APIMASK_WT_CACHE_DB_IN);

	env = db->env;
	ienv = db->ienv;
	idb = db->idb;

	WT_ASSERT(env, bytes % WT_FRAGMENT == 0);

	/* Check for the page in the cache. */
	hashq = &ienv->hqh[WT_HASH(ienv, offset)];
	TAILQ_FOREACH(page, hashq, hq)
		if (page->offset == offset && page->file_id == idb->file_id)
			break;
	if (page != NULL) {
		++page->ref;

		/* Move to the head of the hash queue */
		TAILQ_REMOVE(hashq, page, hq);
		TAILQ_INSERT_HEAD(hashq, page, hq);

		/* Move to the tail of the LRU queue. */
		TAILQ_REMOVE(&ienv->lqh, page, q);
		TAILQ_INSERT_TAIL(&ienv->lqh, page, q);

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

	/* Check for exceeding the size of the cache. */
	if (ienv->cache_bytes > ienv->cache_bytes_max) {
		/*
		 * __wt_cache_clean will pass us back a page it free'd, if
		 * it's of the correct size.
		 */
		if ((ret = __wt_cache_clean(env, bytes, &page)) != 0)
			return (ret);
	} else
		page = NULL;

	if (page == NULL)
		WT_PAGE_ALLOC(env, bytes, page, ret);

	/* Initialize the page. */
	page->offset = offset;
	page->addr = WT_OFF_TO_ADDR(db, offset);
	page->bytes = bytes;
	page->file_id = idb->file_id;
	page->ref = 1;
	TAILQ_INSERT_TAIL(&ienv->lqh, page, q);
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
			    "file offset %lu with length %lu was read and "
			    "had a checksum error",
			    (u_long)offset, (u_long)bytes);
			ret = WT_ERROR;
			goto err;
		}
	}

	*pagep = page;
	return (0);

err:	(void)__wt_cache_discard(env, page);
	return (ret);
}

/*
 * __wt_cache_db_out --
 *	Unpin bytes of a file, writing as necessary.
 */
int
__wt_cache_db_out(DB *db, WT_PAGE *page, u_int32_t flags)
{
	ENV *env;
	int ret;

	env = db->env;

	WT_DB_FCHK(db, "__wt_cache_db_out", flags, WT_APIMASK_WT_CACHE_DB_OUT);

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
		    (ret = __wt_cache_write(env, db, page)) != 0)
			return (ret);
		if ((ret = __wt_cache_discard(env, page)) != 0)
			return (ret);
	}

	return (0);
}

/*
 * __wt_cache_clean --
 *	Clear some space out of the cache.
 */
static int
__wt_cache_clean(ENV *env, u_int32_t bytes, WT_PAGE **pagep)
{
	IENV *ienv;
	WT_PAGE *page;
	WT_PAGE_HDR *hdr;
	u_int64_t bytes_free, bytes_need_free;
	int ret;

	*pagep = NULL;

	ienv = env->ienv;
	ret = 0;

	bytes_free = 0;
	bytes_need_free = bytes * 3;

	for (;;) {
		TAILQ_FOREACH(page, &ienv->lqh, q)
			if (page->ref == 0)
				break;
		if (page == NULL)
			break;

		/* Write the page if it's been modified. */
		if (F_ISSET(page, WT_MODIFIED)) {
			WT_STAT_INCR(env->hstats, CACHE_WRITE_EVICT,
			    "dirty pages evicted from the cache");

			if ((ret = __wt_cache_write(env, NULL, page)) != 0)
				return (ret);
		} else
			WT_STAT_INCR(env->hstats, CACHE_EVICT,
			    "clean pages evicted from the cache");

		/* Return the page if it's the right size. */
		if (page->bytes == bytes) {
			TAILQ_REMOVE(
			    &ienv->hqh[WT_HASH(ienv, page->offset)], page, hq);
			TAILQ_REMOVE(&ienv->lqh, page, q);

			/* Clear the page. */
			if (page->indx != NULL)
			    __wt_bt_page_recycle(env, page, 0);
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
		if ((ret = __wt_cache_discard(env, page)) != 0)
			break;

		/*
		 * We return any page we find of the right size, so getting
		 * here means there were pages at the head of the LRU chain
		 * that weren't useful, nor were they the right size.   So,
		 * let's free some memory -- keep going until we've free'd
		 * 3x what we need.
		 */
		if (bytes_free > bytes_need_free &&
		    ienv->cache_bytes <= ienv->cache_bytes_max)
			break;
	}
	return (ret);
}

/*
 * __wt_cache_write --
 *	Write a page to the backing database file.
 */
static int
__wt_cache_write(ENV *env, DB *db, WT_PAGE *page)
{
	WT_PAGE_HDR *hdr;
	int ret;

	WT_STAT_INCR(env->hstats, CACHE_WRITE, "writes from the cache");

	/* If not included, find the underlying DB handle. */
	if (db == NULL) {
		TAILQ_FOREACH(db, &env->dbqh, q)
			if (page->file_id == db->idb->file_id)
				break;
		WT_ASSERT(env, db != NULL);
	}

	/* Update the checksum. */
	hdr = page->hdr;
	hdr->checksum = 0;
	hdr->checksum = __wt_cksum(hdr, page->bytes);

	/* Write, and if successful, clear the modified flag. */
	if ((ret = __wt_write(
	    env, db->idb->fh, page->offset, page->bytes, hdr)) == 0) {
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
static int
__wt_cache_discard(ENV *env, WT_PAGE *page)
{
	IENV *ienv;

	ienv = env->ienv;

	WT_ASSERT(env, page->ref == 0);

	WT_ASSERT(env, ienv->cache_bytes >= page->bytes);
	ienv->cache_bytes -= page->bytes;

	TAILQ_REMOVE(&ienv->hqh[WT_HASH(ienv, page->addr)], page, hq);
	TAILQ_REMOVE(&ienv->lqh, page, q);

	if (page->indx != NULL)
		__wt_bt_page_recycle(env, page, 1);

	WT_STAT_DECR(env->hstats, CACHE_CLEAN, NULL);

	__wt_free(env, page->hdr);
	__wt_free(env, page);
	return (0);
}

#ifdef HAVE_DIAGNOSTIC
/*
 * __wt_cache_dump --
 *	Dump the current hash and LRU queues.
 */
static int
__wt_cache_dump(ENV *env, char *ofile, FILE *fp)
{
	IENV *ienv;
	WT_PAGE *page;
	WT_PAGE_HQH *hashq;
	int bucket_empty, do_close, i;
	char *sep;

	ienv = env->ienv;

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
	TAILQ_FOREACH(page, &ienv->lqh, q) {
		fprintf(fp, "%s%lu", sep, (u_long)page->addr);
		sep = ", ";
	}
	fprintf(fp, "\n");
	for (i = 0; i < ienv->hashsize; ++i) {
		sep = "";
		bucket_empty = 1;
		hashq = &ienv->hqh[i];
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
