/*-
 * Copyright (c) 2008-2013 WiredTiger, Inc.
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
 * __wt_metadata_get_ckptlist --
 *	Public entry point to __wt_meta_ckptlist_get (for wt list).
 */
int
__wt_metadata_get_ckptlist(
    WT_SESSION *session, const char *name, WT_CKPT **ckptbasep)
{
	return (__wt_meta_ckptlist_get(
	    (WT_SESSION_IMPL *)session, name, ckptbasep));
}

/*
 * __wt_metadata_free_ckptlist --
 *	Public entry point to __wt_meta_ckptlist_free (for wt list).
 */
void
__wt_metadata_free_ckptlist(WT_SESSION *session, WT_CKPT *ckptbase)
{
	__wt_meta_ckptlist_free((WT_SESSION_IMPL *)session, ckptbase);
}
