/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef threading_LockGuard_h
#define threading_LockGuard_h

namespace js {

template <typename Mutex> class MOZ_RAII UnlockGuard;

template <typename Mutex>
class MOZ_RAII LockGuard
{
  friend class UnlockGuard<Mutex>;
  friend class ConditionVariable;
  Mutex& lock;

public:
  explicit LockGuard(Mutex& aLock)
    : lock(aLock)
  {
    lock.lock();
  }

  ~LockGuard() {
    lock.unlock();
  }
};

template <typename Mutex>
class MOZ_RAII UnlockGuard
{
  Mutex& lock;

public:
  explicit UnlockGuard(LockGuard<Mutex>& aGuard)
    : lock(aGuard.lock)
  {
    lock.unlock();
  }

  ~UnlockGuard() {
    lock.lock();
  }
};

} // namespace js

#endif // threading_LockGuard_h
