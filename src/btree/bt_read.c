/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wt_internal.h"

static int  __cache_read(WT_SESSION_IMPL *, WT_PAGE *, WT_REF *, int);

#define	WT_READ_REQ_FOREACH(rr, rr_end, cache)				\
	for ((rr) = (cache)->read_request,				\
	    (rr_end) = (rr) + WT_ELEMENTS((cache)->read_request);	\
	    (rr) < (rr_end); ++(rr))

/*
 * __cache_read_req_set --
 *	Initialize a request slot.
 */
static inline void
__cache_read_req_set(WT_SESSION_IMPL *session,
    WT_READ_REQ *rr, WT_PAGE *parent, WT_REF *ref, int dsk_verify)
{
	rr->parent = parent;
	rr->ref = ref;
	rr->dsk_verify = dsk_verify;
	/*
	 * Publish: there must be a barrier to ensure the structure fields are
	 * set before the read thread can see the request.
	 */
	WT_PUBLISH(rr->session, session);
}

/*
 * __cache_read_req_clr --
 *	Clear a request slot.
 */
static inline void
__cache_read_req_clr(WT_READ_REQ *rr)
{
	/*
	 * Publish: no barrier is required as there are no associated structure
	 * fields that need to be reset.
	 */
	rr->session = NULL;
}

/*
 * __wt_read_server_wake --
 *	See if the read server thread needs to be awakened.
 */
void
__wt_read_server_wake(WT_CONNECTION_IMPL *conn, int force)
{
	WT_CACHE *cache;
	WT_SESSION_IMPL *session;
	uint64_t bytes_inuse, bytes_max;

	cache = conn->cache;
	session = &conn->default_session;

	/*
	 * If we're 10% over the maximum cache, shut out reads (which include
	 * page allocations) until we evict to at least 5% under the maximum
	 * cache.  The idea is that we don't want to run on the edge all the
	 * time -- if we're seriously out of space, get things under control
	 * before opening up for more reads.
	 */
	bytes_inuse = __wt_cache_bytes_inuse(cache);
	bytes_max = WT_STAT(conn->stats, cache_bytes_max);
	if (!cache->read_lockout &&
	    bytes_inuse > bytes_max + (bytes_max / 10)) {
		WT_VERBOSE(session, READSERVER,
		    "read server locks out reads: "
		    "bytes-inuse %" PRIu64 " of bytes-max %" PRIu64,
		    bytes_inuse, bytes_max);
		cache->read_lockout = 1;
	}

	/* Wait for eviction to free some space. */
	while (!force && cache->read_lockout) {
		if (__wt_cache_bytes_inuse(cache) <=
		    bytes_max - (bytes_max / 20))
			cache->read_lockout = 0;
		else {
			__wt_evict_server_wake(conn, 1);
			__wt_yield();
		}
	}

	__wt_cond_signal(session, cache->read_cond);
}

/*
 * __wt_cache_read_serial_func --
 *	Read/allocation serialization function called when a page-in requires
 *	allocation or a read.
 */
void
__wt_cache_read_serial_func(WT_SESSION_IMPL *session)
{
	WT_CACHE *cache;
	WT_PAGE *parent;
	WT_READ_REQ *rr, *rr_end;
	WT_REF *ref;
	int dsk_verify;

	__wt_cache_read_unpack(session, &parent, &ref, &dsk_verify);

	cache = S2C(session)->cache;

	/* Find an empty slot and enter the read request. */
	WT_READ_REQ_FOREACH(rr, rr_end, cache)
		if (rr->session == NULL) {
			__cache_read_req_set(
			    session, rr, parent, ref, dsk_verify);
			return;
		}

	__wt_errx(session, "read server request table full");
	__wt_session_serialize_wrapup(session, NULL, WT_ERROR);
}

/*
 * __wt_cache_read_server --
 *	Thread to do file reads.
 */
