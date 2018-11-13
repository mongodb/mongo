// Copyright 2017 The Abseil Authors.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "absl/synchronization/internal/waiter.h"

#include "absl/base/config.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#include <sys/time.h>
#include <unistd.h>
#endif

#ifdef __linux__
#include <linux/futex.h>
#include <sys/syscall.h>
#endif

#ifdef ABSL_HAVE_SEMAPHORE_H
#include <semaphore.h>
#endif

#include <errno.h>
#include <stdio.h>
#include <time.h>

#include <atomic>
#include <cassert>
#include <cstdint>
#include "absl/base/internal/raw_logging.h"
#include "absl/base/internal/thread_identity.h"
#include "absl/base/optimization.h"
#include "absl/synchronization/internal/kernel_timeout.h"

namespace absl {
namespace synchronization_internal {

static void MaybeBecomeIdle() {
  base_internal::ThreadIdentity *identity =
      base_internal::CurrentThreadIdentityIfPresent();
  assert(identity != nullptr);
  const bool is_idle = identity->is_idle.load(std::memory_order_relaxed);
  const int ticker = identity->ticker.load(std::memory_order_relaxed);
  const int wait_start = identity->wait_start.load(std::memory_order_relaxed);
  if (!is_idle && ticker - wait_start > Waiter::kIdlePeriods) {
    identity->is_idle.store(true, std::memory_order_relaxed);
  }
}

#if ABSL_WAITER_MODE == ABSL_WAITER_MODE_FUTEX

// Some Android headers are missing these definitions even though they
// support these futex operations.
#ifdef __BIONIC__
#ifndef SYS_futex
#define SYS_futex __NR_futex
#endif
#ifndef FUTEX_WAIT_BITSET
#define FUTEX_WAIT_BITSET 9
#endif
#ifndef FUTEX_PRIVATE_FLAG
#define FUTEX_PRIVATE_FLAG 128
#endif
#ifndef FUTEX_CLOCK_REALTIME
#define FUTEX_CLOCK_REALTIME 256
#endif
#ifndef FUTEX_BITSET_MATCH_ANY
#define FUTEX_BITSET_MATCH_ANY 0xFFFFFFFF
#endif
#endif
class Futex {
 public:
  static int WaitUntil(std::atomic<int32_t> *v, int32_t val,
                       KernelTimeout t) {
    int err = 0;
    if (t.has_timeout()) {
      // https://locklessinc.com/articles/futex_cheat_sheet/
      // Unlike FUTEX_WAIT, FUTEX_WAIT_BITSET uses absolute time.
      struct timespec abs_timeout = t.MakeAbsTimespec();
      // Atomically check that the futex value is still 0, and if it
      // is, sleep until abs_timeout or until woken by FUTEX_WAKE.
      err = syscall(
          SYS_futex, reinterpret_cast<int32_t *>(v),
          FUTEX_WAIT_BITSET | FUTEX_PRIVATE_FLAG | FUTEX_CLOCK_REALTIME, val,
          &abs_timeout, nullptr, FUTEX_BITSET_MATCH_ANY);
    } else {
      // Atomically check that the futex value is still 0, and if it
      // is, sleep until woken by FUTEX_WAKE.
      err = syscall(SYS_futex, reinterpret_cast<int32_t *>(v),
                    FUTEX_WAIT | FUTEX_PRIVATE_FLAG, val, nullptr);
    }
    if (err != 0) {
      err = -errno;
    }
    return err;
  }

