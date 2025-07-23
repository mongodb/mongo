/*
 * librdkafka - Apache Kafka C library
 *
 * Copyright (c) 2018 Magnus Edenhill
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
 * @brief Extra methods added to tinycthread/c11threads
 */

#include "rd.h"
#include "rdtime.h"
#include "tinycthread.h"


int thrd_setname(const char *name) {
#if HAVE_PTHREAD_SETNAME_GNU
        if (!pthread_setname_np(pthread_self(), name))
                return thrd_success;
#elif HAVE_PTHREAD_SETNAME_DARWIN
        pthread_setname_np(name);
        return thrd_success;
#elif HAVE_PTHREAD_SETNAME_FREEBSD
        pthread_set_name_np(pthread_self(), name);
        return thrd_success;
#endif
        return thrd_error;
}

int thrd_is_current(thrd_t thr) {
#if defined(_TTHREAD_WIN32_)
        return GetThreadId(thr) == GetCurrentThreadId();
#else
        return (pthread_self() == thr);
#endif
}


#ifdef _WIN32
void cnd_wait_enter(cnd_t *cond) {
        /* Increment number of waiters */
        EnterCriticalSection(&cond->mWaitersCountLock);
        ++cond->mWaitersCount;
        LeaveCriticalSection(&cond->mWaitersCountLock);
}

void cnd_wait_exit(cnd_t *cond) {
        /* Increment number of waiters */
        EnterCriticalSection(&cond->mWaitersCountLock);
        --cond->mWaitersCount;
        LeaveCriticalSection(&cond->mWaitersCountLock);
}
#endif



int cnd_timedwait_ms(cnd_t *cnd, mtx_t *mtx, int timeout_ms) {
        if (timeout_ms == -1 /* INFINITE*/)
                return cnd_wait(cnd, mtx);
#if defined(_TTHREAD_WIN32_)
        return _cnd_timedwait_win32(cnd, mtx, (DWORD)timeout_ms);
#else
        struct timeval tv;
        struct timespec ts;

        gettimeofday(&tv, NULL);
        ts.tv_sec  = tv.tv_sec;
        ts.tv_nsec = tv.tv_usec * 1000;

        ts.tv_sec += timeout_ms / 1000;
        ts.tv_nsec += (timeout_ms % 1000) * 1000000;

        if (ts.tv_nsec >= 1000000000) {
                ts.tv_sec++;
                ts.tv_nsec -= 1000000000;
        }

        return cnd_timedwait(cnd, mtx, &ts);
#endif
}

int cnd_timedwait_msp(cnd_t *cnd, mtx_t *mtx, int *timeout_msp) {
        rd_ts_t pre = rd_clock();
        int r;
        r = cnd_timedwait_ms(cnd, mtx, *timeout_msp);
        if (r != thrd_timedout) {
                /* Subtract spent time */
                (*timeout_msp) -= (int)(rd_clock() - pre) / 1000;
        }
        return r;
}

int cnd_timedwait_abs(cnd_t *cnd, mtx_t *mtx, const struct timespec *tspec) {
        if (tspec->tv_sec == RD_POLL_INFINITE)
                return cnd_wait(cnd, mtx);
        else if (tspec->tv_sec == RD_POLL_NOWAIT)
                return thrd_timedout;

        return cnd_timedwait(cnd, mtx, tspec);
}


/**
 * @name Read-write locks
 * @{
 */
#ifndef _WIN32
int rwlock_init(rwlock_t *rwl) {
        int r = pthread_rwlock_init(rwl, NULL);
        if (r) {
                errno = r;
                return thrd_error;
        }
        return thrd_success;
}

int rwlock_destroy(rwlock_t *rwl) {
        int r = pthread_rwlock_destroy(rwl);
        if (r) {
                errno = r;
                return thrd_error;
        }
        return thrd_success;
}

int rwlock_rdlock(rwlock_t *rwl) {
        int r = pthread_rwlock_rdlock(rwl);
        assert(r == 0);
        return thrd_success;
}

int rwlock_wrlock(rwlock_t *rwl) {
        int r = pthread_rwlock_wrlock(rwl);
        assert(r == 0);
        return thrd_success;
}

int rwlock_rdunlock(rwlock_t *rwl) {
        int r = pthread_rwlock_unlock(rwl);
        assert(r == 0);
        return thrd_success;
}

int rwlock_wrunlock(rwlock_t *rwl) {
        int r = pthread_rwlock_unlock(rwl);
        assert(r == 0);
        return thrd_success;
}
/**@}*/


#endif /* !_MSC_VER */
