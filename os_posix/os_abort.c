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
 * __wt_abort --
 *	Abort the process, dropping core.
 */
void
__wt_abort(ENV *env)
{
	if (env != NULL)
		__wt_msg(env, "aborting WiredTiger library");
	abort();
	/* NOTREACHED */
}
