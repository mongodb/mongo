/*
 * Copyright 2013-present MongoDB, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "common-prelude.h"
#include "common-config.h"
#include "common-macros-private.h"

#ifndef COMMON_THREAD_PRIVATE_H
#define COMMON_THREAD_PRIVATE_H

#define BSON_INSIDE
#include "bson/bson-compat.h"
#include "bson/bson-config.h"
#include "bson/bson-macros.h"
#undef BSON_INSIDE

BSON_BEGIN_DECLS

#define mcommon_thread_create COMMON_NAME (thread_create)
#define mcommon_thread_join COMMON_NAME (thread_join)

#if defined(BSON_OS_UNIX)
#include <pthread.h>

#define BSON_ONCE_FUN(n) void n (void)
#define BSON_ONCE_RETURN return
#define BSON_ONCE_INIT PTHREAD_ONCE_INIT
#define bson_once(o, c)                           \
   do {                                           \
      BSON_ASSERT (pthread_once ((o), (c)) == 0); \
   } while (0)
#define bson_once_t pthread_once_t
#define bson_thread_t pthread_t
#define BSON_THREAD_FUN(_function_name, _arg_name) void *(_function_name) (void *(_arg_name))
#define BSON_THREAD_FUN_TYPE(_function_name) void *(*(_function_name)) (void *)
#define BSON_THREAD_RETURN return NULL

/* this macro can be defined as a as a build configuration option
 * with -DENABLE_DEBUG_ASSERTIONS=ON.  its purpose is to allow for functions
 * that require a mutex to be locked on entry to assert that the mutex
 * is actually locked.
 * this can prevent bugs where a caller forgets to lock the mutex. */

#ifndef MONGOC_ENABLE_DEBUG_ASSERTIONS

#define bson_mutex_destroy(m)                         \
   do {                                               \
      BSON_ASSERT (pthread_mutex_destroy ((m)) == 0); \
   } while (0)

#define bson_mutex_init(_n)                               \
   do {                                                   \
      BSON_ASSERT (pthread_mutex_init ((_n), NULL) == 0); \
   } while (0)

#define bson_mutex_lock(m)                         \
   do {                                            \
      BSON_ASSERT (pthread_mutex_lock ((m)) == 0); \
   } while (0)

#define bson_mutex_t pthread_mutex_t

#define bson_mutex_unlock(m)                         \
   do {                                              \
      BSON_ASSERT (pthread_mutex_unlock ((m)) == 0); \
   } while (0)

#else
typedef struct {
   pthread_t lock_owner;
   pthread_mutex_t wrapped_mutex;
   bool valid_tid;
} bson_mutex_t;

#define bson_mutex_destroy(mutex)                                         \
   do {                                                                   \
      BSON_ASSERT (pthread_mutex_destroy (&(mutex)->wrapped_mutex) == 0); \
   } while (0);

#define bson_mutex_init(mutex)                                               \
   do {                                                                      \
      BSON_ASSERT (pthread_mutex_init (&(mutex)->wrapped_mutex, NULL) == 0); \
      (mutex)->valid_tid = false;                                            \
   } while (0);

#define bson_mutex_lock(mutex)                                         \
   do {                                                                \
      BSON_ASSERT (pthread_mutex_lock (&(mutex)->wrapped_mutex) == 0); \
      (mutex)->lock_owner = pthread_self ();                           \
      (mutex)->valid_tid = true;                                       \
   } while (0);

#define bson_mutex_unlock(mutex)                                         \
   do {                                                                  \
      (mutex)->valid_tid = false;                                        \
      BSON_ASSERT (pthread_mutex_unlock (&(mutex)->wrapped_mutex) == 0); \
   } while (0);

#endif

#else
#include <process.h>
#define BSON_ONCE_FUN(n) BOOL CALLBACK n (PINIT_ONCE _ignored_a, PVOID _ignored_b, PVOID *_ignored_c)
#define BSON_ONCE_INIT INIT_ONCE_STATIC_INIT
#define BSON_ONCE_RETURN return true
#define bson_mutex_destroy DeleteCriticalSection
#define bson_mutex_init InitializeCriticalSection
#define bson_mutex_lock EnterCriticalSection
#define bson_mutex_t CRITICAL_SECTION
#define bson_mutex_unlock LeaveCriticalSection
#define bson_once(o, c)                                         \
   do {                                                         \
      BSON_ASSERT (InitOnceExecuteOnce ((o), (c), NULL, NULL)); \
   } while (0)
