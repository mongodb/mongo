/* -*- mode: c; tab-width: 2; indent-tabs-mode: nil; -*-
Copyright (c) 2012 Marcus Geelnard
Copyright (c) 2013-2014 Evan Nemerson

This software is provided 'as-is', without any express or implied
warranty. In no event will the authors be held liable for any damages
arising from the use of this software.

Permission is granted to anyone to use this software for any purpose,
including commercial applications, and to alter it and redistribute it
freely, subject to the following restrictions:

    1. The origin of this software must not be misrepresented; you must not
    claim that you wrote the original software. If you use this software
    in a product, an acknowledgment in the product documentation would be
    appreciated but is not required.

    2. Altered source versions must be plainly marked as such, and must not be
    misrepresented as being the original software.

    3. This notice may not be removed or altered from any source
    distribution.
*/

#include "rd.h"
#include <stdlib.h>

#if !WITH_C11THREADS

/* Platform specific includes */
#if defined(_TTHREAD_POSIX_)
  #include <signal.h>
  #include <sched.h>
  #include <unistd.h>
  #include <sys/time.h>
  #include <errno.h>
#elif defined(_TTHREAD_WIN32_)
  #include <process.h>
  #include <sys/timeb.h>
#endif


/* Standard, good-to-have defines */
#ifndef NULL
  #define NULL (void*)0
#endif
#ifndef TRUE
  #define TRUE 1
#endif
#ifndef FALSE
  #define FALSE 0
#endif

