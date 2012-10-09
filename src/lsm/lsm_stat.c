/*-
 * Copyright (c) 2008-2012 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __wt_lsm_stat_init --
 *	Initialize a LSM statistics structure.
 */
int
__wt_lsm_stat_init(WT_SESSION_IMPL *session, WT_LSM_TREE *lsm_tree)
{
	WT_UNUSED(session);
	WT_UNUSED(lsm_tree);
	/*
	 * TODO: Make a copy of the stat fields so the stat cursor gets a
	 * consistent view? If so should the copy belong to the stat cursor?
	 */
	return (0);
}
