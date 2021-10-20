/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __wt_abort --
 *     Abort the process, dropping core.
 */
void
__wt_abort(WT_SESSION_IMPL *session) WT_GCC_FUNC_ATTRIBUTE((noreturn))
  WT_GCC_FUNC_ATTRIBUTE((visibility("default")))
{
#ifdef HAVE_ATTACH
    u_int i;

    __wt_errx(session, "process ID %" PRIdMAX ": waiting for debugger...", (intmax_t)getpid());

    /* Sleep forever, the debugger will interrupt us when it attaches. */
    for (i = 0; i < WT_MILLION; ++i)
        __wt_sleep(100, 0);
#else
    __wt_errx(session, "aborting WiredTiger library");
#endif
    abort();
    /* NOTREACHED */
}
