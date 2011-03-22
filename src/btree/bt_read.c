/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wt_internal.h"
#include "btree.i"

static int __wt_cache_read(WT_READ_REQ *);

/*
 * __wt_workq_read_server --
 *	See if the read server thread needs to be awakened.
 */
void
__wt_workq_read_server(CONNECTION *conn, int force)
{
	WT_CACHE *cache;
	uint64_t bytes_inuse, bytes_max;

	cache = conn->cache;

	/*
	 * If we're 10% over the maximum cache, shut out reads (which include
	 * page allocations) until we evict to at least 5% under the maximum
	 * cache.  The idea is that we don't want to run on the edge all the
	 * time -- if we're seriously out of space, get things under control
	 * before opening up for more reads.
	 */
	bytes_inuse = __wt_cache_bytes_inuse(cache);
	bytes_max = WT_STAT(cache->stats, CACHE_BYTES_MAX);
	if (cache->read_lockout) {
		if (bytes_inuse <= bytes_max - (bytes_max / 20))
			cache->read_lockout = 0;
	} else if (bytes_inuse > bytes_max + (bytes_max / 10)) {
		WT_VERBOSE(conn, WT_VERB_READ,
		    (&conn->default_session,
		    "workQ locks out reads: bytes-inuse %llu of bytes-max %llu",
		    (unsigned long long)bytes_inuse,
		    (unsigned long long)bytes_max));
		cache->read_lockout = 1;
	}

	/* If the cache read server is running, there's nothing to do. */
	if (!cache->read_sleeping)
		return;

	/*
	 * If reads are locked out and we're not forcing the issue (that's when
	 * closing the environment, or if there's a priority read waiting to be
	 * handled), we're done.
	 */
	if (!force && cache->read_lockout)
		return;

	cache->read_sleeping = 0;
	__wt_unlock(&conn->default_session, cache->mtx_read);
}

/*
 * __wt_cache_read_serial_func --
 *	Read/allocation serialization function called when a page-in requires
 *	allocation or a read.
 */
int
__wt_cache_read_serial_func(SESSION *session)
{
	WT_CACHE *cache;
	WT_PAGE *parent;
	WT_READ_REQ *rr, *rr_end;
	WT_REF *ref;
	int dsk_verify;

	__wt_cache_read_unpack(session, parent, ref, dsk_verify);

	cache = S2C(session)->cache;

	/* Find an empty slot and enter the read request. */
	rr = cache->read_request;
	rr_end = rr + WT_ELEMENTS(cache->read_request);
	for (; rr < rr_end; ++rr)
		if (WT_READ_REQ_ISEMPTY(rr)) {
			WT_READ_REQ_SET(rr, session, parent, ref, dsk_verify);
			return (0);
		}
	__wt_err(session, 0, "read server request table full");
	return (WT_RESTART);
}

/*
 * __wt_cache_read_server --
 *	Thread to do file reads.
 */
void *
__wt_cache_read_server(void *arg)
{
	CONNECTION *conn;
	WT_CACHE *cache;
	WT_READ_REQ *rr, *rr_end;
	SESSION *session;
	int didwork, ret;

	conn = arg;
	cache = conn->cache;
	ret = 0;

	rr = cache->read_request;
	rr_end = rr + WT_ELEMENTS(cache->read_request);

	for (;;) {
		WT_VERBOSE(conn, WT_VERB_READ,
		    (&conn->default_session, "cache read server sleeping"));
		cache->read_sleeping = 1;
		__wt_lock(&conn->default_session, cache->mtx_read);
		WT_VERBOSE(conn, WT_VERB_READ,
		    (&conn->default_session, "cache read server waking"));

		/*
		 * Check for environment exit; do it here, instead of the top of
		 * the loop because doing it here keeps us from doing a bunch of
		 * worked when simply awakened to quit.
		 */
		if (!F_ISSET(conn, WT_SERVER_RUN))
			break;

		/*
		 * Walk the read-request queue, looking for reads (defined by
		 * a valid SESSION handle).  If we find a read request, perform
		 * it, flush the result and clear the request slot, then wake
		 * up the requesting thread.
		 */
		do {
			didwork = 0;
			for (rr = cache->read_request; rr < rr_end; ++rr) {
				if ((session = rr->session) == NULL)
					continue;
				if (cache->read_lockout)
					continue;
				didwork = 1;

				ret = __wt_cache_read(rr);

				/*
				 * The request slot clear doesn't need to be
				 * flushed, but we have to flush the read
				 * result, might as well include it.
				 */
				WT_READ_REQ_CLR(rr);

				__wt_session_serialize_wrapup(
				    session, NULL, ret);
			}
		} while (didwork);
	}

	WT_VERBOSE(conn, WT_VERB_READ,
	    (&conn->default_session, "cache read server exiting"));
	return (NULL);
}

/*
 * __wt_cache_read --
 *	Read a page from the file.
 */
static int
__wt_cache_read(WT_READ_REQ *rr)
{
	WT_CACHE *cache;
	WT_PAGE *page;
	WT_PAGE_DISK *dsk;
	WT_REF *ref;
	SESSION *session;
	uint32_t addr, size;
	int ret;

	session = rr->session;
	ref = rr->ref;
	addr = ref->addr;
	size = ref->size;

	cache = S2C(session)->cache;
	page = NULL;
	dsk = NULL;
	ret = 0;

	/* Review the possible page states. */
	switch (WT_REF_STATE(ref->state)) {
	case WT_REF_DISK:			/* On-disk, read it */
		break;
	case WT_REF_MEM:			/* In-memory, already read */
		return (0);
	case WT_REF_EVICTED:			/* Logically evicted */
		/*
		 * The page was logically evicted, waiting on a merge with its
		 * parent during the parent's eviction, when it was accessed.
		 * Re-activate the page.   We could probably do this in the
		 * page read function, but this shouldn't be a common path and
		 * I'm hesitant to have multiple threads of control working on
		 * a page's state.
		 */
		WT_REF_SET_STATE(ref, WT_REF_MEM);
		return (0);
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
	WT_RET(__wt_calloc_def(session, 1, &page));
	WT_ERR(__wt_calloc(session, (size_t)size, sizeof(uint8_t), &dsk));

	/* Read the page. */
	WT_VERBOSE(S2C(session), WT_VERB_READ, (session,
	    "cache read addr/size %lu/%lu", (u_long)addr, (u_long)size));

	WT_ERR(__wt_disk_read(session, dsk, addr, size));
	WT_CACHE_PAGE_IN(cache, size);

	/* If the page needs to be verified, that's next. */
	if (rr->dsk_verify)
		WT_ERR(__wt_verify_dsk_page(session, dsk, addr, size));

	/*
	 * Fill in the WT_PAGE addr, size.
	 * Reference the parent's WT_PAGE and WT_{COL,ROW}_REF structures.
	 * Reference the underlying disk page.
	 */
	page->addr = addr;
	page->size = size;
	page->parent = rr->parent;
	page->parent_ref = ref;
	page->XXdsk = dsk;

	/*
	 * Build the in-memory version of the page -- just return on error,
	 * the called function cleaned up everything, including the physical
	 * page.
	 */
	WT_RET(__wt_page_inmem(session, page));

	/*
	 * The page is now available -- set the LRU so the page is not selected
	 * for eviction.
	 */
	page->read_gen = ++cache->read_gen;
	ref->page = page;
	WT_REF_SET_STATE(ref, WT_REF_MEM);

	return (0);

err:	if (dsk != NULL)
		__wt_free(session, dsk);
	if (page != NULL)
		__wt_free(session, page);
	return (ret);
}
