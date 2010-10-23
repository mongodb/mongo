/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2010 WiredTiger, Inc.
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
__wt_hazard_set(WT_TOC *toc, WT_CACHE_ENTRY *e, WT_PAGE *page)
{
	ENV *env;
	WT_PAGE **hp;

	env = toc->env;

	/*
	 * This code is called in two ways -- first, if the cache entry is set,
	 * we're reading the cache and we want to get a reference to the page
	 * the cache entry references.   If the cache entry is not set, we're
	 * allocating a new page, and it needs to be referenced before it's put
	 * into the cache.
	 */
	WT_ASSERT(env, e == NULL || page == NULL);
	if (e != NULL) {
		/*
		 * If page is NULL, we raced (that is, we were looking at a
		 * cache entry, everything matched, and as we retrieved the
		 * page address from memory for a hazard reference, the page
		 * disappeared).   That makes us done.  Note we can't USE the
		 * page for anything until we finish the WT_CACHE_ENTRY dance,
		 * this test is only to avoid using a NULL address as part of
		 * that dance.
		 */
		if ((page = e->page) == NULL)
			return (0);
	}

	WT_VERBOSE(env,
	    WT_VERB_HAZARD, (env, "toc %p hazard %p: set", toc, page));

	/*
	 * Dance:
	 * The memory location making a page "real" is the WT_CACHE_ENTRY's
	 * state field, which can be reset from WT_OK to WT_DRAIN at any time
	 * by the cache drain server.
	 *
	 * Add the page to the WT_TOC's hazard list (which flushes the write),
	 * then see if the state field is still WT_OK.  If it's still WT_OK,
	 * we know we can use the page because the cache drain server will see
	 * our hazard reference before it discards the buffer (the drain server
	 * sets the WT_DRAIN state, flushes memory, and then checks the hazard
	 * references).
	 */
	for (hp = toc->hazard; hp < toc->hazard + env->hazard_size; ++hp) {
		if (*hp != NULL)
			continue;

		/*
		 * Memory flush needed; the hazard array isn't declared volatile
		 * and an explicit memory flush is necessary.
		 */
		*hp = page;
		WT_MEMORY_FLUSH;

		/*
		 * If the cache entry isn't set, it's an allocation and we're
		 * done.
		 */
		if (e == NULL)
			return (1);

		/*
		 * If the cache entry is set, check to see if it's still valid.
		 * Valid means the state is WT_OK, or the state is WT_DRAIN and
		 * this thread is allowed to see pages flagged for draining.
		 */
		if (e->state == WT_OK ||
		    (e->state == WT_DRAIN && F_ISSET(toc, WT_READ_DRAIN)))
			return (1);

		/* The cache drain server owns this page, we can't have it. */
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

	/*
	 * If page is NULL, we raced (that is, we were looking at a cache entry,
	 * everything matched, and as we retrieved the page address from memory
	 * for a hazard reference, the page disappeared).  It's OK -- we ignored
	 * the request to get a hazard reference, and we can ignore the request
	 * to clear the hazard reference.
	 */
	if (page == NULL)
		return;

	env = toc->env;

	WT_VERBOSE(env,
	    WT_VERB_HAZARD, (env, "toc %p hazard %p: clr", toc, page));

	/* Clear the caller's hazard pointer. */
	for (hp = toc->hazard; hp < toc->hazard + env->hazard_size; ++hp)
		if (*hp == page) {
			*hp = NULL;
			/*
			 * We don't have to flush memory here for correctness;
			 * it would give the cache server thread faster access
			 * to the block were the block selected to be drained,
			 * but the generation number was just set which makes
			 * it unlikely to be selected for draining.
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
	 * cache drain thread immediate access to any page our reference blocks.
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
