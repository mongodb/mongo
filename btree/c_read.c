/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008 WiredTiger Software.
 *	All rights reserved.
 *
 * $Id$
 */

#include "wt_internal.h"

static int __wt_cache_hb_entry_grow(WT_TOC *, WT_CACHE_HB *, WT_CACHE_ENTRY **);
static int __wt_cache_read(WT_TOC *, WT_IO_REQ *);

/*
 * __wt_workq_cache_server --
 *	Called to check on the cache server threads.
 */
void
__wt_workq_cache_server(ENV *env)
{
	IENV *ienv;
	WT_CACHE *cache;
	u_int64_t bytes_inuse, bytes_max;

	ienv = env->ienv;
	cache = ienv->cache;

	bytes_inuse = WT_CACHE_BYTES_INUSE(cache);
	bytes_max = WT_STAT(cache->stats, CACHE_BYTES_MAX);

	/*
	 * If we're 10% over the maximum cache, shut out page allocations until
	 * we drain to at least 5% under the maximum cache.
	 */
	if (cache->read_lockout) {
		if (bytes_inuse <= bytes_max - (bytes_max / 20))
			cache->read_lockout = 0;
	} else {
		if (bytes_inuse > bytes_max + (bytes_max / 10)) {
			cache->read_lockout = 1;
			WT_STAT_INCR(cache->stats, CACHE_READ_LOCKOUT);
		}
	}

	/* Wake the cache drain thread if it's sleeping and it needs to run. */
	if (cache->drain_sleeping &&
	    (bytes_inuse > bytes_max || cache->read_lockout))
		__wt_unlock(cache->mtx_drain);

	/* A read is scheduled -- wake the I/O thread if it's sleeping. */
	if (!cache->read_lockout && cache->io_sleeping)
		__wt_unlock(cache->mtx_io);
}

/*
 * __wt_cache_in_serial_func --
 *	Ask the I/O thread to read a page into the cache.
 */
int
__wt_cache_in_serial_func(WT_TOC *toc)
{
	ENV *env;
	IENV *ienv;
	WT_CACHE *cache;
	WT_IO_REQ *rr, *rr_end;
	WT_PAGE **pagep;
	u_int32_t addr, bytes;

	__wt_cache_in_unpack(toc, addr, bytes, pagep);

	env = toc->env;
	ienv = env->ienv;
	cache = ienv->cache;

	/* Find an empty slot and enter the read request. */
	rr = cache->read_request;
	rr_end =
	    rr + sizeof(cache->read_request) / sizeof(cache->read_request[0]);
	for (; rr < rr_end; ++rr)
		if (rr->toc == NULL) {
			/*
			 * Fill in the arguments, flush memory, then the WT_TOC
			 * field that turns the slot on.
			 */
			rr->addr = addr;
			rr->bytes = bytes;
			rr->pagep = pagep;
			WT_MEMORY_FLUSH;
			rr->toc = toc;
			WT_MEMORY_FLUSH;
			return (0);
		}
	__wt_api_env_errx(env, "cache server read request table full");
	return (WT_RESTART);
}

/*
 * __wt_cache_io --
 *	Server thread to do cache I/O.
 */
void *
__wt_cache_io(void *arg)
{
	ENV *env;
	IENV *ienv;
	WT_CACHE *cache;
	WT_IO_REQ *rr, *rr_end;
	WT_TOC *toc;
	int didwork;

	env = arg;
	ienv = env->ienv;
	cache = ienv->cache;

	rr = cache->read_request;
	rr_end =
	    rr + sizeof(cache->read_request) / sizeof(cache->read_request[0]);

	while (F_ISSET(ienv, WT_SERVER_RUN)) {
		/*
		 * No need for an explicit memory flush, the io_sleeping flag
		 * is declared volatile.
		 */
		cache->io_sleeping = 1;
		__wt_lock(env, cache->mtx_io);
		cache->io_sleeping = 0;

		/*
		 * Look for work (unless reads are locked out for now).  If we
		 * find a requested read, perform it, flush the result, clear
		 * the request slot, and wake up the requesting thread.   If we
		 * don't find any work, go back to sleep.  Strictly speaking,
		 * the request slot clear doesn't need to be flushed, but we
		 * have to flush, might as well include it.
		 */
		do {
			didwork = 0;
			for (rr = cache->read_request; rr < rr_end; ++rr)
				if ((toc = rr->toc) != NULL) {
					toc->wq_ret = __wt_cache_read(toc, rr);
					rr->toc = NULL;
					WT_MEMORY_FLUSH;
					__wt_unlock(toc->mtx);
					didwork = 1;
				}
		} while (cache->read_lockout == 0 && didwork == 1);
	}
	return (NULL);
}

