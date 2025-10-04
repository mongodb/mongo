/*
 * librdkafka - Apache Kafka C library
 *
 * Copyright (c) 2018-2022, Magnus Edenhill
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */


/**
 * @brief Extra methods added to tinychtread/c11threads
 */


#ifndef _TINYCTHREAD_EXTRA_H_
#define _TINYCTHREAD_EXTRA_H_


#ifndef _WIN32
#include <pthread.h> /* needed for rwlock_t */
#endif


/**
 * @brief Set thread system name if platform supports it (pthreads)
 * @return thrd_success or thrd_error
 */
int thrd_setname(const char *name);

/**
 * @brief Checks if passed thread is the current thread.
 * @return non-zero if same thread, else 0.
 */
int thrd_is_current(thrd_t thr);


#ifdef _WIN32
/**
 * @brief Mark the current thread as waiting on cnd.
 *
 * @remark This is to be used when the thread uses its own
 * WaitForMultipleEvents() call rather than cnd_timedwait().
 *
 * @sa cnd_wait_exit()
 */
void cnd_wait_enter(cnd_t *cond);

/**
 * @brief Mark the current thread as no longer waiting on cnd.
 */
void cnd_wait_exit(cnd_t *cond);
#endif


/**
 * @brief Same as cnd_timedwait() but takes a relative timeout in milliseconds.
 */
int cnd_timedwait_ms(cnd_t *cnd, mtx_t *mtx, int timeout_ms);

/**
 * @brief Same as cnd_timedwait_ms() but updates the remaining time.
 */
int cnd_timedwait_msp(cnd_t *cnd, mtx_t *mtx, int *timeout_msp);

/**
 * @brief Same as cnd_timedwait() but honours
 *        RD_POLL_INFINITE (uses cnd_wait()),
 *        and RD_POLL_NOWAIT (return thrd_timedout immediately).
 *
 *  @remark Set up \p tspec with rd_timeout_init_timespec().
 */
int cnd_timedwait_abs(cnd_t *cnd, mtx_t *mtx, const struct timespec *tspec);



/**
 * @brief Read-write locks
 */

#if defined(_TTHREAD_WIN32_)
typedef struct rwlock_t {
        SRWLOCK lock;
        LONG rcnt;
        LONG wcnt;
} rwlock_t;
#define rwlock_init(rwl)                                                       \
        do {                                                                   \
                (rwl)->rcnt = (rwl)->wcnt = 0;                                 \
                InitializeSRWLock(&(rwl)->lock);                               \
        } while (0)
#define rwlock_destroy(rwl)
#define rwlock_rdlock(rwl)                                                     \
        do {                                                                   \
                if (0)                                                         \
                        printf("Thr %i: at %i:   RDLOCK %p   %s (%i, %i)\n",   \
                               GetCurrentThreadId(), __LINE__, rwl,            \
                               __FUNCTION__, (rwl)->rcnt, (rwl)->wcnt);        \
                assert((rwl)->rcnt >= 0 && (rwl)->wcnt >= 0);                  \
                AcquireSRWLockShared(&(rwl)->lock);                            \
                InterlockedIncrement(&(rwl)->rcnt);                            \
        } while (0)
#define rwlock_wrlock(rwl)                                                     \
        do {                                                                   \
                if (0)                                                         \
                        printf("Thr %i: at %i:   WRLOCK %p   %s (%i, %i)\n",   \
                               GetCurrentThreadId(), __LINE__, rwl,            \
                               __FUNCTION__, (rwl)->rcnt, (rwl)->wcnt);        \
                assert((rwl)->rcnt >= 0 && (rwl)->wcnt >= 0);                  \
                AcquireSRWLockExclusive(&(rwl)->lock);                         \
                InterlockedIncrement(&(rwl)->wcnt);                            \
        } while (0)
