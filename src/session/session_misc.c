/*-
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __wt_oldest_split_gen --
 *	Calculate the oldest active split generation.
 */
uint64_t
__wt_oldest_split_gen(WT_SESSION_IMPL *session)
{
	WT_CONNECTION_IMPL *conn;
	WT_SESSION_IMPL *s;
	uint64_t gen, oldest;
	u_int i, session_cnt;

	conn = S2C(session);
	WT_ORDERED_READ(session_cnt, conn->session_cnt);
	for (i = 0, s = conn->sessions, oldest = conn->split_gen + 1;
	    i < session_cnt;
	    i++, s++)
		if (((gen = s->split_gen) != 0) && gen < oldest)
			oldest = gen;

	return (oldest);
}

/*
 * __wt_session_fotxn_add --
 *	Add a new entry into the session's free-on-transaction generation list.
 */
int
__wt_session_fotxn_add(WT_SESSION_IMPL *session, void *p, size_t len)
{
	WT_FOTXN *fotxn;

	WT_ASSERT(session, p != NULL);

	/* Grow the list as necessary. */
	WT_RET(__wt_realloc_def(session,
	    &session->fotxn_size, session->fotxn_cnt + 1, &session->fotxn));

	fotxn = session->fotxn + session->fotxn_cnt++;
	fotxn->split_gen = WT_ATOMIC_ADD(S2C(session)->split_gen, 1);
	fotxn->p = p;
	fotxn->len = len;

	WT_STAT_FAST_CONN_ATOMIC_INCRV(session, rec_split_stashed_bytes, len);
	WT_STAT_FAST_CONN_ATOMIC_INCR(session, rec_split_stashed_objects);

	/* See if we can free any previous entries. */
	if (session->fotxn_cnt > 1)
		__wt_session_fotxn_discard(session);

	return (0);
}

/*
 * __wt_session_fotxn_discard --
 *	Discard any memory from the session's free-on-transaction generation
 *	list that we can.
 */
void
__wt_session_fotxn_discard(WT_SESSION_IMPL *session)
{
	WT_FOTXN *fotxn;
	uint64_t oldest;
	size_t i;

	/* Get the oldest split generation. */
	oldest = __wt_oldest_split_gen(session);

	for (i = 0, fotxn = session->fotxn;
	    i < session->fotxn_cnt;
	    ++i, ++fotxn) {
		if (fotxn->p == NULL)
			continue;
		else if (fotxn->split_gen >= oldest)
			break;
		/*
		 * It's a bad thing if another thread is in this memory
		 * after we free it, make sure nothing good happens to
		 * that thread.
		 */
		WT_STAT_FAST_CONN_ATOMIC_DECRV(
		    session, rec_split_stashed_bytes, fotxn->len);
		WT_STAT_FAST_CONN_ATOMIC_DECR(
		    session, rec_split_stashed_objects);
		__wt_overwrite_and_free_len(session, fotxn->p, fotxn->len);
	}

	/*
	 * If there are enough free slots at the beginning of the list, shuffle
	 * everything down.
	 */
	if (i > 100 || i == session->fotxn_cnt) {
		if ((session->fotxn_cnt -= i) > 0)
			memmove(session->fotxn, fotxn,
			    session->fotxn_cnt * sizeof(*fotxn));
		memset(session->fotxn + session->fotxn_cnt, 0,
		    i * sizeof(*fotxn));
	}
}

/*
 * __wt_session_fotxn_discard_all --
 *	Discard all memory from a session's free-on-transaction generation
 *	list.
 */
void
__wt_session_fotxn_discard_all(
    WT_SESSION_IMPL *session_safe, WT_SESSION_IMPL *session)
{
	WT_FOTXN *fotxn;
	size_t i;

	/*
	 * This function is called during WT_CONNECTION.close to discard any
	 * memory that remains.  For that reason, we take two WT_SESSION_IMPL
	 * arguments: session_safe is still linked to the WT_CONNECTION and
	 * can be safely used for calls to other WiredTiger functions, while
	 * session is the WT_SESSION_IMPL we're cleaning up.
	 */
	for (i = 0, fotxn = session->fotxn;
	    i < session->fotxn_cnt;
	    ++i, ++fotxn)
		if (fotxn->p != NULL)
			__wt_free(session_safe, fotxn->p);

	__wt_free(session_safe, session->fotxn);
}
