/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008 WiredTiger Software.
 *	All rights reserved.
 *
 * $Id$
 */

#include "wt_internal.h"

static int __wt_cache_clean(ENV *);
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
	u_int32_t buckets, i;
	int ret;

	ienv = env->ienv;

	/*
	 * Initialize the cache page queues.  Size for a cache filled with
	 * 16KB pages, and 8 pages per bucket (which works out to 8 buckets
	 * per MB).
	 */
	buckets = env->cachesize * 8;
	ienv->hashsize = __wt_prime(buckets);
	if ((ret =
	    __wt_calloc(ienv, buckets, sizeof(ienv->hqh[0]), &ienv->hqh)) != 0)
		return (ret);
	for (i = 0; i < buckets; ++i)
		TAILQ_INIT(&ienv->hqh[i]);
	TAILQ_INIT(&ienv->lqh);

	/* We track cache utilization in units of 512B */
	ienv->cache_frags_max = env->cachesize * (MEGABYTE / 512);

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
	IDB *idb;
	WT_PAGE *page;
	int ret, tret;

	ienv = env->ienv;
	ret = 0;

	/* Discard pages. */
	while ((page = TAILQ_FIRST(&ienv->lqh)) != NULL) {
		/* There shouldn't be any pinned pages. */
		WT_ASSERT(ienv, page->ref == 0);

		if ((tret = __wt_cache_discard(env, page)) != 0 && ret == 0)
			ret = tret;
	}

	/* There shouldn't be any allocated fragments. */
	WT_ASSERT(ienv, ienv->cache_frags == 0);
}

/*
 * __wt_cache_db_open --
 *	Open an underlying database file in the cache.
 */
int
__wt_cache_db_open(DB *db)
{
	IDB *idb;
	IENV *ienv;
	off_t size;
	int ret;

	ienv = db->ienv;
	idb = db->idb;

	/* Try and open the fle. */
	if ((ret = __wt_open(ienv, idb->file_name, idb->mode,
	    F_ISSET(idb, WT_CREATE) ? WT_OPEN_CREATE : 0, &idb->fh)) != 0)
		return (ret);

	if ((ret = __wt_filesize(ienv, idb->fh, &size)) != 0)
		goto err;

	/* Convert the size in bytes to "fragments". */
	idb->frags = (u_int32_t)(size / db->fragsize);

	return (0);

err:	(void)__wt_close(ienv, idb->fh);
	return (ret);
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
		if (page->fileid != idb->fileid)
			continue;

		/* There shouldn't be any pinned pages. */
		WT_ASSERT(ienv, page->ref == 0);

		if (F_ISSET(page, WT_MODIFIED) &&
		    (tret = __wt_cache_write(env, db, page)) != 0 && ret == 0)
			ret = tret;
		if ((tret = __wt_cache_discard(env, page)) != 0 && ret == 0)
			ret = tret;
	}

	return (__wt_close(ienv, idb->fh));
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
		if (page->fileid != idb->fileid)
			continue;

		/* There shouldn't be any pinned pages. */
		WT_ASSERT(ienv, page->ref == 0);

		if (F_ISSET(page, WT_MODIFIED) &&
		    (ret = __wt_cache_write(env, db, page)) != 0)
			return (ret);
	}
	return (0);
}

/*
 * __wt_cache_db_alloc --
 *	Allocate fragments from a file.
 */
int
__wt_cache_db_alloc(DB *db, u_int32_t frags, WT_PAGE **pagep)
{
	ENV *env;
	IDB *idb;
	IENV *ienv;
	WT_PAGE *page;
	int ret;

	env = db->env;
	ienv = db->ienv;
	idb = db->idb;

	*pagep = NULL;

	/* Check for an inability to grow the file. */
	if (UINT32_MAX - idb->frags < frags) {
		__wt_db_errx(db,
		    "Requested additional space is not available; the file"
		    " cannot grow that much");
		return (WT_ERROR);
	}

	/* Check for exceeding the size of the cache. */
	while (ienv->cache_frags > ienv->cache_frags_max)
		if ((ret = __wt_cache_clean(env)) != 0)
			return (ret);

	/*
	 * Allocate memory for the in-memory page information and for the page
	 * itself.  They're separate allocation calls so we get better alignment
	 * from the underlying heap memory allocator.  Clear the memory because
	 * code depends on values being 0 when starting.
	 *
	 * Initialize the in-memory page structure.
	 */
	if ((ret = __wt_calloc(ienv, 1, sizeof(WT_PAGE), &page)) != 0)
		return (ret);
	if ((ret = __wt_calloc(
	    ienv, 1, (size_t)WT_FRAGS_TO_BYTES(db, frags), &page->hdr)) != 0) {
		__wt_free(ienv, page);
		return (ret);
	}
	page->fileid = idb->fileid;
	page->addr = idb->frags;
	idb->frags += frags;
	page->frags = frags;
	ienv->cache_frags += frags;
	page->ref = 1;
	TAILQ_INSERT_TAIL(&ienv->lqh, page, q);
	TAILQ_INSERT_HEAD(&ienv->hqh[WT_HASH(ienv, page->addr)], page, hq);

	__wt_page_inmem_alloc(db, page);

	WT_STAT_INCR(env, CACHE_ALLOC, "pages allocated in the cache");

	*pagep = page;
	return (0);
}