#define rwlock_rdunlock(rwl)                                                   \
        do {                                                                   \
                if (0)                                                         \
                        printf("Thr %i: at %i: RDUNLOCK %p   %s (%i, %i)\n",   \
                               GetCurrentThreadId(), __LINE__, rwl,            \
                               __FUNCTION__, (rwl)->rcnt, (rwl)->wcnt);        \
                assert((rwl)->rcnt > 0 && (rwl)->wcnt >= 0);                   \
                ReleaseSRWLockShared(&(rwl)->lock);                            \
                InterlockedDecrement(&(rwl)->rcnt);                            \
        } while (0)
#define rwlock_wrunlock(rwl)                                                   \
        do {                                                                   \
                if (0)                                                         \
                        printf("Thr %i: at %i: RWUNLOCK %p   %s (%i, %i)\n",   \
                               GetCurrentThreadId(), __LINE__, rwl,            \
                               __FUNCTION__, (rwl)->rcnt, (rwl)->wcnt);        \
                assert((rwl)->rcnt >= 0 && (rwl)->wcnt > 0);                   \
                ReleaseSRWLockExclusive(&(rwl)->lock);                         \
                InterlockedDecrement(&(rwl)->wcnt);                            \
        } while (0)

#define rwlock_rdlock_d(rwl)                                                   \
        do {                                                                   \
                if (1)                                                         \
                        printf("Thr %i: at %i:   RDLOCK %p   %s (%i, %i)\n",   \
                               GetCurrentThreadId(), __LINE__, rwl,            \
                               __FUNCTION__, (rwl)->rcnt, (rwl)->wcnt);        \
                assert((rwl)->rcnt >= 0 && (rwl)->wcnt >= 0);                  \
                AcquireSRWLockShared(&(rwl)->lock);                            \
                InterlockedIncrement(&(rwl)->rcnt);                            \
        } while (0)
#define rwlock_wrlock_d(rwl)                                                   \
        do {                                                                   \
                if (1)                                                         \
                        printf("Thr %i: at %i:   WRLOCK %p   %s (%i, %i)\n",   \
                               GetCurrentThreadId(), __LINE__, rwl,            \
                               __FUNCTION__, (rwl)->rcnt, (rwl)->wcnt);        \
                assert((rwl)->rcnt >= 0 && (rwl)->wcnt >= 0);                  \
                AcquireSRWLockExclusive(&(rwl)->lock);                         \
                InterlockedIncrement(&(rwl)->wcnt);                            \
        } while (0)
#define rwlock_rdunlock_d(rwl)                                                 \
        do {                                                                   \
                if (1)                                                         \
                        printf("Thr %i: at %i: RDUNLOCK %p   %s (%i, %i)\n",   \
                               GetCurrentThreadId(), __LINE__, rwl,            \
                               __FUNCTION__, (rwl)->rcnt, (rwl)->wcnt);        \
                assert((rwl)->rcnt > 0 && (rwl)->wcnt >= 0);                   \
                ReleaseSRWLockShared(&(rwl)->lock);                            \
                InterlockedDecrement(&(rwl)->rcnt);                            \
        } while (0)
#define rwlock_wrunlock_d(rwl)                                                 \
        do {                                                                   \
                if (1)                                                         \
                        printf("Thr %i: at %i: RWUNLOCK %p   %s (%i, %i)\n",   \
                               GetCurrentThreadId(), __LINE__, rwl,            \
                               __FUNCTION__, (rwl)->rcnt, (rwl)->wcnt);        \
                assert((rwl)->rcnt >= 0 && (rwl)->wcnt > 0);                   \
                ReleaseSRWLockExclusive(&(rwl)->lock);                         \
                InterlockedDecrement(&(rwl)->wcnt);                            \
        } while (0)


#else
typedef pthread_rwlock_t rwlock_t;

int rwlock_init(rwlock_t *rwl);
int rwlock_destroy(rwlock_t *rwl);
int rwlock_rdlock(rwlock_t *rwl);
int rwlock_wrlock(rwlock_t *rwl);
int rwlock_rdunlock(rwlock_t *rwl);
int rwlock_wrunlock(rwlock_t *rwl);

#endif


#endif /* _TINYCTHREAD_EXTRA_H_ */
