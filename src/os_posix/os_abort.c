/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wt_internal.h"

/*
 * __wt_abort --
 *	Abort the process, dropping core.
 */
void
__wt_abort(WT_SESSION_IMPL *session)
{
	__wt_msg(session, "aborting WiredTiger library");

	__wt_attach(session);

	abort();
	/* NOTREACHED */
}
