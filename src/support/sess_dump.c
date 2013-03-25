/*-
 * Copyright (c) 2008-2013 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

#ifdef HAVE_DIAGNOSTIC
/*
 * __wt_session_dump_all --
 *	Dump information about all open sessions.
 */
void
__wt_session_dump_all(WT_SESSION_IMPL *session)
{
	WT_CONNECTION_IMPL *conn;
	WT_SESSION_IMPL *s;
	uint32_t i;

	if (session == NULL)
		return;

	conn = S2C(session);
	for (s = conn->sessions, i = 0; i < conn->session_size; ++s, ++i)
		if (s->active)
			__wt_session_dump(s);
}

/*
 * __wt_session_dump --
 *	Dump information about a session.
 */
void
__wt_session_dump(WT_SESSION_IMPL *session)
{
	WT_CURSOR *cursor;
	WT_HAZARD *hp;
	int first;

	(void)__wt_msg(session, "session: %s%s%p",
	    session->name == NULL ? "" : session->name,
	    session->name == NULL ? "" : " ", session);

	first = 0;
	TAILQ_FOREACH(cursor, &session->cursors, q) {
		if (++first == 1)
			(void)__wt_msg(session, "\tcursors:");
		(void)__wt_msg(session, "\t\t%p", cursor);
	}

	first = 0;
	for (hp = session->hazard;
	    hp < session->hazard + session->hazard_size; ++hp) {
		if (hp->page == NULL)
			continue;
		if (++first == 1)
			(void)__wt_msg(session, "\thazard pointers:");
#ifdef HAVE_DIAGNOSTIC
		(void)__wt_msg(session,
		    "\t\t%p (%s, line %d)", hp->page, hp->file, hp->line);
#else
		(void)__wt_msg(session, "\t\t%p", hp->page);
#endif
	}
}
#endif
