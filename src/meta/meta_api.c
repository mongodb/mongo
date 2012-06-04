/*-
 * Copyright (c) 2008-2012 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * wt_metadata_get --
 *	Public entry point to __wt_metadata_read (for wt dump and list).
 */
int
__wt_metadata_get(WT_SESSION *session, const char *uri, const char **valuep)
{
	return (__wt_metadata_read((WT_SESSION_IMPL *)session, uri, valuep));
}

/*
 * __wt_snaplist_get --
 *	Public entry point to __wt_meta_snaplist_get (for wt list).
 */
int
__wt_metadata_get_snaplist(
    WT_SESSION *session, const char *name, WT_SNAPSHOT **snapbasep)
{
	return (__wt_meta_snaplist_get(
	    (WT_SESSION_IMPL *)session, name, snapbasep));
}

/*
 * __wt_snaplist_free --
 *	Public entry point to __wt_snapshot_list_free (for wt list).
 */
void
__wt_metadata_free_snaplist(WT_SESSION *session, WT_SNAPSHOT *snapbase)
{
	__wt_meta_snaplist_free((WT_SESSION_IMPL *)session, snapbase);
}