#ifdef __cplusplus
extern "C" {
#endif

static RD_TLS int thrd_is_detached;


int mtx_init(mtx_t *mtx, int type)
{
#if defined(_TTHREAD_WIN32_)
  mtx->mAlreadyLocked = FALSE;
  mtx->mRecursive = type & mtx_recursive;
  mtx->mTimed = type & mtx_timed;
  if (!mtx->mTimed)
  {
    InitializeCriticalSection(&(mtx->mHandle.cs));
  }
  else
  {
    mtx->mHandle.mut = CreateMutex(NULL, FALSE, NULL);
    if (mtx->mHandle.mut == NULL)
    {
      return thrd_error;
    }
  }
  return thrd_success;
#else
  int ret;
  pthread_mutexattr_t attr;
  pthread_mutexattr_init(&attr);
  if (type & mtx_recursive)
  {
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
  }
  ret = pthread_mutex_init(mtx, &attr);
  pthread_mutexattr_destroy(&attr);
  return ret == 0 ? thrd_success : thrd_error;
#endif
}

void mtx_destroy(mtx_t *mtx)
{
#if defined(_TTHREAD_WIN32_)
  if (!mtx->mTimed)
  {
    DeleteCriticalSection(&(mtx->mHandle.cs));
  }
  else
  {
    CloseHandle(mtx->mHandle.mut);
  }
#else
  pthread_mutex_destroy(mtx);
#endif
}

int mtx_lock(mtx_t *mtx)
{
#if defined(_TTHREAD_WIN32_)
  if (!mtx->mTimed)
  {
    EnterCriticalSection(&(mtx->mHandle.cs));
  }
  else
  {
    switch (WaitForSingleObject(mtx->mHandle.mut, INFINITE))
    {
      case WAIT_OBJECT_0:
        break;
      case WAIT_ABANDONED:
      default:
        return thrd_error;
    }
  }

  if (!mtx->mRecursive)
  {
    rd_assert(!mtx->mAlreadyLocked); /* Would deadlock */
    mtx->mAlreadyLocked = TRUE;
  }
  return thrd_success;
#else
  return pthread_mutex_lock(mtx) == 0 ? thrd_success : thrd_error;
#endif
}

int mtx_timedlock(mtx_t *mtx, const struct timespec *ts)
{
#if defined(_TTHREAD_WIN32_)
  struct timespec current_ts;
  DWORD timeoutMs;

  if (!mtx->mTimed)
  {
    return thrd_error;
  }

  timespec_get(&current_ts, TIME_UTC);

  if ((current_ts.tv_sec > ts->tv_sec) || ((current_ts.tv_sec == ts->tv_sec) && (current_ts.tv_nsec >= ts->tv_nsec)))
  {
    timeoutMs = 0;
  }
  else
  {
    timeoutMs  = (DWORD)(ts->tv_sec  - current_ts.tv_sec)  * 1000;
    timeoutMs += (ts->tv_nsec - current_ts.tv_nsec) / 1000000;
    timeoutMs += 1;
  }

  /* TODO: the timeout for WaitForSingleObject doesn't include time
     while the computer is asleep. */
  switch (WaitForSingleObject(mtx->mHandle.mut, timeoutMs))
  {
    case WAIT_OBJECT_0:
      break;
    case WAIT_TIMEOUT:
      return thrd_timedout;
    case WAIT_ABANDONED:
    default:
      return thrd_error;
  }

  if (!mtx->mRecursive)
  {
    rd_assert(!mtx->mAlreadyLocked); /* Would deadlock */
    mtx->mAlreadyLocked = TRUE;
  }

  return thrd_success;
#elif defined(_POSIX_TIMEOUTS) && (_POSIX_TIMEOUTS >= 200112L) && defined(_POSIX_THREADS) && (_POSIX_THREADS >= 200112L)
  switch (pthread_mutex_timedlock(mtx, ts)) {
    case 0:
      return thrd_success;
    case ETIMEDOUT:
      return thrd_timedout;
    default:
      return thrd_error;
  }
#else
  int rc;
  struct timespec cur, dur;

  /* Try to acquire the lock and, if we fail, sleep for 5ms. */
  while ((rc = pthread_mutex_trylock (mtx)) == EBUSY) {
    timespec_get(&cur, TIME_UTC);

    if ((cur.tv_sec > ts->tv_sec) || ((cur.tv_sec == ts->tv_sec) && (cur.tv_nsec >= ts->tv_nsec)))
    {
      break;
    }

    dur.tv_sec = ts->tv_sec - cur.tv_sec;
    dur.tv_nsec = ts->tv_nsec - cur.tv_nsec;
    if (dur.tv_nsec < 0)
    {
      dur.tv_sec--;
      dur.tv_nsec += 1000000000;
    }

    if ((dur.tv_sec != 0) || (dur.tv_nsec > 5000000))
    {
      dur.tv_sec = 0;
      dur.tv_nsec = 5000000;
    }

    nanosleep(&dur, NULL);
  }

  switch (rc) {
    case 0:
      return thrd_success;
    case ETIMEDOUT:
    case EBUSY:
      return thrd_timedout;
    default:
      return thrd_error;
  }
#endif
}

int mtx_trylock(mtx_t *mtx)
{
#if defined(_TTHREAD_WIN32_)
  int ret;

  if (!mtx->mTimed)
  {
    ret = TryEnterCriticalSection(&(mtx->mHandle.cs)) ? thrd_success : thrd_busy;
  }
  else
  {
    ret = (WaitForSingleObject(mtx->mHandle.mut, 0) == WAIT_OBJECT_0) ? thrd_success : thrd_busy;
  }

  if ((!mtx->mRecursive) && (ret == thrd_success))
  {
    if (mtx->mAlreadyLocked)
    {
      LeaveCriticalSection(&(mtx->mHandle.cs));
      ret = thrd_busy;
    }
    else
    {
      mtx->mAlreadyLocked = TRUE;
    }
  }
  return ret;
#else
  return (pthread_mutex_trylock(mtx) == 0) ? thrd_success : thrd_busy;
#endif
}

int mtx_unlock(mtx_t *mtx)
{
#if defined(_TTHREAD_WIN32_)
  mtx->mAlreadyLocked = FALSE;
  if (!mtx->mTimed)
  {
    LeaveCriticalSection(&(mtx->mHandle.cs));
  }
  else
  {
    if (!ReleaseMutex(mtx->mHandle.mut))
    {
      return thrd_error;
    }
  }
  return thrd_success;
#else
  return pthread_mutex_unlock(mtx) == 0 ? thrd_success : thrd_error;;
#endif
}

#if defined(_TTHREAD_WIN32_)
#define _CONDITION_EVENT_ONE 0
#define _CONDITION_EVENT_ALL 1
#endif

int cnd_init(cnd_t *cond)
{
#if defined(_TTHREAD_WIN32_)
  cond->mWaitersCount = 0;

  /* Init critical section */
  InitializeCriticalSection(&cond->mWaitersCountLock);

  /* Init events */
  cond->mEvents[_CONDITION_EVENT_ONE] = CreateEvent(NULL, FALSE, FALSE, NULL);
  if (cond->mEvents[_CONDITION_EVENT_ONE] == NULL)
  {
    cond->mEvents[_CONDITION_EVENT_ALL] = NULL;
    return thrd_error;
  }
  cond->mEvents[_CONDITION_EVENT_ALL] = CreateEvent(NULL, TRUE, FALSE, NULL);
  if (cond->mEvents[_CONDITION_EVENT_ALL] == NULL)
  {
    CloseHandle(cond->mEvents[_CONDITION_EVENT_ONE]);
    cond->mEvents[_CONDITION_EVENT_ONE] = NULL;
    return thrd_error;
  }

  return thrd_success;
#else
  return pthread_cond_init(cond, NULL) == 0 ? thrd_success : thrd_error;
#endif
}

void cnd_destroy(cnd_t *cond)
{
#if defined(_TTHREAD_WIN32_)
  if (cond->mEvents[_CONDITION_EVENT_ONE] != NULL)
  {
    CloseHandle(cond->mEvents[_CONDITION_EVENT_ONE]);
  }
  if (cond->mEvents[_CONDITION_EVENT_ALL] != NULL)
  {
    CloseHandle(cond->mEvents[_CONDITION_EVENT_ALL]);
  }
  DeleteCriticalSection(&cond->mWaitersCountLock);
#else
  pthread_cond_destroy(cond);
#endif
}

int cnd_signal(cnd_t *cond)
{
#if defined(_TTHREAD_WIN32_)
  int haveWaiters;

  /* Are there any waiters? */
  EnterCriticalSection(&cond->mWaitersCountLock);
  haveWaiters = (cond->mWaitersCount > 0);
  LeaveCriticalSection(&cond->mWaitersCountLock);

  /* If we have any waiting threads, send them a signal */
  if(haveWaiters)
  {
    if (SetEvent(cond->mEvents[_CONDITION_EVENT_ONE]) == 0)
    {
      return thrd_error;
    }
  }

  return thrd_success;
#else
  return pthread_cond_signal(cond) == 0 ? thrd_success : thrd_error;
#endif
}

int cnd_broadcast(cnd_t *cond)
{
#if defined(_TTHREAD_WIN32_)
  int haveWaiters;

  /* Are there any waiters? */
  EnterCriticalSection(&cond->mWaitersCountLock);
  haveWaiters = (cond->mWaitersCount > 0);
  LeaveCriticalSection(&cond->mWaitersCountLock);

  /* If we have any waiting threads, send them a signal */
  if(haveWaiters)
  {
    if (SetEvent(cond->mEvents[_CONDITION_EVENT_ALL]) == 0)
    {
      return thrd_error;
    }
  }

  return thrd_success;
#else
  return pthread_cond_broadcast(cond) == 0 ? thrd_success : thrd_error;
#endif
}

#if defined(_TTHREAD_WIN32_)
int _cnd_timedwait_win32(cnd_t *cond, mtx_t *mtx, DWORD timeout)
{
  int result, lastWaiter;

  /* Increment number of waiters */
  EnterCriticalSection(&cond->mWaitersCountLock);
  ++ cond->mWaitersCount;
  LeaveCriticalSection(&cond->mWaitersCountLock);

  /* Release the mutex while waiting for the condition (will decrease
     the number of waiters when done)... */
  mtx_unlock(mtx);

  /* Wait for either event to become signaled due to cnd_signal() or
     cnd_broadcast() being called */
  result = WaitForMultipleObjects(2, cond->mEvents, FALSE, timeout);

  /* Check if we are the last waiter */
  EnterCriticalSection(&cond->mWaitersCountLock);
  -- cond->mWaitersCount;
  lastWaiter = (result == (WAIT_OBJECT_0 + _CONDITION_EVENT_ALL)) &&
               (cond->mWaitersCount == 0);
  LeaveCriticalSection(&cond->mWaitersCountLock);

  /* If we are the last waiter to be notified to stop waiting, reset the event */
  if (lastWaiter)
  {
    if (ResetEvent(cond->mEvents[_CONDITION_EVENT_ALL]) == 0)
    {
      /* The mutex is locked again before the function returns, even if an error occurred */
      mtx_lock(mtx);
      return thrd_error;
    }
  }

  /* The mutex is locked again before the function returns, even if an error occurred */
  mtx_lock(mtx);

  if (result == WAIT_TIMEOUT)
            return thrd_timedout;
  else if (result == (int)WAIT_FAILED)
            return thrd_error;

  return thrd_success;
}
#endif

int cnd_wait(cnd_t *cond, mtx_t *mtx)
{
#if defined(_TTHREAD_WIN32_)
  return _cnd_timedwait_win32(cond, mtx, INFINITE);
#else
  return pthread_cond_wait(cond, mtx) == 0 ? thrd_success : thrd_error;
#endif
}

int cnd_timedwait(cnd_t *cond, mtx_t *mtx, const struct timespec *ts)
{
#if defined(_TTHREAD_WIN32_)
  struct timespec now;
  if (timespec_get(&now, TIME_UTC) == TIME_UTC)
  {
    unsigned long long nowInMilliseconds = now.tv_sec * 1000 + now.tv_nsec / 1000000;
    unsigned long long tsInMilliseconds  = ts->tv_sec * 1000 + ts->tv_nsec / 1000000;
    DWORD delta = (tsInMilliseconds > nowInMilliseconds) ?
      (DWORD)(tsInMilliseconds - nowInMilliseconds) : 0;
    return _cnd_timedwait_win32(cond, mtx, delta);
  }
  else
    return thrd_error;
#else
  int ret;
  ret = pthread_cond_timedwait(cond, mtx, ts);
  if (ret == ETIMEDOUT)
  {
    return thrd_timedout;
  }
  return ret == 0 ? thrd_success : thrd_error;
#endif
}



#if defined(_TTHREAD_WIN32_)
struct TinyCThreadTSSData {
  void* value;
  tss_t key;
  struct TinyCThreadTSSData* next;
};

static tss_dtor_t _tinycthread_tss_dtors[1088] = { NULL, };

static _Thread_local struct TinyCThreadTSSData* _tinycthread_tss_head = NULL;
static _Thread_local struct TinyCThreadTSSData* _tinycthread_tss_tail = NULL;

static void _tinycthread_tss_cleanup (void);

static void _tinycthread_tss_cleanup (void) {
  struct TinyCThreadTSSData* data;
  int iteration;
  unsigned int again = 1;
  void* value;

  for (iteration = 0 ; iteration < TSS_DTOR_ITERATIONS && again > 0 ; iteration++)
  {
    again = 0;
    for (data = _tinycthread_tss_head ; data != NULL ; data = data->next)
    {
      if (data->value != NULL)
      {
        value = data->value;
        data->value = NULL;

        if (_tinycthread_tss_dtors[data->key] != NULL)
        {
          again = 1;
          _tinycthread_tss_dtors[data->key](value);
        }
      }
    }
  }

  while (_tinycthread_tss_head != NULL) {
    data = _tinycthread_tss_head->next;
    rd_free (_tinycthread_tss_head);
    _tinycthread_tss_head = data;
  }
  _tinycthread_tss_head = NULL;
  _tinycthread_tss_tail = NULL;
}

static void NTAPI _tinycthread_tss_callback(PVOID h, DWORD dwReason, PVOID pv)
{
  (void)h;
  (void)pv;

  if (_tinycthread_tss_head != NULL && (dwReason == DLL_THREAD_DETACH || dwReason == DLL_PROCESS_DETACH))
  {
    _tinycthread_tss_cleanup();
  }
}

#ifdef _WIN32
  #ifdef _M_X64
    #pragma const_seg(".CRT$XLB")
  #else
    #pragma data_seg(".CRT$XLB")
  #endif
  PIMAGE_TLS_CALLBACK p_thread_callback = _tinycthread_tss_callback;
  #ifdef _M_X64
    #pragma const_seg()
  #else
    #pragma data_seg()
  #endif
#else
  PIMAGE_TLS_CALLBACK p_thread_callback __attribute__((section(".CRT$XLB"))) = _tinycthread_tss_callback;
#endif

#endif /* defined(_TTHREAD_WIN32_) */

/** Information to pass to the new thread (what to run). */
typedef struct {
  thrd_start_t mFunction; /**< Pointer to the function to be executed. */
  void * mArg;            /**< Function argument for the thread function. */
} _thread_start_info;

/* Thread wrapper function. */
#if defined(_TTHREAD_WIN32_)
static DWORD WINAPI _thrd_wrapper_function(LPVOID aArg)
#elif defined(_TTHREAD_POSIX_)
static void * _thrd_wrapper_function(void * aArg)
#endif
{
  thrd_start_t fun;
  void *arg;
  int  res;

  /* Get thread startup information */
  _thread_start_info *ti = (_thread_start_info *) aArg;
  fun = ti->mFunction;
  arg = ti->mArg;

  /* The thread is responsible for freeing the startup information */
  rd_free((void *)ti);

  /* Call the actual client thread function */
  res = fun(arg);

#if defined(_TTHREAD_WIN32_)
  if (_tinycthread_tss_head != NULL)
  {
    _tinycthread_tss_cleanup();
  }

  return (DWORD)res;
#else
  return (void*)(intptr_t)res;
#endif
}

int thrd_create(thrd_t *thr, thrd_start_t func, void *arg)
{
  /* Fill out the thread startup information (passed to the thread wrapper,
     which will eventually free it) */
  _thread_start_info* ti = (_thread_start_info*)rd_malloc(sizeof(_thread_start_info));
  if (ti == NULL)
  {
    return thrd_nomem;
  }
  ti->mFunction = func;
  ti->mArg = arg;

  /* Create the thread */
#if defined(_TTHREAD_WIN32_)
  *thr = CreateThread(NULL, 0, _thrd_wrapper_function, (LPVOID) ti, 0, NULL);
#elif defined(_TTHREAD_POSIX_)
  {
          int err;
          if((err = pthread_create(thr, NULL, _thrd_wrapper_function,
                                   (void *)ti)) != 0) {
                  errno = err;
                  *thr = 0;
          }
  }
#endif

  /* Did we fail to create the thread? */
  if(!*thr)
  {
    rd_free(ti);
    return thrd_error;
  }

  return thrd_success;
}

thrd_t thrd_current(void)
{
#if defined(_TTHREAD_WIN32_)
  return GetCurrentThread();
#else
  return pthread_self();
#endif
}

int thrd_detach(thrd_t thr)
{
  thrd_is_detached = 1;
#if defined(_TTHREAD_WIN32_)
  /* https://stackoverflow.com/questions/12744324/how-to-detach-a-thread-on-windows-c#answer-12746081 */
  return CloseHandle(thr) != 0 ? thrd_success : thrd_error;
#else
  return pthread_detach(thr) == 0 ? thrd_success : thrd_error;
#endif
}

int thrd_equal(thrd_t thr0, thrd_t thr1)
{
#if defined(_TTHREAD_WIN32_)
  return thr0 == thr1;
#else
  return pthread_equal(thr0, thr1);
#endif
}

void thrd_exit(int res)
{
#if defined(_TTHREAD_WIN32_)
  if (_tinycthread_tss_head != NULL)
  {
    _tinycthread_tss_cleanup();
  }

  ExitThread(res);
#else
  pthread_exit((void*)(intptr_t)res);
#endif
}

int thrd_join(thrd_t thr, int *res)
{
#if defined(_TTHREAD_WIN32_)
  DWORD dwRes;

  if (WaitForSingleObject(thr, INFINITE) == WAIT_FAILED)
  {
    return thrd_error;
  }
  if (res != NULL)
  {
    if (GetExitCodeThread(thr, &dwRes) != 0)
    {
      *res = dwRes;
    }
    else
    {
      return thrd_error;
    }
  }
  CloseHandle(thr);
#elif defined(_TTHREAD_POSIX_)
  void *pres;
  if (pthread_join(thr, &pres) != 0)
  {
    return thrd_error;
  }
  if (res != NULL)
  {
    *res = (int)(intptr_t)pres;
  }
#endif
  return thrd_success;
}

int thrd_sleep(const struct timespec *duration, struct timespec *remaining)
{
#if !defined(_TTHREAD_WIN32_)
  return nanosleep(duration, remaining);
#else
  struct timespec start;
  DWORD t;

  timespec_get(&start, TIME_UTC);

  t = SleepEx((DWORD)(duration->tv_sec * 1000 +
              duration->tv_nsec / 1000000 +
              (((duration->tv_nsec % 1000000) == 0) ? 0 : 1)),
              TRUE);

  if (t == 0) {
    return 0;
  } else if (remaining != NULL) {
    timespec_get(remaining, TIME_UTC);
    remaining->tv_sec -= start.tv_sec;
    remaining->tv_nsec -= start.tv_nsec;
    if (remaining->tv_nsec < 0)
    {
      remaining->tv_nsec += 1000000000;
      remaining->tv_sec -= 1;
    }
  } else {
    return -1;
  }

  return 0;
#endif
}

void thrd_yield(void)
{
#if defined(_TTHREAD_WIN32_)
  Sleep(0);
#else
  sched_yield();
#endif
}

int tss_create(tss_t *key, tss_dtor_t dtor)
{
#if defined(_TTHREAD_WIN32_)
  *key = TlsAlloc();
  if (*key == TLS_OUT_OF_INDEXES)
  {
    return thrd_error;
  }
  _tinycthread_tss_dtors[*key] = dtor;
#else
  if (pthread_key_create(key, dtor) != 0)
  {
    return thrd_error;
  }
#endif
  return thrd_success;
}

void tss_delete(tss_t key)
{
#if defined(_TTHREAD_WIN32_)
  struct TinyCThreadTSSData* data = (struct TinyCThreadTSSData*) TlsGetValue (key);
  struct TinyCThreadTSSData* prev = NULL;
  if (data != NULL)
  {
    if (data == _tinycthread_tss_head)
    {
      _tinycthread_tss_head = data->next;
    }
    else
    {
      prev = _tinycthread_tss_head;
      if (prev != NULL)
      {
        while (prev->next != data)
        {
          prev = prev->next;
        }
      }
    }

    if (data == _tinycthread_tss_tail)
    {
      _tinycthread_tss_tail = prev;
    }

    rd_free (data);
  }
  _tinycthread_tss_dtors[key] = NULL;
  TlsFree(key);
#else
  pthread_key_delete(key);
#endif
}

void *tss_get(tss_t key)
{
#if defined(_TTHREAD_WIN32_)
  struct TinyCThreadTSSData* data = (struct TinyCThreadTSSData*)TlsGetValue(key);
  if (data == NULL)
  {
    return NULL;
  }
  return data->value;
#else
  return pthread_getspecific(key);
#endif
}

int tss_set(tss_t key, void *val)
{
#if defined(_TTHREAD_WIN32_)
  struct TinyCThreadTSSData* data = (struct TinyCThreadTSSData*)TlsGetValue(key);
  if (data == NULL)
  {
    data = (struct TinyCThreadTSSData*)rd_malloc(sizeof(struct TinyCThreadTSSData));
    if (data == NULL)
    {
      return thrd_error;
	}

    data->value = NULL;
    data->key = key;
    data->next = NULL;

    if (_tinycthread_tss_tail != NULL)
    {
      _tinycthread_tss_tail->next = data;
    }
    else
    {
      _tinycthread_tss_tail = data;
    }

    if (_tinycthread_tss_head == NULL)
    {
      _tinycthread_tss_head = data;
    }

    if (!TlsSetValue(key, data))
    {
      rd_free (data);
	  return thrd_error;
    }
  }
  data->value = val;
#else
  if (pthread_setspecific(key, val) != 0)
  {
    return thrd_error;
  }
#endif
  return thrd_success;
}

#if defined(_TTHREAD_EMULATE_TIMESPEC_GET_)
int _tthread_timespec_get(struct timespec *ts, int base)
{
#if defined(_TTHREAD_WIN32_)
  struct _timeb tb;
#elif !defined(CLOCK_REALTIME)
  struct timeval tv;
#endif

  if (base != TIME_UTC)
  {
    return 0;
  }

#if defined(_TTHREAD_WIN32_)
  _ftime_s(&tb);
  ts->tv_sec = (time_t)tb.time;
  ts->tv_nsec = 1000000L * (long)tb.millitm;
#elif defined(CLOCK_REALTIME)
  base = (clock_gettime(CLOCK_REALTIME, ts) == 0) ? base : 0;
#else
  gettimeofday(&tv, NULL);
  ts->tv_sec = (time_t)tv.tv_sec;
  ts->tv_nsec = 1000L * (long)tv.tv_usec;
#endif

  return base;
}
#endif /* _TTHREAD_EMULATE_TIMESPEC_GET_ */

#if defined(_TTHREAD_WIN32_)
void call_once(once_flag *flag, void (*func)(void))
{
  /* The idea here is that we use a spin lock (via the
     InterlockedCompareExchange function) to restrict access to the
     critical section until we have initialized it, then we use the
     critical section to block until the callback has completed
     execution. */
  while (flag->status < 3)
  {
    switch (flag->status)
    {
      case 0:
        if (InterlockedCompareExchange (&(flag->status), 1, 0) == 0) {
          InitializeCriticalSection(&(flag->lock));
          EnterCriticalSection(&(flag->lock));
          flag->status = 2;
          func();
          flag->status = 3;
          LeaveCriticalSection(&(flag->lock));
          return;
        }
        break;
      case 1:
        break;
      case 2:
        EnterCriticalSection(&(flag->lock));
        LeaveCriticalSection(&(flag->lock));
        break;
    }
  }
}
#endif /* defined(_TTHREAD_WIN32_) */



#ifdef __cplusplus
}
#endif

#endif /* !WITH_C11THREADS */
