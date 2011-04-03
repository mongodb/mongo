/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wt_internal.h"

/*
 * __wt_hazard_set --
 *	Set a hazard reference.
 */
int
__wt_hazard_set(SESSION *session, WT_REF *ref
#ifdef HAVE_DIAGNOSTIC
    , const char *file, int line
#endif
    )
{
	CONNECTION *conn;
	WT_HAZARD *hp;

	conn = S2C(session);

	/*
	 * Do the dance:
	 *
	 * The memory location which makes a page "real" is the WT_REF's state
	 * of WT_REF_MEM, in which the WT_REF_EVICT flag can be set at any time
	 * by the eviction server.
	 *
	 * Add the WT_REF reference to the SESSION's hazard list and flush the
	 * write, then see if the state field is still WT_REF_MEM.  If it's
	 * still WT_REF_MEM, we can use the page because the page eviction
	 * server will see our hazard reference before it discards the buffer
	 * (the eviction server sets the WT_REF_EVICT flag, flushes memory, and
	 * then checks the hazard references).
	 */
	for (hp = session->hazard;
	    hp < session->hazard + conn->hazard_size; ++hp) {
		if (hp->page != NULL)
			continue;

		/*
		 * Memory flush needed; the hazard array isn't declared volatile
		 * and an explicit memory flush is necessary.
		 */
		hp->page = ref->page;
#ifdef HAVE_DIAGNOSTIC
		hp->file = file;
		hp->line = line;
#endif
		WT_MEMORY_FLUSH;

		/*
		 * If the cache entry is set, check to see if it's still valid
		 * (where valid means a state of WT_REF_MEM).
		 */
		if (WT_REF_STATE(ref->state) == WT_REF_MEM) {
			WT_VERBOSE(conn, WT_VERB_HAZARD, (session,
			    "session %p hazard %p: set", session, ref->page));
			return (1);
		}

		/*
		 * The page isn't available, it's being considered for eviction
		 * (or being evicted for all we know).  If the eviction server
		 * sees our hazard reference before evicting the page, it will
		 * return the page to use, no harm done.  In the worst case, we
		 * could be asleep for a long time; that won't hurt anything,
		 * we just might prevent random pages from being evicted.  We
		 * flush memory to clear our reference, not for correctness but
		 * to minimize the amount of time we're tying down a pointer we
		 * know we can't have.
		 */
		hp->page = NULL;
		WT_MEMORY_FLUSH;
		return (0);
	}

	__wt_errx(session, "SESSION has no more hazard reference slots");
	WT_ASSERT(session, hp < session->hazard + conn->hazard_size);
	return (0);
}

/*
 * __wt_hazard_clear --
 *	Clear a hazard reference.
 */
void
__wt_hazard_clear(SESSION *session, WT_PAGE *page)
{
	CONNECTION *conn;
	WT_HAZARD *hp;

	conn = S2C(session);

	WT_VERBOSE(conn, WT_VERB_HAZARD, (session,
	    "session %p hazard %p: clr", session, page));

	/* Clear the caller's hazard pointer. */
	for (hp = session->hazard;
	    hp < session->hazard + conn->hazard_size; ++hp)
		if (hp->page == page) {
			hp->page = NULL;
			/*
			 * We don't have to flush memory here for correctness;
			 * it would give the page server thread faster access
			 * to the block were the block selected to be evicted,
			 * but the generation number was just set which makes
			 * it unlikely to be selected for eviction.
			 */
			return;
		}
	__wt_errx(session, "SESSION hazard reference not found");
	WT_ASSERT(session, hp < session->hazard + conn->hazard_size);
}

/*
 * __wt_hazard_empty --
 *	Verify that no hazard references are set.
 */
void
__wt_hazard_empty(SESSION *session, const char *name)
{
	CONNECTION *conn;
	WT_HAZARD *hp;

	conn = S2C(session);

	/*
	 * Check for a set hazard reference and complain if we find one.  Clear
	 * any we find because it's not a correctness problem (any hazard ref
	 * we find can't be real because the SESSION is being closed when we're
	 * called).   We do this work because it's not expensive, and we don't
	 * want to let a hazard reference lie around, keeping a page from being
	 * flushed.  The flush isn't necessary for correctness, but gives the
	 * cache eviction thread immediate access to any page our reference
	 * blocks.
	 */
	for (hp = session->hazard;
	    hp < session->hazard + conn->hazard_size; ++hp)
		if (hp->page != NULL) {
			__wt_errx(session,
#ifdef HAVE_DIAGNOSTIC
			    "%s: hazard reference lost: (%p: %s, line %d)",
			    name, hp->page, hp->file, hp->line);
#else
			    "%s: hazard reference lost: (%p)"
			    name, hp->page)
#endif
			hp->page = NULL;
			WT_MEMORY_FLUSH;
		}
}
