/*-
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __sweep --
 *	Close unused dhandles on the connection dhandle list.
 */
static int
__sweep(WT_SESSION_IMPL *session)
{
	WT_CONNECTION_IMPL *conn;
	WT_DATA_HANDLE *dhandle, *dhandle_next;
	WT_DECL_RET;
	time_t now;

	conn = S2C(session);

	/*
	 * Session's cache handles unless the session itself is closed, at which
	 * time the handle reference counts are immediately decremented.  Don't
	 * discard handles that have been open recently.
	 */
	WT_RET(__wt_seconds(session, &now));

	dhandle = SLIST_FIRST(&conn->dhlh);
	for (; dhandle != NULL; dhandle = dhandle_next) {
		dhandle_next = SLIST_NEXT(dhandle, l);
		if (dhandle->session_ref != 0 ||
		    now - dhandle->timeofdeath <= WT_DHANDLE_SWEEP_WAIT)
			continue;

		/*
		 * We have a candidate for closing; if it's open, flush dirty
		 * leaf pages, then acquire an exclusive lock on the handle
		 * and close it. We might be blocking opens for a long time
		 * (over disk I/O), but the handle was quiescent for awhile.
		 *
		 * The close can fail if an update cannot be written (updates in
		 * a no-longer-referenced file might not yet be globally visible
		 * if sessions have disjoint sets of files open).  If the handle
		 * is busy, skip it, we'll retry the close the next time, after
		 * the transaction state has progressed.
		 */
		if (F_ISSET(dhandle, WT_DHANDLE_OPEN)) {
			WT_WITH_DHANDLE(session, dhandle,
			    ret = __wt_cache_op(
			    session, NULL, WT_SYNC_WRITE_LEAVES));
			WT_RET(ret);

			/*
			 * We don't set WT_DHANDLE_EXCLUSIVE deliberately, we
			 * want opens to block on us rather than returning an
			 * EBUSY error to the application.
			 */
			ret = __wt_try_writelock(session, dhandle->rwlock);
			if (ret == EBUSY) {
				ret = 0;
				continue;
			}
			WT_RET(ret);

			WT_WITH_DHANDLE(session, dhandle,
			    ret = __wt_conn_btree_sync_and_close(session, 0));
			if (ret == EBUSY)
				ret = 0;

			WT_TRET(__wt_writeunlock(session, dhandle->rwlock));
			WT_RET(ret);
		}

		/*
		 * Attempt to discard the handle (the called function checks the
		 * handle-open flag after acquiring appropriate locks, which is
		 * why we don't do any special handling of EBUSY returns above,
		 * that path never cleared the handle-open flag.
		 */
		ret = __wt_conn_dhandle_discard_single(session, dhandle, 0);
		if (ret == EBUSY)
			ret = 0;
		WT_RET(ret);
	}
	return (0);
}

/*
 * __sweep_server --
 *	The handle sweep server thread.
 */
static void *
__sweep_server(void *arg)
{
	WT_CONNECTION_IMPL *conn;
	WT_DECL_RET;
	WT_SESSION_IMPL *session;

	session = arg;
	conn = S2C(session);

	/*
	 * Sweep for dead handles.
	 */
	while (F_ISSET(conn, WT_CONN_SERVER_RUN) &&
	    F_ISSET(conn, WT_CONN_SERVER_SWEEP)) {

		/* Wait until the next event. */
		WT_ERR(
		    __wt_cond_wait(session, conn->sweep_cond, 30 * WT_MILLION));

		/* Sweep the handles. */
		WT_ERR(__sweep(session));
	}

	if (0) {
err:		WT_PANIC_MSG(session, ret, "handle sweep server error");
	}
	return (NULL);
}

/*
 * __wt_sweep_create --
 *	Start the handle sweep thread.
 */
int
__wt_sweep_create(WT_SESSION_IMPL *session)
{
	WT_CONNECTION_IMPL *conn;

	conn = S2C(session);

	/* Set first, the thread might run before we finish up. */
	F_SET(conn, WT_CONN_SERVER_SWEEP);

	WT_RET(__wt_open_internal_session(
	    conn, "sweep-server", 1, 1, &conn->sweep_session));
	session = conn->sweep_session;

	/*
	 * Handle sweep does enough I/O it may be called upon to perform slow
	 * operations for the block manager.
	 */
	F_SET(session, WT_SESSION_CAN_WAIT);

	WT_RET(__wt_cond_alloc(
	    session, "handle sweep server", 0, &conn->sweep_cond));

	WT_RET(__wt_thread_create(
	    session, &conn->sweep_tid, __sweep_server, session));
	conn->sweep_tid_set = 1;

	return (0);
}

/*
 * __wt_sweep_destroy --
 *	Destroy the handle-sweep thread.
 */
int
__wt_sweep_destroy(WT_SESSION_IMPL *session)
{
	WT_CONNECTION_IMPL *conn;
	WT_DECL_RET;
	WT_SESSION *wt_session;

	conn = S2C(session);

	F_CLR(conn, WT_CONN_SERVER_SWEEP);
	if (conn->sweep_tid_set) {
		WT_TRET(__wt_cond_signal(session, conn->sweep_cond));
		WT_TRET(__wt_thread_join(session, conn->sweep_tid));
		conn->sweep_tid_set = 0;
	}
	WT_TRET(__wt_cond_destroy(session, &conn->sweep_cond));

	if (conn->sweep_session != NULL) {
		wt_session = &conn->sweep_session->iface;
		WT_TRET(wt_session->close(wt_session, NULL));

		conn->sweep_session = NULL;
	}
	return (ret);
}