  static int Wake(std::atomic<int32_t> *v, int32_t count) {
    int err = syscall(SYS_futex, reinterpret_cast<int32_t *>(v),
                      FUTEX_WAKE | FUTEX_PRIVATE_FLAG, count);
    if (ABSL_PREDICT_FALSE(err < 0)) {
      err = -errno;
    }
    return err;
  }
};

void Waiter::Init() {
  futex_.store(0, std::memory_order_relaxed);
}

bool Waiter::Wait(KernelTimeout t) {
  // Loop until we can atomically decrement futex from a positive
  // value, waiting on a futex while we believe it is zero.
  while (true) {
    int32_t x = futex_.load(std::memory_order_relaxed);
    if (x != 0) {
      if (!futex_.compare_exchange_weak(x, x - 1,
                                        std::memory_order_acquire,
                                        std::memory_order_relaxed)) {
        continue;  // Raced with someone, retry.
      }
      return true;  // Consumed a wakeup, we are done.
    }

    const int err = Futex::WaitUntil(&futex_, 0, t);
    if (err != 0) {
      if (err == -EINTR || err == -EWOULDBLOCK) {
        // Do nothing, the loop will retry.
      } else if (err == -ETIMEDOUT) {
        return false;
      } else {
        ABSL_RAW_LOG(FATAL, "Futex operation failed with error %d\n", err);
      }
    }

    MaybeBecomeIdle();
  }
}

void Waiter::Post() {
  if (futex_.fetch_add(1, std::memory_order_release) == 0) {
    // We incremented from 0, need to wake a potential waker.
    Poke();
  }
}

void Waiter::Poke() {
  // Wake one thread waiting on the futex.
  const int err = Futex::Wake(&futex_, 1);
  if (ABSL_PREDICT_FALSE(err < 0)) {
    ABSL_RAW_LOG(FATAL, "Futex operation failed with error %d\n", err);
  }
}

#elif ABSL_WAITER_MODE == ABSL_WAITER_MODE_CONDVAR

class PthreadMutexHolder {
 public:
  explicit PthreadMutexHolder(pthread_mutex_t *mu) : mu_(mu) {
    const int err = pthread_mutex_lock(mu_);
    if (err != 0) {
      ABSL_RAW_LOG(FATAL, "pthread_mutex_lock failed: %d", err);
    }
  }

  PthreadMutexHolder(const PthreadMutexHolder &rhs) = delete;
  PthreadMutexHolder &operator=(const PthreadMutexHolder &rhs) = delete;

  ~PthreadMutexHolder() {
    const int err = pthread_mutex_unlock(mu_);
    if (err != 0) {
      ABSL_RAW_LOG(FATAL, "pthread_mutex_unlock failed: %d", err);
    }
  }

 private:
  pthread_mutex_t *mu_;
};

void Waiter::Init() {
  const int err = pthread_mutex_init(&mu_, 0);
  if (err != 0) {
    ABSL_RAW_LOG(FATAL, "pthread_mutex_init failed: %d", err);
  }

  const int err2 = pthread_cond_init(&cv_, 0);
  if (err2 != 0) {
    ABSL_RAW_LOG(FATAL, "pthread_cond_init failed: %d", err2);
  }

  waiter_count_.store(0, std::memory_order_relaxed);
  wakeup_count_.store(0, std::memory_order_relaxed);
}

bool Waiter::Wait(KernelTimeout t) {
  struct timespec abs_timeout;
  if (t.has_timeout()) {
    abs_timeout = t.MakeAbsTimespec();
  }

  PthreadMutexHolder h(&mu_);
  waiter_count_.fetch_add(1, std::memory_order_relaxed);
  // Loop until we find a wakeup to consume or timeout.
  while (true) {
    int x = wakeup_count_.load(std::memory_order_relaxed);
    if (x != 0) {
      if (!wakeup_count_.compare_exchange_weak(x, x - 1,
                                               std::memory_order_acquire,
                                               std::memory_order_relaxed)) {
        continue;  // Raced with someone, retry.
      }
      // Successfully consumed a wakeup, we're done.
      waiter_count_.fetch_sub(1, std::memory_order_relaxed);
      return true;
    }

    // No wakeups available, time to wait.
    if (!t.has_timeout()) {
      const int err = pthread_cond_wait(&cv_, &mu_);
      if (err != 0) {
        ABSL_RAW_LOG(FATAL, "pthread_cond_wait failed: %d", err);
      }
    } else {
      const int err = pthread_cond_timedwait(&cv_, &mu_, &abs_timeout);
      if (err == ETIMEDOUT) {
        waiter_count_.fetch_sub(1, std::memory_order_relaxed);
        return false;
      }
      if (err != 0) {
        ABSL_RAW_LOG(FATAL, "pthread_cond_wait failed: %d", err);
      }
    }
    MaybeBecomeIdle();
  }
}

void Waiter::Post() {
  wakeup_count_.fetch_add(1, std::memory_order_release);
  Poke();
}

void Waiter::Poke() {
  if (waiter_count_.load(std::memory_order_relaxed) == 0) {
    return;
  }
  // Potentially a waker. Take the lock and check again.
  PthreadMutexHolder h(&mu_);
  if (waiter_count_.load(std::memory_order_relaxed) == 0) {
    return;
  }
  const int err = pthread_cond_signal(&cv_);
  if (err != 0) {
    ABSL_RAW_LOG(FATAL, "pthread_cond_signal failed: %d", err);
  }
}

#elif ABSL_WAITER_MODE == ABSL_WAITER_MODE_SEM

void Waiter::Init() {
  if (sem_init(&sem_, 0, 0) != 0) {
    ABSL_RAW_LOG(FATAL, "sem_init failed with errno %d\n", errno);
  }
  wakeups_.store(0, std::memory_order_relaxed);
}

bool Waiter::Wait(KernelTimeout t) {
  struct timespec abs_timeout;
  if (t.has_timeout()) {
    abs_timeout = t.MakeAbsTimespec();
  }

  // Loop until we timeout or consume a wakeup.
  while (true) {
    int x = wakeups_.load(std::memory_order_relaxed);
    if (x != 0) {
      if (!wakeups_.compare_exchange_weak(x, x - 1,
                                          std::memory_order_acquire,
                                          std::memory_order_relaxed)) {
        continue;  // Raced with someone, retry.
      }
      // Successfully consumed a wakeup, we're done.
      return true;
    }

    // Nothing to consume, wait (looping on EINTR).
    while (true) {
      if (!t.has_timeout()) {
        if (sem_wait(&sem_) == 0) break;
        if (errno == EINTR) continue;
        ABSL_RAW_LOG(FATAL, "sem_wait failed: %d", errno);
      } else {
        if (sem_timedwait(&sem_, &abs_timeout) == 0) break;
        if (errno == EINTR) continue;
        if (errno == ETIMEDOUT) return false;
        ABSL_RAW_LOG(FATAL, "sem_timedwait failed: %d", errno);
      }
    }
    MaybeBecomeIdle();
  }
}

void Waiter::Post() {
  wakeups_.fetch_add(1, std::memory_order_release);  // Post a wakeup.
  Poke();
}

void Waiter::Poke() {
  if (sem_post(&sem_) != 0) {  // Wake any semaphore waiter.
    ABSL_RAW_LOG(FATAL, "sem_post failed with errno %d\n", errno);
  }
}

#elif ABSL_WAITER_MODE == ABSL_WAITER_MODE_WIN32

class LockHolder {
 public:
  explicit LockHolder(SRWLOCK* mu) : mu_(mu) {
    AcquireSRWLockExclusive(mu_);
  }

