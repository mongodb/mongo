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
//

#ifndef ABSL_SYNCHRONIZATION_INTERNAL_WAITER_H_
#define ABSL_SYNCHRONIZATION_INTERNAL_WAITER_H_

#include "absl/base/config.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#endif

#ifdef ABSL_HAVE_SEMAPHORE_H
#include <semaphore.h>
#endif

#include <atomic>
#include <cstdint>

#include "absl/base/internal/thread_identity.h"
#include "absl/synchronization/internal/kernel_timeout.h"

// May be chosen at compile time via -DABSL_FORCE_WAITER_MODE=<index>
#define ABSL_WAITER_MODE_FUTEX 0
#define ABSL_WAITER_MODE_SEM 1
#define ABSL_WAITER_MODE_CONDVAR 2
#define ABSL_WAITER_MODE_WIN32 3

#if defined(ABSL_FORCE_WAITER_MODE)
#define ABSL_WAITER_MODE ABSL_FORCE_WAITER_MODE
#elif defined(_WIN32)
#define ABSL_WAITER_MODE ABSL_WAITER_MODE_WIN32
#elif defined(__linux__)
#define ABSL_WAITER_MODE ABSL_WAITER_MODE_FUTEX
#elif defined(ABSL_HAVE_SEMAPHORE_H)
#define ABSL_WAITER_MODE ABSL_WAITER_MODE_SEM
#else
#define ABSL_WAITER_MODE ABSL_WAITER_MODE_CONDVAR
#endif

namespace absl {
namespace synchronization_internal {

// Waiter is an OS-specific semaphore.
class Waiter {
 public:
  // No constructor, instances use the reserved space in ThreadIdentity.
  // All initialization logic belongs in `Init()`.
  Waiter() = delete;
  Waiter(const Waiter&) = delete;
  Waiter& operator=(const Waiter&) = delete;

  // Prepare any data to track waits.
  void Init();

  // Blocks the calling thread until a matching call to `Post()` or
  // `t` has passed. Returns `true` if woken (`Post()` called),
  // `false` on timeout.
  bool Wait(KernelTimeout t);

  // Restart the caller of `Wait()` as with a normal semaphore.
  void Post();

  // If anyone is waiting, wake them up temporarily and cause them to
  // call `MaybeBecomeIdle()`. They will then return to waiting for a
  // `Post()` or timeout.
  void Poke();

  // Returns the Waiter associated with the identity.
  static Waiter* GetWaiter(base_internal::ThreadIdentity* identity) {
    static_assert(
        sizeof(Waiter) <= sizeof(base_internal::ThreadIdentity::WaiterState),
        "Insufficient space for Waiter");
    return reinterpret_cast<Waiter*>(identity->waiter_state.data);
  }

  // How many periods to remain idle before releasing resources
#ifndef THREAD_SANITIZER
  static const int kIdlePeriods = 60;
#else
  // Memory consumption under ThreadSanitizer is a serious concern,
  // so we release resources sooner. The value of 1 leads to 1 to 2 second
  // delay before marking a thread as idle.
  static const int kIdlePeriods = 1;
#endif

 private:
#if ABSL_WAITER_MODE == ABSL_WAITER_MODE_FUTEX
  // Futexes are defined by specification to be 32-bits.
  // Thus std::atomic<int32_t> must be just an int32_t with lockfree methods.
  std::atomic<int32_t> futex_;
  static_assert(sizeof(int32_t) == sizeof(futex_), "Wrong size for futex");

#elif ABSL_WAITER_MODE == ABSL_WAITER_MODE_CONDVAR
  pthread_mutex_t mu_;
  pthread_cond_t cv_;
  std::atomic<int> waiter_count_;
  std::atomic<int> wakeup_count_;  // Unclaimed wakeups, written under lock.

#elif ABSL_WAITER_MODE == ABSL_WAITER_MODE_SEM
  sem_t sem_;
  // This seems superfluous, but for Poke() we need to cause spurious
  // wakeups on the semaphore. Hence we can't actually use the
  // semaphore's count.
  std::atomic<int> wakeups_;

#elif ABSL_WAITER_MODE == ABSL_WAITER_MODE_WIN32
  // The Windows API has lots of choices for synchronization
  // primivitives.  We are using SRWLOCK and CONDITION_VARIABLE
  // because they don't require a destructor to release system
  // resources.
  SRWLOCK mu_;
  CONDITION_VARIABLE cv_;
  std::atomic<int> waiter_count_;
  std::atomic<int> wakeup_count_;

#else
  #error Unknown ABSL_WAITER_MODE
#endif
};

}  // namespace synchronization_internal
}  // namespace absl

#endif  // ABSL_SYNCHRONIZATION_INTERNAL_WAITER_H_
