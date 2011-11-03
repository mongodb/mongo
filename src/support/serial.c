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
 * thread), and updating Btree pages (a request of the workQ thread).
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
	 *	set a function/argument pair in the WT_SESSION_IMPL handle,
	 *	flush memory,
	 *	update the WT_SESSION_IMPL workq state, and
	 *	spin or block.
	 *
	 * The workQ thread notices the state change and calls the serialization
	 * function.
	 */
	session->wq_args = args;
	session->wq_func = func;
	session->wq_sleeping = op == WT_WORKQ_FUNC ? 0 : 1;

#ifdef HAVE_WORKQ
	/*
	 * Publish: there must be a barrier to ensure the structure fields are
	 * set before wq_state field change makes the entry visible to the workQ
	 * thread.
	 */
	WT_PUBLISH(session->wq_state, op);

	/*
	 * Callers can spin on the session state (implying the call is quickly
	 * satisfied), or block until its mutex is unlocked by another thread
	 * when the operation has completed.
	 */
	if (op == WT_WORKQ_FUNC) {
		while (session->wq_state != WT_WORKQ_NONE)
			__wt_yield();

		/*
		 * Use a read-barrier to ensure we do not load the wq_ret field
		 * until the wq_state field has been published.
		 */
		WT_READ_BARRIER();
	} else
		__wt_lock(session, session->mtx);
#else
	/*
	 * Functions are called directly (holding a spinlock), only
	 * communication with other threads goes through serialization.
	 */
	__wt_spin_lock(session, &conn->workq_lock);
	func(session);
	__wt_spin_unlock(session, &conn->workq_lock);

	switch (op) {
	case WT_WORKQ_EVICT:
		__wt_workq_evict_server(conn, 1);
		__wt_lock(session, session->mtx);
		break;
	case WT_WORKQ_READ:
		__wt_workq_read_server(conn, 0);
		__wt_lock(session, session->mtx);
		break;
	default:
		break;
	}
#endif

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
	 * get credit for reading the right version of the page, otherwise, they
	 * will get bounced by the workQ for reading an old version of the page.
	 */
	WT_PUBLISH(session->wq_state, WT_WORKQ_NONE);

	/* If the calling thread is sleeping, wake it up. */
	if (session->wq_sleeping)
		__wt_unlock(session, session->mtx);
}
