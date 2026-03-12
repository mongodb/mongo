/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef gc_GCParallelTask_h
#define gc_GCParallelTask_h

#include "mozilla/LinkedList.h"
#include "mozilla/Maybe.h"
#include "mozilla/TimeStamp.h"

#include <utility>

#include "gc/GCContext.h"
#include "js/Utility.h"
#include "threading/ProtectedData.h"
#include "vm/HelperThreads.h"
#include "vm/HelperThreadTask.h"

#define JS_MEMBER_FN_PTR_TYPE(ClassT, ReturnT, /* ArgTs */...) \
  ReturnT (ClassT::*)(__VA_ARGS__)

#define JS_CALL_MEMBER_FN_PTR(Receiver, Ptr, /* Args */...) \
  ((Receiver)->*(Ptr))(__VA_ARGS__)

namespace js {

namespace gcstats {
enum class PhaseKind : uint8_t;
}

namespace gc {

class GCRuntime;

static inline mozilla::TimeDuration TimeSince(mozilla::TimeStamp prev) {
  mozilla::TimeStamp now = mozilla::TimeStamp::Now();
  // Sadly this happens sometimes.
  MOZ_ASSERT(now >= prev);
  if (now < prev) {
    now = prev;
  }
  return now - prev;
}

}  // namespace gc

class AutoLockHelperThreadState;
class GCParallelTask;
class HelperThread;

// A wrapper around a linked list to enforce synchronization.
class GCParallelTaskList {
  mozilla::LinkedList<GCParallelTask> tasks;

 public:
  bool isEmpty(const AutoLockHelperThreadState& lock) {
    gHelperThreadLock.assertOwnedByCurrentThread();
    return tasks.isEmpty();
  }

  void insertBack(GCParallelTask* task, const AutoLockHelperThreadState& lock) {
    gHelperThreadLock.assertOwnedByCurrentThread();
    tasks.insertBack(task);
  }

  GCParallelTask* popFirst(const AutoLockHelperThreadState& lock) {
    gHelperThreadLock.assertOwnedByCurrentThread();
    return tasks.popFirst();
  }

