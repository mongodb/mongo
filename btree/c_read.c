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
static int __wt_cache_read(WT_READ_REQ *);

/*
 * __wt_cache_read_server --
 *	Thread to do database reads.
 */
void *
__wt_cache_read_server(void *arg)
{
	ENV *env;
	IENV *ienv;
	WT_CACHE *cache;
	WT_READ_REQ *rr, *rr_end;
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
		 * Go to sleep; no memory flush needed, the io_sleeping field
		 * is declared volatile.
		 */
		WT_VERBOSE(env,
		    WT_VERB_SERVERS, (env, "cache I/O server sleeping"));
		cache->io_sleeping = 1;
		__wt_lock(env, cache->mtx_io);

		/*
		 * Walk the read-request queue, looking for reads (defined by
		 * a valid WT_TOC handle, a reference to a cache slot, and a
		 * state marked WT_READ).  It's an accumulated set of things:
		 * the original calling thread set the WT_TOC handle when it
		 * scheduled the read, and the cache server thread filled in
		 * the WT_CACHE_ENTRY slot, and set the slot's state field,
		 * when it figured out a read was necessary.
		 *
		 * If we find a read request, perform it, flush the result and
		 * clear the request slot, then wake up the requesting thread.
		 * The request slot clear doesn't need to be flushed, but we
		 * have to flush the read result, might as well include it.
		 *
		 * If we don't find any work, go back to sleep.
		 */
		do {
			didwork = 0;
			for (rr = cache->read_request; rr < rr_end; ++rr) {
				if (rr->toc == NULL ||
				    rr->entry == NULL ||
				    rr->entry->state != WT_READ)
					continue;
				toc = rr->toc;
				if ((toc->wq_ret = __wt_page_read(
				    toc->db, rr->entry->page)) == 0)
					rr->entry->state = WT_OK;
				else
					rr->entry->state = WT_EMPTY;
				rr->toc = NULL;
				WT_MEMORY_FLUSH;
				__wt_unlock(env, toc->mtx);
				didwork = 1;
			}
		} while (didwork);
	}
	return (NULL);
}

/*
 * __wt_cache_server_read --
 *	Function to check for reads and schedule the I/O thread.
 */
int
__wt_cache_server_read(void *arg, int *didworkp)
{
	ENV *env;
	IENV *ienv;
	WT_CACHE *cache;
	WT_READ_REQ *rr, *rr_end;
	WT_TOC *toc;

	env = arg;
	ienv = env->ienv;
	cache = ienv->cache;

	rr = cache->read_request;
	rr_end =
	    rr + sizeof(cache->read_request) / sizeof(cache->read_request[0]);

	/*
	 * For every read request, create the cache infrastructure necessary to
	 * instantiate the page.   If it's an allocation, we did all the work
	 * that's necessary, wake the calling thread; if it's a read, wake the
	 * read thread if it's asleep and let it do the work.
	 */
	for (rr = cache->read_request; rr < rr_end; ++rr) {
		/*
		 * If rr->toc is NULL, there's no entry to review.
		 * If rr->entry is set, the entry is waiting on the read thread.
		 */
		if (rr->toc == NULL || rr->entry != NULL)
			continue;
		*didworkp = 1;

		/*
		 * If an error occurred or the page was successfully allocated
		 * and that's all we need to do, return the value, clear the
		 * slot, wake the calling thread and we're done.
		 */
		toc = rr->toc;
		if ((toc->wq_ret =
		    __wt_cache_read(rr)) != 0 || rr->entry->state == WT_OK) {
			rr->toc = NULL;
			WT_MEMORY_FLUSH;
			__wt_unlock(env, toc->mtx);
		}
	}
	return (0);
}

/*
* __wt_cache_read --
*	Read or allocate a new page for the cache.
*/
static int
__wt_cache_read(WT_READ_REQ *rr)
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
	WT_TOC *toc;
	u_int32_t addr, size, i;
	int newpage, ret;

	toc = rr->toc;
	db = toc->db;
	env = toc->env;
	idb = db->idb;
	ienv = env->ienv;
	cache = ienv->cache;
	fh = idb->fh;

	addr = rr->addr;
	size = rr->size;
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
	WT_ERR(__wt_calloc(env, (size_t)size, sizeof(u_int8_t), &page->hdr));

	/* If it's an allocation, extend the file; otherwise read the page. */
	if (newpage) {
		/* Extend the file. */
		addr = WT_OFF_TO_ADDR(db, fh->file_size);
		fh->file_size += size;

		WT_STAT_INCR(cache->stats, CACHE_ALLOC);
		WT_STAT_INCR(idb->stats, DB_CACHE_ALLOC);
	}
	page->addr = addr;
	page->size = size;

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
	 * Get a hazard reference before we mark the entry OK: the cache drain
	 * server shouldn't pick a new page with a high read-generation, but
	 * there's no reason to risk it.
	 */
	__wt_hazard_set(toc, page);

	/*
	 * Fill in the cache entry and the caller's page value.  Our callers
	 * use them to figure out what happened (WT_OK/WT_READ means the page
	 * was allocated and we're done vs. not-yet-read, and the penultimate
	 * caller, the Btree thread of control, wants the page reference).
	 */
	rr->entry = empty;
	*rr->pagep = page;

	WT_CACHE_ENTRY_SET(
	    empty, db, page, addr, ++ienv->read_gen, newpage ? WT_OK : WT_READ);

	/*
	 * Wake the read thread if it's sleeping and it needs to run; no memory
	 * flush needed, the io_sleeping field is declared volatile.
	 */
	if (!newpage && cache->io_sleeping) {
		WT_VERBOSE(env,
		    WT_VERB_SERVERS, (env, "waking cache I/O server"));
		cache->io_sleeping = 0;
		__wt_unlock(env, cache->mtx_io);
	}

	WT_VERBOSE(env, WT_VERB_CACHE, (env,
	    "cache %s element/page/addr %p/%p/%lu",
	    newpage ? "allocated" : "read", empty, empty->page, (u_long)addr));

	WT_CACHE_PAGE_IN(cache, size);

	return (0);

err:	if (page != NULL) {
		if (page->hdr != NULL)
			__wt_free(env, page->hdr, size);
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

#define	WT_CACHE_ENTRY_GROW	20
	entries = hb->entry_size + WT_CACHE_ENTRY_GROW;

	WT_VERBOSE(env, WT_VERB_CACHE, (env,
	    "cache hash bucket %lu grows to %lu entries",
	    (u_long)(hb - cache->hb), entries));

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
	for (i = 0; i < WT_CACHE_ENTRY_GROW; ++e, ++i)
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

	return (0);
}
