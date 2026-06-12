/*
 * Copyright 2009-present MongoDB, Inc.
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


#include <common-atomic-private.h>

#ifdef BSON_OS_UNIX
/* For sched_yield() */
#include <sched.h>
#endif

void
mcommon_thrd_yield(void)
{
   BSON_IF_WINDOWS(SwitchToThread();)
   BSON_IF_POSIX(sched_yield();)
}

/**
 * Some platforms do not support compiler intrinsics for atomic operations.
 * We emulate that here using a spin lock and regular arithmetic operations
 */
static int8_t gEmulAtomicLock = 0;

static void
_lock_emul_atomic(void)
{
   int i;
   if (mcommon_atomic_int8_compare_exchange_weak(&gEmulAtomicLock, 0, 1, mcommon_memory_order_acquire) == 0) {
      /* Successfully took the spinlock */
      return;
   }
   /* Failed. Try taking ten more times, then begin sleeping. */
   for (i = 0; i < 10; ++i) {
      if (mcommon_atomic_int8_compare_exchange_weak(&gEmulAtomicLock, 0, 1, mcommon_memory_order_acquire) == 0) {
         /* Succeeded in taking the lock */
         return;
      }
   }
   /* Still don't have the lock. Spin and yield */
   while (mcommon_atomic_int8_compare_exchange_weak(&gEmulAtomicLock, 0, 1, mcommon_memory_order_acquire) != 0) {
      mcommon_thrd_yield();
   }
}

static void
_unlock_emul_atomic(void)
{
   int64_t rv = mcommon_atomic_int8_exchange(&gEmulAtomicLock, 0, mcommon_memory_order_release);
   BSON_ASSERT(rv == 1 && "Released atomic lock while not holding it");
}

int64_t
_mcommon_emul_atomic_int64_fetch_add(volatile int64_t *p, int64_t n, enum mcommon_memory_order _unused)
{
   int64_t ret;

   BSON_UNUSED(_unused);

   _lock_emul_atomic();
   ret = *p;
   *p += n;
   _unlock_emul_atomic();
   return ret;
}

int64_t
_mcommon_emul_atomic_int64_exchange(volatile int64_t *p, int64_t n, enum mcommon_memory_order _unused)
{
   int64_t ret;

   BSON_UNUSED(_unused);

   _lock_emul_atomic();
   ret = *p;
   *p = n;
   _unlock_emul_atomic();
   return ret;
}

int64_t
_mcommon_emul_atomic_int64_compare_exchange_strong(volatile int64_t *p,
                                                   int64_t expect_value,
                                                   int64_t new_value,
                                                   enum mcommon_memory_order _unused)
{
   int64_t ret;

   BSON_UNUSED(_unused);

   _lock_emul_atomic();
   ret = *p;
   if (ret == expect_value) {
      *p = new_value;
   }
   _unlock_emul_atomic();
   return ret;
}

int64_t
_mcommon_emul_atomic_int64_compare_exchange_weak(volatile int64_t *p,
                                                 int64_t expect_value,
                                                 int64_t new_value,
                                                 enum mcommon_memory_order order)
{
   /* We're emulating. We can't do a weak version. */
   return _mcommon_emul_atomic_int64_compare_exchange_strong(p, expect_value, new_value, order);
}


int32_t
_mcommon_emul_atomic_int32_fetch_add(volatile int32_t *p, int32_t n, enum mcommon_memory_order _unused)
{
   int32_t ret;

   BSON_UNUSED(_unused);

   _lock_emul_atomic();
   ret = *p;
   *p += n;
   _unlock_emul_atomic();
   return ret;
}

int32_t
_mcommon_emul_atomic_int32_exchange(volatile int32_t *p, int32_t n, enum mcommon_memory_order _unused)
{
   int32_t ret;

   BSON_UNUSED(_unused);

   _lock_emul_atomic();
   ret = *p;
   *p = n;
   _unlock_emul_atomic();
   return ret;
}

int32_t
_mcommon_emul_atomic_int32_compare_exchange_strong(volatile int32_t *p,
                                                   int32_t expect_value,
                                                   int32_t new_value,
                                                   enum mcommon_memory_order _unused)
{
   int32_t ret;

   BSON_UNUSED(_unused);

   _lock_emul_atomic();
   ret = *p;
   if (ret == expect_value) {
      *p = new_value;
   }
   _unlock_emul_atomic();
   return ret;
}

int32_t
_mcommon_emul_atomic_int32_compare_exchange_weak(volatile int32_t *p,
                                                 int32_t expect_value,
                                                 int32_t new_value,
                                                 enum mcommon_memory_order order)
{
   /* We're emulating. We can't do a weak version. */
   return _mcommon_emul_atomic_int32_compare_exchange_strong(p, expect_value, new_value, order);
}


int
_mcommon_emul_atomic_int_fetch_add(volatile int *p, int n, enum mcommon_memory_order _unused)
{
   int ret;

   BSON_UNUSED(_unused);

   _lock_emul_atomic();
   ret = *p;
   *p += n;
   _unlock_emul_atomic();
   return ret;
}

int
_mcommon_emul_atomic_int_exchange(volatile int *p, int n, enum mcommon_memory_order _unused)
{
   int ret;

   BSON_UNUSED(_unused);

   _lock_emul_atomic();
   ret = *p;
   *p = n;
   _unlock_emul_atomic();
   return ret;
}

int
_mcommon_emul_atomic_int_compare_exchange_strong(volatile int *p,
                                                 int expect_value,
                                                 int new_value,
                                                 enum mcommon_memory_order _unused)
{
   int ret;

   BSON_UNUSED(_unused);

   _lock_emul_atomic();
   ret = *p;
   if (ret == expect_value) {
      *p = new_value;
   }
   _unlock_emul_atomic();
   return ret;
}

int
_mcommon_emul_atomic_int_compare_exchange_weak(volatile int *p,
                                               int expect_value,
                                               int new_value,
                                               enum mcommon_memory_order order)
{
   /* We're emulating. We can't do a weak version. */
   return _mcommon_emul_atomic_int_compare_exchange_strong(p, expect_value, new_value, order);
}

void *
_mcommon_emul_atomic_ptr_exchange(void *volatile *p, void *n, enum mcommon_memory_order _unused)
{
   void *ret;

   BSON_UNUSED(_unused);

   _lock_emul_atomic();
   ret = *p;
   *p = n;
   _unlock_emul_atomic();
   return ret;
}
