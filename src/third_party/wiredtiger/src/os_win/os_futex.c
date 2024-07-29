/*-
 * Copyright (c) 2024-present MongoDB, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/* Not linked by default, and required for WaitOn functionality. */
#pragma comment(lib, "Synchronization.Lib")

/*
 * __wt_futex_wait --
 *     Wait on the futex. The timeout is in microseconds and MUST be greater than zero.
 */
int
__wt_futex_wait(
  volatile WT_FUTEX_WORD *addr, WT_FUTEX_WORD expected, time_t usec, WT_FUTEX_WORD *wake_valp)
{
    DWORD msec;
    DWORD windows_error;
    bool retval;

    WT_ASSERT(NULL, usec > 0);

    msec = (DWORD)WT_MAX(1, usec / 1000);
    retval = WaitOnAddress(addr, &expected, sizeof(WT_FUTEX_WORD), msec);
    if (retval == TRUE) {
        /*
         * Currently we only support Windows on x86. That processor's TSO memory model ensures we
         * will see the write (or possibly a later write) to the futex prior to the call to wake.
         *
         * If we move to support Windows ARM this should be reviewed.
         */
        *wake_valp = *addr;
        return (0);
    }

    windows_error = __wt_getlasterror();
    errno = __wt_map_windows_error(windows_error);
    return (-1);
}

/*
 * __wt_futex_wake --
 *     Wake the futex.
 */
int
__wt_futex_wake(volatile WT_FUTEX_WORD *addr, WT_FUTEX_WAKE wake, WT_FUTEX_WORD wake_val)
{
    WT_ASSERT(NULL, wake == WT_FUTEX_WAKE_ONE || wake == WT_FUTEX_WAKE_ALL);

    InterlockedExchange(addr, wake_val);
    if (wake == WT_FUTEX_WAKE_ONE)
        WakeByAddressSingle(addr);
    else
        WakeByAddressAll(addr);
    return (0);
}
