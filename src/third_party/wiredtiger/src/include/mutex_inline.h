/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

/*
 * Spin locks:
 *
 * These used for cases where fast mutual exclusion is needed (where operations done while holding
 * the spin lock are expected to complete in a small number of instructions.
 */

/*
 * __spin_init_internal --
 *     Initialize the WT portion of a spinlock.
 */
static inline void
__spin_init_internal(WT_SPINLOCK *t, const char *name)
{
    t->name = name;
    t->stat_count_off = t->stat_app_usecs_off = t->stat_int_usecs_off = -1;
    t->stat_session_usecs_off = -1;
    t->initialized = 1;
}

#if SPINLOCK_TYPE == SPINLOCK_GCC

/* Default to spinning 1000 times before yielding. */
#ifndef WT_SPIN_COUNT
#define WT_SPIN_COUNT WT_THOUSAND
#endif

/*
 * __wt_spin_init --
 *     Initialize a spinlock.
 */
static inline int
__wt_spin_init(WT_SESSION_IMPL *session, WT_SPINLOCK *t, const char *name)
{
    WT_UNUSED(session);

    t->lock = 0;
    __spin_init_internal(t, name);
    return (0);
}

/*
 * __wt_spin_destroy --
 *     Destroy a spinlock.
 */
static inline void
__wt_spin_destroy(WT_SESSION_IMPL *session, WT_SPINLOCK *t)
{
    WT_UNUSED(session);

    t->lock = 0;
}

/*
 * __wt_spin_trylock --
 *     Try to lock a spinlock or fail immediately if it is busy.
 */
static inline int
__wt_spin_trylock(WT_SESSION_IMPL *session, WT_SPINLOCK *t)
{
    WT_UNUSED(session);

    return (!__atomic_test_and_set(&t->lock, __ATOMIC_ACQUIRE) ? 0 : EBUSY);
}

/*
 * __wt_spin_lock --
 *     Spin until the lock is acquired.
 */
static inline void
__wt_spin_lock(WT_SESSION_IMPL *session, WT_SPINLOCK *t)
{
    int i;

    WT_UNUSED(session);

    while (__atomic_test_and_set(&t->lock, __ATOMIC_ACQUIRE)) {
        for (i = 0; t->lock && i < WT_SPIN_COUNT; i++)
            WT_PAUSE();
        if (t->lock)
            __wt_yield();
    }
}

/*
 * __wt_spin_unlock --
 *     Release the spinlock.
 */
static inline void
__wt_spin_unlock(WT_SESSION_IMPL *session, WT_SPINLOCK *t)
{
    WT_UNUSED(session);

    __atomic_clear(&t->lock, __ATOMIC_RELEASE);
}

#elif SPINLOCK_TYPE == SPINLOCK_PTHREAD_MUTEX || SPINLOCK_TYPE == SPINLOCK_PTHREAD_MUTEX_ADAPTIVE

/*
 * __wt_spin_init --
 *     Initialize a spinlock.
 */
static inline int
__wt_spin_init(WT_SESSION_IMPL *session, WT_SPINLOCK *t, const char *name)
{
#if SPINLOCK_TYPE == SPINLOCK_PTHREAD_MUTEX_ADAPTIVE
    WT_DECL_RET;
    pthread_mutexattr_t attr;

    WT_RET(pthread_mutexattr_init(&attr));
    ret = pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_ADAPTIVE_NP);
    if (ret == 0)
        ret = pthread_mutex_init(&t->lock, &attr);
    WT_TRET(pthread_mutexattr_destroy(&attr));
    WT_RET(ret);
#else
    WT_RET(pthread_mutex_init(&t->lock, NULL));
#endif
    __spin_init_internal(t, name);

    WT_UNUSED(session);
    return (0);
}

/*
 * __wt_spin_destroy --
 *     Destroy a spinlock.
 */
static inline void
__wt_spin_destroy(WT_SESSION_IMPL *session, WT_SPINLOCK *t)
{
    WT_UNUSED(session);

    if (t->initialized) {
        (void)pthread_mutex_destroy(&t->lock);
        t->initialized = 0;
    }
}

#if SPINLOCK_TYPE == SPINLOCK_PTHREAD_MUTEX || SPINLOCK_TYPE == SPINLOCK_PTHREAD_MUTEX_ADAPTIVE

/*
 * __wt_spin_trylock --
 *     Try to lock a spinlock or fail immediately if it is busy.
 */
static inline int
__wt_spin_trylock(WT_SESSION_IMPL *session, WT_SPINLOCK *t)
{
    WT_UNUSED(session);

    return (pthread_mutex_trylock(&t->lock));
}

/*
 * __wt_spin_lock --
 *     Spin until the lock is acquired.
 */
static inline void
__wt_spin_lock(WT_SESSION_IMPL *session, WT_SPINLOCK *t)
{
    WT_DECL_RET;

    if ((ret = pthread_mutex_lock(&t->lock)) != 0)
        WT_IGNORE_RET(__wt_panic(session, ret, "pthread_mutex_lock: %s", t->name));
}
#endif

/*
 * __wt_spin_unlock --
 *     Release the spinlock.
 */
static inline void
__wt_spin_unlock(WT_SESSION_IMPL *session, WT_SPINLOCK *t)
{
    WT_DECL_RET;

    if ((ret = pthread_mutex_unlock(&t->lock)) != 0)
        WT_IGNORE_RET(__wt_panic(session, ret, "pthread_mutex_unlock: %s", t->name));
}

