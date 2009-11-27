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
static int  __wt_cache_alloc_serialize_func(WT_TOC *);
static void __wt_cache_discard(ENV *, WT_PAGE *);
static int  __wt_cache_discard_serialize_func(WT_TOC *);
static int  __wt_cache_in_serialize_func(WT_TOC *);

#ifdef HAVE_DIAGNOSTIC
static int __wt_cache_dump(ENV *, const char *, FILE *);
#endif

/*
 * Page allocation serialization support.
 */
typedef struct {
	WT_PAGE  *page;				/* Allocated page */
	u_int32_t bytes;			/* Bytes to allocate */
 } __wt_alloc_args;
#define	__wt_cache_alloc_serialize(toc, _page, _bytes) do {		\
	__wt_alloc_args _args;						\
	_args.page = _page;						\
	_args.bytes = _bytes;						\
	WT_TOC_SERIALIZE_REQUEST(toc,					\
	    __wt_cache_alloc_serialize_func, NULL, &_args);		\
} while (0)
#define	__wt_cache_alloc_unpack(toc, _page, _bytes) do {		\
	_page =	((__wt_alloc_args *)(toc)->serialize_args)->page;	\
	_bytes = ((__wt_alloc_args *)(toc)->serialize_args)->bytes;	\
} while (0)

/*
 * Page read serialization support.
 */
typedef struct {
	WT_PAGE  *page;				/* Allocated page */
	u_int32_t bytes;			/* Bytes to allocate */
	off_t	  offset;			/* File offset */
} __wt_in_args;
#define	__wt_cache_in_serialize(toc, _page, _bytes, _offset) do {	\
	__wt_in_args _args;						\
	_args.page = _page;						\
	_args.bytes = _bytes;						\
	_args.offset = _offset;						\
	WT_TOC_SERIALIZE_REQUEST(toc,					\
	    __wt_cache_in_serialize_func, NULL, &_args);		\
} while (0);
#define	__wt_cache_in_unpack(toc, _page, _bytes, _offset) do {		\
	_page =	((__wt_in_args *)(toc)->serialize_args)->page;		\
	_bytes = ((__wt_in_args *)(toc)->serialize_args)->bytes;	\
	_offset = ((__wt_in_args *)(toc)->serialize_args)->offset;	\
} while (0)

/*
 * Page discard serialization support.
 */
