/*-
 * Copyright (c) 2008-2012 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __wt_snaplist_get --
 *	Public entry point to __wt_snapshot_list_get (for "wt list").
 */
int
__wt_snaplist_get(
    WT_SESSION *session, const char *filename, WT_SNAPSHOT **snapbasep)
{
	WT_SESSION_IMPL *session_impl = (WT_SESSION_IMPL *)session;

	return (__wt_snapshot_list_get(session_impl, filename, snapbasep));
}

/*
 * __wt_snaplist_free --
 *	Public entry point to __wt_snapshot_list_free (for "wt list").
 */
void
__wt_snaplist_free(WT_SESSION *session, WT_SNAPSHOT *snapbase)
{
	WT_SESSION_IMPL *session_impl = (WT_SESSION_IMPL *)session;

	return (__wt_snapshot_list_free(session_impl, snapbase));
}
