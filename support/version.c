/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2009 WiredTiger Software.
 *	All rights reserved.
 *
 * $Id$
 */

#include "wt_internal.h"

/*
 * wt_version --
 *	Return library version information.
 */
char *
wt_version(int *majorp, int *minorp, int *patchp)
{
	if (majorp != NULL)
		*majorp = WIREDTIGER_VERSION_MAJOR;
	if (minorp != NULL)
		*minorp = WIREDTIGER_VERSION_MINOR;
	if (patchp != NULL)
		*patchp = WIREDTIGER_VERSION_PATCH;
	return ((char *)WIREDTIGER_VERSION_STRING);
}
