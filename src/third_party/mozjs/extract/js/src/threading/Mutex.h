/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef threading_Mutex_h
#define threading_Mutex_h

#include "mozilla/Assertions.h"
#include "mozilla/Move.h"
#include "mozilla/PlatformMutex.h"
#include "mozilla/ThreadLocal.h"
#include "mozilla/Vector.h"

namespace js {

// A MutexId secifies the name and mutex order for a mutex.
//
// The mutex order defines the allowed order of mutex acqusition on a single
// thread. Mutexes must be acquired in strictly increasing order. Mutexes with
// the same order may not be held at the same time by that thread.
struct MutexId
{
  const char* name;
  uint32_t order;
};

#ifndef DEBUG

class Mutex : public mozilla::detail::MutexImpl
{
public:
  static bool Init() { return true; }
  static void ShutDown() {}

  explicit Mutex(const MutexId& id) {}

  using MutexImpl::lock;
  using MutexImpl::unlock;
};

#else

// In debug builds, js::Mutex is a wrapper over MutexImpl that checks correct
// locking order is observed.
//
// The class maintains a per-thread stack of currently-held mutexes to enable it
// to check this.
class Mutex : public mozilla::detail::MutexImpl
{
public:
  static bool Init();
  static void ShutDown();

  explicit Mutex(const MutexId& id)
   : id_(id)
  {
    MOZ_ASSERT(id_.order != 0);
  }

  void lock();
  void unlock();
  bool ownedByCurrentThread() const;

private:
  const MutexId id_;

  using MutexVector = mozilla::Vector<const Mutex*>;
  static MOZ_THREAD_LOCAL(MutexVector*) HeldMutexStack;
  static MutexVector& heldMutexStack();
};

#endif

} // namespace js

#endif // threading_Mutex_h
