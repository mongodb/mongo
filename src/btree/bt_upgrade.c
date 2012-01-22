/*-
 * Copyright (c) 2008-2012 WiredTiger, Inc.
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
	WT_UNUSED(session);
	WT_UNUSED(cfg);

	/* There's nothing to upgrade, yet. */
	return (0);
}
