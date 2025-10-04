/* MIT License
 *
 * Copyright (c) 2023 Brad House
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 * SPDX-License-Identifier: MIT
 */
#include "ares_setup.h"
#include "ares.h"
#include "ares_private.h"

#ifdef CARES_THREADS
#  ifdef _WIN32

struct ares__thread_mutex {
  CRITICAL_SECTION mutex;
};

ares__thread_mutex_t *ares__thread_mutex_create(void)
{
  ares__thread_mutex_t *mut = ares_malloc_zero(sizeof(*mut));
  if (mut == NULL) {
    return NULL;
  }

  InitializeCriticalSection(&mut->mutex);
  return mut;
}

void ares__thread_mutex_destroy(ares__thread_mutex_t *mut)
{
  if (mut == NULL) {
    return;
  }
  DeleteCriticalSection(&mut->mutex);
  ares_free(mut);
}

void ares__thread_mutex_lock(ares__thread_mutex_t *mut)
{
  if (mut == NULL) {
    return;
  }
  EnterCriticalSection(&mut->mutex);
}

void ares__thread_mutex_unlock(ares__thread_mutex_t *mut)
{
  if (mut == NULL) {
    return;
  }
  LeaveCriticalSection(&mut->mutex);
}

struct ares__thread_cond {
  CONDITION_VARIABLE cond;
};

ares__thread_cond_t *ares__thread_cond_create(void)
{
  ares__thread_cond_t *cond = ares_malloc_zero(sizeof(*cond));
  if (cond == NULL) {
    return NULL;
  }
  InitializeConditionVariable(&cond->cond);
  return cond;
}

void ares__thread_cond_destroy(ares__thread_cond_t *cond)
{
  if (cond == NULL) {
    return;
  }
  ares_free(cond);
}

void ares__thread_cond_signal(ares__thread_cond_t *cond)
{
  if (cond == NULL) {
    return;
  }
  WakeConditionVariable(&cond->cond);
}

void ares__thread_cond_broadcast(ares__thread_cond_t *cond)
{
  if (cond == NULL) {
    return;
  }
  WakeAllConditionVariable(&cond->cond);
}

ares_status_t ares__thread_cond_wait(ares__thread_cond_t  *cond,
                                     ares__thread_mutex_t *mut)
{
  if (cond == NULL || mut == NULL) {
    return ARES_EFORMERR;
  }

  SleepConditionVariableCS(&cond->cond, &mut->mutex, INFINITE);
  return ARES_SUCCESS;
}

ares_status_t ares__thread_cond_timedwait(ares__thread_cond_t  *cond,
                                          ares__thread_mutex_t *mut,
                                          unsigned long         timeout_ms)
{
  if (cond == NULL || mut == NULL) {
    return ARES_EFORMERR;
  }

  if (!SleepConditionVariableCS(&cond->cond, &mut->mutex, timeout_ms)) {
    return ARES_ETIMEOUT;
  }

  return ARES_SUCCESS;
}

struct ares__thread {
  HANDLE thread;
  DWORD  id;

  void  *(*func)(void *arg);
  void  *arg;
  void  *rv;
};

/* Wrap for pthread compatibility */
static DWORD WINAPI ares__thread_func(LPVOID lpParameter)
{
  ares__thread_t *thread = lpParameter;

  thread->rv = thread->func(thread->arg);
  return 0;
}

ares_status_t ares__thread_create(ares__thread_t    **thread,
                                  ares__thread_func_t func, void *arg)
{
  ares__thread_t *thr = NULL;

  if (func == NULL || thread == NULL) {
    return ARES_EFORMERR;
  }

  thr = ares_malloc_zero(sizeof(*thr));
  if (thr == NULL) {
    return ARES_ENOMEM;
  }

  thr->func   = func;
  thr->arg    = arg;
  thr->thread = CreateThread(NULL, 0, ares__thread_func, thr, 0, &thr->id);
  if (thr->thread == NULL) {
    ares_free(thr);
    return ARES_ESERVFAIL;
  }

  *thread = thr;
  return ARES_SUCCESS;
}

ares_status_t ares__thread_join(ares__thread_t *thread, void **rv)
{
  ares_status_t status = ARES_SUCCESS;

  if (thread == NULL) {
    return ARES_EFORMERR;
  }

  if (WaitForSingleObject(thread->thread, INFINITE) != WAIT_OBJECT_0) {
    status = ARES_ENOTFOUND;
  } else {
    CloseHandle(thread->thread);
  }

  if (status == ARES_SUCCESS && rv != NULL) {
    *rv = thread->rv;
  }
  ares_free(thread);

  return status;
}

#  else /* !WIN32 == PTHREAD */
#    include <pthread.h>

/* for clock_gettime() */
#    ifdef HAVE_TIME_H
#      include <time.h>
#    endif

/* for gettimeofday() */
#    ifdef HAVE_SYS_TIME_H
#      include <sys/time.h>
#    endif

struct ares__thread_mutex {
  pthread_mutex_t mutex;
};

