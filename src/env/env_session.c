/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wt_internal.h"

#ifdef HAVE_DIAGNOSTIC
static const char *__wt_session_print_state(WT_SESSION_IMPL *);

/*
 * __wt_session_dump --
 *	Dump information about open sessions.
 */
void
__wt_session_dump(WT_SESSION_IMPL *session)
{
	WT_CONNECTION_IMPL *conn;
	WT_SESSION_IMPL **tp;
	WT_HAZARD *hp;
	WT_MBUF mb;

	conn = S2C(session);

	__wt_mb_init(&conn->default_session, &mb);

	__wt_mb_add(&mb, "%s\n", conn->sep);
	for (tp = conn->sessions; (session = *tp) != NULL; ++tp) {
		__wt_mb_add(&mb, "session: %p {\n", session);
		if (session->wq_func == NULL)
			__wt_mb_add(&mb, "\tworkq func: none\n");
		else
			__wt_mb_add(
			    &mb, "\tworkq func: %p\n", session->wq_func);

		__wt_mb_add(&mb,
		    "\tstate: %s\n", __wt_session_print_state(session));

		for (hp = session->hazard;
		    hp < session->hazard + conn->hazard_size; ++hp) {
			if (hp->page == NULL)
				continue;
#ifdef HAVE_DIAGNOSTIC
			__wt_mb_add(&mb, "\thazard: %lu (%s, line %d)\n",
			    WT_PADDR(hp->page), hp->file, hp->line);
#else
			__wt_mb_add(&mb, "\thazard: %lu\n", WT_PADDR(hp->page));
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
 *	Return the WT_SESSION_IMPL state as a string.
 */
static const char *
__wt_session_print_state(WT_SESSION_IMPL *session)
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
