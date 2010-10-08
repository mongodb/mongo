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
 * __wt_bt_close --
 *	Close a Btree.
 */
int
__wt_bt_close(WT_TOC *toc)
{
	ENV *env;
	IDB *idb;
	int ret;

	env = toc->env;
	idb = toc->db->idb;
	ret = 0;

	if (WT_UNOPENED_DATABASE(idb))
		return (0);

	/*
	 * Discard the pinned root page.  We have a direct reference but we
	 * have to get another one, otherwise the hazard pointers won't be
	 * set/cleared appropriately.
	 */
	WT_TRET(__wt_bt_root_pin(toc, 0));

	/* Close the underlying file handle. */
	WT_TRET(__wt_close(env, idb->fh));
	idb->fh = NULL;

	return (ret);
}