ares__thread_mutex_t *ares__thread_mutex_create(void)
{
  pthread_mutexattr_t   attr;
  ares__thread_mutex_t *mut = ares_malloc_zero(sizeof(*mut));
  if (mut == NULL) {
    return NULL;
  }

  if (pthread_mutexattr_init(&attr) != 0) {
    ares_free(mut);
    return NULL;
  }

  if (pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE) != 0) {
    goto fail;
  }

  if (pthread_mutex_init(&mut->mutex, &attr) != 0) {
    goto fail;
  }

  pthread_mutexattr_destroy(&attr);
  return mut;

fail:
  pthread_mutexattr_destroy(&attr);
  ares_free(mut);
  return NULL;
}

void ares__thread_mutex_destroy(ares__thread_mutex_t *mut)
{
  if (mut == NULL) {
    return;
  }
  pthread_mutex_destroy(&mut->mutex);
  ares_free(mut);
}

void ares__thread_mutex_lock(ares__thread_mutex_t *mut)
{
  if (mut == NULL) {
    return;
  }
  pthread_mutex_lock(&mut->mutex);
}

void ares__thread_mutex_unlock(ares__thread_mutex_t *mut)
{
  if (mut == NULL) {
    return;
  }
  pthread_mutex_unlock(&mut->mutex);
}

struct ares__thread_cond {
  pthread_cond_t cond;
};

ares__thread_cond_t *ares__thread_cond_create(void)
{
  ares__thread_cond_t *cond = ares_malloc_zero(sizeof(*cond));
  if (cond == NULL) {
    return NULL;
  }
  pthread_cond_init(&cond->cond, NULL);
  return cond;
}

void ares__thread_cond_destroy(ares__thread_cond_t *cond)
{
  if (cond == NULL) {
    return;
  }
  pthread_cond_destroy(&cond->cond);
  ares_free(cond);
}

void ares__thread_cond_signal(ares__thread_cond_t *cond)
{
  if (cond == NULL) {
    return;
  }
  pthread_cond_signal(&cond->cond);
}

void ares__thread_cond_broadcast(ares__thread_cond_t *cond)
{
  if (cond == NULL) {
    return;
  }
  pthread_cond_broadcast(&cond->cond);
}

ares_status_t ares__thread_cond_wait(ares__thread_cond_t  *cond,
                                     ares__thread_mutex_t *mut)
{
  if (cond == NULL || mut == NULL) {
    return ARES_EFORMERR;
  }

  pthread_cond_wait(&cond->cond, &mut->mutex);
  return ARES_SUCCESS;
}

static void ares__timespec_timeout(struct timespec *ts, unsigned long add_ms)
{
#    if defined(HAVE_CLOCK_GETTIME) && defined(CLOCK_REALTIME)
  clock_gettime(CLOCK_REALTIME, ts);
#    elif defined(HAVE_GETTIMEOFDAY)
  struct timeval tv;
  gettimeofday(&tv, NULL);
  ts->tv_sec  = tv.tv_sec;
  ts->tv_nsec = tv.tv_usec * 1000;
#    else
#      error cannot determine current system time
#    endif

  ts->tv_sec  += add_ms / 1000;
  ts->tv_nsec += (add_ms % 1000) * 1000000;

  /* Normalize if needed */
  if (ts->tv_nsec >= 1000000000) {
    ts->tv_sec  += ts->tv_nsec / 1000000000;
    ts->tv_nsec %= 1000000000;
  }
}

ares_status_t ares__thread_cond_timedwait(ares__thread_cond_t  *cond,
                                          ares__thread_mutex_t *mut,
                                          unsigned long         timeout_ms)
{
  struct timespec ts;

  if (cond == NULL || mut == NULL) {
    return ARES_EFORMERR;
  }

  ares__timespec_timeout(&ts, timeout_ms);

  if (pthread_cond_timedwait(&cond->cond, &mut->mutex, &ts) != 0) {
    return ARES_ETIMEOUT;
  }

  return ARES_SUCCESS;
}

struct ares__thread {
  pthread_t thread;
};

ares_status_t ares__thread_create(ares__thread_t    **thread,
                                  ares__thread_func_t func, void *arg)
{
  ares__thread_t *thr = NULL;

  if (func == NULL || thread == NULL) {
    return ARES_EFORMERR;
  }

  thr = ares_malloc_zero(sizeof(*thr));
  if (thr == NULL) {
    return ARES_ENOMEM;
  }
  if (pthread_create(&thr->thread, NULL, func, arg) != 0) {
    ares_free(thr);
    return ARES_ESERVFAIL;
  }

  *thread = thr;
  return ARES_SUCCESS;
}

ares_status_t ares__thread_join(ares__thread_t *thread, void **rv)
{
  void         *ret    = NULL;
  ares_status_t status = ARES_SUCCESS;

  if (thread == NULL) {
    return ARES_EFORMERR;
  }

  if (pthread_join(thread->thread, &ret) != 0) {
    status = ARES_ENOTFOUND;
  }
  ares_free(thread);

  if (status == ARES_SUCCESS && rv != NULL) {
    *rv = ret;
  }
  return status;
}

