/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wt_internal.h"

/*
 * Serialization: serialization support allows scheduling operations requiring
 * serialized access to a piece of memory, normally by a different thread of
 * control.  This includes reading pages into memory (a request of the read
 * thread), and updating Btree pages.
 *
 * __wt_session_serialize_func --
 *	Schedule a serialization request, and block or spin until it completes.
 */
int
__wt_session_serialize_func(WT_SESSION_IMPL *session,
    wq_state_t op, void (*func)(WT_SESSION_IMPL *), void *args)
{
	WT_CONNECTION_IMPL *conn;

	conn = S2C(session);

	/*
	 * Threads serializing access to data using a function:
	 *	call the function while holding a spinlock
	 *	update the session sleeping state, and
	 *	if necessary, block until an async action completes.
	 */
	session->wq_args = args;
	session->wq_sleeping = (op != WT_SERIAL_FUNC);

	/* Functions are serialized by holding a spinlock. */
	__wt_spin_lock(session, &conn->serial_lock);
	func(session);
	__wt_spin_unlock(session, &conn->serial_lock);

	switch (op) {
	case WT_SERIAL_EVICT:
		__wt_evict_server_wake(conn, 1);
		break;
	case WT_SERIAL_READ:
		__wt_read_server_wake(conn, 0);
		break;
	default:
		return (session->wq_ret);
	}

	/*
	 * If we are waiting on a server thread, block on the session
	 * mutex: when the operation is complete, this will be unlocked
	 * and we can continue.
	 */
	__wt_lock(session, session->mtx);
	return (session->wq_ret);
}

/*
 * __wt_session_serialize_wrapup --
 *	Server function cleanup.
 */
void
__wt_session_serialize_wrapup(WT_SESSION_IMPL *session, WT_PAGE *page, int ret)
{
	session->wq_ret = ret;			/* Set the return value. */

	/* If passed a page and the return value is OK, we modified the page. */
	if (page != NULL && ret == 0) {
		/*
		 * Publish: there must be a barrier to ensure that all changes
		 * to the page are flushed before we update the page's write
		 * generation, otherwise a thread searching the page might see
		 * the page's write generation update before the changes to the
		 * page, which breaks the protocol.
		 */
		WT_WRITE_BARRIER();
		WT_PAGE_SET_MODIFIED(page);
	}

	/*
	 * Publish: there must be a barrier to ensure the return value is set
	 * before the calling thread can see its results, and the page's new
	 * write generation makes it to memory.  The latter isn't a correctness
	 * issue, the write generation just needs to be updated so that readers
	 * get credit for reading the right version of the page, otherwise,
	 * they will have to retry their update for reading an old version of
	 * the page.
	 */
	WT_PUBLISH(session->wq_state, WT_SERIAL_NONE);

	/* If the calling thread is sleeping, wake it up. */
	if (session->wq_sleeping)
		__wt_unlock(session, session->mtx);
}
