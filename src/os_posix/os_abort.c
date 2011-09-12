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
	__wt_errx(session, "aborting WiredTiger library");

#if HAVE_DIAGNOSTIC
	__wt_attach(session);
#endif

	abort();
	/* NOTREACHED */
}
