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
__wt_env_set_verbose_verify(ENV *env, u_int32_t which)
{
	IENV *ienv;

	ienv = env->ienv;

	ENV_FLAG_CHK(ienv, "Env.set_verbose", which, WT_APIMASK_ENV_VERBOSE);
	return (0);
}