typedef struct {
	WT_PAGE  *page;				/* Allocated page */
} __wt_discard_args;
#define	__wt_cache_discard_serialize(toc, _serial, _page) do {		\
	__wt_discard_args _args;					\
	_args.page = _page;						\
	WT_TOC_SERIALIZE_REQUEST(toc,					\
	    __wt_cache_discard_serialize_func, _serial, &_args);	\
} while (0)
#define	__wt_cache_discard_unpack(toc, _page) do {			\
	_page = ((__wt_discard_args *)(toc)->serialize_args)->page;	\
} while (0)

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

	cache->bytes_max = env->cachesize * WT_MEGABYTE;

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
	 * Size for a cache filled with 8KB pages, and 4 pages per bucket (or,
	 * 32 buckets per MB).
	 */
	cache->hashsize = __wt_prime(env->cachesize * 32);
	WT_STAT_SET(ienv->stats, HASH_BUCKETS, "hash buckets", cache->hashsize);

	WT_RET(__wt_calloc(env, cache->hashsize, sizeof(WT_HB), &cache->hb));

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

	/*
	 * Discard all pages.  No server support needed, this is done when the
	 * environment is closed, after all threads of control have exited the
	 * cache.
	 *
	 * There shouldn't be any modified pages, because all of the databases
	 * have been closed.
	 */
	for (i = 0; i < cache->hashsize; ++i)
		while ((page = cache->hb[i].list) != NULL) {
			WT_ASSERT(env, !F_ISSET(page, WT_MODIFIED));
			__wt_cache_discard(env, page);
			__wt_bt_page_recycle(env, page);
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
__wt_cache_sync(WT_TOC *toc, WT_FH *fh, void (*f)(const char *, u_int32_t))
{
	ENV *env;
	WT_CACHE *cache;
	WT_HB *hb;
	WT_PAGE *page;
	u_int32_t fcnt;
	u_int i;

	env = toc->env;
	cache = &env->ienv->cache;

	/*
	 * Write any modified pages -- if the handle is set, write only pages
	 * belong to the specified file.
	 *
	 * We only report progress on every 10 writes, to minimize callbacks.
	 */
	for (i = 0, fcnt = 0; i < cache->hashsize; ++i) {
		hb = &cache->hb[i];
		WT_TOC_SERIALIZE_WAIT(toc, &hb->serialize_private);
		for (page = hb->list; page != NULL; page = page->next) {
			if (!F_ISSET(page, WT_MODIFIED | WT_PINNED))
				continue;
			if (fh != NULL && fh != page->fh)
				continue;
			WT_RET(__wt_cache_write(env, page));

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
	IENV *ienv;
	WT_PAGE *page;
	int ret;

	*pagep = NULL;

	db = toc->db;
	env = toc->env;
	idb = db->idb;
	ienv = env->ienv;

	WT_ASSERT(env, bytes % WT_FRAGMENT == 0);

	/*
	 * If the cache is too big, and we can restart the operation, return
	 * that error.
	 */
	if (F_ISSET(toc, WT_CACHE_LOCK_RESTART) && ienv->cache_lockout)
		return (WT_RESTART);

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
	 * the allocation of bytes from the file, the change of total bytes
	 * in the cache, and the insert onto the hash queue.
	 */
	__wt_cache_alloc_serialize(toc, page, bytes);

	*pagep = page;
	return (0);
}

/*
 * __wt_cache_alloc_serialize_func --
 *	Server function to allocate bytes from a file.
 */
static int
__wt_cache_alloc_serialize_func(WT_TOC *toc)
{
	DB *db;
	IENV *ienv;
	WT_CACHE *cache;
	WT_FH *fh;
	WT_HB *hb;
	WT_PAGE *page;
	u_int32_t bytes;

	__wt_cache_alloc_unpack(toc, page, bytes);

	db = toc->db;
	ienv = toc->env->ienv;
	cache = &ienv->cache;
	fh = db->idb->fh;

	/* Initialize the page structure, allocating "bytes" from the file. */
	page->fh = fh;
	page->offset = fh->file_size;
	fh->file_size += bytes;
	page->addr = WT_OFF_TO_ADDR(db, page->offset);
	page->bytes = bytes;
	page->page_gen = ++ienv->page_gen;

	/* Insert as the head of the linked list. */
	hb = &cache->hb[WT_HASH(cache, page->offset)];
	page->next = hb->list;
	hb->list = page;

	/* Increment total cache byte count. */
	cache->bytes_alloc += bytes;

	return (0);
}

/*
 * __wt_cache_in --
 *	Return a database page, reading as necessary.
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
	WT_HB *hb;
	WT_PAGE *page;
	WT_PAGE_HDR *hdr;
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

	/*
	 * If there's a prviate serialization request on the hash bucket,
	 * wait until it clears.
	 */
	hb = &cache->hb[WT_HASH(cache, offset)];
	WT_TOC_SERIALIZE_WAIT(toc, &hb->serialize_private);

	/* Search for the page in the cache. */
	for (bucket_cnt = 0,
	    page = hb->list; page != NULL; ++bucket_cnt, page = page->next)
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

		page->page_gen = ++ienv->page_gen;
		*pagep = page;
		return (0);
	}

	WT_STAT_INCR(env->ienv->stats,
	    CACHE_MISS, "cache miss: reads not found in the cache");
	WT_STAT_INCR(idb->stats,
	    DB_CACHE_MISS, "cache miss: reads not found in the cache");

	/*
	 * If the cache is too big, and we can restart the operation, return
	 * that error.
	 */
	if (F_ISSET(toc, WT_CACHE_LOCK_RESTART) && ienv->cache_lockout)
		return (WT_RESTART);

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

err:		if (page->hdr != NULL)
			__wt_free(env, page->hdr);
		__wt_free(env, page);
		return (ret);
	}

	/* Serialize the insert onto the hash queue. */
	__wt_cache_in_serialize(toc, page, bytes, offset);

	*pagep = page;
	return (0);
}

