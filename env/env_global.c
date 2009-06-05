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
 * __wt_build_verify --
 *	Verify the build itself.
 */
int
__wt_build_verify(void)
{
	/*
	 * Check the build & compiler itself before going further.
	 */
	WT_RET((__wt_bt_build_verify()));

#ifdef HAVE_DIAGNOSTIC
	/* Load debug code the compiler might optimize out. */
	WT_RET((__wt_breakpoint()));
#endif

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
