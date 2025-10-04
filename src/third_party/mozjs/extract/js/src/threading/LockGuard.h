/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef threading_LockGuard_h
#define threading_LockGuard_h

#include "mozilla/Attributes.h"

namespace js {

template <typename GuardT>
class MOZ_RAII UnlockGuard;

template <typename Mutex>
class MOZ_RAII LockGuard {
  friend class UnlockGuard<LockGuard<Mutex>>;
  friend class ConditionVariable;
  Mutex& mutex;

 public:
  explicit LockGuard(Mutex& mutex) : mutex(mutex) { lock(); }
  ~LockGuard() { unlock(); }

  LockGuard(const LockGuard& other) = delete;

 protected:
  void lock() { mutex.lock(); }
  void unlock() { mutex.unlock(); }
};

// RAII class to temporarily unlock a LockGuard.
template <typename GuardT>
class MOZ_RAII UnlockGuard {
  GuardT& guard;

 public:
  explicit UnlockGuard(GuardT& guard) : guard(guard) { guard.unlock(); }
  ~UnlockGuard() { guard.lock(); }

  UnlockGuard(const UnlockGuard& other) = delete;
};

}  // namespace js

#endif  // threading_LockGuard_h
