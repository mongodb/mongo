/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008 WiredTiger Software.
 *	All rights reserved.
 *
 * $Id$
 */

#include "wt_internal.h"

static int  __wt_cache_clean(WT_TOC *, u_int64_t);
static int  __wt_cache_write(ENV *, WT_PAGE *);
static void __wt_cache_alloc_server(WT_TOC *);
static void __wt_cache_discard(ENV *, WT_PAGE *);
static void __wt_cache_discard_server(WT_TOC *);
static void __wt_cache_in_server(WT_TOC *);

#ifdef HAVE_DIAGNOSTIC
static int __wt_cache_dump(ENV *, char *, FILE *);
#endif

typedef struct __wt_req_alloc {
	DB	 *db;				/* Database reference */
	WT_PAGE  *page;				/* Allocated page */
	u_int32_t bytes;			/* Bytes to allocate */
} WT_REQ_ALLOC;
typedef struct __wt_req_discard {
	WT_PAGE  *page;				/* Discarded page */
} WT_REQ_DISCARD;
typedef struct __wt_req_in {
	DB	 *db;				/* Database reference */
	WT_PAGE  *page;				/* Allocated page */
	u_int32_t bytes;			/* Bytes to allocate */
	off_t	  offset;			/* File offset */
} WT_REQ_IN;

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

	/*
	 * Initialize the cache page queues.  No server support needed, this is
	 * done when the environment is first opened, before there are multiple
	 * threads of control using the cache.
	 *
	 * Size for a cache filled with 16KB pages, and 8 pages per bucket
	 * (which works out to 8 buckets per MB).
	 */
	cache = &ienv->cache;
	cache->hashsize = __wt_prime(env->cachesize * 8);
	cache->hashsize = 32771;
	WT_STAT_SET(ienv->stats, HASH_BUCKETS, "hash buckets", cache->hashsize);
	WT_RET(
	    __wt_calloc(env, cache->hashsize, sizeof(*cache->hb), &cache->hb));

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
	WT_CACHE *cache;
	WT_PAGE *page;
	u_int i;
	int ret;

	cache = &env->ienv->cache;
	ret = 0;

	if (!F_ISSET(cache, WT_INITIALIZED))
		return (0);

	/* Write any modified pages. */
	WT_TRET(__wt_cache_sync(env, NULL));

	/*
	 * Discard all pages.  No server support needed, this is done when the
	 * environment is closed, after all threads of control have exited the
	 * cache.
	 */
	for (i = 0; i < cache->hashsize; ++i)
		while ((page = cache->hb[i]) != NULL) {
			__wt_cache_discard(env, page);

			__wt_bt_page_recycle(env, page);
			WT_STAT_DECR(env->ienv->stats, CACHE_CLEAN, NULL);
		}

	/* There shouldn't be any allocated bytes. */
	WT_ASSERT(env, cache->bytes_alloc == 0);

	/* Discard allocated memory. */
	WT_FREE_AND_CLEAR(env, cache->hb);
	F_SET(cache, WT_INITIALIZED);

	/* It's really, really dead. */
	memset(cache, 0, sizeof(cache));

	return (ret);
}

/*
 * __wt_cache_sync --
 *	Flush an underlying cache to disk.
 */
