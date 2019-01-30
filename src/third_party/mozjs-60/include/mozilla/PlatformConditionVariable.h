/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_ConditionVariable_h
#define mozilla_ConditionVariable_h

#include "mozilla/Attributes.h"
#include "mozilla/Move.h"
#include "mozilla/PlatformMutex.h"
#include "mozilla/TimeStamp.h"

#include <stdint.h>
#ifndef XP_WIN
# include <pthread.h>
#endif

namespace mozilla {

namespace detail {

enum class CVStatus {
  NoTimeout,
  Timeout
};

class ConditionVariableImpl {
public:
  struct PlatformData;

  MFBT_API ConditionVariableImpl();
  MFBT_API ~ConditionVariableImpl();

  // Wake one thread that is waiting on this condition.
  MFBT_API void notify_one();

  // Wake all threads that are waiting on this condition.
  MFBT_API void notify_all();

  // Block the current thread of execution until this condition variable is
  // woken from another thread via notify_one or notify_all.
  MFBT_API void wait(MutexImpl& lock);

  MFBT_API CVStatus wait_for(MutexImpl& lock,
                             const mozilla::TimeDuration& rel_time);

private:
  ConditionVariableImpl(const ConditionVariableImpl&) = delete;
  ConditionVariableImpl& operator=(const ConditionVariableImpl&) = delete;

  PlatformData* platformData();

#ifndef XP_WIN
  void* platformData_[sizeof(pthread_cond_t) / sizeof(void*)];
  static_assert(sizeof(pthread_cond_t) / sizeof(void*) != 0 &&
                sizeof(pthread_cond_t) % sizeof(void*) == 0,
                "pthread_cond_t must have pointer alignment");
#else
  void* platformData_[4];
#endif
};

} // namespace detail

} // namespace mozilla

#endif // mozilla_ConditionVariable_h
