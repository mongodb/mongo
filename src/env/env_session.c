/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wt_internal.h"

/*
 * __wt_connection_session --
 *	CONNECTION.session method.
 */
int
__wt_connection_session(CONNECTION *conn, SESSION **sessionp)
{
	SESSION *session;
	uint32_t slot;

	*sessionp = NULL;

	/* Check to see if there's an available SESSION slot. */
	if (conn->toc_cnt == conn->session_size - 1) {
		__wt_err(&conn->default_session, 0,
		    "WiredTiger only configured to support %d thread contexts",
		    conn->session_size);
		return (WT_ERROR);
	}

	/*
	 * The SESSION reference list is compact, the SESSION array is not.
	 * Find the first empty SESSION slot.
	 */
	for (slot = 0, session = conn->toc_array;
	    session->iface.connection != NULL;
	    ++session, ++slot)
		;

	/* Clear previous contents of the SESSION entry, they get re-used. */
	memset(session, 0, sizeof(SESSION));

	session->iface.connection = &conn->iface;
	session->hazard = conn->hazard + slot * conn->hazard_size;

	/* We can't use the new session: it hasn't been configured yet. */
	WT_RET(__wt_mtx_alloc(
	    &conn->default_session, "session", 1, &session->mtx));

	__wt_methods_session_lockout(session);
	__wt_methods_session_init_transition(session);

	/* Make the entry visible to the workQ. */
	conn->sessions[conn->toc_cnt++] = session;
	WT_MEMORY_FLUSH;

	*sessionp = session;
	return (0);
}

/*
 * __wt_session_close --
 *	SESSION.close method.
 */
int
__wt_session_close(SESSION *session)
{
	CONNECTION *conn;
	SESSION **tp;
	SESSION_BUFFER *sb;
	int ret;

	conn = S2C(session);
	ret = 0;

	WT_CONN_FCHK_RET(
	    conn, "SESSION.close", session->flags, WT_APIMASK_SESSION, ret);

	/*
	 * The "in" reference count is artificially incremented by 1 as
	 * long as an SESSION buffer is referenced by the SESSION thread;
	 * we don't want them freed because a page was evicted and their
	 * count went to 0.  Decrement the reference count on the buffer
	 * as part of releasing it.  There's a similar reference count
	 * decrement when the SESSION structure is discarded.
	 *
	 * XXX
	 * There's a race here: if this code, or the SESSION structure
	 * close code, and the page discard code race, it's possible
	 * neither will realize the buffer is no longer needed and free
	 * it.  The fix is to involve the eviction or workQ threads:
	 * they may need a linked list of buffers they review to ensure
	 * it never happens.  I'm living with this now: it's unlikely
	 * and it's a memory leak if it ever happens.
	 */
	sb = session->sb;
	if (sb != NULL && --sb->in == sb->out)
		__wt_free(session, sb);

	/* Discard WT_ITEM memory. */
	__wt_free(session, session->key.item.data);
	__wt_free(session, session->value.item.data);
	__wt_scr_free(session);

	/* Unlock and destroy the thread's mutex. */
	if (session->mtx != NULL) {
		__wt_unlock(session, session->mtx);
		(void)__wt_mtx_destroy(session, session->mtx);
	}

	/*
	 * Replace the SESSION reference we're closing with the last entry in
	 * the table, then clear the last entry.  As far as the walk of the
	 * workQ is concerned, it's OK if the SESSION appears twice, or if it
	 * doesn't appear at all, so these lines can race all they want.
	 */
	for (tp = conn->sessions; *tp != session; ++tp)
		;
	--conn->toc_cnt;
	*tp = conn->sessions[conn->toc_cnt];
	conn->sessions[conn->toc_cnt] = NULL;

	/* Make the SESSION array entry available for re-use. */
	session->iface.connection = NULL;
	WT_MEMORY_FLUSH;

	return (ret);
}

/*
 * __wt_session_api_set --
 *	Pair SESSION and BTREE handle, allocating the SESSION as necessary.
 */
int
__wt_session_api_set(
    CONNECTION *conn, const char *name, BTREE *btree, SESSION **sessionp)
{
	SESSION *session;

	/*
	 * We pass around SESSIONs internally in the Btree, (rather than a
	 * BTREE), because the BTREE's are free-threaded, and the SESSIONs are
	 * per-thread.  Lots of the API calls don't require the application to
	 * allocate and manage the SESSION, which means we have to do it for
	 * them.
	 *
	 * SESSIONs always reference a BTREE handle, and we do that here, as
	 * well.
	 */
	if ((session = *sessionp) == NULL) {
		WT_RET(conn->session(conn, 0, sessionp));
		session = *sessionp;
	}
	session->btree = btree;
	session->name = name;
	return (0);
}

/*
 * __wt_session_api_clr --
 *	Clear the SESSION, freeing it if it was allocated by the library.
 */
int
__wt_session_api_clr(SESSION *session, const char *name, int islocal)
{
	/*
	 * The SESSION should hold no more hazard references; this is a
	 * diagnostic check, but it's cheap so we do it all the time.
	 */
	__wt_hazard_empty(session, name);

	if (islocal)
		return (session->close(session, 0));

	session->btree = NULL;
	session->name = NULL;
	return (0);
}

#ifdef HAVE_DIAGNOSTIC
static const char *__wt_session_print_state(SESSION *);

int
__wt_session_dump(CONNECTION *conn)
{
	WT_MBUF mb;
	SESSION *session, **tp;
	WT_PAGE **hp;

	__wt_mb_init(&conn->default_session, &mb);

	__wt_mb_add(&mb, "%s\n", conn->sep);
	for (tp = conn->sessions; (session = *tp) != NULL; ++tp) {
		__wt_mb_add(&mb,
		    "session: %p {\n\tworkq func: ", session);
		if (session->wq_func == NULL)
			__wt_mb_add(&mb, "none");
		else
			__wt_mb_add(&mb, "%p", session->wq_func);

		__wt_mb_add(&mb,
		    " state: %s", __wt_session_print_state(session));

		__wt_mb_add(&mb, "\n\thazard: ");
		for (hp = session->hazard;
		    hp < session->hazard + conn->hazard_size; ++hp)
			__wt_mb_add(&mb, "%p ", *hp);

		__wt_mb_add(&mb, "\n}");
		if (session->name != NULL)
			__wt_mb_add(&mb, " %s", session->name);
		__wt_mb_write(&mb);
	}

	__wt_mb_discard(&mb);
	return (0);
}

/*
 * __wt_session_print_state --
 *	Return the SESSION state as a string.
 */
static const char *
__wt_session_print_state(SESSION *session)
{
	switch (session->wq_state) {
	case WT_WORKQ_READ:
		return ("read");
	case WT_WORKQ_READ_SCHED:
		return ("read scheduled");
	case WT_WORKQ_FUNC:
		return ("function");
	case WT_WORKQ_NONE:
		return ("none");
	}
	return ("unknown");
	/* NOTREACHED */
}
#endif
