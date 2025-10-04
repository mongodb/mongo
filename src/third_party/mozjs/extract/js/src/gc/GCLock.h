/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/*
 * GC-internal classes for acquiring and releasing the GC lock.
 */

#ifndef gc_GCLock_h
#define gc_GCLock_h

#include "vm/Runtime.h"

namespace js {

/*
 * RAII class that takes the GC lock while it is live.
 *
 * Usually functions will pass const references of this class.  However
 * non-const references can be used to either temporarily release the lock by
 * use of AutoUnlockGC or to start background allocation when the lock is
 * released.
 */
class MOZ_RAII AutoLockGC {
 public:
  explicit AutoLockGC(gc::GCRuntime* gc) : gc(gc) { lock(); }
  explicit AutoLockGC(JSRuntime* rt) : AutoLockGC(&rt->gc) {}

  ~AutoLockGC() { lockGuard_.reset(); }

  LockGuard<Mutex>& guard() { return lockGuard_.ref(); }

 protected:
  void lock() {
    MOZ_ASSERT(lockGuard_.isNothing());
    lockGuard_.emplace(gc->lock);
  }

  void unlock() {
    MOZ_ASSERT(lockGuard_.isSome());
    lockGuard_.reset();
  }

  gc::GCRuntime* const gc;

 private:
  mozilla::Maybe<LockGuard<Mutex>> lockGuard_;

  AutoLockGC(const AutoLockGC&) = delete;
  AutoLockGC& operator=(const AutoLockGC&) = delete;

  friend class UnlockGuard<AutoLockGC>;  // For lock/unlock.
};

/*
 * Same as AutoLockGC except it can optionally start a background chunk
 * allocation task when the lock is released.
 */
class MOZ_RAII AutoLockGCBgAlloc : public AutoLockGC {
 public:
  explicit AutoLockGCBgAlloc(gc::GCRuntime* gc) : AutoLockGC(gc) {}
  explicit AutoLockGCBgAlloc(JSRuntime* rt) : AutoLockGCBgAlloc(&rt->gc) {}

  ~AutoLockGCBgAlloc() {
    unlock();

    /*
     * We have to do this after releasing the lock because it may acquire
     * the helper lock which could cause lock inversion if we still held
     * the GC lock.
     */
    if (startBgAlloc) {
      gc->startBackgroundAllocTaskIfIdle();
    }
  }

  /*
   * This can be used to start a background allocation task (if one isn't
   * already running) that allocates chunks and makes them available in the
   * free chunks list.  This happens after the lock is released in order to
   * avoid lock inversion.
   */
  void tryToStartBackgroundAllocation() { startBgAlloc = true; }

 private:
  // true if we should start a background chunk allocation task after the
  // lock is released.
  bool startBgAlloc = false;
};

using AutoUnlockGC = UnlockGuard<AutoLockGC>;

}  // namespace js

#endif /* gc_GCLock_h */
