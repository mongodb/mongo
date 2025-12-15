/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef vm_OffThreadPromiseRuntimeState_h
#define vm_OffThreadPromiseRuntimeState_h

#include <stddef.h>  // size_t

#include "jstypes.h"  // JS_PUBLIC_API

#include "ds/Fifo.h"           // js::Fifo
#include "ds/PriorityQueue.h"  // js::PriorityQueue
#include "js/AllocPolicy.h"    // js::SystemAllocPolicy
#include "js/HashTable.h"      // js::DefaultHasher, js::HashSet
#include "js/Promise.h"  // JS::Dispatchable, JS::Dispatchable::MaybeShuttingDown,
                         // JS::DispatchToEventLoopCallback,
                         // JS::DelayedDispatchToEventLoopCallback
#include "js/RootingAPI.h"                // JS::Handle, JS::PersistentRooted
#include "threading/ConditionVariable.h"  // js::ConditionVariable
#include "vm/PromiseObject.h"             // js::PromiseObject

struct JS_PUBLIC_API JSContext;
struct JS_PUBLIC_API JSRuntime;

namespace js {

class AutoLockHelperThreadState;
class OffThreadPromiseRuntimeState;

// [SMDOC] OffThreadPromiseTask: an off-main-thread task that resolves a promise
//
// An OffThreadPromiseTask is an abstract base class holding a JavaScript
// promise that will be resolved (fulfilled or rejected) with the results of a
// task possibly performed by some other thread. OffThreadPromiseTasks can be
// undispatched (meaning that they can be dropped if the JSContext owning the
// promise shuts down before the tasks resolves) or dispatched (meaning that
// shutdown should wait for the task to join).
//
// An OffThreadPromiseTask's lifecycle is as follows:
//
// - Some JavaScript native wishes to return a promise of the result of some
//   computation that might be performed by other threads (say, helper threads
//   or the embedding's I/O threads), so it creates a PromiseObject to represent
//   the result, and an OffThreadPromiseTask referring to it. After handing the
//   OffThreadPromiseTask to the code doing the actual work, the native is free
//   to return the PromiseObject to its caller.
//
// - When the computation is done, successfully or otherwise, it populates the
//   OffThreadPromiseTask—which is actually an instance of some concrete
//   subclass specific to the task—with the information needed to resolve the
//   promise, and calls OffThreadPromiseTask::dispatchResolveAndDestroy. This
//   enqueues a runnable on the JavaScript thread to which the promise belongs.
//
// - When it gets around to the runnable, the JavaScript thread calls the
//   OffThreadPromiseTask's `resolve` method, which the concrete subclass has
//   overriden to resolve the promise appropriately. This probably enqueues a
//   promise reaction job.
//
// - The JavaScript thread then deletes the OffThreadPromiseTask.
//
// During shutdown, the process is slightly different. Enqueuing runnables to
// the JavaScript thread begins to fail. Undispatched tasks are immediately
// destroyed. JSRuntime shutdown waits for all outstanding dispatched tasks
// to call their run methods, and then deletes them on the main thread,
// without calling `resolve`.
//
// For example, the JavaScript function WebAssembly.compile uses
// OffThreadPromiseTask to manage the result of a helper thread task, accepting
// binary WebAssembly code and returning a promise of a compiled
// WebAssembly.Module. It would like to do this compilation work on a helper
// thread. When called by JavaScript, WebAssembly.compile creates a promise,
// builds a CompileBufferTask (the OffThreadPromiseTask concrete subclass) to
// keep track of it, and then hands that to a helper thread. When the helper
// thread is done, successfully or otherwise, it calls the CompileBufferTask's
// dispatchResolveAndDestroy method, which enqueues a runnable to the JavaScript
// thread to resolve the promise and delete the CompileBufferTask.
// (CompileBufferTask actually implements PromiseHelperTask, which implements
// OffThreadPromiseTask; PromiseHelperTask is what our helper thread scheduler
// requires.)
//
// OffThreadPromiseTasks are not limited to use with helper threads. For
// example, a function returning a promise of the result of a network operation
// could provide the code collecting the incoming data with an
// OffThreadPromiseTask for the promise, and let the embedding's network I/O
// threads call dispatchResolveAndDestroy.
//
// OffThreadPromiseTask may also be used purely on the main thread, as a way to
// "queue a task" in HTML terms. Note that a "task" is not the same as a
// "microtask" and there are separate queues for tasks and microtasks that are
// drained at separate times in the browser. The task queue is implemented by
// the browser's main event loop. The microtask queue is implemented
// by JS::JobQueue, used for promises and gets drained before returning to
// the event loop. Thus OffThreadPromiseTask can only be used when the spec
// says "queue a task", as the WebAssembly APIs do. In some cases, like
// Atomics.waitAsync, the choice between queuing a task or a microtask depends
// on whether the promise is being resolved from the owning thread or another
// thread. In such cases, ExtractAndForget can be used from the owning thread to
// cancel the task and return the underlying promise, which can then be resolved
// the normal way.
//
// An OffThreadPromiseTask has a JSContext, and must be constructed and have its
// 'init' method called on that JSContext's thread. Once
// initialized, its dispatchResolveAndDestroy method may be called from any
// thread. Other than calling `ExtractAndForget`, or `DestroyUndispatchedTask`
// during shutdown, this is the only safe way to destruct an
// OffThreadPromiseTask; doing so ensures the OffThreadPromiseTask's destructor
// will run on the JSContext's thread, either from the event loop or during
// shutdown.
//
// OffThreadPromiseTask::dispatchResolveAndDestroy uses the
// JS::DispatchToEventLoopCallback provided by the embedding to enqueue
// runnables on the JavaScript thread. See the comments for
// DispatchToEventLoopCallback for details.

class OffThreadPromiseTask : public JS::Dispatchable {
  friend class OffThreadPromiseRuntimeState;

