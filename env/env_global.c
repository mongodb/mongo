/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008 WiredTiger Software.
 *	All rights reserved.
 *
 * $Id$
 */

#include "wt_internal.h"

WT_GLOBALS __wt_globals;

/*
 * __wt_build_verify --
 *	Verify the build itself.
 */
int
__wt_build_verify(void)
{
	int ret;

	/*
	 * Check the build & compiler itself before going further.
	 */
	if ((ret = __wt_bt_build_verify()) != 0)
		return (ret);

#ifdef HAVE_DIAGNOSTIC
	/* Load debug code the compiler might optimize out. */
	if ((ret = __wt_breakpoint()) != 0)
		return (ret);
#endif

	return (0);
}

void *__wt_addr;				/* Memory flush address. */

/*
 * __wt_global_init --
 *	Initialize the globals.
 */
int
__wt_global_init(void)
{
	WT_STOC *stoc;
	u_int i;
	int ret;

	__wt_addr = &WT_GLOBAL(running);

	/*
	 * Allocate an initial list of server slots.
	 *
	 * The normal state of the blocking mutex is locked.
	 */
#define	WT_SERVERQ_SIZE	64
	WT_GLOBAL(sq_entries) = WT_SERVERQ_SIZE;
	if ((ret = __wt_calloc(
	    NULL, WT_SERVERQ_SIZE, sizeof(WT_STOC), &WT_GLOBAL(sq))) != 0)
		return (ret);
	for (i = 0; i < WT_GLOBAL(sq_entries); ++i) {
		stoc = WT_GLOBAL(sq) + i;
		if ((ret = __wt_stat_alloc_stoc_stats(NULL, &stoc->stats)) != 0)
			return (ret);
	}

	/* Initialize the global mutex. */
	if ((ret = __wt_mtx_init(&WT_GLOBAL(mtx))) != 0)
		return (ret);

	WT_GLOBAL(sep) = "=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=";

	return (0);
}

/*
 * __wt_breakpoint --
 *	A simple place to put a breakpoint, if you need one.
 */
int
__wt_breakpoint(void)
{
	return (0);
}