#  endif

ares_bool_t ares_threadsafety(void)
{
  return ARES_TRUE;
}

#else /* !CARES_THREADS */

/* NoOp */
ares__thread_mutex_t *ares__thread_mutex_create(void)
{
  return NULL;
}

void ares__thread_mutex_destroy(ares__thread_mutex_t *mut)
{
  (void)mut;
}

void ares__thread_mutex_lock(ares__thread_mutex_t *mut)
{
  (void)mut;
}

void ares__thread_mutex_unlock(ares__thread_mutex_t *mut)
{
  (void)mut;
}

ares__thread_cond_t *ares__thread_cond_create(void)
{
  return NULL;
}

void ares__thread_cond_destroy(ares__thread_cond_t *cond)
{
  (void)cond;
}

void ares__thread_cond_signal(ares__thread_cond_t *cond)
{
  (void)cond;
}

void ares__thread_cond_broadcast(ares__thread_cond_t *cond)
{
  (void)cond;
}

ares_status_t ares__thread_cond_wait(ares__thread_cond_t  *cond,
                                     ares__thread_mutex_t *mut)
{
  (void)cond;
  (void)mut;
  return ARES_ENOTIMP;
}

ares_status_t ares__thread_cond_timedwait(ares__thread_cond_t  *cond,
                                          ares__thread_mutex_t *mut,
                                          unsigned long         timeout_ms)
{
  (void)cond;
  (void)mut;
  (void)timeout_ms;
  return ARES_ENOTIMP;
}

ares_status_t ares__thread_create(ares__thread_t    **thread,
                                  ares__thread_func_t func, void *arg)
{
  (void)thread;
  (void)func;
  (void)arg;
  return ARES_ENOTIMP;
}

ares_status_t ares__thread_join(ares__thread_t *thread, void **rv)
{
  (void)thread;
  (void)rv;
  return ARES_ENOTIMP;
}

ares_bool_t ares_threadsafety(void)
{
  return ARES_FALSE;
}
#endif


ares_status_t ares__channel_threading_init(ares_channel_t *channel)
{
  ares_status_t status = ARES_SUCCESS;

  /* Threading is optional! */
  if (!ares_threadsafety()) {
    return ARES_SUCCESS;
  }

  channel->lock = ares__thread_mutex_create();
  if (channel->lock == NULL) {
    status = ARES_ENOMEM;
    goto done;
  }

  channel->cond_empty = ares__thread_cond_create();
  if (channel->cond_empty == NULL) {
    status = ARES_ENOMEM;
    goto done;
  }

done:
  if (status != ARES_SUCCESS) {
    ares__channel_threading_destroy(channel);
  }
  return status;
}

void ares__channel_threading_destroy(ares_channel_t *channel)
{
  ares__thread_mutex_destroy(channel->lock);
  channel->lock = NULL;
  ares__thread_cond_destroy(channel->cond_empty);
  channel->cond_empty = NULL;
}

void ares__channel_lock(ares_channel_t *channel)
{
  ares__thread_mutex_lock(channel->lock);
}

void ares__channel_unlock(ares_channel_t *channel)
{
  ares__thread_mutex_unlock(channel->lock);
}

/* Must not be holding a channel lock already, public function only */
ares_status_t ares_queue_wait_empty(ares_channel_t *channel, int timeout_ms)
{
  ares_status_t  status = ARES_SUCCESS;
  struct timeval tout;

  if (!ares_threadsafety()) {
    return ARES_ENOTIMP;
  }

  if (channel == NULL) {
    return ARES_EFORMERR;
  }

  if (timeout_ms >= 0) {
    tout          = ares__tvnow();
    tout.tv_sec  += timeout_ms / 1000;
    tout.tv_usec += (timeout_ms % 1000) * 1000;
  }

  ares__thread_mutex_lock(channel->lock);
  while (ares__llist_len(channel->all_queries)) {
    if (timeout_ms < 0) {
      ares__thread_cond_wait(channel->cond_empty, channel->lock);
    } else {
      struct timeval tv_remaining;
      struct timeval tv_now = ares__tvnow();
      unsigned long  tms;

      ares__timeval_remaining(&tv_remaining, &tv_now, &tout);
      tms = (unsigned long)((tv_remaining.tv_sec * 1000) +
                            (tv_remaining.tv_usec / 1000));
      if (tms == 0) {
        status = ARES_ETIMEOUT;
      } else {
        status =
          ares__thread_cond_timedwait(channel->cond_empty, channel->lock, tms);
      }
    }
  }
  ares__thread_mutex_unlock(channel->lock);
  return status;
}

void ares_queue_notify_empty(ares_channel_t *channel)
{
  if (channel == NULL) {
    return;
  }

  /* We are guaranteed to be holding a channel lock already */
  if (ares__llist_len(channel->all_queries)) {
    return;
  }

  /* Notify all waiters of the conditional */
  ares__thread_cond_broadcast(channel->cond_empty);
}
