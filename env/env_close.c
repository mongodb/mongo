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
__wt_env_close(WT_STOC *stoc)
{
	wt_args_env_close_unpack;
	int ret;

	ret = 0;

	WT_ENV_FCHK_NOTFATAL(
	    env, "ENV.close", flags, WT_APIMASK_ENV_CLOSE, ret);

	/* Re-cycle the underlying IENV structure. */
	WT_TRET(__wt_ienv_destroy(env, 1));

	/*
	 * Reset the methods that are permitted.
	 * If anything failed, we're done with this handle.
	 */
	if (ret == 0)
		__wt_env_config_methods(env);
	else
		__wt_env_config_methods_lockout(env);

	return (ret);
}
