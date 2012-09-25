/*-
 * Copyright (c) 2008-2012 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

#ifdef HAVE_DIAGNOSTIC
static void __hazard_dump(WT_SESSION_IMPL *);
#endif

/*
 * __wt_hazard_set --
 *	Set a hazard reference.
 */
int
__wt_hazard_set(WT_SESSION_IMPL *session, WT_REF *ref, int *busyp
#ifdef HAVE_DIAGNOSTIC
    , const char *file, int line
#endif
    )
{
	WT_BTREE *btree;
	WT_CONNECTION_IMPL *conn;
	WT_HAZARD *hp;

	btree = session->btree;
	conn = S2C(session);
	*busyp = 0;

	/* If a file can never be evicted, hazard references aren't required. */
	if (F_ISSET(btree, WT_BTREE_NO_HAZARD))
		return (0);

	/*
	 * Do the dance:
	 *
	 * The memory location which makes a page "real" is the WT_REF's state
	 * of WT_REF_MEM, which can be set to WT_REF_LOCKED at any time by the
	 * page eviction server.
	 *
	 * Add the WT_REF reference to the session's hazard list and flush the
	 * write, then see if the page's state is still valid.  If so, we can
	 * use the page because the page eviction server will see our hazard
	 * reference before it discards the page (the eviction server sets the
	 * state to WT_REF_LOCKED, then flushes memory and checks the hazard
	 * references).
	 */
	for (hp = session->hazard; ; ++hp) {
		/* Expand the number of hazard references if available.*/
		if (hp >= session->hazard + conn->hazard_size) {
			if (conn->hazard_size >= conn->hazard_max)
				break;
			WT_PUBLISH(conn->hazard_size,
			    WT_MIN(conn->hazard_size + WT_HAZARD_INCR,
			    conn->hazard_max));
		}

		if (hp->page != NULL)
			continue;

		hp->page = ref->page;
#ifdef HAVE_DIAGNOSTIC
		hp->file = file;
		hp->line = line;
#endif
		/* Publish the hazard reference before reading page's state. */
		WT_FULL_BARRIER();

		/*
		 * Check if the page state is still valid, where valid means a
		 * state of WT_REF_MEM or WT_REF_EVICT_WALK and the pointer is
		 * unchanged.  (The pointer can change, it means the page was
		 * evicted between the time we set our hazard reference and the
		 * publication.  It would theoretically be possible for the
		 * page to be evicted and a different page read into the same
		 * memory, so the pointer hasn't changed but the contents have.
		 * That's OK, we found this page in tree's key space, whatever
		 * page we find here is the page page for us to use.)
		 */
		if (ref->page == hp->page &&
		    (ref->state == WT_REF_MEM ||
		    ref->state == WT_REF_EVICT_WALK)) {
			WT_VERBOSE_RET(session, hazard,
			    "session %p hazard %p: set", session, ref->page);

			++session->nhazard;
			return (0);
		}

		/*
		 * The page isn't available, it's being considered for eviction
		 * (or being evicted, for all we know).  If the eviction server
		 * sees our hazard reference before evicting the page, it will
		 * return the page to use, no harm done, if it doesn't, it will
		 * go ahead and complete the eviction.
		 *
		 * We don't bother publishing this update: the worst case is we
		 * prevent some random page from being evicted.
		 */
		hp->page = NULL;
		*busyp = 1;
		return (0);
	}

	__wt_errx(session,
	    "session %p: hazard reference table full", session);
#ifdef HAVE_DIAGNOSTIC
	__hazard_dump(session);
#endif

	return (ENOMEM);
}

/*
 * __wt_hazard_clear --
 *	Clear a hazard reference.
 */
void
__wt_hazard_clear(WT_SESSION_IMPL *session, WT_PAGE *page)
{
	WT_BTREE *btree;
	WT_CONNECTION_IMPL *conn;
	WT_HAZARD *hp;

	btree = session->btree;
	conn = S2C(session);

	/* If a file can never be evicted, hazard references aren't required. */
	if (F_ISSET(btree, WT_BTREE_NO_HAZARD))
		return;

	/*
	 * The default value for a WT_HAZARD slot is NULL, but clearing a
	 * NULL reference isn't a good idea.
	 */
	WT_ASSERT(session, page != NULL);

	/* Clear the caller's hazard pointer. */
	for (hp = session->hazard;
	    hp < session->hazard + conn->hazard_size; ++hp)
		if (hp->page == page) {
			/*
			 * Check to see if the page has grown too big and force
			 * eviction.  We have to request eviction while holding
			 * a hazard reference (else the page might disappear out
			 * from under us), but we can't wake the eviction server
			 * until we've released our hazard reference because our
			 * hazard reference blocks the page eviction.  A little
			 * dance: check the page, schedule the forced eviction,
			 * clear/publish the hazard reference, wake the eviction
			 * server.
			 *
			 * We don't publish the hazard reference clear in the
			 * general case.  It's not required for correctness;
			 * it gives the page server thread faster access to the
			 * page were the page selected for eviction, but the
			 * generation number was just set, so it's unlikely the
			 * page will be selected for eviction.
			 */
			if (__wt_eviction_page_check(session, page)) {
				__wt_evict_page_request(session, page);
				WT_PUBLISH(hp->page, NULL);
				__wt_evict_server_wake(session);
			} else
				hp->page = NULL;

			/*
			 * If this was the last hazard reference in the session,
			 * we may need to update our transactional context.
			 */
			--session->nhazard;
			return;
		}
	__wt_errx(session,
	    "session %p: clear hazard reference %p: not found", session, page);
#ifdef HAVE_DIAGNOSTIC
	__hazard_dump(session);
#endif
}

/*
 * __wt_hazard_close --
 *	Verify that no hazard references are set.
 */
void
__wt_hazard_close(WT_SESSION_IMPL *session)
{
	WT_CONNECTION_IMPL *conn;
	WT_HAZARD *hp;
	int found;

	conn = S2C(session);

	/* Check for a set hazard reference and complain if we find one. */
	for (found = 0, hp = session->hazard;
	    hp < session->hazard + conn->hazard_size; ++hp)
		if (hp->page != NULL) {
			__wt_errx(session,
			    "session %p: hazard reference table not empty: "
			    "page %p",
			    session, hp->page);
#ifdef HAVE_DIAGNOSTIC
			__hazard_dump(session);
#endif
			found = 1;
			break;
		}
	if (!found)
		return;

	/*
	 * Clear any hazard references because it's not a correctness problem
	 * (any hazard reference we find can't be real because the session is
	 * being closed when we're called).   We do this work because session
	 * close isn't that common that it's an expensive check, and we don't
	 * want to let a hazard reference lie around, keeping a page from being
	 * evicted.
	 */
	for (hp = session->hazard;
	    hp < session->hazard + conn->hazard_size; ++hp)
		if (hp->page != NULL)
			__wt_hazard_clear(session, hp->page);

	if (session->nhazard != 0)
		__wt_errx(session, "session %p: "
		    "hazard reference count didn't match table entries",
		    session);
}

#ifdef HAVE_DIAGNOSTIC
/*
 * __hazard_dump --
 *	Display the list of hazard references.
 */
static void
__hazard_dump(WT_SESSION_IMPL *session)
{
	WT_CONNECTION_IMPL *conn;
	WT_HAZARD *hp;

	conn = S2C(session);

	for (hp = session->hazard;
	    hp < session->hazard + conn->hazard_size; ++hp)
		if (hp->page != NULL)
			__wt_errx(session,
			    "session %p: hazard reference %p: %s, line %d",
			    session, hp->page, hp->file, hp->line);
}
#endif