  JSRuntime* runtime_;
  JS::PersistentRooted<PromiseObject*> promise_;

  // The registered_ flag indicates that this task is counted as part of
  // numRegistered_ of OffThreadPromiseRuntimeState, which will wait untill
  // all registered tasks have been run or destroyed.
  bool registered_;

  // Indicates that this is an undispatched cancellable task, which is a member
  // of the Cancellable list.  If cancellable is set to false, we can no longer
  // terminate the task early, this means it is no longer tracked by the
  // Cancellable list, and a dispatch has been attempted.
  bool cancellable_;

  void operator=(const OffThreadPromiseTask&) = delete;
  OffThreadPromiseTask(const OffThreadPromiseTask&) = delete;

  void unregister(OffThreadPromiseRuntimeState& state);

 protected:
  OffThreadPromiseTask(JSContext* cx, JS::Handle<PromiseObject*> promise);

  // To be called by OffThreadPromiseTask and implemented by the derived class.
  virtual bool resolve(JSContext* cx, JS::Handle<PromiseObject*> promise) {
    MOZ_CRASH("Tasks should override resolve");
  };

  // JS::Dispatchable override implementation.
  // Runs the task, and ends with 'js_delete(this)'.
  void run(JSContext* cx, MaybeShuttingDown maybeShuttingDown) final;

  // JS::Dispatchable override implementation, moves ownership to
  // OffThreadPromiseRuntimeState's failed_ list, for cleanup on shutdown.
  void transferToRuntime() final;

  // To be called by `destroy` during shutdown and implemented by the derived
  // class (for undispatched tasks only). Gives the task a chance to clean up
  // before being deleted.
  virtual void prepareForCancel() {
    MOZ_CRASH("Undispatched tasks should override prepareForCancel");
  }

 public:
  ~OffThreadPromiseTask() override;
  static void DestroyUndispatchedTask(OffThreadPromiseTask* task);

  JSRuntime* runtime() { return runtime_; }

  // Calling `init` on an OffThreadPromiseTask informs the runtime that it must
  // wait on shutdown for this task to rejoin the active JSContext by calling
  // dispatchResolveAndDestroy().
  bool init(JSContext* cx);
  bool init(JSContext* cx, const AutoLockHelperThreadState& lock);

