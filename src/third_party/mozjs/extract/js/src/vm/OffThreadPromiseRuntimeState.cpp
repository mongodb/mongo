/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "vm/OffThreadPromiseRuntimeState.h"

#include "mozilla/Assertions.h"  // MOZ_ASSERT{,_IF}

#include <utility>  // mozilla::Swap

#include "jspubtd.h"  // js::CurrentThreadCanAccessRuntime

#include "js/AllocPolicy.h"  // js::ReportOutOfMemory
#include "js/HeapAPI.h"      // JS::shadow::Zone
#include "js/Promise.h"  // JS::Dispatchable, JS::DispatchToEventLoopCallback
#include "js/Utility.h"  // js_delete, js::AutoEnterOOMUnsafeRegion
#include "threading/ProtectedData.h"  // js::UnprotectedData
#include "vm/HelperThreads.h"         // js::AutoLockHelperThreadState
#include "vm/JSContext.h"             // JSContext
#include "vm/PromiseObject.h"         // js::PromiseObject
#include "vm/Realm.h"                 // js::AutoRealm
#include "vm/Runtime.h"               // JSRuntime

#include "vm/Realm-inl.h"  // js::AutoRealm::AutoRealm

using JS::Handle;

using js::OffThreadPromiseRuntimeState;
using js::OffThreadPromiseTask;

OffThreadPromiseTask::OffThreadPromiseTask(JSContext* cx,
                                           JS::Handle<PromiseObject*> promise)
    : runtime_(cx->runtime()), promise_(cx, promise), registered_(false) {
  MOZ_ASSERT(runtime_ == promise_->zone()->runtimeFromMainThread());
  MOZ_ASSERT(CurrentThreadCanAccessRuntime(runtime_));
  MOZ_ASSERT(cx->runtime()->offThreadPromiseState.ref().initialized());
}

OffThreadPromiseTask::~OffThreadPromiseTask() {
  MOZ_ASSERT(CurrentThreadCanAccessRuntime(runtime_));

  OffThreadPromiseRuntimeState& state = runtime_->offThreadPromiseState.ref();
  MOZ_ASSERT(state.initialized());

  if (registered_) {
    unregister(state);
  }
}

bool OffThreadPromiseTask::init(JSContext* cx) {
  MOZ_ASSERT(cx->runtime() == runtime_);
  MOZ_ASSERT(CurrentThreadCanAccessRuntime(runtime_));

  OffThreadPromiseRuntimeState& state = runtime_->offThreadPromiseState.ref();
  MOZ_ASSERT(state.initialized());

  AutoLockHelperThreadState lock;

  if (!state.live().putNew(this)) {
    ReportOutOfMemory(cx);
    return false;
  }

  registered_ = true;
  return true;
}

void OffThreadPromiseTask::unregister(OffThreadPromiseRuntimeState& state) {
  MOZ_ASSERT(registered_);
  AutoLockHelperThreadState lock;
  state.live().remove(this);
  registered_ = false;
}

void OffThreadPromiseTask::run(JSContext* cx,
                               MaybeShuttingDown maybeShuttingDown) {
  MOZ_ASSERT(cx->runtime() == runtime_);
  MOZ_ASSERT(CurrentThreadCanAccessRuntime(runtime_));
  MOZ_ASSERT(registered_);

  // Remove this task from live_ before calling `resolve`, so that if `resolve`
  // itself drains the queue reentrantly, the queue will not think this task is
  // yet to be queued and block waiting for it.
  //
  // The unregister method synchronizes on the helper thread lock and ensures
  // that we don't delete the task while the helper thread is still running.
  OffThreadPromiseRuntimeState& state = runtime_->offThreadPromiseState.ref();
  MOZ_ASSERT(state.initialized());
  unregister(state);

  if (maybeShuttingDown == JS::Dispatchable::NotShuttingDown) {
    // We can't leave a pending exception when returning to the caller so do
    // the same thing as Gecko, which is to ignore the error. This should
    // only happen due to OOM or interruption.
    AutoRealm ar(cx, promise_);
    if (!resolve(cx, promise_)) {
      cx->clearPendingException();
    }
  }

  js_delete(this);
}

