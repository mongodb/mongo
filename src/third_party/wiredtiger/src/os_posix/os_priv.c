/*-
 * Copyright (c) 2014-2016 MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __wt_has_priv --
 *	Return if the process has special privileges, defined as having
 *	different effective and read UIDs or GIDs.
 */
bool
__wt_has_priv(void)
{
	return (getuid() != geteuid() || getgid() != getegid());
}
