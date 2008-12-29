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
 * __wt_env_set_verbose_verify --
 *	Verify an argument to the Env.set_verbose setter.
 */
int
__wt_env_set_verbose_verify(ENV *env, u_int32_t *whichp)
{
	ENV_FLAG_CHK(env, "Env.set_verbose", *whichp, WT_APIMASK_ENV_VERBOSE);
	return (0);
}
