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
 *	Close an Env handle.
 */
int
__wt_env_close(ENV *env, u_int32_t flags)
{
	int ret;

	ENV_FLAG_CHK_NOTFATAL(
	    env, "ENV.close", flags, WT_APIMASK_ENV_CLOSE, ret);

	/* Destroy the cache. */
	ret = __wt_cache_close(env);

	/* Reset the methods that are permitted. */
	__wt_env_config_methods(env);

	return (ret);
}
