/*-
 * Copyright (c) 2008-2012 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
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

#ifdef HAVE_DIAGNOSTIC
	__wt_attach(session);
#endif

	abort();
	/* NOTREACHED */
}
