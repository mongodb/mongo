/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wt_internal.h"

/*
 * __wt_connection_cache_cache_size_set_verify --
 *	Verify an argument to the Env.cache_size_set method.
 */
int
__wt_connection_cache_size_set_verify(CONNECTION *conn, uint32_t cache_size)
{
	return (__wt_api_arg_min(&conn->default_session,
	    "Env.cache_size_set", "cache size", cache_size, 1));
}

/*
 * __wt_connection_cache_hash_size_set_verify --
 *	Verify an argument to the Env.hash_size_set method.
 */
int
__wt_connection_cache_hash_size_set_verify(CONNECTION *conn, uint32_t hash_size)
{
	return (__wt_api_arg_min(&conn->default_session,
	    "Env.hash_size_set", "hash size", hash_size, 1));
}

/*
 * __wt_connection_cache_hazard_size_set_verify --
 *	Verify an argument to the Env.hazard_size_set method.
 */
int
__wt_connection_hazard_size_set_verify(CONNECTION *conn, uint32_t hazard_size)
{
	return (__wt_api_arg_min(&conn->default_session,
	    "Env.hazard_size_set", "hazard size", hazard_size, 1));
}

/*
 * __wt_connection_session_size_set_verify --
 *	Verify an argument to the Env.toc_size_set method.
 */
int
__wt_connection_session_size_set_verify(CONNECTION *conn, uint32_t toc_size)
{
	return (__wt_api_arg_min(&conn->default_session,
	    "Env.toc_size_set", "session size", toc_size, 1));
}

/*
 * __wt_connection_verbose_set_verify --
 *	Verify an argument to the Env.verbose_set method.
 */
int
__wt_connection_verbose_set_verify(CONNECTION *conn, uint32_t verbose)
{
#ifdef HAVE_VERBOSE
	WT_CONN_FCHK(conn,
	    "Env.verbose_set", verbose, WT_APIMASK_CONNECTION_VERBOSE_SET);
	return (0);
#else
	return (__wt_api_config(&conn->default_session,
	    "Env.verbose_set", "--enable-verbose"));
#endif
}
