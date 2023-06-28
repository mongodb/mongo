/*-
 * Public Domain 2014-present MongoDB, Inc.
 * Public Domain 2008-2014 WiredTiger, Inc.
 *
 * This is free and unencumbered software released into the public domain.
 *
 * Anyone is free to copy, modify, publish, use, compile, sell, or
 * distribute this software, either in source code form or as a compiled
 * binary, for any purpose, commercial or non-commercial, and by any
 * means.
 *
 * In jurisdictions that recognize copyright laws, the author or authors
 * of this software dedicate any and all copyright interest in the
 * software to the public domain. We make this dedication for the benefit
 * of the public at large and to the detriment of our heirs and
 * successors. We intend this dedication to be an overt act of
 * relinquishment in perpetuity of all present and future rights to this
 * software under copyright law.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */

#include "windows_shim.h"

/*
 * sleep --
 *     TODO: Add a comment describing this function.
 */
int
sleep(int seconds)
{
    Sleep(seconds * WT_THOUSAND);
    return (0);
}

/*
 * usleep --
 *     TODO: Add a comment describing this function.
 */
int
usleep(useconds_t useconds)
{
    uint32_t milli;
    milli = useconds / WT_THOUSAND;

    if (milli == 0)
        milli++;

    Sleep(milli);

    return (0);
}

/*
 * gettimeofday --
 *     TODO: Add a comment describing this function.
 */
int
gettimeofday(struct timeval *tp, void *tzp)
{
    FILETIME time;
    uint64_t ns100;

    tzp = tzp;

    GetSystemTimeAsFileTime(&time);

    ns100 = (((int64_t)time.dwHighDateTime << 32) + time.dwLowDateTime) - 116444736000000000LL;
    tp->tv_sec = ns100 / (10 * WT_MILLION);
    tp->tv_usec = (long)((ns100 % (10 * WT_MILLION)) / 10);

    return (0);
}

/*
 * pthread_rwlock_destroy --
 *     TODO: Add a comment describing this function.
 */
int
pthread_rwlock_destroy(pthread_rwlock_t *lock)
{
    lock = lock; /* Unused variable. */
    return (0);
}

/*
 * pthread_rwlock_init --
 *     TODO: Add a comment describing this function.
 */
int
pthread_rwlock_init(pthread_rwlock_t *rwlock, const pthread_rwlockattr_t *ignored)
{
    ignored = ignored; /* Unused variable. */
    InitializeSRWLock(&rwlock->rwlock);
    rwlock->exclusive_locked = 0;

    return (0);
}

/*
 * pthread_rwlock_unlock --
 *     TODO: Add a comment describing this function.
 */
int
pthread_rwlock_unlock(pthread_rwlock_t *rwlock)
{
    if (rwlock->exclusive_locked != 0) {
        rwlock->exclusive_locked = 0;
        ReleaseSRWLockExclusive(&rwlock->rwlock);
    } else
        ReleaseSRWLockShared(&rwlock->rwlock);

    return (0);
}

/*
 * pthread_rwlock_tryrdlock --
 *     TODO: Add a comment describing this function.
 */
int
pthread_rwlock_tryrdlock(pthread_rwlock_t *rwlock)
{
    return (TryAcquireSRWLockShared(&rwlock->rwlock) ? 0 : EBUSY);
}

/*
 * pthread_rwlock_rdlock --
 *     TODO: Add a comment describing this function.
 */
int
pthread_rwlock_rdlock(pthread_rwlock_t *rwlock)
{
    AcquireSRWLockShared(&rwlock->rwlock);
    return (0);
}

/*
 * pthread_rwlock_trywrlock --
 *     TODO: Add a comment describing this function.
 */
int
pthread_rwlock_trywrlock(pthread_rwlock_t *rwlock)
{
    if (TryAcquireSRWLockExclusive(&rwlock->rwlock)) {
        rwlock->exclusive_locked = GetCurrentThreadId();
        return (0);
    }

    return (EBUSY);
}

/*
 * pthread_rwlock_wrlock --
 *     TODO: Add a comment describing this function.
 */
int
pthread_rwlock_wrlock(pthread_rwlock_t *rwlock)
{
    AcquireSRWLockExclusive(&rwlock->rwlock);

    rwlock->exclusive_locked = GetCurrentThreadId();

    return (0);
}
