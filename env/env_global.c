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
 * __wt_env_build_verify --
 *	Verify the build itself.
 */
int
__wt_env_build_verify(IENV *ienv)
{
	int ret;

	/*
	 * Check the build & compiler itself before going further.
	 */
	if ((ret = __wt_bt_build_verify(ienv)) != 0)
		return (ret);

#ifdef HAVE_DIAGNOSTIC
	/* Load debug code the compiler might optimize out. */
	if ((ret = __wt_bt_force_load()) != 0)
		return (ret);
#endif

	return (0);
}
