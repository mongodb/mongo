/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 *
 * $Id$
 */

#include "wt_internal.h"

/*
 * __wt_hazard_set --
 *	Set a hazard reference.
 */
int
__wt_hazard_set(WT_TOC *toc, WT_REF *ref)
{
	ENV *env;
	WT_PAGE **hp;

	env = toc->env;

	/*
	 * Do the dance:
	 *
	 * The memory location making a page "real" is the WT_REF's state which
	 * can be reset from WT_REF_OK to WT_REF_EVICT at any time by the page
	 * eviction server.
	 *
	 * Add the WT_REF reference to the WT_TOC's hazard list and flush the
	 * write, then see if the state field is still WT_REF_OK.  If it's still
	 * WT_REF_OK, we can use the page because the page eviction server will
	 * see our hazard reference before it discards the buffer (the eviction
	 * server sets the WT_REF state to WT_REF_EVICT, flushes memory, and
	 * then checks the hazard references).
	 */
	for (hp = toc->hazard; hp < toc->hazard + env->hazard_size; ++hp) {
		if (*hp != NULL)
			continue;

		/*
		 * Memory flush needed; the hazard array isn't declared volatile
		 * and an explicit memory flush is necessary.
		 */
		*hp = ref->page;
		WT_MEMORY_FLUSH;

		/*
		 * If the cache entry is set, check to see if it's still valid.
		 * Valid means a state of WT_REF_OK, or a state of WT_REF_EVICT
		 * and this thread is allowed to see pages flagged for eviction.
		 */
		if (ref->state == WT_REF_CACHE ||
		    (ref->state == WT_REF_EVICT &&
		    F_ISSET(toc, WT_READ_EVICT))) {
			WT_VERBOSE(env, WT_VERB_HAZARD,
			    (env, "toc %p hazard %p: set", toc, ref->page));
			return (1);
		}

		/* The cache eviction server owns the page, we can't have it. */
		*hp = NULL;
		return (0);
	}

	__wt_api_env_errx(env, "WT_TOC has no more hazard reference slots");
	WT_ASSERT(env, hp < toc->hazard + env->hazard_size);
	return (0);
}

/*
 * __wt_hazard_clear --
 *	Clear a hazard reference.
 */
void
__wt_hazard_clear(WT_TOC *toc, WT_PAGE *page)
{
	ENV *env;
	WT_PAGE **hp;

	env = toc->env;

	WT_VERBOSE(env,
	    WT_VERB_HAZARD, (env, "toc %p hazard %p: clr", toc, page));

	/* Clear the caller's hazard pointer. */
	for (hp = toc->hazard; hp < toc->hazard + env->hazard_size; ++hp)
		if (*hp == page) {
			*hp = NULL;
			/*
			 * We don't have to flush memory here for correctness;
			 * it would give the page server thread faster access
			 * to the block were the block selected to be evicted,
			 * but the generation number was just set which makes
			 * it unlikely to be selected for eviction.
			 */
			return;
		}
	__wt_api_env_errx(env, "WT_TOC hazard reference not found");
	WT_ASSERT(env, hp < toc->hazard + env->hazard_size);
}

/*
 * __wt_hazard_empty --
 *	Verify that no hazard references are set.
 */
void
__wt_hazard_empty(WT_TOC *toc, const char *name)
{
	ENV *env;
	WT_PAGE **hp;

	env = toc->env;

	/*
	 * Check for a set hazard reference and complain if we find one.  Clear
	 * any we find because it's not a correctness problem (any hazard ref
	 * we find can't be real because the WT_TOC is being closed when we're
	 * called).   We do this work because it's not expensive, and we don't
	 * want to let a hazard reference lie around, keeping a page from being
	 * flushed.  The flush isn't necessary for correctness, but gives the
	 * cache eviction thread immediate access to any page our reference
	 * blocks.
	 */
	for (hp = toc->hazard; hp < toc->hazard + env->hazard_size; ++hp)
		if (*hp != NULL) {
			__wt_api_env_errx(env,
			    "%s: returned with a hazard reference set (%p)",
			    name, *hp);
			*hp = NULL;
			WT_MEMORY_FLUSH;
		}
}