/*
 * __wt_cache_in_serialize_func --
 *	Server function to read bytes from a file.
 */
static int
__wt_cache_in_serialize_func(WT_TOC *toc)
{
	DB *db;
	IENV *ienv;
	WT_CACHE *cache;
	WT_FH *fh;
	WT_HB *hb;
	WT_PAGE *page;
	off_t offset;
	u_int32_t bytes;

	__wt_cache_in_unpack(toc, page, bytes, offset);

	db = toc->db;
	ienv = toc->env->ienv;
	cache = &ienv->cache;
	fh = db->idb->fh;

	/* Initialize the page structure. */
	page->fh = fh;
	page->offset = offset;
	page->addr = WT_OFF_TO_ADDR(db, offset);
	page->bytes = bytes;
	page->page_gen = ++ienv->page_gen;

	/*
	 * BUG!!!
	 * Somebody else may have read the same page, check.
	 */

	/* Insert as the head of the linked list. */
	hb = &cache->hb[WT_HASH(cache, page->offset)];
	page->next = hb->list;
	hb->list = page;

	/* Increment total cache byte count. */
	cache->bytes_alloc += bytes;

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

	/* Unformatted pages are discarded as soon as they're returned. */
	if (F_ISSET(page, WT_UNFORMATTED))
		__wt_bt_page_recycle(env, page);

	return (0);
}

/*
 * __wt_cache_srvr --
 *	Server routine to process the cache.
 */
void *
__wt_cache_srvr(void *arg)
{
	ENV *env;
	IENV *ienv;
	WT_CACHE *cache;
	WT_HB *hb, *chosen_hb;
	WT_PAGE *page, *chosen_page;
	WT_TOC *toc;
	u_int32_t chosen_gen, i;
	int ret, review, review_max;

	env = arg;
	ienv = env->ienv;
	cache = &env->ienv->cache;

	/* Create a WT_TOC so we can make serialization requests. */
	if (env->toc(env, 0, &toc) != 0)
		return (NULL);
	toc->name = "cache server";

	/*
	 * Review 1% of the hash buckets to choose a buffer to toss, but never
	 * less than 10 buckets.
	 */
	if ((review_max = cache->hashsize / 100) < 10)
		review_max = 20;

	i = 0;
	while (F_ISSET(ienv, WT_SERVER_RUN)) {
		/*
		 * If there's no work to do, go to sleep.  We check the workQ's
		 * cache_lockout field because the workQ wants us to be more
		 * agressive about cleaning up than we would normally be.
		 */
		if (ienv->cache_lockout == 0 &&
		    cache->bytes_alloc <= cache->bytes_max) {
			F_SET(cache, WT_SERVER_SLEEPING);
			WT_MEMORY_FLUSH;
			__wt_lock(env, &cache->mtx);
			continue;
		}

		/*
		 * Look at review_max hash buckets, and pick a page with the
		 * lowest LRU.  An unformatted page is totally useless, and
		 * won't need to be written, so it's even better.
		 */
		for (chosen_gen = 0, review = 0;; ++i) {
			if (i == cache->hashsize)
				i = 0;
			hb = &cache->hb[i];
			for (page = hb->list; page != NULL; page = page->next) {
				if (F_ISSET(page, WT_PINNED))
					continue;
				if (chosen_gen == 0 ||
				    page->page_gen < chosen_gen) {
					chosen_hb = hb;
					chosen_page = page;
					chosen_gen = page->page_gen;
				}
			}
			if (++review >= review_max && chosen_gen != 0)
				break;
		}
		hb = chosen_hb;
		page = chosen_page;

		/* Write the page if it's been modified. */
		if (F_ISSET(page, WT_MODIFIED)) {
			WT_STAT_INCR(env->ienv->stats, CACHE_WRITE_EVICT,
			    "dirty pages evicted from the cache");

			if ((ret = __wt_cache_write(env, page)) != 0) {
				__wt_api_env_err(env, ret,
				    "cache server thread unable to write page");
				return (NULL);
			}
		} else
			WT_STAT_INCR(env->ienv->stats, CACHE_EVICT,
			    "clean pages evicted from the cache");

		/*
		 * If the page is modified while we wait for serialized access,
		 * keep the page.
		 */
		__wt_cache_discard_serialize(toc, &hb->serialize_private, page);
		if (toc->serialize_ret == 0)
			__wt_bt_page_recycle(env, page);
	}

	(void)toc->close(toc, 0);

	return (NULL);
}