void OffThreadPromiseTask::dispatchResolveAndDestroy() {
  AutoLockHelperThreadState lock;
  dispatchResolveAndDestroy(lock);
}

void OffThreadPromiseTask::dispatchResolveAndDestroy(
    const AutoLockHelperThreadState& lock) {
  MOZ_ASSERT(registered_);

  OffThreadPromiseRuntimeState& state = runtime_->offThreadPromiseState.ref();
  MOZ_ASSERT(state.initialized());
  MOZ_ASSERT(state.live().has(this));

  // If the dispatch succeeds, then we are guaranteed that run() will be
  // called on an active JSContext of runtime_.
  if (state.dispatchToEventLoopCallback_(state.dispatchToEventLoopClosure_,
                                         this)) {
    return;
  }

  // The DispatchToEventLoopCallback has rejected this task, indicating that
  // shutdown has begun. Count the number of rejected tasks that have called
  // dispatchResolveAndDestroy, and when they account for the entire contents of
  // live_, notify OffThreadPromiseRuntimeState::shutdown that it is safe to
  // destruct them.
  state.numCanceled_++;
  if (state.numCanceled_ == state.live().count()) {
    state.allCanceled().notify_one();
  }
}

OffThreadPromiseRuntimeState::OffThreadPromiseRuntimeState()
    : dispatchToEventLoopCallback_(nullptr),
      dispatchToEventLoopClosure_(nullptr),
      numCanceled_(0),
      internalDispatchQueueClosed_(false) {}

OffThreadPromiseRuntimeState::~OffThreadPromiseRuntimeState() {
  MOZ_ASSERT(live_.refNoCheck().empty());
  MOZ_ASSERT(numCanceled_ == 0);
  MOZ_ASSERT(internalDispatchQueue_.refNoCheck().empty());
  MOZ_ASSERT(!initialized());
}

void OffThreadPromiseRuntimeState::init(
    JS::DispatchToEventLoopCallback callback, void* closure) {
  MOZ_ASSERT(!initialized());

  dispatchToEventLoopCallback_ = callback;
  dispatchToEventLoopClosure_ = closure;

  MOZ_ASSERT(initialized());
}

/* static */
bool OffThreadPromiseRuntimeState::internalDispatchToEventLoop(
    void* closure, JS::Dispatchable* d) {
  OffThreadPromiseRuntimeState& state =
      *reinterpret_cast<OffThreadPromiseRuntimeState*>(closure);
  MOZ_ASSERT(state.usingInternalDispatchQueue());
  gHelperThreadLock.assertOwnedByCurrentThread();

  if (state.internalDispatchQueueClosed_) {
    return false;
  }

  // The JS API contract is that 'false' means shutdown, so be infallible
  // here (like Gecko).
  AutoEnterOOMUnsafeRegion noOOM;
  if (!state.internalDispatchQueue().pushBack(d)) {
    noOOM.crash("internalDispatchToEventLoop");
  }

  // Wake up internalDrain() if it is waiting for a job to finish.
  state.internalDispatchQueueAppended().notify_one();
  return true;
}

bool OffThreadPromiseRuntimeState::usingInternalDispatchQueue() const {
  return dispatchToEventLoopCallback_ == internalDispatchToEventLoop;
}

void OffThreadPromiseRuntimeState::initInternalDispatchQueue() {
  init(internalDispatchToEventLoop, this);
  MOZ_ASSERT(usingInternalDispatchQueue());
}

bool OffThreadPromiseRuntimeState::initialized() const {
  return !!dispatchToEventLoopCallback_;
}

