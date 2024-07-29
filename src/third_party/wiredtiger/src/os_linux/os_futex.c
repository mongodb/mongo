/*-
 * Copyright (c) 2024-present MongoDB, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

#include <linux/futex.h>
#include <sys/syscall.h>

/*
 * __wt_futex_wait --
 *     Wait on the futex. The timeout is in microseconds and MUST be greater than zero.
 */
int
__wt_futex_wait(
  volatile WT_FUTEX_WORD *addr, WT_FUTEX_WORD expected, time_t usec, WT_FUTEX_WORD *wake_valp)
{
    struct timespec timeout;
    long sysret;

    WT_ASSERT(NULL, usec > 0);

    __wt_usec_to_timespec(usec, &timeout);
    sysret = syscall(SYS_futex, addr, FUTEX_WAIT_PRIVATE, expected, &timeout, NULL, 0);
    if (sysret == 0)
        *wake_valp = __atomic_load_n(addr, __ATOMIC_SEQ_CST);

    return ((int)sysret);
}

/*
 * __wt_futex_wake --
 *     Wake the futex.
 */
int
__wt_futex_wake(volatile WT_FUTEX_WORD *addr, WT_FUTEX_WAKE wake, WT_FUTEX_WORD wake_val)
{
    long sysret;
    int wake_op;

    WT_ASSERT(NULL, wake == WT_FUTEX_WAKE_ONE || wake == WT_FUTEX_WAKE_ALL);

    wake_op = (wake == WT_FUTEX_WAKE_ALL) ? INT_MAX : 1;
    __atomic_store_n(addr, wake_val, __ATOMIC_SEQ_CST);
    sysret = syscall(SYS_futex, addr, FUTEX_WAKE_PRIVATE, wake_op, NULL, 0);

    return ((int)((sysret >= 0) ? 0 : sysret));
}
