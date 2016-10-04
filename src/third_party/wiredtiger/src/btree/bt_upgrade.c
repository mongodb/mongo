/*-
 * Copyright (c) 2014-2016 MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __wt_upgrade --
 *	Upgrade a file.
 */
int
__wt_upgrade(WT_SESSION_IMPL *session, const char *cfg[])
{
	WT_UNUSED(cfg);

	/* There's nothing to upgrade, yet. */
	WT_RET(__wt_progress(session, NULL, 1));
	return (0);
}