/*
 * __wt_cache_db_in --
 *	Pin a fragment of a file, reading as necessary.
 */
int
__wt_cache_db_in(DB *db,
    u_int32_t addr, u_int32_t frags, WT_PAGE **pagep, u_int32_t flags)
{
	ENV *env;
	IDB *idb;
	IENV *ienv;
	WT_PAGE *page;
	WT_PAGE_HDR *hdr;
	WT_PAGE_HQH *hashq;
	size_t bytes;
	int ret;

	DB_FLAG_CHK(db, "__wt_cache_in", flags, WT_APIMASK_WT_CACHE_DB_IN);

	env = db->env;
	ienv = db->ienv;
	idb = db->idb;

	*pagep = NULL;

	/* Check for the page in the cache. */
	hashq = &ienv->hqh[WT_HASH(ienv, addr)];
	TAILQ_FOREACH(page, hashq, hq)
		if (page->addr == addr && page->fileid == idb->fileid)
			break;
	if (page != NULL) {
		++page->ref;

		/* Move to the head of the hash queue */
		TAILQ_REMOVE(hashq, page, hq);
		TAILQ_INSERT_HEAD(hashq, page, hq);

		/* Move to the tail of the LRU queue. */
		TAILQ_REMOVE(&ienv->lqh, page, q);
		TAILQ_INSERT_TAIL(&ienv->lqh, page, q);

		WT_STAT_INCR(env, CACHE_HIT, "reads found in the cache");

		*pagep = page;
		return (0);
	}

	/* Check for exceeding the size of the cache. */
	while (ienv->cache_frags > ienv->cache_frags_max)
		if ((ret = __wt_cache_clean(env)) != 0)
			return (ret);

	/* Allocate the memory to hold a new page. */
	bytes = (size_t)WT_FRAGS_TO_BYTES(db, frags);
	if ((ret = __wt_malloc(ienv, bytes, &hdr)) != 0)
		return (ret);

	/* Read the page. */
	if ((ret = __wt_read(ienv,
	    idb->fh, WT_FRAGS_TO_BYTES(db, addr), bytes, hdr)) != 0)
		goto err;

	/* Verify the checksum. */
	if (!LF_ISSET(WT_NO_CHECKSUM)) {
		u_int32_t checksum = hdr->checksum;
		hdr->checksum = 0;
		if (checksum != __wt_cksum(hdr, bytes)) {
			__wt_db_errx(db,
			    "fragment %lu was read and had a checksum error",
			    (u_long)addr);
			ret = WT_ERROR;
			goto err;
		}
	}

	/* Allocate and initialize the in-memory page structure. */
	if ((ret = __wt_calloc(ienv, 1, sizeof(WT_PAGE), &page)) != 0)
		goto err;
	page->fileid = idb->fileid;
	page->addr = addr;
	page->frags = frags;
	page->hdr = hdr;
	ienv->cache_frags += frags;
	page->ref = 1;
	TAILQ_INSERT_TAIL(&ienv->lqh, page, q);
	TAILQ_INSERT_HEAD(hashq, page, hq);

	if (!LF_ISSET(WT_NO_INMEM_PAGE) &&
	    (ret = __wt_page_inmem(db, page)) != 0)
		goto err;

	WT_STAT_INCR(env, CACHE_MISS, "reads not found in the cache");

	WT_ASSERT(ienv, __wt_db_verify_page(db, page, NULL, NULL) == 0);

	*pagep = page;
	return (0);

err:	if (page != NULL)
		__wt_free(ienv, page);
	__wt_free(ienv, hdr);
	return (ret);
}

