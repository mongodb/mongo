/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wt_internal.h"

/*
 * __wt_upgrade --
 *	Upgrade a file.
 */
int
__wt_upgrade(WT_SESSION_IMPL *session, const char *cfg[])
{
	WT_UNUSED(session);		/* XXX: unused for now */
	WT_UNUSED(cfg);			/* XXX: unused for now */

	/* There's nothing to upgrade, yet. */
	return (0);
}
