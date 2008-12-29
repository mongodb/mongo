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
__wt_env_open(ENV *env, const char *home, mode_t mode, u_int32_t flags)
{
	/*lint -esym(715,home) */
	/*lint -esym(715,mode) */
	/*lint -esym(715,flags)

	/* Turn on the methods that require open. */
	__wt_env_config_methods_open(env);
	
	return (0);
}
