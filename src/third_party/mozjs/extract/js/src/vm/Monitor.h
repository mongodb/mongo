/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef vm_Monitor_h
#define vm_Monitor_h

#include "threading/ConditionVariable.h"
#include "threading/Mutex.h"

namespace js {

// A base class used for types intended to be used in a parallel
// fashion.  Combines a lock and a condition variable.  You can
// acquire the lock or signal the condition variable using the
// |AutoLockMonitor| type.

class Monitor {
 protected:
  friend class AutoLockMonitor;
  friend class AutoUnlockMonitor;

  Mutex lock_ MOZ_UNANNOTATED;
  ConditionVariable condVar_;

 public:
  explicit Monitor(const MutexId& id) : lock_(id) {}
};

class AutoLockMonitor : public LockGuard<Mutex> {
 private:
  using Base = LockGuard<Mutex>;
  Monitor& monitor;

 public:
  explicit AutoLockMonitor(Monitor& monitor)
      : Base(monitor.lock_), monitor(monitor) {}

  bool isFor(Monitor& other) const { return &monitor.lock_ == &other.lock_; }

  void wait(ConditionVariable& condVar) { condVar.wait(*this); }

  void wait() { wait(monitor.condVar_); }

  void notify(ConditionVariable& condVar) { condVar.notify_one(); }

  void notify() { notify(monitor.condVar_); }

  void notifyAll(ConditionVariable& condVar) { condVar.notify_all(); }

  void notifyAll() { notifyAll(monitor.condVar_); }
};

class AutoUnlockMonitor {
 private:
  Monitor& monitor;

 public:
  explicit AutoUnlockMonitor(Monitor& monitor) : monitor(monitor) {
    monitor.lock_.unlock();
  }

  ~AutoUnlockMonitor() { monitor.lock_.lock(); }

  bool isFor(Monitor& other) const { return &monitor.lock_ == &other.lock_; }
};

}  // namespace js

#endif /* vm_Monitor_h */
