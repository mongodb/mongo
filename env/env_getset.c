/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2010 WiredTiger, Inc.
 *	All rights reserved.
 *
 * $Id$
 */

#include "wt_internal.h"

/*
 * __wt_env_cache_cache_size_set_verify --
 *	Verify an argument to the Env.cache_size_set method.
 */
int
__wt_env_cache_size_set_verify(ENV *env, u_int32_t cache_size)
{
	return (__wt_api_arg_min(env,
	    "Env.cache_size_set", "cache size", cache_size, 1));
}

/*
 * __wt_env_cache_hash_size_set_verify --
 *	Verify an argument to the Env.hash_size_set method.
 */
int
__wt_env_cache_hash_size_set_verify(ENV *env, u_int32_t hash_size)
{
	return (__wt_api_arg_min(env,
	    "Env.hash_size_set", "hash size", hash_size, 1));
}

/*
 * __wt_env_cache_hazard_size_set_verify --
 *	Verify an argument to the Env.hazard_size_set method.
 */
int
__wt_env_hazard_size_set_verify(ENV *env, u_int32_t hazard_size)
{
	return (__wt_api_arg_min(env,
	    "Env.hazard_size_set", "hazard size", hazard_size, 1));
}

/*
 * __wt_env_toc_size_set_verify --
 *	Verify an argument to the Env.toc_size_set method.
 */
int
__wt_env_toc_size_set_verify(ENV *env, u_int32_t toc_size)
{
	return (__wt_api_arg_min(env,
	    "Env.toc_size_set", "toc size", toc_size, 1));
}

/*
 * __wt_env_verbose_set_verify --
 *	Verify an argument to the Env.verbose_set method.
 */
int
__wt_env_verbose_set_verify(ENV *env, u_int32_t verbose)
{
#ifdef HAVE_VERBOSE
	WT_ENV_FCHK(env,
	    "Env.verbose_set", verbose, WT_APIMASK_ENV_VERBOSE_SET);
	return (0);
#else
	return (__wt_api_config(env, "Env.verbose_set", "--enable-verbose"));
#endif
}
