/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008 WiredTiger Software.
 *	All rights reserved.
 *
 * $Id$
 */

#include "wt_internal.h"

WT_GLOBALS __wt_globals = {
	"=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=",	/* sep */
	{ 0 },							/* err_buf */
	0							/* file_id */
};

/*
 * __wt_env_build_verify --
 *	Verify the build itself.
 */
int
__wt_env_build_verify(void)
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

/*
 * __wt_breakpoint --
 *	A simple place to put a breakpoint, if you need one.
 */
int
__wt_breakpoint(void)
{
	return (0);
}
