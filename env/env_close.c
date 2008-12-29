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
 * __wt_env_close --
 *	Close a Env handle.
 */
int
__wt_env_close(ENV *env, u_int32_t flags)
{
	int ret;

	ret = 0;

	ENV_FLAG_CHK_NOTFATAL(
	    env, "ENV.close", flags, WT_APIMASK_ENV_CLOSE, ret);

	/* Reset the methods that are permitted. */
	__wt_env_config_methods(env);
	
	return (ret);
}