int
__wt_cache_sync(ENV *env, WT_FH *fh)
{
	WT_CACHE *cache;
	WT_PAGE *page;
	u_int i;

	cache = &env->ienv->cache;

	/*
	 * Write any modified pages -- if the handle is set, write only pages
	 * belong to the specified file.
	 */
	for (i = 0; i < cache->hashsize; ++i)
		for (page = cache->hb[i]; page != NULL; page = page->next) {
			if (!F_ISSET(page, WT_MODIFIED))
				continue;
			if (fh != NULL && fh != page->fh)
				continue;
			WT_RET(__wt_cache_write(env, page));
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
	WT_REQ_ALLOC req;
	int ret;

	*pagep = NULL;

	db = toc->db;
	env = toc->env;
	idb = db->idb;

	WT_ASSERT(env, bytes % WT_FRAGMENT == 0);

	WT_STAT_INCR(
	    env->ienv->stats, CACHE_ALLOC, "pages allocated in the cache");
	WT_STAT_INCR(
	    idb->stats, DB_CACHE_ALLOC, "pages allocated in the cache");

	/*
	 * Allocate memory for the in-memory page information and for the page
	 * itself. They're two separate allocation calls so we (hopefully) get
	 * better alignment from the underlying heap memory allocator.
	 * Clear the memory because code depends on initial values of 0.
	 */
	WT_RET(__wt_calloc(env, 1, sizeof(WT_PAGE), &page));
	WT_ERR(__wt_calloc(env, 1, (size_t)bytes, &page->hdr));
	if (0) {
err:		__wt_free(env, page);
		return (ret);
	}

	/*
	 * Allocate "bytes" bytes from the end of the file; we must serialize
	 * the size of the file, the bytes in the cache and the insert onto
	 * the hash queue, so ask the workQ thread of control to do the work
	 * for us.
	 */
	req.db = db;
	req.page = page;
	req.bytes = bytes;
	WT_TOC_REQUEST(toc, __wt_cache_alloc_server, &req);

	*pagep = page;
	return (0);
}

/*
 * __wt_cache_alloc_server --
 *	Server function to allocate bytes from a file.
 */
static void
__wt_cache_alloc_server(WT_TOC *toc)
{
	DB *db;
	IENV *ienv;
	WT_CACHE *cache;
	WT_FH *fh;
	WT_PAGE *page;
	WT_REQ_ALLOC *req;
	WT_PAGE **hb;
	u_int32_t bytes;

	/* Unpack request. */
	req = toc->request_args;
	db = req->db;
	page = req->page;
	bytes = req->bytes;

	ienv = db->ienv;
	cache = &ienv->cache;
	fh = db->idb->fh;

	/* Initialize the page structure, allocating "bytes" from the file. */
	page->fh = fh;
	page->offset = fh->file_size;
	fh->file_size += bytes;
	page->addr = WT_OFF_TO_ADDR(db, page->offset);
	page->bytes = bytes;
	page->generation = ++ienv->generation;

	/* Insert onto the head of the linked list. */
	hb = &cache->hb[WT_HASH(cache, page->offset)];
	page->next = *hb;
	*hb = page;

	/* Increment total cache byte count. */
	cache->bytes_alloc += bytes;

	WT_STAT_INCR(ienv->stats, CACHE_CLEAN, NULL);
	WT_STAT_INCR(ienv->stats,
	    WORKQ_CACHE_ALLOC_REQUESTS, "workQ cache allocations");

	WT_TOC_REQUEST_COMPLETE(toc);
}

/*
 * __wt_cache_in --
 *	Pin bytes of a file, reading as necessary.
 */
int
__wt_cache_in(WT_TOC *toc,
    off_t offset, u_int32_t bytes, u_int32_t flags, WT_PAGE **pagep)
{
	static u_int longest_bucket_cnt = 0;
	DB *db;
	ENV *env;
	IDB *idb;
	IENV *ienv;
	WT_CACHE *cache;
	WT_PAGE **hb, *page;
	WT_PAGE_HDR *hdr;
	WT_REQ_IN req;
	u_int bucket_cnt;
	int ret;

	*pagep = NULL;

	db = toc->db;
	env = toc->env;
	idb = db->idb;
	ienv = env->ienv;
	cache = &ienv->cache;

	WT_ASSERT(env, bytes % WT_FRAGMENT == 0);

	WT_ENV_FCHK(env, "__wt_cache_in", flags, WT_APIMASK_WT_CACHE_IN);

	/* Check for the page in the cache. */
	hb = &cache->hb[WT_HASH(cache, offset)];
	for (bucket_cnt = 0,
	    page = *hb; page != NULL; ++bucket_cnt, page = page->next)
		if (page->offset == offset && idb->fh == page->fh)
			break;
	if (bucket_cnt > longest_bucket_cnt) {
		WT_STAT_SET(ienv->stats, LONGEST_BUCKET,
		    "longest hash bucket chain search", bucket_cnt);
		longest_bucket_cnt = bucket_cnt;
	}
	if (page != NULL) {
		WT_STAT_INCR(env->ienv->stats,
		    CACHE_HIT, "cache hit: reads found in the cache");
		WT_STAT_INCR(idb->stats,
		    DB_CACHE_HIT, "cache hit: reads found in the cache");

		page->generation = ++ienv->generation;
		*pagep = page;
		return (0);
	}

	WT_STAT_INCR(env->ienv->stats,
	    CACHE_MISS, "cache miss: reads not found in the cache");
	WT_STAT_INCR(idb->stats,
	    DB_CACHE_MISS, "cache miss: reads not found in the cache");

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
err:			if (page->hdr != NULL)
				__wt_free(env, page->hdr);
			__wt_free(env, page);
			return (ret);
		}
	}

	/*
	 * We must serialize the insert onto the hash queue, so ask the workQ
	 * thread of control to do the work for us.
	 */
	req.db = db;
	req.page = page;
	req.bytes = bytes;
	req.offset = offset;
	WT_TOC_REQUEST(toc, __wt_cache_in_server, &req);

	*pagep = page;
	return (0);
}