void *
__wt_cache_read_server(void *arg)
{
	WT_CONNECTION_IMPL *conn;
	WT_SESSION_IMPL *session, *request_session;
	WT_CACHE *cache;
	WT_READ_REQ *rr, *rr_end;
	int didwork, ret;

	conn = arg;
	cache = conn->cache;
	ret = 0;

	/* We need a session handle because we're reading/writing pages. */
	if ((ret = __wt_open_session(conn, 1, NULL, NULL, &session)) != 0) {
		__wt_err(session, ret, "cache read server error");
		return (NULL);
	}

	while (F_ISSET(conn, WT_SERVER_RUN)) {
		WT_VERBOSE(session, READSERVER, "read server sleeping");
		__wt_cond_wait(session, cache->read_cond);
		if (!F_ISSET(conn, WT_SERVER_RUN))
			break;
		WT_VERBOSE(session, READSERVER, "read server waking");

		/*
		 * Walk the read-request queue, looking for reads (defined by
		 * a valid WT_SESSION_IMPL handle).  If we find a read request,
		 * perform it, flush the result and clear the request slot,
		 * then wake up the requesting thread.
		 */
		do {
			didwork = 0;
			WT_READ_REQ_FOREACH(rr, rr_end, cache) {
				if ((request_session = rr->session) == NULL)
					continue;
				if (cache->read_lockout)
					continue;
				didwork = 1;

				/* Reference the correct WT_BTREE handle. */
				WT_SET_BTREE_IN_SESSION(
				    session, request_session->btree);

				ret = __cache_read(session,
				    rr->parent, rr->ref, rr->dsk_verify);

				/*
				 * The request slot clear doesn't need to be
				 * flushed, but we have to flush the read
				 * result, might as well include it.
				 */
				__cache_read_req_clr(rr);
				__wt_session_serialize_wrapup(
				    request_session, NULL, ret);

				WT_CLEAR_BTREE_IN_SESSION(session);
			}
		} while (didwork);
	}

	WT_VERBOSE(session, READSERVER, "read server exiting");
	(void)session->iface.close(&session->iface, NULL);

	return (NULL);
}

/*
 * __cache_read --
 *	Read a page from the file.
 */
static int
__cache_read(
    WT_SESSION_IMPL *session, WT_PAGE *parent, WT_REF *ref, int dsk_verify)
{
	WT_BUF *tmp;
	WT_PAGE_DISK *dsk;
	uint32_t addr, size;
	int ret;

	tmp = NULL;
	dsk = NULL;
	addr = ref->addr;
	size = ref->size;
	ret = 0;

	/* Review the possible page states. */
	switch (ref->state) {
	case WT_REF_DISK:
		/* Page is on disk, and that's our problem.  Read it. */
		break;
	case WT_REF_MEM:
		/* Page is in memory, must have already been read. */
		return (0);
	case WT_REF_LOCKED:
		/* Page being considered for eviction: not our problem. */
		return (0);
	}

	/*
	 * The page isn't in the cache, and since we're the only path for the
	 * page to get into the cache, we don't have to worry further, and we
	 * might as well get to it.
	 */
	WT_VERBOSE(
	    session, READSERVER, "read %" PRIu32 "/%" PRIu32, addr, size);

	/* Allocate memory for the page's disk image. */
	WT_RET(__wt_scr_alloc(session, size, &tmp));

	/* Read the page and steal the resulting buffer. */
	WT_ERR(__wt_block_read(
	    session, tmp, addr, size, dsk_verify ? WT_VERIFY : 0));

	dsk = __wt_buf_steal(session, tmp, &size);
	__wt_scr_free(&tmp);

	/*
	 * Build the in-memory version of the page, then re-load the disk
	 * reference: the disk image may have been discarded, use whatever
	 * the page has, or NULL if it doesn't have one.
	 */
	WT_ERR(__wt_page_inmem(session, parent, ref, dsk, &ref->page));
	/* The disk image may have been discarded, use the one in the page. */
	dsk = ref->page->dsk;

	/* Add the page to our cache statistics. */
	__wt_cache_page_read(session, ref->page,
	    sizeof(WT_PAGE) + ((dsk == NULL) ? 0 : dsk->memsize));

	/* No memory flush required, the state variable is volatile. */
	ref->state = WT_REF_MEM;

	return (0);

err:	__wt_scr_free(&tmp);
	__wt_free(session, dsk);
	return (ret);
}