#define bson_once_t INIT_ONCE
#define bson_thread_t HANDLE
#define BSON_THREAD_FUN(_function_name, _arg_name) unsigned (__stdcall _function_name) (void *(_arg_name))
#define BSON_THREAD_FUN_TYPE(_function_name) unsigned (__stdcall * _function_name) (void *)
#define BSON_THREAD_RETURN return 0
#endif

/* Functions that require definitions get the common prefix (_mongoc for
 * libmongoc or _bson for libbson) to avoid duplicate symbols when linking both
 * libbson and libmongoc statically. */
int
mcommon_thread_join (bson_thread_t thread);
// mcommon_thread_create returns 0 on success. Returns a non-zero error code on
// error. Callers may use `bson_strerror_r` to get an error message from the
// returned error code.
int
mcommon_thread_create (bson_thread_t *thread, BSON_THREAD_FUN_TYPE (func), void *arg);

#if defined(MONGOC_ENABLE_DEBUG_ASSERTIONS) && defined(BSON_OS_UNIX)
#define mcommon_mutex_is_locked COMMON_NAME (mutex_is_locked)
bool
mcommon_mutex_is_locked (bson_mutex_t *mutex);
#endif

/**
 * @brief A shared mutex (a read-write lock)
 *
 * A shared mutex can be locked in 'shared' mode or 'exclusive' mode. Only one
 * thread may hold exclusive mode at a time. Any number of threads may hold
 * the lock in shared mode simultaneously. No thread can hold in exclusive mode
 * while another thread holds in shared mode, and vice-versa.
 */
typedef struct bson_shared_mutex_t {
   BSON_IF_WINDOWS (SRWLOCK native;)
   BSON_IF_POSIX (pthread_rwlock_t native;)
} bson_shared_mutex_t;

static BSON_INLINE void
bson_shared_mutex_init (bson_shared_mutex_t *mtx)
{
   BSON_IF_WINDOWS (InitializeSRWLock (&mtx->native));
   BSON_IF_POSIX (BSON_ASSERT (pthread_rwlock_init (&mtx->native, NULL) == 0);)
}

static BSON_INLINE void
bson_shared_mutex_destroy (bson_shared_mutex_t *mtx)
{
   BSON_IF_WINDOWS ((void) mtx;)
   BSON_IF_POSIX (BSON_ASSERT (pthread_rwlock_destroy (&mtx->native) == 0);)
}

static BSON_INLINE void
bson_shared_mutex_lock_shared (bson_shared_mutex_t *mtx)
{
   BSON_IF_WINDOWS (AcquireSRWLockShared (&mtx->native);)
   BSON_IF_POSIX (BSON_ASSERT (pthread_rwlock_rdlock (&mtx->native) == 0);)
}

static BSON_INLINE void
bson_shared_mutex_lock (bson_shared_mutex_t *mtx)
{
   BSON_IF_WINDOWS (AcquireSRWLockExclusive (&mtx->native);)
   BSON_IF_POSIX (BSON_ASSERT (pthread_rwlock_wrlock (&mtx->native) == 0);)
}

static BSON_INLINE void
bson_shared_mutex_unlock (bson_shared_mutex_t *mtx)
{
   BSON_IF_WINDOWS (ReleaseSRWLockExclusive (&mtx->native);)
   BSON_IF_POSIX (BSON_ASSERT (pthread_rwlock_unlock (&mtx->native) == 0);)
}

static BSON_INLINE void
bson_shared_mutex_unlock_shared (bson_shared_mutex_t *mtx)
{
   BSON_IF_WINDOWS (ReleaseSRWLockShared (&mtx->native);)
   BSON_IF_POSIX (BSON_ASSERT (pthread_rwlock_unlock (&mtx->native) == 0);)
}

BSON_END_DECLS

#endif /* COMMON_THREAD_PRIVATE_H */
