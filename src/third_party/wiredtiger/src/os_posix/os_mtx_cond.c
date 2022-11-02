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
    WT_DECL_RET;

    WT_RET(__wt_calloc_one(session, &cond));
    WT_ERR(pthread_mutex_init(&cond->mtx, NULL));

#ifdef HAVE_PTHREAD_COND_MONOTONIC
    {
        pthread_condattr_t condattr;

        WT_ERR(pthread_condattr_init(&condattr));
        ret = pthread_condattr_setclock(&condattr, CLOCK_MONOTONIC);
        if (ret == 0)
            ret = pthread_cond_init(&cond->cond, &condattr);
        WT_TRET(pthread_condattr_destroy(&condattr));
        WT_ERR(ret);
    }
#else
    WT_ERR(pthread_cond_init(&cond->cond, NULL));
#endif

    cond->name = name;
    cond->waiters = 0;

    *condp = cond;
    return (0);

err:
    __wt_free(session, cond);
    return (ret);
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
    struct timespec ts;
    WT_DECL_RET;
    WT_TRACK_OP_DECL;
    bool locked;

    WT_TRACK_OP_INIT(session);

    locked = false;

    /* Fast path if already signalled. */
    *signalled = true;
    if (__wt_atomic_addi32(&cond->waiters, 1) == 0) {
        WT_TRACK_OP_END(session);
        return;
    }

    __wt_verbose_debug2(session, WT_VERB_MUTEX, "wait %s", cond->name);
    WT_STAT_CONN_INCR(session, cond_wait);

    WT_ERR(pthread_mutex_lock(&cond->mtx));
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
/*
 * Get the current time as the basis for calculating when the wait should end. Prefer a monotonic
 * clock source to avoid unexpectedly long sleeps when the system clock is adjusted.
 *
 * Failing that, query the time directly and don't attempt to correct for the clock moving
 * backwards, which would result in a sleep that is too long by however much the clock is updated.
 * This isn't as good as a monotonic clock source but makes the window of vulnerability smaller
 * (i.e., the calculated time is only incorrect if the system clock changes in between us querying
 * it and waiting).
 */
#ifdef HAVE_PTHREAD_COND_MONOTONIC
        WT_SYSCALL_RETRY(clock_gettime(CLOCK_MONOTONIC, &ts), ret);
        if (ret != 0)
            WT_IGNORE_RET(__wt_panic(session, ret, "clock_gettime"));
#else
        __wt_epoch_raw(session, &ts);
#endif
        ts.tv_sec += (time_t)(((uint64_t)ts.tv_nsec + WT_THOUSAND * usecs) / WT_BILLION);
        ts.tv_nsec = (long)(((uint64_t)ts.tv_nsec + WT_THOUSAND * usecs) % WT_BILLION);
        ret = pthread_cond_timedwait(&cond->cond, &cond->mtx, &ts);
    } else
        ret = pthread_cond_wait(&cond->cond, &cond->mtx);

    /*
     * Check pthread_cond_wait() return for EINTR, ETIME and ETIMEDOUT, some systems return these
     * errors.
     */
    if (ret == EINTR ||
#ifdef ETIME
      ret == ETIME ||
#endif
      ret == ETIMEDOUT) {
skipping:
        *signalled = false;
        ret = 0;
    }

err:
    (void)__wt_atomic_subi32(&cond->waiters, 1);

    if (locked)
        WT_TRET(pthread_mutex_unlock(&cond->mtx));

    WT_TRACK_OP_END(session);
    if (ret == 0)
        return;

    WT_IGNORE_RET(__wt_panic(session, ret, "pthread_cond_wait: %s", cond->name));
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

    WT_ERR(pthread_mutex_lock(&cond->mtx));
    ret = pthread_cond_broadcast(&cond->cond);
    WT_TRET(pthread_mutex_unlock(&cond->mtx));
    if (ret == 0)
        return;

err:
    WT_IGNORE_RET(__wt_panic(session, ret, "pthread_cond_broadcast: %s", cond->name));
}

/*
 * __wt_cond_destroy --
 *     Destroy a condition variable.
 */
void
__wt_cond_destroy(WT_SESSION_IMPL *session, WT_CONDVAR **condp)
{
    WT_CONDVAR *cond;
    WT_DECL_RET;

    cond = *condp;
    if (cond == NULL)
        return;

    if ((ret = pthread_cond_destroy(&cond->cond)) != 0)
        WT_IGNORE_RET(__wt_panic(session, ret, "pthread_cond_destroy: %s", cond->name));

    if ((ret = pthread_mutex_destroy(&cond->mtx)) != 0)
        WT_IGNORE_RET(__wt_panic(session, ret, "pthread_mutex_destroy: %s", cond->name));

    __wt_free(session, *condp);
}