  // Cancellable initialization track the task in case it needs to be aborted
  // before it is dispatched. Cancellable tasks are owned by the cancellable_
  // hashMap until they are dispatched.
  static bool InitCancellable(JSContext* cx,
                              js::UniquePtr<OffThreadPromiseTask>&& task);
  static bool InitCancellable(JSContext* cx,
                              const AutoLockHelperThreadState& lock,
                              js::UniquePtr<OffThreadPromiseTask>&& task);

  // Remove the cancellable task from the runtime cancellable list and
  // call DispatchResolveAndDestroy with a newly created UniquePtr,
  // so that ownership moves to the embedding.
  //
  // If a task is never removed from the cancellable list, it is deleted on
  // shutdown without running.
  void removeFromCancellableListAndDispatch();
  void removeFromCancellableListAndDispatch(
      const AutoLockHelperThreadState& lock);

  // These first two methods will wrap a pointer in a uniquePtr for the purpose
  // of passing it's ownership eventually to the embedding. These are used by
  // WASM and PromiseHelperTask.
  void dispatchResolveAndDestroy();
  void dispatchResolveAndDestroy(const AutoLockHelperThreadState& lock);

  // An initialized OffThreadPromiseTask can be dispatched to an active
  // JSContext of its Promise's JSRuntime from any thread. Normally, this will
  // lead to resolve() being called on JSContext thread, given the Promise.
  // However, if shutdown interrupts, resolve() may not be called, though the
  // OffThreadPromiseTask will be destroyed on a JSContext thread.
  static void DispatchResolveAndDestroy(
      js::UniquePtr<OffThreadPromiseTask>&& task);
  static void DispatchResolveAndDestroy(
      js::UniquePtr<OffThreadPromiseTask>&& task,
      const AutoLockHelperThreadState& lock);

  static PromiseObject* ExtractAndForget(OffThreadPromiseTask* task,
                                         const AutoLockHelperThreadState& lock);
};

using OffThreadPromiseTaskSet =
    HashSet<OffThreadPromiseTask*, DefaultHasher<OffThreadPromiseTask*>,
            SystemAllocPolicy>;

using DispatchableFifo =
    Fifo<js::UniquePtr<JS::Dispatchable>, 0, SystemAllocPolicy>;

class DelayedDispatchable {
  js::UniquePtr<JS::Dispatchable> dispatchable_;
  mozilla::TimeStamp endTime_;

 public:
  DelayedDispatchable(DelayedDispatchable&& other)
      : dispatchable_(other.dispatchable()), endTime_(other.endTime()) {}

  DelayedDispatchable(js::UniquePtr<JS::Dispatchable>&& dispatchable,
                      mozilla::TimeStamp endTime)
      : dispatchable_(std::move(dispatchable)), endTime_(endTime) {}

  void operator=(DelayedDispatchable&& other) {
    dispatchable_ = other.dispatchable();
    endTime_ = other.endTime();
  }
  js::UniquePtr<JS::Dispatchable> dispatchable() {
    return std::move(dispatchable_);
  }
  mozilla::TimeStamp endTime() const { return endTime_; }

  static bool higherPriority(const DelayedDispatchable& a,
                             const DelayedDispatchable& b) {
    return a.endTime_ < b.endTime_;
  }
};

using DelayedDispatchablePriorityQueue =
    PriorityQueue<DelayedDispatchable, DelayedDispatchable, 0,
                  SystemAllocPolicy>;

class OffThreadPromiseRuntimeState {
  friend class OffThreadPromiseTask;

  // These fields are initialized once before any off-thread usage and thus do
  // not require a lock.
  JS::DispatchToEventLoopCallback dispatchToEventLoopCallback_;
  JS::DelayedDispatchToEventLoopCallback delayedDispatchToEventLoopCallback_;
  void* dispatchToEventLoopClosure_;

  // A set of all OffThreadPromiseTasks that have successfully called 'init'.
  // This set doesn't own tasks. OffThreadPromiseTask's destructor removes them
  // from the set.
  HelperThreadLockData<size_t> numRegistered_;