/*
 * __wt_cache_in_server --
 *	Server function to read bytes from a file.
 */
static void
__wt_cache_in_server(WT_TOC *toc)
{
	DB *db;
	IENV *ienv;
	WT_CACHE *cache;
	WT_FH *fh;
	WT_PAGE *page;
	WT_REQ_IN *req;
	WT_PAGE **hb;
	off_t offset;
	u_int32_t bytes;

	/* Unpack request. */
	req = toc->request_args;
	db = req->db;
	page = req->page;
	bytes = req->bytes;
	offset = req->offset;

	ienv = db->ienv;
	cache = &ienv->cache;
	fh = db->idb->fh;

	/* Initialize the page structure. */
	page->fh = fh;
	page->offset = offset;
	page->addr = WT_OFF_TO_ADDR(db, offset);
	page->bytes = bytes;
	page->generation = ++ienv->generation;

	/* Insert onto the head of the linked list. */
	hb = &cache->hb[WT_HASH(cache, page->offset)];
	page->next = *hb;
	*hb = page;

	/* Increment total cache byte count. */
	cache->bytes_alloc += bytes;

	WT_STAT_INCR(ienv->stats, CACHE_CLEAN, NULL);

	WT_TOC_REQUEST_COMPLETE(toc);
}

/*
 * __wt_cache_out --
 *	Unpin bytes of a file, writing as necessary.
 */
int
__wt_cache_out(WT_TOC *toc, WT_PAGE *page, u_int32_t flags)
{
	ENV *env;
	WT_REQ_DISCARD req;

	env = toc->env;

	WT_ENV_FCHK(env, "__wt_cache_out", flags, WT_APIMASK_WT_CACHE_OUT);

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
			WT_RET(__wt_cache_write(env, page));
		req.page = page;
		WT_TOC_REQUEST(toc, __wt_cache_discard_server, &req);

		__wt_bt_page_recycle(env, page);
		WT_STAT_DECR(env->ienv->stats, CACHE_CLEAN, NULL);
	}

	return (0);
}

#if 0
/*
 * __wt_cache_clean --
 *	Clear some space out of the cache.
 */