/*
 * __wt_cache_db_out --
 *	Unpin a fragment of a file, writing as necessary.
 */
int
__wt_cache_db_out(DB *db, WT_PAGE *page, u_int32_t flags)
{
	ENV *env;
	IENV *ienv;
	int ret;

	env = db->env;
	ienv = db->ienv;

	DB_FLAG_CHK(db, "__wt_cache_db_out", flags, WT_APIMASK_WT_CACHE_DB_OUT);

	/* Check and decrement the reference count. */
	WT_ASSERT(ienv, page->ref > 0);
	--page->ref;

	WT_ASSERT(ienv, __wt_db_verify_page(db, page, NULL, NULL) == 0);

	/* If the page is dirty, set the modified flag. */
	if (LF_ISSET(WT_MODIFIED)) {
		F_SET(page, WT_MODIFIED);
		WT_STAT_INCR(env, CACHE_DIRTY, "dirty pages in the cache");
	}

	/*
	 * If the page is not dirty and marked worthless, discard it.  (We
	 * can't discard modified pages, some thread of control thinks it's
	 * useful...).
	 */
	if (LF_ISSET(WT_DISCARD) &&
	    !F_ISSET(page, WT_MODIFIED) &&
	    (ret = __wt_cache_discard(env, page)) != 0)
		return (ret);

	return (0);
}

/*
 * __wt_cache_clean --
 *	Clear some space out of the cache.
 */
static int
__wt_cache_clean(ENV *env)
{
	IENV *ienv;
	WT_PAGE *page;
	int ret;

	ienv = env->ienv;

	/* Find an unpinned page to discard. */
	TAILQ_FOREACH_REVERSE(page, &ienv->lqh, __wt_page_lqh, q)
		if (page->ref == 0)
			break;

	/* Write the page if it's been modified. */
	if (F_ISSET(page, WT_MODIFIED)) {
		WT_STAT_INCR(env, CACHE_WRITE_EVICT,
		    "dirty pages evicted from the cache");

		if ((ret = __wt_cache_write(env, NULL, page)) != 0)
			return (ret);
	} else
		WT_STAT_INCR(env,
		    CACHE_EVICT, "clean pages evicted from the cache");

	/* Discard the page. */
	return (__wt_cache_discard(env, page));
}

/*
 * __wt_cache_write --
 *	Write a page to the backing database file.
 */
static int
__wt_cache_write(ENV *env, DB *db, WT_PAGE *page)
{
	IDB *idb;
	IENV *ienv;
	WT_PAGE_HDR *hdr;
	size_t bytes;
	int ret;

	ienv = env->ienv;

	WT_STAT_INCR(env, CACHE_WRITE, "writes from the cache");
	WT_STAT_DECR(env, CACHE_DIRTY, NULL);

	/* If not included, find the underlying DB handle. */
	if (db == NULL) {
		TAILQ_FOREACH(db, &env->dbqh, q)
			if (page->fileid == db->idb->fileid)
				break;
		WT_ASSERT(ienv, db != NULL);
	}
	idb = db->idb;

	/* Update the checksum. */
	bytes = (size_t)WT_FRAGS_TO_BYTES(db, page->frags);
	hdr = page->hdr;
	hdr->checksum = 0;
	hdr->checksum = __wt_cksum(hdr, bytes);

	/* Write, and if successful, clear the modified flag. */
	if ((ret = __wt_write(ienv, idb->fh,
	    (off_t)WT_FRAGS_TO_BYTES(db, page->addr), bytes, hdr)) == 0)
		F_CLR(page, WT_MODIFIED);

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
	WT_INDX *indx;
	u_int32_t i;

	ienv = env->ienv;

	WT_ASSERT(ienv, page->ref == 0);

	if (ienv->cache_frags < page->frags) {
		__wt_env_errx(env, "allocated cache size went negative");
		return (WT_ERROR);
	}
	ienv->cache_frags -= page->frags;

	TAILQ_REMOVE(&ienv->hqh[WT_HASH(ienv, page->addr)], page, hq);
	TAILQ_REMOVE(&ienv->lqh, page, q);

	if (page->indx != NULL) {
		if (F_ISSET(page, WT_ALLOCATED))
			WT_INDX_FOREACH(page, indx, i)
				if (F_ISSET(indx, WT_ALLOCATED))
					__wt_free(ienv, indx->data);
		__wt_free(ienv, page->indx);
	}
	__wt_free(ienv, page->hdr);
	__wt_free(ienv, page);
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