  // The cancellable hashmap tracks the registered, but thusfar
  // undispatched tasks. Not all undispatched tasks are cancellable, namely
  // webassembly helpers are held in an undispatched state, but are not
  // cancellable. Until the task is dispatched, this hashmap acts as the owner
  // of the task. In order to place something in the cancellable list, use
  // InitCancellable. Any task owned by the cancellable list cannot be
  // dispatched using DispatchResolveAndDestroy. Instead, use the method on
  // OffThreadPromiseTask removeFromCancellableAndDispatch.
  HelperThreadLockData<OffThreadPromiseTaskSet> cancellable_;

  // This list owns tasks that have failed to dispatch or failed to execute.
  // The list is cleared on shutdown.
  HelperThreadLockData<DispatchableFifo> failed_;

  // The allFailed_ condition is waited on and notified during engine
  // shutdown, communicating when all off-thread tasks in failed_ are safe to be
  // destroyed from the (shutting down) main thread. This condition is met when
  // numRegistered_ == failed().count(), where the collection of failed tasks
  // mean "the DispatchToEventLoopCallback failed after this task was dispatched
  // for execution".
  HelperThreadLockData<ConditionVariable> allFailed_;
  HelperThreadLockData<size_t> numFailed_;

  // The queue of JS::Dispatchables used by the DispatchToEventLoopCallback that
  // calling js::UseInternalJobQueues installs.
  HelperThreadLockData<DispatchableFifo> internalDispatchQueue_;
  HelperThreadLockData<ConditionVariable> internalDispatchQueueAppended_;
  HelperThreadLockData<bool> internalDispatchQueueClosed_;
  HelperThreadLockData<DelayedDispatchablePriorityQueue>
      internalDelayedDispatchPriorityQueue_;

  ConditionVariable& allFailed() { return allFailed_.ref(); }

  DispatchableFifo& failed() { return failed_.ref(); }
  OffThreadPromiseTaskSet& cancellable() { return cancellable_.ref(); }

  DispatchableFifo& internalDispatchQueue() {
    return internalDispatchQueue_.ref();
  }
  ConditionVariable& internalDispatchQueueAppended() {
    return internalDispatchQueueAppended_.ref();
  }
  DelayedDispatchablePriorityQueue& internalDelayedDispatchPriorityQueue() {
    return internalDelayedDispatchPriorityQueue_.ref();
  }

  void dispatchDelayedTasks();

  static bool internalDispatchToEventLoop(void*,
                                          js::UniquePtr<JS::Dispatchable>&&);
  static bool internalDelayedDispatchToEventLoop(
      void*, js::UniquePtr<JS::Dispatchable>&&, uint32_t);
  bool usingInternalDispatchQueue() const;

  void operator=(const OffThreadPromiseRuntimeState&) = delete;
  OffThreadPromiseRuntimeState(const OffThreadPromiseRuntimeState&) = delete;

 public:
  OffThreadPromiseRuntimeState();
  ~OffThreadPromiseRuntimeState();
  void init(JS::DispatchToEventLoopCallback callback,
            JS::DelayedDispatchToEventLoopCallback delayCallback,
            void* closure);
  void initInternalDispatchQueue();
  bool initialized() const;

  // If initInternalDispatchQueue() was called, internalDrain() can be
  // called to periodically drain the dispatch queue before shutdown.
  void internalDrain(JSContext* cx);
  bool internalHasPending();
  bool internalHasPending(AutoLockHelperThreadState& lock);

  void stealFailedTask(JS::Dispatchable* dispatchable);

  bool dispatchToEventLoop(js::UniquePtr<JS::Dispatchable>&& dispatchable);
  bool delayedDispatchToEventLoop(
      js::UniquePtr<JS::Dispatchable>&& dispatchable, uint32_t delay);

  // shutdown() must be called by the JSRuntime while the JSRuntime is valid.
  void shutdown(JSContext* cx);
};

}  // namespace js

#endif  // vm_OffThreadPromiseRuntimeState_h
