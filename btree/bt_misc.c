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
 * __wt_db_build_verify --
 *	Verify the Btree build itself.
 */
int
__wt_db_build_verify(IENV *ienv)
{
	ENV *env;

	env = ienv->env;

	/*
	 * The compiler had better not have padded our structures -- make
	 * sure the page header structure is exactly what we expect.
	 */
	if (sizeof(WT_PAGE_HDR) != WT_HDR_SIZE) {
		__wt_env_errx(env,
		    "WiredTiger build failed, the WT_PAGE_HDR structure isn't"
		    " %d bytes in size", WT_HDR_SIZE);
		return (WT_ERROR);
	}
	if (WT_ALIGN(sizeof(WT_PAGE_HDR), sizeof(u_int32_t)) != WT_HDR_SIZE) {
		__wt_env_errx(env,
		    "Build verification failed, the WT_PAGE_HDR structure"
		    " isn't aligned correctly");
		return (WT_ERROR);
	}

	return (0);
}
