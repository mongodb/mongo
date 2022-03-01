/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_PlatformMutex_h
#define mozilla_PlatformMutex_h

#include <utility>

#include "mozilla/Attributes.h"
#include "mozilla/Types.h"

#if !defined(XP_WIN) && !defined(__wasi__)
#  include <pthread.h>
#endif

namespace mozilla {

namespace detail {

class ConditionVariableImpl;

class MutexImpl {
 public:
  struct PlatformData;

  explicit MFBT_API MutexImpl();
  MFBT_API ~MutexImpl();

 protected:
  MFBT_API void lock();
  MFBT_API void unlock();
  // We have a separate, forwarding API so internal uses don't have to go
  // through the PLT.
  MFBT_API bool tryLock();

 private:
  MutexImpl(const MutexImpl&) = delete;
  void operator=(const MutexImpl&) = delete;
  MutexImpl(MutexImpl&&) = delete;
  void operator=(MutexImpl&&) = delete;
  bool operator==(const MutexImpl& rhs) = delete;

  void mutexLock();
  bool mutexTryLock();

  PlatformData* platformData();

#if !defined(XP_WIN) && !defined(__wasi__)
  void* platformData_[sizeof(pthread_mutex_t) / sizeof(void*)];
  static_assert(sizeof(pthread_mutex_t) / sizeof(void*) != 0 &&
                    sizeof(pthread_mutex_t) % sizeof(void*) == 0,
                "pthread_mutex_t must have pointer alignment");
#else
  void* platformData_[6];
#endif

  friend class mozilla::detail::ConditionVariableImpl;
};

}  // namespace detail

}  // namespace mozilla

#endif  // mozilla_PlatformMutex_h
