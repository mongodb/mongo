/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wt_internal.h"

#undef	STATIN
#define	STATIN	static inline

static int  __cache_read(WT_SESSION_IMPL *, WT_PAGE *, WT_REF *, int);
STATIN void __cache_read_req_clr(WT_READ_REQ *);
STATIN void __cache_read_req_set(
		WT_SESSION_IMPL *, WT_READ_REQ *, WT_PAGE *, WT_REF *, int);

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
	WT_MEMORY_FLUSH;		/* Flush before turning entry on */

	rr->session = session;
	WT_MEMORY_FLUSH;		/* Turn entry on */
}

/*
 * __cache_read_req_set --
 *	Clear a request slot.
 */
static inline void
__cache_read_req_clr(WT_READ_REQ *rr)
{
	rr->session = NULL;
	WT_MEMORY_FLUSH;		/* Turn entry off */
}

/*
 * __wt_workq_read_server --
 *	See if the read server thread needs to be awakened.
 */
void
__wt_workq_read_server(WT_CONNECTION_IMPL *conn, int force)
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
	if (cache->read_lockout) {
		if (bytes_inuse <= bytes_max - (bytes_max / 20))
			cache->read_lockout = 0;
	} else if (bytes_inuse > bytes_max + (bytes_max / 10)) {
		WT_VERBOSE(session, READSERVER,
		    "workQ locks out reads: "
		    "bytes-inuse %" PRIu64 " of bytes-max %" PRIu64,
		    bytes_inuse, bytes_max);
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
			__cache_read_req_set
			    (session, rr, parent, ref, dsk_verify);
			return (0);
		}
	__wt_errx(session, "read server request table full");
	return (WT_ERROR);
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
	WT_SESSION *wt_session;
	int didwork, ret;

	conn = arg;
	cache = conn->cache;
	ret = 0;

	/*
	 * We need a thread of control because we're reading/writing pages.
	 * Start with the default session to keep error handling simple.
	 *
	 * There is some complexity involved in using the public API, because
	 * public sessions are implicitly closed during WT_CONNECTION->close.
	 * If the eviction thread's session were to go on the public list, the
	 * eviction thread would have to be shut down before the public session
	 * handles are closed.
	 */
	session = &conn->default_session;
	if ((ret = conn->iface.open_session(&conn->iface,
	    NULL, NULL, &wt_session)) != 0) {
		__wt_err(session, ret, "cache read server error");
		return (NULL);
	}
	session = (WT_SESSION_IMPL *)wt_session;
	/*
	 * Don't close this session during WT_CONNECTION->close: we do it
	 * before the thread completes.
	 */
	F_SET(session, WT_SESSION_INTERNAL);

	for (;;) {
		WT_VERBOSE(session, READSERVER, "read server sleeping");
		cache->read_sleeping = 1;
		__wt_lock(session, cache->mtx_read);
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
	(void)wt_session->close(wt_session, NULL);

	return (NULL);
}

/*
 * __wt_workq_read_server_exit --
 *	The exit flag is set, wake the read server to exit.
 */
void
__wt_workq_read_server_exit(WT_CONNECTION_IMPL *conn)
{
	WT_SESSION_IMPL *session;
	WT_CACHE *cache;

	session = &conn->default_session;
	cache = conn->cache;

	__wt_unlock(session, cache->mtx_read);
}

/*
 * __cache_read --
 *	Read a page from the file.
 */
static int
__cache_read(
    WT_SESSION_IMPL *session, WT_PAGE *parent, WT_REF *ref, int dsk_verify)
{
	WT_PAGE_DISK *dsk;
	uint32_t addr, size;
	int ret;

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

	/*
	 * Allocate memory for the page (memory for the in-memory version of the
	 * page is allocated later (they're two separate allocation calls so we
	 * hopefully get better alignment from the underlying heap allocator).
	 */
	WT_ERR(__wt_calloc(session, (size_t)size, sizeof(uint8_t), &dsk));

	/* Read the page. */
	WT_ERR(__wt_disk_read(session, dsk, addr, size));

	/* Verify the disk image on demand. */
	if (dsk_verify)
		WT_ERR(__wt_verify_dsk(session, dsk, addr, size, 0));

	/* Build the in-memory version of the page. */
	WT_ERR(__wt_page_inmem(session, parent, ref, dsk, &ref->page));

	/* Add the page to our cache statistics. */
	__wt_cache_page_read(session, ref->page, WT_SIZEOF32(WT_PAGE) + size);

	/* No memory flush required, the state variable is volatile. */
	ref->state = WT_REF_MEM;

	return (0);

err:	__wt_free(session, dsk);
	return (ret);
}
