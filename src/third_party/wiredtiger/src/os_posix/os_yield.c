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
__wt_yield(void) WT_GCC_FUNC_ATTRIBUTE((visibility("default")))
{
    /*
     * Yielding the processor isn't documented as a memory barrier, and it's a reasonable
     * expectation to have. There's no reason not to explicitly include a barrier since we're giving
     * up the CPU, and ensures callers aren't ever surprised.
     */
    WT_FULL_BARRIER();

    sched_yield();
}

/*
 * If HAVE_DIAGNOSTIC is 0 clang will raise an error when the noreturn attribute is missing, and if
 * HAVE_DIAGNOSTIC is 1 clang will raise an error when the noreturn attribute is present. This
 * function should never be called when HAVE_DIAGNOSTIC is 0 so we can ignore the missing-noreturn
 * warning here.
 */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-noreturn"
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
    sched_yield();
#endif
}
#pragma GCC diagnostic pop