/*
 * __wt_cache_discard_serialize_func --
 *	Server version: discard a page of a file.
 */
static int
__wt_cache_discard_serialize_func(WT_TOC *toc)
{
	WT_PAGE *page;

	__wt_cache_discard_unpack(toc, page);

	/*
	 * If the page was modified or pinned while we waited for serialized
	 * access, return with a failed status.
	 */
	if (F_ISSET(page, WT_MODIFIED | WT_PINNED))
		return (1);

	__wt_cache_discard(toc->env, page);
	return (0);
}

/*
 * __wt_cache_discard --
 *	Remove a page from its hash bucket.
 */
static void
__wt_cache_discard(ENV *env, WT_PAGE *page)
{
	WT_CACHE *cache;
	WT_HB *hb;
	WT_PAGE *tpage;

	cache = &env->ienv->cache;

	WT_ASSERT(env, cache->bytes_alloc >= page->bytes);
	cache->bytes_alloc -= page->bytes;

	hb = &cache->hb[WT_HASH(cache, page->offset)];
	if (hb->list == page)
		hb->list = page->next;
	else
		for (tpage = hb->list;; tpage = tpage->next)
			if (tpage->next == page) {
				tpage->next = page->next;
				break;
			}
}

/*
 * __wt_cache_write --
 *	Write a page to the backing database file.
 */
static int
__wt_cache_write(ENV *env, WT_PAGE *page)
{
	WT_PAGE_HDR *hdr;

	/* Update the checksum. */
	hdr = page->hdr;
	hdr->checksum = 0;
	hdr->checksum = __wt_cksum(hdr, page->bytes);

	/* Write, and if successful, clear the modified flag. */
	WT_RET(__wt_write(env, page->fh, page->offset, page->bytes, hdr));

	F_CLR(page, WT_MODIFIED);

	WT_STAT_INCR(env->ienv->stats, CACHE_WRITE, "writes from the cache");

	return (0);
}

#ifdef HAVE_DIAGNOSTIC
/*
 * __wt_cache_dump --
 *	Dump a cache.
 */
static int
__wt_cache_dump(ENV *env, const char *ofile, FILE *fp)
{
	WT_CACHE *cache;
	WT_PAGE *page;
	u_long page_total;
	u_int32_t i;
	int do_close, page_count;
	char *sep;

	WT_RET(__wt_diag_set_fp(ofile, &fp, &do_close));

	fprintf(fp, "Cache dump: ==================\n");
	page_total = 0;
	cache = &env->ienv->cache;
	for (i = 0; i < cache->hashsize; ++i) {
		sep = "";
		page_count = 0;
		for (
		    page = cache->hb[i].list; page != NULL; page = page->next) {
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