void OffThreadPromiseRuntimeState::internalDrain(JSContext* cx) {
  MOZ_ASSERT(usingInternalDispatchQueue());

  for (;;) {
    JS::Dispatchable* d;
    {
      AutoLockHelperThreadState lock;

      MOZ_ASSERT(!internalDispatchQueueClosed_);
      MOZ_ASSERT_IF(!internalDispatchQueue().empty(), !live().empty());
      if (live().empty()) {
        return;
      }

      // There are extant live OffThreadPromiseTasks. If none are in the queue,
      // block until one of them finishes and enqueues a dispatchable.
      while (internalDispatchQueue().empty()) {
        internalDispatchQueueAppended().wait(lock);
      }

      d = internalDispatchQueue().popCopyFront();
    }

    // Don't call run() with lock held to avoid deadlock.
    d->run(cx, JS::Dispatchable::NotShuttingDown);
  }
}

bool OffThreadPromiseRuntimeState::internalHasPending() {
  MOZ_ASSERT(usingInternalDispatchQueue());

  AutoLockHelperThreadState lock;
  MOZ_ASSERT(!internalDispatchQueueClosed_);
  MOZ_ASSERT_IF(!internalDispatchQueue().empty(), !live().empty());
  return !live().empty();
}

void OffThreadPromiseRuntimeState::shutdown(JSContext* cx) {
  if (!initialized()) {
    return;
  }

  AutoLockHelperThreadState lock;

  // When the shell is using the internal event loop, we must simulate our
  // requirement of the embedding that, before shutdown, all successfully-
  // dispatched-to-event-loop tasks have been run.
  if (usingInternalDispatchQueue()) {
    DispatchableFifo dispatchQueue;
    {
      std::swap(dispatchQueue, internalDispatchQueue());
      MOZ_ASSERT(internalDispatchQueue().empty());
      internalDispatchQueueClosed_ = true;
    }

    // Don't call run() with lock held to avoid deadlock.
    AutoUnlockHelperThreadState unlock(lock);
    for (JS::Dispatchable* d : dispatchQueue) {
      d->run(cx, JS::Dispatchable::ShuttingDown);
    }
  }

  // An OffThreadPromiseTask may only be safely deleted on its JSContext's
  // thread (since it contains a PersistentRooted holding its promise), and
  // only after it has called dispatchResolveAndDestroy (since that is our
  // only indication that its owner is done writing into it).
  //
  // OffThreadPromiseTasks accepted by the DispatchToEventLoopCallback are
  // deleted by their 'run' methods. Only dispatchResolveAndDestroy invokes
  // the callback, and the point of the callback is to call 'run' on the
  // JSContext's thread, so the conditions above are met.
  //
  // But although the embedding's DispatchToEventLoopCallback promises to run
  // every task it accepts before shutdown, when shutdown does begin it starts
  // rejecting tasks; we cannot count on 'run' to clean those up for us.
  // Instead, dispatchResolveAndDestroy keeps a count of rejected ('canceled')
  // tasks; once that count covers everything in live_, this function itself
  // runs only on the JSContext's thread, so we can delete them all here.
  while (live().count() != numCanceled_) {
    MOZ_ASSERT(numCanceled_ < live().count());
    allCanceled().wait(lock);
  }

  // Now that live_ contains only cancelled tasks, we can just delete
  // everything.
  for (OffThreadPromiseTaskSet::Range r = live().all(); !r.empty();
       r.popFront()) {
    OffThreadPromiseTask* task = r.front();

    // We don't want 'task' to unregister itself (which would mutate live_ while
    // we are iterating over it) so reset its internal registered_ flag.
    MOZ_ASSERT(task->registered_);
    task->registered_ = false;
    js_delete(task);
  }
  live().clear();
  numCanceled_ = 0;

  // After shutdown, there should be no OffThreadPromiseTask activity in this
  // JSRuntime. Revert to the !initialized() state to catch bugs.
  dispatchToEventLoopCallback_ = nullptr;
  MOZ_ASSERT(!initialized());
}