static int
__wt_cache_clean(WT_TOC *toc, u_int64_t bytes_to_free)
{
	DB *db;
	ENV *env;
	WT_CACHE *cache;
	WT_PAGE *page;
	WT_PAGE_HDR *hdr;
	u_int64_t bytes;

	db = toc->db;
	env = toc->env;
	cache = toc->cache;

	bytes_free = 0;
	bytes_need_free = (u_int64_t)bytes * 3;

	/*
	 * Free 10% of each cache until we get to our limit, starting at a new
	 * cache each time.
	 */

	do {
		if (page == NULL)
			break;

		/* Write the page if it's been modified. */
		if (F_ISSET(page, WT_MODIFIED)) {
			WT_STAT_INCR(env->ienv->stats, CACHE_WRITE_EVICT,
			    "dirty pages evicted from the cache");

			WT_RET(__wt_cache_write(env, page));
		} else
			WT_STAT_INCR(env->ienv->stats, CACHE_EVICT,
			    "clean pages evicted from the cache");

		bytes += page->bytes;

		WT_RET(__wt_cache_discard(env, page));
	} while (bytes < bytes_to_free

	return (0);
}
#endif

/*
 * __wt_cache_write --
 *	Write a page to the backing database file.
 */
static int
__wt_cache_write(ENV *env, WT_PAGE *page)
{
	WT_PAGE_HDR *hdr;

	/*
	 * BUG!!!
	 * There's a race here, we need to ask the server (or an I/O thread)
	 * for help.
	 */
	/* Update the checksum. */
	hdr = page->hdr;
	hdr->checksum = 0;
	hdr->checksum = __wt_cksum(hdr, page->bytes);

	/* Write, and if successful, clear the modified flag. */
	WT_RET(__wt_write(env, page->fh, page->offset, page->bytes, hdr));

	F_CLR(page, WT_MODIFIED);

	WT_STAT_DECR(env->ienv->stats, CACHE_DIRTY, NULL);
	WT_STAT_INCR(env->ienv->stats, CACHE_CLEAN, NULL);
	WT_STAT_INCR(env->ienv->stats, CACHE_WRITE, "writes from the cache");

	return (0);
}

/*
 * __wt_cache_discard_server --
 *	Sever version: discard a page of a file.
 */
static void
__wt_cache_discard_server(WT_TOC *toc)
{
	__wt_cache_discard(toc->env,
	    ((WT_REQ_DISCARD *)toc->request_args)->page);
	WT_TOC_REQUEST_COMPLETE(toc);
}

/*
 * __wt_cache_discard --
 *	Discard a page of a file.
 */
static void
__wt_cache_discard(ENV *env, WT_PAGE *page)
{
	WT_CACHE *cache;
	WT_PAGE **hb, *tpage;

	cache = &env->ienv->cache;

	WT_ASSERT(env, cache->bytes_alloc >= page->bytes);
	cache->bytes_alloc -= page->bytes;

	hb = &cache->hb[WT_HASH(cache, page->offset)];
	if (*hb == page)
		*hb = page->next;
	else
		for (tpage = *hb;; tpage = tpage->next)
			if (tpage->next == page) {
				tpage->next = page->next;
				break;
			}
}

#ifdef HAVE_DIAGNOSTIC
/*
 * __wt_cache_dump --
 *	Dump a cache.
 */
static int
__wt_cache_dump(ENV *env, char *ofile, FILE *fp)
{
	WT_CACHE *cache;
	WT_PAGE *page;
	u_long page_total;
	u_int32_t i;
	int do_close, page_count;
	char *sep;

	WT_RET(__wt_bt_dump_set_fp(ofile, &fp, &do_close));

	fprintf(fp, "Cache dump: ==================\n");
	page_total = 0;
	cache = &env->ienv->cache;
	for (i = 0; i < cache->hashsize; ++i) {
		sep = "";
		page_count = 0;
		for (page = cache->hb[i]; page != NULL; page = page->next) {
			++page_total;
			if (page_count == 0) {
				fprintf(fp, "hash bucket %3d: ", i);
				page_count = 1;
			}
			fprintf(fp, "%s%lu", sep, (u_long)page->addr);
			sep = ", ";
		}
		if (page_count != 0)
			fprintf(fp, "\n");
	}

	fprintf(fp, "total pages: %lu\n", page_total);
	fprintf(fp, "==============================\n");

	if (do_close)
		(void)fclose(fp);

	return (0);
}
#endif
