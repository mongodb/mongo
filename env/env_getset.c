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
 * __wt_env_verbose_set_verify --
 *	Verify an argument to the Env.set_verbose setter.
 */
int
__wt_env_verbose_set_verify(ENV *env, u_int32_t verbose)
{
	WT_ENV_FCHK(env, "Env.set_verbose", verbose, WT_APIMASK_ENV_VERBOSE);
	return (0);
}
