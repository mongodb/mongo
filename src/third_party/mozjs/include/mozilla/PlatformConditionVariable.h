/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_ConditionVariable_h
#define mozilla_ConditionVariable_h

#include <stdint.h>

#include <utility>

#include "mozilla/Attributes.h"
#include "mozilla/PlatformMutex.h"
#include "mozilla/TimeStamp.h"
#if !defined(XP_WIN) && !defined(__wasi__)
#  include <pthread.h>
#endif

namespace mozilla {

enum class CVStatus { NoTimeout, Timeout };

namespace detail {

class ConditionVariableImpl {
 public:
  struct PlatformData;

  MFBT_API ConditionVariableImpl();
  MFBT_API ~ConditionVariableImpl();

  // Wake one thread that is waiting on this condition.
  MFBT_API void notify_one();

  // Wake all threads that are waiting on this condition.
  MFBT_API void notify_all();

  // Atomically release |lock| and sleep the current thread of execution on
  // this condition variable.
  // |lock| will be re-acquired before this function returns.
  // The thread may be woken from sleep from another thread via notify_one()
  // or notify_all(), but may also wake spuriously.  The caller should recheck
  // its predicate after this function returns, typically in a while loop.
  MFBT_API void wait(MutexImpl& lock);

  MFBT_API CVStatus wait_for(MutexImpl& lock,
                             const mozilla::TimeDuration& rel_time);

 private:
  ConditionVariableImpl(const ConditionVariableImpl&) = delete;
  ConditionVariableImpl& operator=(const ConditionVariableImpl&) = delete;

  PlatformData* platformData();

#if !defined(XP_WIN) && !defined(__wasi__)
  void* platformData_[sizeof(pthread_cond_t) / sizeof(void*)];
  static_assert(sizeof(pthread_cond_t) / sizeof(void*) != 0 &&
                    sizeof(pthread_cond_t) % sizeof(void*) == 0,
                "pthread_cond_t must have pointer alignment");
#else
  void* platformData_[4];
#endif
};

}  // namespace detail

}  // namespace mozilla

#endif  // mozilla_ConditionVariable_h
