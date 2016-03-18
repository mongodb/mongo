/*-
 * Copyright (c) 2014-2016 MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __wt_os_init --
 *	Initialize the OS layer.
 */
int
__wt_os_init(WT_SESSION_IMPL *session)
{
	return (F_ISSET(S2C(session), WT_CONN_IN_MEMORY) ?
	    __wt_os_inmemory(session) : __wt_os_posix(session));
}

/*
 * __wt_os_cleanup --
 *	Clean up the OS layer.
 */
int
__wt_os_cleanup(WT_SESSION_IMPL *session)
{
	return (F_ISSET(S2C(session), WT_CONN_IN_MEMORY) ?
	    __wt_os_inmemory_cleanup(session) : __wt_os_posix_cleanup(session));
}
