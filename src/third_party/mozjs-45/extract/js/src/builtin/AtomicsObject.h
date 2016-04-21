/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef builtin_AtomicsObject_h
#define builtin_AtomicsObject_h

#include "jslock.h"
#include "jsobj.h"

namespace js {

class AtomicsObject : public JSObject
{
  public:
    static const Class class_;
    static JSObject* initClass(JSContext* cx, Handle<GlobalObject*> global);
    static bool toString(JSContext* cx, unsigned int argc, Value* vp);

    // Defined return values for futexWait.
    // The error values must be negative because APIs such as futexWaitOrRequeue
    // return a value that is either the number of tasks woken or an error code.
    enum FutexWaitResult : int32_t {
        FutexOK = 0,
        FutexNotequal = -1,
        FutexTimedout = -2
    };
};

bool atomics_compareExchange(JSContext* cx, unsigned argc, Value* vp);
bool atomics_exchange(JSContext* cx, unsigned argc, Value* vp);
bool atomics_load(JSContext* cx, unsigned argc, Value* vp);
bool atomics_store(JSContext* cx, unsigned argc, Value* vp);
bool atomics_fence(JSContext* cx, unsigned argc, Value* vp);
bool atomics_add(JSContext* cx, unsigned argc, Value* vp);
bool atomics_sub(JSContext* cx, unsigned argc, Value* vp);
bool atomics_and(JSContext* cx, unsigned argc, Value* vp);
bool atomics_or(JSContext* cx, unsigned argc, Value* vp);
bool atomics_xor(JSContext* cx, unsigned argc, Value* vp);
bool atomics_isLockFree(JSContext* cx, unsigned argc, Value* vp);
bool atomics_futexWait(JSContext* cx, unsigned argc, Value* vp);
bool atomics_futexWake(JSContext* cx, unsigned argc, Value* vp);
bool atomics_futexWakeOrRequeue(JSContext* cx, unsigned argc, Value* vp);

/* asm.js callouts */
int32_t atomics_add_asm_callout(int32_t vt, int32_t offset, int32_t value);
int32_t atomics_sub_asm_callout(int32_t vt, int32_t offset, int32_t value);
int32_t atomics_and_asm_callout(int32_t vt, int32_t offset, int32_t value);
int32_t atomics_or_asm_callout(int32_t vt, int32_t offset, int32_t value);
int32_t atomics_xor_asm_callout(int32_t vt, int32_t offset, int32_t value);
int32_t atomics_cmpxchg_asm_callout(int32_t vt, int32_t offset, int32_t oldval, int32_t newval);
int32_t atomics_xchg_asm_callout(int32_t vt, int32_t offset, int32_t value);

class FutexRuntime
{
public:
    static bool initialize();
    static void destroy();

    static void lock();
    static void unlock();

    FutexRuntime();
    bool initInstance();
    void destroyInstance();

    // Parameters to wake().
    enum WakeReason {
        WakeExplicit,           // Being asked to wake up by another thread
        WakeForJSInterrupt      // Interrupt requested
    };

    // Block the calling thread and wait.
    //
    // The futex lock must be held around this call.
    //
    // The timeout is the number of milliseconds, with fractional
    // times allowed; specify positive infinity for an indefinite wait.
    //
    // wait() will not wake up spuriously.  It will return true and
    // set *result to a return code appropriate for
    // Atomics.futexWait() on success, and return false on error.
    bool wait(JSContext* cx, double timeout, AtomicsObject::FutexWaitResult* result);

    // Wake the thread represented by this Runtime.
    //
    // The futex lock must be held around this call.  (The sleeping
    // thread will not wake up until the caller of futexWake()
    // releases the lock.)
    //
    // If the thread is not waiting then this method does nothing.
    //
    // If the thread is waiting in a call to futexWait() and the
    // reason is WakeExplicit then the futexWait() call will return
    // with Woken.
    //
    // If the thread is waiting in a call to futexWait() and the
    // reason is WakeForJSInterrupt then the futexWait() will return
    // with WaitingNotifiedForInterrupt; in the latter case the caller
    // of futexWait() must handle the interrupt.
    void wake(WakeReason reason);

    bool isWaiting();

  private:
    enum FutexState {
        Idle,                        // We are not waiting or woken
        Waiting,                     // We are waiting, nothing has happened yet
        WaitingNotifiedForInterrupt, // We are waiting, but have been interrupted,
                                     //   and have not yet started running the
                                     //   interrupt handler
        WaitingInterrupted,          // We are waiting, but have been interrupted
                                     //   and are running the interrupt handler
        Woken                        // Woken by a script call to futexWake
    };

    // Condition variable that this runtime will wait on.
    PRCondVar* cond_;

    // Current futex state for this runtime.  When not in a wait this
    // is Idle; when in a wait it is Waiting or the reason the futex
    // is about to wake up.
    FutexState state_;

    // Shared futex lock for all runtimes.  We can perhaps do better,
    // but any lock will need to be per-domain (consider SharedWorker)
    // or coarser.
    static mozilla::Atomic<PRLock*> lock_;

#ifdef DEBUG
    // Null or the thread holding the lock.
    static mozilla::Atomic<PRThread*> lockHolder_;
#endif
};

JSObject*
InitAtomicsClass(JSContext* cx, HandleObject obj);

}  /* namespace js */

#endif /* builtin_AtomicsObject_h */
