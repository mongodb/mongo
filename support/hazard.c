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
void
__wt_hazard_set(WT_TOC *toc, WT_PAGE *page)
{
	ENV *env;
	WT_PAGE **hp;

	env = toc->env;

	/* Set the caller's hazard pointer. */
	for (hp = toc->hazard; hp < toc->hazard + env->hazard_size; ++hp)
		if (*hp == NULL) {
			*hp = page;
			/* Flush memory to make the hazard reference visible. */
			WT_MEMORY_FLUSH;
			return;
		}
	WT_ASSERT(env, hp < toc->hazard + env->hazard_size);
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

	/* Clear the caller's hazard pointer. */
	for (hp = toc->hazard; hp < toc->hazard + env->hazard_size; ++hp)
		if (*hp == page) {
			*hp = NULL;
			/*
			 * We don't have to flush memory here for correctness,
			 * but that gives the drain thread immediate access to
			 * the buffer.
			 */
			WT_MEMORY_FLUSH;
			return;
		}
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
	 * flushed.
	 */
	for (hp = toc->hazard; hp < toc->hazard + env->hazard_size; ++hp)
		if (*hp != NULL) {
			__wt_api_env_errx(env,
			    "WT_TOC closed with a hazard reference set");
			*hp = NULL;
			WT_MEMORY_FLUSH;
		}
}
