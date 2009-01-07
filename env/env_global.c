/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008 WiredTiger Software.
 *	All rights reserved.
 *
 * $Id$
 */

#include "wt_internal.h"

WT_GLOBAL __wt_globals = {
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
	if ((ret = __wt_db_build_verify()) != 0)
		return (ret);

#ifdef HAVE_DIAGNOSTIC
	/* Load debug code the compiler might optimize out. */
	if ((ret = __wt_db_force_load()) != 0)
		return (ret);
#endif

	return (0);
}
