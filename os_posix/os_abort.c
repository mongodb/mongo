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
__wt_abort(IENV *ienv)
{
	/*lint -esym(715,ienv)
	 *
	 * ienv isn't referenced, but this layer always takes an ienv
	 * argument.
	 */
	abort();
	/* NOTREACHED */
}
