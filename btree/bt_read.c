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
static int __wt_cache_read(WT_TOC *);

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
	bytes_max = WT_STAT(ienv->stats, CACHE_BYTES_MAX);

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
			WT_STAT_INCR(ienv->stats, CACHE_READ_LOCKOUT);
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
 * __wt_workq_schedule_read --
 *	Ask the I/O thread to read a page into the cache.
 */
int
__wt_workq_schedule_read(WT_TOC *toc)
{
	ENV *env;
	IENV *ienv;
	WT_CACHE *cache;
	WT_TOC **rr, **rr_end;

	env = toc->env;
	ienv = env->ienv;
	cache = ienv->cache;

	/* Find an empty slot and enter the read request. */
	rr = cache->read_request;
	rr_end =
	    rr + sizeof(cache->read_request) / sizeof(cache->read_request[0]);
	for (; rr < rr_end; ++rr)
		if (rr[0] == NULL) {
			rr[0] = toc;
			WT_MEMORY_FLUSH;
			return (0);
		}
	return (1);
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
	WT_TOC **rr, **rr_end, *toc;
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
		 * find a requested read, perform it, flush the result, and
		 * wake up the requesting thread.   If we don't find any work,
		 * go back to sleep.
		 */
		do {
			didwork = 0;
			for (rr = cache->read_request; rr < rr_end; ++rr)
				if ((toc = rr[0]) != NULL) {
					toc->wq_ret = __wt_cache_read(toc);
					toc->wq_state = 0;
					rr[0] = NULL;
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
__wt_cache_read(WT_TOC *toc)
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
	int ret;

	db = toc->db;
	env = toc->env;
	idb = db->idb;
	ienv = env->ienv;
	cache = ienv->cache;
	fh = idb->fh;

	addr = toc->wq_addr;
	bytes = toc->wq_bytes;

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
	if (addr != WT_ADDR_INVALID) {
		hb = &cache->hb[WT_HASH(cache, addr)];
		WT_CACHE_FOREACH_PAGE(cache, hb, e, i) {
			if (e->state == WT_EMPTY) {
				empty = e;
				continue;
			}
			if (e->db == db && e->addr == addr)
				return (0);
		}
		WT_STAT_INCR(idb->stats, DB_CACHE_MISS);
		WT_STAT_INCR(env->ienv->stats, CACHE_MISS);
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
	if (addr == WT_ADDR_INVALID) {
		/* Extend the file. */
		addr = WT_OFF_TO_ADDR(db, fh->file_size);
		fh->file_size += bytes;

		/* Return the address & bytes to the caller. */
		toc->wq_addr = addr;
		toc->wq_bytes = bytes;

		WT_STAT_INCR(idb->stats, DB_CACHE_ALLOC);
		WT_STAT_INCR(env->ienv->stats, CACHE_ALLOC);
		WT_VERBOSE(env,
		    WT_VERB_CACHE, (env,
		        "cache I/O server allocated page %lu", (u_long)addr));
	} else {
		WT_VERBOSE(env,
		    WT_VERB_CACHE, (env,
		        "cache I/O server read page %lu", (u_long)addr));

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

	/* Initialize the page structure. */
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
	}
	if (empty == NULL)
		WT_ERR(__wt_cache_hb_entry_grow(toc, hb, &empty));

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

	return (0);

err:	if (page->hdr != NULL)
		__wt_free(env, page->hdr, bytes);
	if (page != NULL)
		__wt_free(env, page, sizeof(WT_PAGE));
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

	entries = hb->entry_size + WT_CACHE_ENTRY_ALLOC;

	WT_VERBOSE(env, WT_VERB_CACHE, (env,
	    "I/O server: hash bucket %lu grows to %lu entries",
	    (u_long)(hb - cache->hb), entries));

	/* Allocate the new WT_ENTRY array. */
	WT_RET(__wt_calloc(env, (size_t)entries, sizeof(WT_CACHE_ENTRY), &new));
	if (WT_STAT(cache->stats, CACHE_MAX_BUCKET_ENTRIES) < entries)
		WT_STAT_SET(cache->stats, CACHE_MAX_BUCKET_ENTRIES, entries);

	/* Copy any previous values from the old array to the new array. */
	if (hb->entry != NULL)
		memcpy(new, hb->entry, hb->entry_size * sizeof(WT_CACHE_ENTRY));

	/* Optionally copy out the first empty slot. */
	e = new + hb->entry_size;
	if (emptyp != NULL)
		*emptyp = e;

	/* Initialize allocated slots' state. */
	for (i = 0; i < WT_CACHE_ENTRY_ALLOC; ++e, ++i)
		e->state = WT_EMPTY;

	/* Free any previous array. */
	if (hb->entry != NULL)
		WT_FLIST_INSERT(
		    toc, hb->entry, hb->entry_size * sizeof(WT_CACHE_ENTRY));

	/*
	 * Update the arry in place -- it's OK because the old and new memory
	 * chunks are identical up to the old length, it doesn't matter which
	 * memory write goes out first.
	 */
	hb->entry = new;
	hb->entry_size = entries;
	WT_MEMORY_FLUSH;

	return (0);
}