/*
 * __wt_cache_read --
 *	Read or allocate a new page for the cache.
 */
static int
__wt_cache_read(WT_TOC *toc, WT_IO_REQ *rr)
{
	DB *db;
	ENV *env;
	IDB *idb;
	IENV *ienv;
	WT_CACHE *cache;
	WT_CACHE_ENTRY *e, *empty;
	WT_CACHE_HB *hb;
	WT_FH *fh;
	WT_PAGE *page;
	WT_PAGE_HDR *hdr;
	off_t offset;
	u_int32_t addr, bytes, checksum, i;
	int newpage, ret;

	db = toc->db;
	env = toc->env;
	idb = db->idb;
	ienv = env->ienv;
	cache = ienv->cache;
	fh = idb->fh;

	addr = rr->addr;
	bytes = rr->bytes;
	newpage = addr == WT_ADDR_INVALID ? 1 : 0;

	/*
	 * Read a page from the file -- allocations end up here too, because
	 * we have to single-thread extending the file, and we can share page
	 * allocation and cache insertion code.
	 *
	 * Check to see if some other thread brought the page into the cache
	 * while we waited to run.   We don't care what state the page is in,
	 * we have to restart the operation since it appears in the cache.
	 * The obvious problem is the page is waiting to drain.   For all we
	 * know, the drain server will find a hazard reference and restore the
	 * page to the cache.
	 *
	 * While we're walking the hash bucket, keep track of any empty slots
	 * we find, we may need one.
	 */
	empty = NULL;
	if (!newpage) {
		hb = &cache->hb[WT_HASH(cache, addr)];
		WT_CACHE_FOREACH_PAGE(cache, hb, e, i) {
			if (e->state == WT_EMPTY) {
				empty = e;
				continue;
			}
			if (e->db == db && e->addr == addr)
				return (WT_RESTART);
		}
		WT_STAT_INCR(cache->stats, CACHE_MISS);
		WT_STAT_INCR(idb->stats, DB_CACHE_MISS);
	}

	/*
	 * The page isn't in the cache, and since we're the only path for the
	 * page to get into the cache, we don't have to worry further, and
	 * we might as well get to it.
	 *
	 * Allocate memory for the in-memory page information and for the page
	 * itself. They're two separate allocation calls so we (hopefully) get
	 * better alignment from the underlying heap memory allocator.
	 */
	WT_RET(__wt_calloc(env, 1, sizeof(WT_PAGE), &page));
	WT_ERR(__wt_calloc(env, (size_t)bytes, sizeof(u_int8_t), &page->hdr));

	/* If it's an allocation, extend the file; otherwise read the page. */
	if (newpage) {
		/* Extend the file. */
		addr = WT_OFF_TO_ADDR(db, fh->file_size);
		fh->file_size += bytes;

		WT_STAT_INCR(cache->stats, CACHE_ALLOC);
		WT_STAT_INCR(idb->stats, DB_CACHE_ALLOC);
	} else {
		offset = WT_ADDR_TO_OFF(db, addr);
		WT_ERR(__wt_read(env, fh, offset, bytes, page->hdr));

		/* Verify the checksum. */
		hdr = page->hdr;
		checksum = hdr->checksum;
		hdr->checksum = 0;
		if (checksum != __wt_cksum(hdr, bytes)) {
			__wt_api_env_errx(env,
			    "file offset %llu with length %lu was read and had "
			    "a checksum error",
			    (u_quad)offset, (u_long)bytes);
			ret = WT_ERROR;
			goto err;
		}
	}

	/* Fill in the page structure. */
	page->addr = addr;
	page->bytes = bytes;

	/*
	 * If we found an empty slot in our original walk of the hash bucket,
	 * use it; otherwise, look for another one.  If there aren't any, grow
	 * the array.
	 */
	if (empty == NULL) {
		hb = &cache->hb[WT_HASH(cache, addr)];
		WT_CACHE_FOREACH_PAGE(cache, hb, e, i)
			if (e->state == WT_EMPTY) {
				empty = e;
				break;
			}
		if (empty == NULL)
			WT_ERR(__wt_cache_hb_entry_grow(toc, hb, &empty));
	}

	/*
	 * Get a hazard reference before we mark the entry OK, the cache drain
	 * server shouldn't pick our new page, but there's no reason to risk
	 * it.
	 */
	__wt_hazard_set(toc, page);

	/*
	 * Fill in everything but the state, flush, then fill in the state.  No
	 * additional flush is necessary, the state field is declared volatile.
	 * The state turns on the entry for both the cache drain server thread
	 * and any readers.
	 */
	empty->addr = addr;
	empty->db = db;
	empty->gen = ++ienv->page_gen;
	empty->page = page;
	WT_MEMORY_FLUSH;
	empty->state = WT_OK;

	WT_CACHE_PAGE_IN(cache, bytes);

	WT_VERBOSE(env, WT_VERB_CACHE,
	    (env, "cache I/O server %s element/page %#lx/%lu",
	    newpage ? "allocated" : "read",
	    WT_PTR_TO_ULONG(empty), (u_long)addr));

	/* Return the page to the caller. */
	*rr->pagep = page;

	return (0);

err:	if (page != NULL) {
		if (page->hdr != NULL)
			__wt_free(env, page->hdr, bytes);
		__wt_free(env, page, sizeof(WT_PAGE));
	}
	return (ret);
}

