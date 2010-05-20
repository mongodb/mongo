/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008 WiredTiger Software.
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
		 * disappeared).   That makes us done.
		 */
		if ((page = e->page) == NULL)
			return (0);
	}

	WT_VERBOSE(env,
	    WT_VERB_HAZARD, (env, "toc %p hazard %p: set", toc, page));

	/* Set the caller's hazard pointer. */
	for (hp = toc->hazard; hp < toc->hazard + env->hazard_size; ++hp)
		if (*hp == NULL) {
			*hp = page;
			/*
			 * Memory flush needed; the hazard array isn't declared
			 * volatile, so an explicit memory flush is necessary.
			 */
			WT_MEMORY_FLUSH;

			/*
			 * Check to see if the cache entry is still valid.  If
			 * it is, we're good to go, the cache server won't take
			 * this block.   Else, the cache server did take this
			 * block, we can't have it.
			 */
			if (e != NULL && e->state != WT_OK) {
				*hp = NULL;
				return (0);
			}

			return (1);
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
__wt_hazard_empty(WT_TOC *toc)
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
			    "WT_TOC left library with a hazard reference set");
			*hp = NULL;
			WT_MEMORY_FLUSH;
		}
}
