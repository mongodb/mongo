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
 * __wt_env_open --
 *	Open a Env handle.
 */
int
__wt_env_open(wt_args_env_open *argp)
{
	wt_args_env_open_unpack;
	int ret;

	ENV_FLAG_CHK(env, "Env.open", flags, WT_APIMASK_ENV_OPEN);

	/* Turn on the methods that require open. */
	__wt_env_config_methods_open(env);

	/* Initialize the cache. */
	if ((ret = __wt_cache_open(env)) != 0)
		return (ret);

	return (0);
}
