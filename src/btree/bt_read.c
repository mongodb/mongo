/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wt_internal.h"

#define	WT_READ_REQ_FOREACH(rr, rr_end, cache)				\
	for ((rr) = (cache)->read_request,				\
	    (rr_end) = (rr) + (cache)->max_read_request;		\
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
__wt_read_server_wake(WT_SESSION_IMPL *session, int force)
{
	WT_CACHE *cache;

	cache = S2C(session)->cache;

	if (!force)
		__wt_eviction_check(session, NULL);

	/* If reads are locked out, eviction will signal the read thread. */
	if (force || !cache->read_lockout)
		__wt_cond_signal(session, cache->read_cond);
}

/*
 * __wt_read_begin_serial_func --
 *	Serialization function called when a page-in requires a read.
 *	The return value indicates whether the read should proceed in
 *	the worker thread.
 */
void
__wt_read_begin_serial_func(WT_SESSION_IMPL *session)
{
	WT_CACHE *cache;
	WT_REF *ref;

	cache = S2C(session)->cache;

	__wt_read_begin_unpack(session, &ref);

	__wt_eviction_check(session, NULL);

	if (cache->read_lockout || ref->state != WT_REF_DISK)
		__wt_session_serialize_wrapup(session, NULL, WT_RESTART);
	else {
		ref->state = WT_REF_READING;
		__wt_session_serialize_wrapup(session, NULL, 0);
	}
}

/*
 * __wt_read_end_serial_func --
 *	Serialization function called when a page-in is complete.
 */
void
__wt_read_end_serial_func(WT_SESSION_IMPL *session)
{
	WT_PAGE_DISK *dsk;
	WT_REF *ref;

	__wt_read_end_unpack(session, &ref);

	/* Add the page to our cache statistics. */
	dsk = ref->page->dsk;
	__wt_cache_page_read(session, ref->page,
	    sizeof(WT_PAGE) + ((dsk == NULL) ? 0 : dsk->memsize));

	ref->state = WT_REF_MEM;

	__wt_session_serialize_wrapup(session, NULL, 0);
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
		WT_VERBOSE(session, readserver, "sleeping");
		__wt_cond_wait(session, cache->read_cond);
		if (!F_ISSET(conn, WT_SERVER_RUN))
			break;
		WT_VERBOSE(session, readserver, "waking");

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

				ret = __wt_cache_read(session,
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

	WT_VERBOSE(session, readserver, "exiting");
	(void)session->iface.close(&session->iface, NULL);

	return (NULL);
}

/*
 * __wt_cache_read --
 *	Read a page from the file.
 */
int
__wt_cache_read(
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
	case WT_REF_READING:
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
	 *
	 * Allocate memory for the page's disk image.
	 */
	WT_RET(__wt_scr_alloc(session, size, &tmp));

	/* Read the page and steal the resulting buffer. */
	WT_ERR(__wt_block_read(
	    session, tmp, addr, size, dsk_verify ? WT_VERIFY : 0));

	dsk = __wt_buf_steal(session, tmp, NULL);
	__wt_scr_free(&tmp);

	/*
	 * Build the in-memory version of the page, then re-load the disk
	 * reference: the disk image may have been discarded, use whatever
	 * the page has, or NULL if it doesn't have one.
	 */
	WT_ERR(__wt_page_inmem(session, parent, ref, dsk, &ref->page));

	WT_VERBOSE(session, read,
	    "page %p (%" PRIu32 "/%" PRIu32 ", %s)",
	    ref->page, addr, size, __wt_page_type_string(ref->page->type));

	return (0);

err:	__wt_scr_free(&tmp);
	__wt_free(session, dsk);
	return (ret);
}
