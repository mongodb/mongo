/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"
#include <signal.h>

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

/*
 * __wt_debug_crash --
 *     If windows then abort else kill the process without creating a core.
 */
void
__wt_debug_crash(WT_SESSION_IMPL *session) WT_GCC_FUNC_ATTRIBUTE((visibility("default")))
{
#ifdef _WIN32
    __wt_abort(session);
#else
    WT_UNUSED(session);
    (void)kill(getpid(), SIGKILL);
#endif
}
