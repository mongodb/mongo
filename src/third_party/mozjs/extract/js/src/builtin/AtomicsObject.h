/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef builtin_AtomicsObject_h
#define builtin_AtomicsObject_h

#include "mozilla/Maybe.h"
#include "mozilla/TimeStamp.h"

#include "threading/ConditionVariable.h"
#include "threading/ProtectedData.h"  // js::ThreadData
#include "vm/NativeObject.h"

namespace js {

class SharedArrayRawBuffer;

class AtomicsObject : public NativeObject {
 public:
  static const JSClass class_;
};

class FutexThread {
  friend class AutoLockFutexAPI;

 public:
  [[nodiscard]] static bool initialize();
  static void destroy();

  static void lock();
  static void unlock();

  FutexThread();
  [[nodiscard]] bool initInstance();
  void destroyInstance();

  // Parameters to notify().
  enum NotifyReason {
    NotifyExplicit,       // Being asked to wake up by another thread
    NotifyForJSInterrupt  // Interrupt requested
  };

  // Result codes from wait() and atomics_wait_impl().
  enum class WaitResult {
    Error,     // Error has been reported, just propagate error signal
    NotEqual,  // Did not wait because the values differed
    OK,        // Waited and was woken
    TimedOut   // Waited and timed out
  };

  // Block the calling thread and wait.
  //
  // The futex lock must be held around this call.
  //
  // The timeout is the number of milliseconds, with fractional
  // times allowed; specify mozilla::Nothing() for an indefinite
  // wait.
  //
  // wait() will not wake up spuriously.
  [[nodiscard]] WaitResult wait(
      JSContext* cx, js::UniqueLock<js::Mutex>& locked,
      const mozilla::Maybe<mozilla::TimeDuration>& timeout);

  // Notify the thread this is associated with.
  //
  // The futex lock must be held around this call.  (The sleeping
  // thread will not wake up until the caller of Atomics.notify()
  // releases the lock.)
  //
  // If the thread is not waiting then this method does nothing.
  //
  // If the thread is waiting in a call to wait() and the
  // reason is NotifyExplicit then the wait() call will return
  // with Woken.
  //
  // If the thread is waiting in a call to wait() and the
  // reason is NotifyForJSInterrupt then the wait() will return
  // with WaitingNotifiedForInterrupt; in the latter case the caller
  // of wait() must handle the interrupt.
  void notify(NotifyReason reason);

  bool isWaiting();

  // If canWait() returns false (the default) then wait() is disabled
  // on the thread to which the FutexThread belongs.
  bool canWait() { return canWait_; }

  void setCanWait(bool flag) { canWait_ = flag; }

 private:
  enum FutexState {
    Idle,                         // We are not waiting or woken
    Waiting,                      // We are waiting, nothing has happened yet
    WaitingNotifiedForInterrupt,  // We are waiting, but have been interrupted,
                                  //   and have not yet started running the
                                  //   interrupt handler
    WaitingInterrupted,           // We are waiting, but have been interrupted
                                  //   and are running the interrupt handler
    Woken                         // Woken by a script call to Atomics.notify
  };

  // Condition variable that this runtime will wait on.
  js::ConditionVariable* cond_;

  // Current futex state for this runtime.  When not in a wait this
  // is Idle; when in a wait it is Waiting or the reason the futex
  // is about to wake up.
  FutexState state_;

  // Shared futex lock for all runtimes.  We can perhaps do better,
  // but any lock will need to be per-domain (consider SharedWorker)
  // or coarser.
  static mozilla::Atomic<js::Mutex*, mozilla::SequentiallyConsistent> lock_;

  // A flag that controls whether waiting is allowed.
  ThreadData<bool> canWait_;
};

// Go to sleep if the int32_t value at the given address equals `value`.
[[nodiscard]] FutexThread::WaitResult atomics_wait_impl(
    JSContext* cx, SharedArrayRawBuffer* sarb, size_t byteOffset, int32_t value,
    const mozilla::Maybe<mozilla::TimeDuration>& timeout);

// Go to sleep if the int64_t value at the given address equals `value`.
[[nodiscard]] FutexThread::WaitResult atomics_wait_impl(
    JSContext* cx, SharedArrayRawBuffer* sarb, size_t byteOffset, int64_t value,
    const mozilla::Maybe<mozilla::TimeDuration>& timeout);

// Notify some waiters on the given address.  If `count` is negative then notify
// all.  The return value is nonnegative and is the number of waiters woken.  If
// the number of waiters woken exceeds INT64_MAX then this never returns.  If
// `count` is nonnegative then the return value is never greater than `count`.
[[nodiscard]] int64_t atomics_notify_impl(SharedArrayRawBuffer* sarb,
                                          size_t byteOffset, int64_t count);

} /* namespace js */

#endif /* builtin_AtomicsObject_h */
