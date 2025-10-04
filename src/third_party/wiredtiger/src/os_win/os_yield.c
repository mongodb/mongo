/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __wt_yield --
 *     Yield the thread of control.
 */
void
__wt_yield(void)
{
    /*
     * Yielding the processor isn't documented as a memory barrier, and it's a reasonable
     * expectation to have. There's no reason not to explicitly include a barrier since we're giving
     * up the CPU, and ensures callers aren't ever surprised.
     */
    WT_FULL_BARRIER();

    SwitchToThread();
}

/*
 * __wt_yield_no_barrier --
 *     Yield the thread of control. Don't set any memory barriers as this may hide memory
 *     synchronization errors in the surrounding code. It's not explicitly documented that yielding
 *     without a memory barrier is safe, so this function should only be used for testing in
 *     diagnostic mode.
 */
void
__wt_yield_no_barrier(void) WT_GCC_FUNC_ATTRIBUTE((visibility("default")))
{
#ifndef HAVE_DIAGNOSTIC
    __wt_abort(NULL);
#else
    SwitchToThread();
#endif
}
