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

	/* Check to see if there's an available session slot. */
	if (conn->toc_cnt == conn->session_size - 1) {
		__wt_err(&conn->default_session, 0,
		    "WiredTiger only configured to support %d thread contexts",
		    conn->session_size);
		return (WT_ERROR);
	}

	/*
	 * The session reference list is compact, the session array is not.
	 * Find the first empty session slot.
	 */
	for (slot = 0, session = conn->toc_array;
	    session->iface.connection != NULL;
	    ++session, ++slot)
		;

	/* Session entries are re-used, clear the old contents. */
	WT_CLEAR(*session);

	session->iface.connection = &conn->iface;
	session->event_handler = conn->default_session.event_handler;
	session->hazard = conn->hazard + slot * conn->hazard_size;

	/* We can't use the new session: it hasn't been configured yet. */
	WT_RET(__wt_mtx_alloc(
	    &conn->default_session, "session", 1, &session->mtx));

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
	int ret;

	conn = S2C(session);
	ret = 0;

	/* Unpin the current per-SESSION buffer. */
	if (session->sb != NULL)
		__wt_sb_decrement(session, session->sb);

	/* Discard scratch buffers. */
	__wt_scr_free(session);

	/* Unlock and destroy the thread's mutex. */
	if (session->mtx != NULL) {
		__wt_unlock(session, session->mtx);
		(void)__wt_mtx_destroy(session, session->mtx);
	}

	/*
	 * Replace the session reference we're closing with the last entry in
	 * the table, then clear the last entry.  As far as the walk of the
	 * workQ is concerned, it's OK if the session appears twice, or if it
	 * doesn't appear at all, so these lines can race all they want.
	 */
	for (tp = conn->sessions; *tp != session; ++tp)
		;
	--conn->toc_cnt;
	*tp = conn->sessions[conn->toc_cnt];
	conn->sessions[conn->toc_cnt] = NULL;

	/* Make the session array entry available for re-use. */
	session->iface.connection = NULL;
	WT_MEMORY_FLUSH;

	return (ret);
}

#ifdef HAVE_DIAGNOSTIC
static const char *__wt_session_print_state(SESSION *);

/*
 * __wt_session_dump --
 *	Dump information about open sessions.
 */
void
__wt_session_dump(SESSION *session)
{
	CONNECTION *conn;
	SESSION **tp;
	WT_HAZARD *hp;
	WT_MBUF mb;

	conn = S2C(session);

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
		    hp < session->hazard + conn->hazard_size; ++hp) {
			if (hp->page == NULL)
				continue;
#ifdef HAVE_DIAGNOSTIC
			__wt_mb_add(&mb, "\t\t%lu: %s, line %d\n",
			    WT_PADDR(hp->page), hp->file, hp->line);
#else
			__wt_mb_add(&mb, "\t\t%lu\n", WT_PADDR(hp->page));
#endif
		}

		__wt_mb_add(&mb, "}");
		if (session->name != NULL)
			__wt_mb_add(&mb, " %s", session->name);
		__wt_mb_write(&mb);
	}

	__wt_mb_discard(&mb);
}

/*
 * __wt_session_print_state --
 *	Return the SESSION state as a string.
 */
static const char *
__wt_session_print_state(SESSION *session)
{
	switch (session->wq_state) {
	case WT_WORKQ_EVICT:
		return ("evict");
	case WT_WORKQ_EVICT_SCHED:
		return ("evict scheduled");
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
