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
	return (__wt_api_arg_min(env, "Env.hazard_size_set",
	    "hazard size", hazard_size, WT_HAZARD_SIZE_DEFAULT));
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
	WT_ENV_FCHK(env,
	    "Env.verbose_set", WT_APIMASK_ENV_VERBOSE_SET, verbose);
	return (0);
}