#elif SPINLOCK_TYPE == SPINLOCK_MSVC

/*
 * __wt_spin_init --
 *     Initialize a spinlock.
 */
static inline int
__wt_spin_init(WT_SESSION_IMPL *session, WT_SPINLOCK *t, const char *name)
{
    DWORD windows_error;

    if (InitializeCriticalSectionAndSpinCount(&t->lock, 4 * WT_THOUSAND) == 0) {
        windows_error = __wt_getlasterror();
        __wt_errx(session, "%s: InitializeCriticalSectionAndSpinCount: %s", name,
          __wt_formatmessage(session, windows_error));
        return (__wt_map_windows_error(windows_error));
    }

    __spin_init_internal(t, name);
    return (0);
}

/*
 * __wt_spin_destroy --
 *     Destroy a spinlock.
 */
static inline void
__wt_spin_destroy(WT_SESSION_IMPL *session, WT_SPINLOCK *t)
{
    WT_UNUSED(session);

    if (t->initialized) {
        DeleteCriticalSection(&t->lock);
        t->initialized = 0;
    }
}

/*
 * __wt_spin_trylock --
 *     Try to lock a spinlock or fail immediately if it is busy.
 */
static inline int
__wt_spin_trylock(WT_SESSION_IMPL *session, WT_SPINLOCK *t)
{
    WT_UNUSED(session);

    BOOL b = TryEnterCriticalSection(&t->lock);
    return (b == 0 ? EBUSY : 0);
}

/*
 * __wt_spin_lock --
 *     Spin until the lock is acquired.
 */
static inline void
__wt_spin_lock(WT_SESSION_IMPL *session, WT_SPINLOCK *t)
{
    WT_UNUSED(session);

    EnterCriticalSection(&t->lock);
}

/*
 * __wt_spin_unlock --
 *     Release the spinlock.
 */
static inline void
__wt_spin_unlock(WT_SESSION_IMPL *session, WT_SPINLOCK *t)
{
    WT_UNUSED(session);

    LeaveCriticalSection(&t->lock);
}

#else

#error Unknown spinlock type

#endif

/*
 * WT_SPIN_INIT_TRACKED --
 *	Spinlock initialization, with tracking.
 *
 * Implemented as a macro so we can pass in a statistics field and convert
 * it into a statistics structure array offset.
 */
#define WT_SPIN_INIT_TRACKED(session, t, name)                                                    \
    do {                                                                                          \
        WT_RET(__wt_spin_init(session, t, #name));                                                \
        (t)->stat_count_off =                                                                     \
          (int16_t)WT_STATS_FIELD_TO_OFFSET(S2C(session)->stats, lock_##name##_count);            \
        (t)->stat_app_usecs_off =                                                                 \
          (int16_t)WT_STATS_FIELD_TO_OFFSET(S2C(session)->stats, lock_##name##_wait_application); \
        (t)->stat_int_usecs_off =                                                                 \
          (int16_t)WT_STATS_FIELD_TO_OFFSET(S2C(session)->stats, lock_##name##_wait_internal);    \
    } while (0)

#define WT_SPIN_INIT_SESSION_TRACKED(session, t, name)                                      \
    do {                                                                                    \
        WT_SPIN_INIT_TRACKED(session, t, name);                                             \
        (t)->stat_session_usecs_off =                                                       \
          (int16_t)WT_SESSION_STATS_FIELD_TO_OFFSET(&(session)->stats, lock_##name##_wait); \
    } while (0)

/*
 * __wt_spin_lock_track --
 *     Spinlock acquisition, with tracking.
 */
static inline void
__wt_spin_lock_track(WT_SESSION_IMPL *session, WT_SPINLOCK *t)
{
    uint64_t time_diff, time_start, time_stop;
    int64_t *session_stats, **stats;

    if (t->stat_count_off != -1 && WT_STAT_ENABLED(session)) {
        time_start = __wt_clock(session);
        __wt_spin_lock(session, t);
        time_stop = __wt_clock(session);
        time_diff = WT_CLOCKDIFF_US(time_stop, time_start);
        stats = (int64_t **)S2C(session)->stats;
        session_stats = (int64_t *)&(session->stats);
        stats[session->stat_bucket][t->stat_count_off]++;
        if (F_ISSET(session, WT_SESSION_INTERNAL))
            stats[session->stat_bucket][t->stat_int_usecs_off] += (int64_t)time_diff;
        else {
            stats[session->stat_bucket][t->stat_app_usecs_off] += (int64_t)time_diff;
        }

        /*
         * Not all spin locks increment session statistics. Check whether the offset is initialized
         * to determine whether they are enabled.
         */
        if (t->stat_session_usecs_off != -1)
            session_stats[t->stat_session_usecs_off] += (int64_t)time_diff;
    } else
        __wt_spin_lock(session, t);
}

/*
 * __wt_spin_trylock_track --
 *     Try to lock a spinlock or fail immediately if it is busy. Track if successful.
 */
static inline int
__wt_spin_trylock_track(WT_SESSION_IMPL *session, WT_SPINLOCK *t)
{
    int64_t **stats;

    if (t->stat_count_off != -1 && WT_STAT_ENABLED(session)) {
        WT_RET(__wt_spin_trylock(session, t));
        stats = (int64_t **)S2C(session)->stats;
        stats[session->stat_bucket][t->stat_count_off]++;
        return (0);
    }
    return (__wt_spin_trylock(session, t));
}