/*
 * __wt_cache_hb_entry_grow --
 *	Grow the hash bucket's WT_CACHE_ENTRY array.
 */
static int
__wt_cache_hb_entry_grow(WT_TOC *toc, WT_CACHE_HB *hb, WT_CACHE_ENTRY **emptyp)
{
	ENV *env;
	WT_CACHE *cache;
	WT_CACHE_ENTRY *e, *new;
	u_int32_t i, entries;

	env = toc->env;
	cache = env->ienv->cache;

	entries = hb->entry_size + WT_CACHE_ENTRY_DEFAULT;

	WT_VERBOSE(env, WT_VERB_CACHE, (env,
	    "I/O server: hash bucket %lu grows to %lu entries",
	    (u_long)(hb - cache->hb), entries));

	/*
	 * Lock the hash buckets, we can't copy them while the cache drain
	 * server has them pinned (that is, has references to pages in the
	 * bucket).
	 */
	__wt_lock(env, cache->mtx_hb);

	/* Allocate the new WT_ENTRY array. */
	WT_RET(__wt_calloc(env, (size_t)entries, sizeof(WT_CACHE_ENTRY), &new));
	if (WT_STAT(cache->stats, CACHE_MAX_BUCKET_ENTRIES) < entries)
		WT_STAT_SET(cache->stats, CACHE_MAX_BUCKET_ENTRIES, entries);

	/* Copy any previous values from the old array to the new array. */
	memcpy(new, hb->entry, hb->entry_size * sizeof(WT_CACHE_ENTRY));

	/* Set and return the first empty slot. */
	e = new + hb->entry_size;
	*emptyp = e;

	/* Initialize newly allocated slots' state. */
	for (i = 0; i < WT_CACHE_ENTRY_DEFAULT; ++e, ++i)
		e->state = WT_EMPTY;

	/* Schedule the previous array to be freed. */
	WT_FLIST_INSERT(
	    toc, hb->entry, hb->entry_size * sizeof(WT_CACHE_ENTRY));

	/*
	 * Update the array in place -- because the old and new memory chunks
	 * are identical up to the old length, it doesn't matter which write
	 * goes out first.
	 */
	hb->entry = new;
	hb->entry_size = entries;
	WT_MEMORY_FLUSH;

	__wt_unlock(cache->mtx_hb);

	return (0);
}