  LockHolder(const LockHolder&) = delete;
  LockHolder& operator=(const LockHolder&) = delete;

  ~LockHolder() {
    ReleaseSRWLockExclusive(mu_);
  }

 private:
  SRWLOCK* mu_;
};

void Waiter::Init() {
  InitializeSRWLock(&mu_);
  InitializeConditionVariable(&cv_);
  waiter_count_.store(0, std::memory_order_relaxed);
  wakeup_count_.store(0, std::memory_order_relaxed);
}

bool Waiter::Wait(KernelTimeout t) {
  LockHolder h(&mu_);
  waiter_count_.fetch_add(1, std::memory_order_relaxed);

  // Loop until we find a wakeup to consume or timeout.
  while (true) {
    int x = wakeup_count_.load(std::memory_order_relaxed);
    if (x != 0) {
      if (!wakeup_count_.compare_exchange_weak(x, x - 1,
                                               std::memory_order_acquire,
                                               std::memory_order_relaxed)) {
        continue;  // Raced with someone, retry.
      }
      // Successfully consumed a wakeup, we're done.
      waiter_count_.fetch_sub(1, std::memory_order_relaxed);
      return true;
    }

    // No wakeups available, time to wait.
    if (!SleepConditionVariableSRW(
            &cv_, &mu_, t.InMillisecondsFromNow(), 0)) {
      // GetLastError() returns a Win32 DWORD, but we assign to
      // unsigned long to simplify the ABSL_RAW_LOG case below.  The uniform
      // initialization guarantees this is not a narrowing conversion.
      const unsigned long err{GetLastError()};  // NOLINT(runtime/int)
      if (err == ERROR_TIMEOUT) {
        waiter_count_.fetch_sub(1, std::memory_order_relaxed);
        return false;
      } else {
        ABSL_RAW_LOG(FATAL, "SleepConditionVariableSRW failed: %lu", err);
      }
    }

    MaybeBecomeIdle();
  }
}

void Waiter::Post() {
  wakeup_count_.fetch_add(1, std::memory_order_release);
  Poke();
}

void Waiter::Poke() {
  if (waiter_count_.load(std::memory_order_relaxed) == 0) {
    return;
  }
  // Potentially a waker. Take the lock and check again.
  LockHolder h(&mu_);
  if (waiter_count_.load(std::memory_order_relaxed) == 0) {
    return;
  }
  WakeConditionVariable(&cv_);
}

#else
#error Unknown ABSL_WAITER_MODE
#endif

}  // namespace synchronization_internal
}  // namespace absl
