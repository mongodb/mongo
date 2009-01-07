/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008 WiredTiger Software.
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
	/*lint -esym(715,env)
	 *
	 * env isn't referenced, but this layer always takes an env
	 * argument.
	 */
	abort();
	/* NOTREACHED */
}
