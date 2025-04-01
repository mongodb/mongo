/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef threading_Mutex_h
#define threading_Mutex_h

#include "mozilla/Assertions.h"
#include "mozilla/Maybe.h"
#include "mozilla/PlatformMutex.h"
#include "mozilla/ThreadLocal.h"
#include "mozilla/Vector.h"

#include <utility>

#include "threading/ThreadId.h"

namespace js {

// A MutexId secifies the name and mutex order for a mutex.
//
// The mutex order defines the allowed order of mutex acqusition on a single
// thread. Mutexes must be acquired in strictly increasing order. Mutexes with
// the same order may not be held at the same time by that thread.
struct MutexId {
  const char* name;
  uint32_t order;
};

// The Mutex class below wraps mozilla::detail::MutexImpl, but we don't want to
// use public inheritance, and private inheritance is problematic because
// Mutex's friends can access the private parent class as if it was public
// inheritance.  So use a data member, but for Mutex to access the data member
// we must override it and make Mutex a friend.
class MutexImpl : public mozilla::detail::MutexImpl {
 protected:
  MutexImpl() {}

  friend class Mutex;
};

// In debug builds, js::Mutex is a wrapper over MutexImpl that checks correct
// locking order is observed.
//
// The class maintains a per-thread stack of currently-held mutexes to enable it
// to check this.
class Mutex {
 private:
  MutexImpl impl_;

#ifdef DEBUG
  const MutexId id_;
  Mutex* prev_ = nullptr;
  ThreadId owningThread_;

  static MOZ_THREAD_LOCAL(Mutex*) HeldMutexStack;
#endif

 public:
#ifdef DEBUG
  static bool Init();

  explicit Mutex(const MutexId& id) : id_(id) { MOZ_ASSERT(id_.order != 0); }

  void lock();
  bool tryLock();
  void unlock();
  bool isOwnedByCurrentThread() const;
  void assertOwnedByCurrentThread() const;
#else
  static bool Init() { return true; }

  explicit Mutex(const MutexId& id) {}

  void lock() { impl_.lock(); }
  bool tryLock() { return impl_.tryLock(); }
  void unlock() { impl_.unlock(); }
  void assertOwnedByCurrentThread() const {};
#endif

 private:
#ifdef DEBUG
  void preLockChecks() const;
  void postLockChecks();
  void preUnlockChecks();
#endif

  friend class ConditionVariable;
};

}  // namespace js

#endif  // threading_Mutex_h
