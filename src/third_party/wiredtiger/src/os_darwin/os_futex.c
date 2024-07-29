/*-
 * Copyright (c) 2024-present MongoDB, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"
#include <sys/errno.h>

#define PRIVATE 1
#include <ulock.h>
#undef PRIVATE

/*
 * __wt_futex_wait --
 *     Wait on the futex. The timeout is in microseconds and MUST be greater than zero.
 */
int
__wt_futex_wait(
  volatile WT_FUTEX_WORD *addr, WT_FUTEX_WORD expected, time_t usec, WT_FUTEX_WORD *wake_valp)
{
    WT_DECL_RET;
    uint64_t nsec;

    WT_ASSERT(NULL, usec > 0);

    nsec = (uint64_t)usec * WT_THOUSAND;

    /*
     * This is a private API in the Apple system library. It replaces __ulock_wait which is found in
     * older MacOS releases. Documentation is non-existent, best reference is the Apple
     * implementation of libpthread.
     */
    ret = __ulock_wait2(UL_COMPARE_AND_WAIT_SHARED | ULF_NO_ERRNO, (void *)addr, expected, nsec, 0);
    if (ret >= 0 || ret == -EFAULT) {
        /*
         * Apparently a return value of -EFAULT may indicate the page containing the futex has been
         * paged out. On Linux this error only indicates one or more of the arguments is an invalid
         * user space address.
         *
         * There is no way to validate that futex address is valid, so reading from the address
         * should either block the caller until the page is available, or possibly (but less likely)
         * result in a crash.
         */
        *wake_valp = __atomic_load_n(addr, __ATOMIC_SEQ_CST);
        ret = 0;
    } else {
        errno = -ret;
        ret = -1;
    }

    return (ret);
}

/*
 * __wt_futex_wake --
 *     Wake the futex.
 */
int
__wt_futex_wake(volatile WT_FUTEX_WORD *addr, WT_FUTEX_WAKE wake, WT_FUTEX_WORD wake_val)
{
    WT_DECL_RET;
    uint32_t op;

    WT_ASSERT(NULL, wake == WT_FUTEX_WAKE_ONE || wake == WT_FUTEX_WAKE_ALL);

    op = UL_COMPARE_AND_WAIT_SHARED | ULF_NO_ERRNO;
    if (wake == WT_FUTEX_WAKE_ALL)
        op |= ULF_WAKE_ALL;
    __atomic_store_n(addr, wake_val, __ATOMIC_SEQ_CST);

    /*
     * The wake value (last param) is uint64_t which feels unsafe: as the futex word size is only
     * uint32_t. Consulting Apple's pthread library, this parameter is only used when
     * ULF_WAKE_THREAD flag is specified.
     */
    ret = __ulock_wake(op, (void *)addr, 0);
    switch (ret) {
    case -ENOENT:
        /* No waiters were awoken: don't treat this as an error.  */
        ret = 0;
        break;
    case -EINTR: /* Fall thru. */
    case -EAGAIN:
        errno = EINTR;
        ret = -1;
        break;
    }

    return (ret);
}
