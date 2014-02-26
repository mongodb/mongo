/*-
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __wt_session_fotxn_add --
 *	Add a new entry into the session's free-on-transaction generation list.
 */
int
__wt_session_fotxn_add(WT_SESSION_IMPL *session, const void *p)
{
	WT_FOTXN *fotxn;
	size_t i;

	/* Grow the list as necessary. */
	WT_RET(__wt_realloc_def(session,
	    &session->fotxn_size, session->fotxn_cnt + 1, &session->fotxn));

	/* Find an empty slot. */
	for (i = 0, fotxn = session->fotxn;
	    i < session->fotxn_size / sizeof(session->fotxn[0]);  ++i, ++fotxn)
		if (fotxn->p == NULL) {
			fotxn->p = p;
			fotxn->txnid = S2C(session)->txn_global.current;
			break;
		}
	++session->fotxn_cnt;

	/* See if we can free any previous entries. */
	if (session->fotxn_cnt > 1)
		__wt_session_fotxn_discard(session, 0);

	return (0);
}

/*
 * __wt_session_fotxn_discard --
 *	Discard any memory the session accumulated.
 */
void
__wt_session_fotxn_discard(WT_SESSION_IMPL *session, int connection_close)
{
	WT_FOTXN *fotxn;
	uint64_t oldest_id;
	size_t i;

	/*
	 * Get the oldest transaction ID not yet visible to a running
	 * transaction.
	 */
	oldest_id = S2C(session)->txn_global.oldest_id;

	for (i = 0, fotxn = session->fotxn;
	    session->fotxn_cnt > 0 &&
	    i < session->fotxn_size / sizeof(session->fotxn[0]);  ++i, ++fotxn)
		if (fotxn->p != NULL &&
		    (connection_close || TXNID_LT(fotxn->txnid, oldest_id))) {
#ifdef HAVE_DIAGNOSTIC
			/*
			 * It's a bad thing if another thread is in this array
			 * after we free it, and it's a race I want to find.
			 * Make sure nothing good happens to that thread.
			 */
			{
			WT_PAGE_INDEX *pindex = (WT_PAGE_INDEX *)fotxn->p;
			memset(pindex, WT_DEBUG_BYTE,
			    sizeof(WT_PAGE_INDEX) +
			    pindex->entries * sizeof(WT_REF *));
			}
#endif
			--session->fotxn_cnt;
			__wt_free(session, fotxn->p);
		}
	if (connection_close)
		__wt_free(session, session->fotxn);
}
