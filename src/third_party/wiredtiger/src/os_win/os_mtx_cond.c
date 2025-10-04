/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __wt_cond_alloc --
 *     Allocate and initialize a condition variable.
 */
int
__wt_cond_alloc(WT_SESSION_IMPL *session, const char *name, WT_CONDVAR **condp)
{
    WT_CONDVAR *cond;

    WT_RET(__wt_calloc_one(session, &cond));

    InitializeCriticalSection(&cond->mtx);

    /* Initialize the condition variable to permit self-blocking. */
    InitializeConditionVariable(&cond->cond);

    cond->name = name;
    cond->waiters = 0;

    *condp = cond;
    return (0);
}

/*
 * __wt_cond_wait_signal --
 *     Wait on a mutex, optionally timing out. If we get it before the time out period expires, let
 *     the caller know.
 */
void
__wt_cond_wait_signal(WT_SESSION_IMPL *session, WT_CONDVAR *cond, uint64_t usecs,
  bool (*run_func)(WT_SESSION_IMPL *), bool *signalled)
{
    BOOL sleepret;
    DWORD milliseconds, windows_error;
    bool locked;
    uint64_t milliseconds64;

    locked = false;

    /* Fast path if already signalled. */
    *signalled = true;
    if (__wt_atomic_addi32(&cond->waiters, 1) == 0)
        return;

    __wt_verbose_debug2(session, WT_VERB_MUTEX, "wait %s", cond->name);
    WT_STAT_CONN_INCR(session, cond_wait);

    EnterCriticalSection(&cond->mtx);
    locked = true;

    /*
     * It's possible to race with threads waking us up. That's not a problem if there are multiple
     * wakeups because the next wakeup will get us, or if we're only pausing for a short period.
     * It's a problem if there's only a single wakeup, our waker is likely waiting for us to exit.
     * After acquiring the mutex (so we're guaranteed to be awakened by any future wakeup call),
     * optionally check if we're OK to keep running. This won't ensure our caller won't just loop
     * and call us again, but at least it's not our fault.
     *
     * Assert we're not waiting longer than a second if not checking the run status.
     */
    WT_ASSERT(session, run_func != NULL || usecs <= WT_MILLION);

    if (run_func != NULL && !run_func(session))
        goto skipping;

    if (usecs > 0) {
        milliseconds64 = usecs / WT_THOUSAND;

        /*
         * Check for 32-bit unsigned integer overflow INFINITE is max unsigned int on Windows
         */
        if (milliseconds64 >= INFINITE)
            milliseconds64 = INFINITE - 1;
        milliseconds = (DWORD)milliseconds64;

        /*
         * 0 would mean the CV sleep becomes a TryCV which we do not
         * want
         */
        if (milliseconds == 0)
            milliseconds = 1;

        sleepret = SleepConditionVariableCS(&cond->cond, &cond->mtx, milliseconds);
    } else
        sleepret = SleepConditionVariableCS(&cond->cond, &cond->mtx, INFINITE);

    /*
     * SleepConditionVariableCS returns non-zero on success, 0 on timeout or failure.
     */
    if (sleepret == 0) {
        windows_error = __wt_getlasterror();
        if (windows_error == ERROR_TIMEOUT) {
skipping:
            *signalled = false;
            sleepret = 1;
        }
    }

    (void)__wt_atomic_subi32(&cond->waiters, 1);

    if (locked)
        LeaveCriticalSection(&cond->mtx);

    if (sleepret != 0)
        return;

    __wt_err(session, __wt_map_windows_error(windows_error), "SleepConditionVariableCS: %s: %s",
      cond->name, __wt_formatmessage(session, windows_error));
    WT_IGNORE_RET(__wt_panic(
      session, __wt_map_windows_error(windows_error), "SleepConditionVariableCS: %s", cond->name));
}

/*
 * __wt_cond_signal --
 *     Signal a waiting thread.
 */
void
__wt_cond_signal(WT_SESSION_IMPL *session, WT_CONDVAR *cond)
{
    WT_DECL_RET;

    __wt_verbose_debug2(session, WT_VERB_MUTEX, "signal %s", cond->name);

    /*
     * Our callers often set flags to cause a thread to exit. Add a barrier to ensure exit flags are
     * seen by the sleeping threads, otherwise we can wake up a thread, it immediately goes back to
     * sleep, and we'll hang. Use a full barrier (we may not write before waiting on thread join).
     */
    WT_FULL_BARRIER();

    /*
     * Fast path if we are in (or can enter), a state where the next waiter will return immediately
     * as already signaled.
     */
    if (cond->waiters == -1 || (cond->waiters == 0 && __wt_atomic_casi32(&cond->waiters, 0, -1)))
        return;

    EnterCriticalSection(&cond->mtx);
    WakeAllConditionVariable(&cond->cond);
    LeaveCriticalSection(&cond->mtx);
}

/*
 * __wt_cond_destroy --
 *     Destroy a condition variable.
 */
void
__wt_cond_destroy(WT_SESSION_IMPL *session, WT_CONDVAR **condp)
{
    WT_CONDVAR *cond;

    cond = *condp;
    if (cond == NULL)
        return;

    /* Do nothing to delete Condition Variable */
    DeleteCriticalSection(&cond->mtx);
    __wt_free(session, *condp);
}