  size_t sizeOfExcludingThis(mozilla::MallocSizeOf aMallocSizeOf,
                             const AutoLockHelperThreadState& lock) const {
    gHelperThreadLock.assertOwnedByCurrentThread();
    return tasks.sizeOfExcludingThis(aMallocSizeOf);
  }
};

// A generic task used to dispatch work to the helper thread system.
// Users override the pure-virtual run() method.
class GCParallelTask : private mozilla::LinkedListElement<GCParallelTask>,
                       public HelperThreadTask {
  friend class mozilla::LinkedList<GCParallelTask>;
  friend class mozilla::LinkedListElement<GCParallelTask>;

 public:
  gc::GCRuntime* const gc;

  // This can be PhaseKind::NONE for tasks that take place outside a GC.
  const gcstats::PhaseKind phaseKind;

  gc::GCUse use;

 private:
  // The state of the parallel computation.
  enum class State {
    // The task is idle. Either start() has not been called or join() has
    // returned.
    Idle,

    // The task is waiting in the per-runtime queue.
    Queued,

    // The task has been started but has not yet begun running on a helper
    // thread.
    Dispatched,

    // The task is currently running on a helper thread.
    Running,

    // The task has finished running but has not yet been joined by the main
    // thread.
    Finished
  };

  UnprotectedData<State> state_;

  HelperThreadLockData<bool> dispatchedToThreadPool;

  // May be set to the time this task was queued to collect telemetry.
  mozilla::TimeStamp maybeQueueTime_;

  // Amount of time this task took to execute.
  MainThreadOrGCTaskData<mozilla::TimeDuration> duration_;

 protected:
  // A flag to signal a request for early completion of the off-thread task.
  mozilla::Atomic<bool, mozilla::Relaxed> cancel_;

 public:
  explicit GCParallelTask(gc::GCRuntime* gc, gcstats::PhaseKind phaseKind,
                          gc::GCUse use = gc::GCUse::Unspecified)
      : gc(gc),
        phaseKind(phaseKind),
        use(use),
        state_(State::Idle),
        cancel_(false) {}
  GCParallelTask(GCParallelTask&& other) noexcept
      : gc(other.gc),
        phaseKind(other.phaseKind),
        use(other.use),
        state_(other.state_),
        cancel_(false) {}

  explicit GCParallelTask(const GCParallelTask&) = delete;

  // Derived classes must override this to ensure that join() gets called
  // before members get destructed.
  virtual ~GCParallelTask();

  // Time spent in the most recent invocation of this task.
  mozilla::TimeDuration duration() const { return duration_; }

  // The simple interface to a parallel task works exactly like pthreads.
  void start();
  void join(mozilla::Maybe<mozilla::TimeStamp> deadline = mozilla::Nothing());

  // If multiple tasks are to be started or joined at once, it is more
  // efficient to take the helper thread lock once and use these methods.
  void startWithLockHeld(AutoLockHelperThreadState& lock);
  void joinWithLockHeld(
      AutoLockHelperThreadState& lock,
      mozilla::Maybe<mozilla::TimeStamp> deadline = mozilla::Nothing());

  // Instead of dispatching to a helper, run the task on the current thread.
  void runFromMainThread();
  void runFromMainThread(AutoLockHelperThreadState& lock);

  // If the task is not already running, either start it or run it on the main
  // thread if that fails.
  void startOrRunIfIdle(AutoLockHelperThreadState& lock);

  // Set the cancel flag and wait for the task to finish.
  void cancelAndWait();

  // Report whether the task is idle. This means either before start() has been
  // called or after join() has been called.
  bool isIdle() const;
  bool isIdle(const AutoLockHelperThreadState& lock) const {
    return state_ == State::Idle;
  }

  // Report whether the task has been started. This means after start() has been
  // called but before the task has run to completion. The task may not yet have
  // started running.
  bool wasStarted() const;
  bool wasStarted(const AutoLockHelperThreadState& lock) const {
    return isDispatched(lock) || isRunning(lock);
  }

  bool isQueued(const AutoLockHelperThreadState& lock) const {
    return state_ == State::Queued;
  }

  bool isDispatched(const AutoLockHelperThreadState& lock) const {
    return state_ == State::Dispatched;
  }

  bool isNotYetRunning(const AutoLockHelperThreadState& lock) const {
    return state_ == State::Idle || state_ == State::Queued ||
           state_ == State::Dispatched;
  }

  const char* getName() override { return "GCParallelTask"; }

 protected:
  // Override this method to provide the task's functionality.
  virtual void run(AutoLockHelperThreadState& lock) = 0;

  virtual void recordDuration();

  bool isCancelled() const { return cancel_; }

 private:
  void assertIdle() const {
    // Don't lock here because that adds extra synchronization in debug
    // builds that may hide bugs. There's no race if the assertion passes.
    MOZ_ASSERT(state_ == State::Idle);
  }

  bool isRunning(const AutoLockHelperThreadState& lock) const {
    return state_ == State::Running;
  }
  bool isFinished(const AutoLockHelperThreadState& lock) const {
    return state_ == State::Finished;
  }

  void setQueued(const AutoLockHelperThreadState& lock) {
    MOZ_ASSERT(isIdle(lock));
    state_ = State::Queued;
  }
  void setDispatched(const AutoLockHelperThreadState& lock) {
    MOZ_ASSERT(isIdle(lock) || isQueued(lock));
    state_ = State::Dispatched;
  }
  void setRunning(const AutoLockHelperThreadState& lock) {
    MOZ_ASSERT(isNotYetRunning(lock));
    state_ = State::Running;
  }
  void setFinished(const AutoLockHelperThreadState& lock) {
    MOZ_ASSERT(isRunning(lock));
    state_ = State::Finished;
  }
  void setIdle(const AutoLockHelperThreadState& lock) {
    MOZ_ASSERT(!isRunning(lock));
    state_ = State::Idle;
  }
  friend class gc::GCRuntime;

  void joinNonIdleTask(mozilla::Maybe<mozilla::TimeStamp> deadline,
                       AutoLockHelperThreadState& lock);

  void runTask(JS::GCContext* gcx, AutoLockHelperThreadState& lock);

  // Implement the HelperThreadTask interface.
  ThreadType threadType() override {
    return ThreadType::THREAD_TYPE_GCPARALLEL;
  }
  void runHelperThreadTask(AutoLockHelperThreadState& locked) override;
  void onThreadPoolDispatch() override;
};

} /* namespace js */
#endif /* gc_GCParallelTask_h */
