/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef gc_GCParallelTask_h
#define gc_GCParallelTask_h

#include "mozilla/Move.h"

#include "js/TypeDecls.h"
#include "threading/ProtectedData.h"

namespace js {

// A generic task used to dispatch work to the helper thread system.
// Users supply a function pointer to call.
//
// Note that we don't use virtual functions here because destructors can write
// the vtable pointer on entry, which can causes races if synchronization
// happens there.
class GCParallelTask
{
  public:
    using TaskFunc = void (*)(GCParallelTask*);

  private:
    JSRuntime* const runtime_;
    TaskFunc func_;

    // The state of the parallel computation.
    enum TaskState {
        NotStarted,
        Dispatched,
        Finished,
    };
    UnprotectedData<TaskState> state;

    // Amount of time this task took to execute.
    ActiveThreadOrGCTaskData<mozilla::TimeDuration> duration_;

    explicit GCParallelTask(const GCParallelTask&) = delete;

  protected:
    // A flag to signal a request for early completion of the off-thread task.
    mozilla::Atomic<bool> cancel_;

  public:
    explicit GCParallelTask(JSRuntime* runtime, TaskFunc func)
      : runtime_(runtime),
        func_(func),
        state(NotStarted),
        duration_(nullptr),
        cancel_(false)
    {}

    GCParallelTask(GCParallelTask&& other)
      : runtime_(other.runtime_),
        func_(other.func_),
        state(other.state),
        duration_(nullptr),
        cancel_(false)
    {}

    // Derived classes must override this to ensure that join() gets called
    // before members get destructed.
    ~GCParallelTask();

    JSRuntime* runtime() { return runtime_; }

    // Time spent in the most recent invocation of this task.
    mozilla::TimeDuration duration() const { return duration_; }

    // The simple interface to a parallel task works exactly like pthreads.
    bool start();
    void join();

    // If multiple tasks are to be started or joined at once, it is more
    // efficient to take the helper thread lock once and use these methods.
    bool startWithLockHeld(AutoLockHelperThreadState& locked);
    void joinWithLockHeld(AutoLockHelperThreadState& locked);

    // Instead of dispatching to a helper, run the task on the current thread.
    void runFromActiveCooperatingThread(JSRuntime* rt);

    // Dispatch a cancelation request.
    enum CancelMode { CancelNoWait, CancelAndWait};
    void cancel(CancelMode mode = CancelNoWait) {
        cancel_ = true;
        if (mode == CancelAndWait)
            join();
    }

    // Check if a task is actively running.
    bool isRunningWithLockHeld(const AutoLockHelperThreadState& locked) const;
    bool isRunning() const;

    void runTask() {
        func_(this);
    }

    // This should be friended to HelperThread, but cannot be because it
    // would introduce several circular dependencies.
  public:
    void runFromHelperThread(AutoLockHelperThreadState& locked);
};

// CRTP template to handle cast to derived type when calling run().
template <typename Derived>
class GCParallelTaskHelper : public GCParallelTask
{
  public:
    explicit GCParallelTaskHelper(JSRuntime* runtime)
      : GCParallelTask(runtime, &runTaskTyped)
    {}
    GCParallelTaskHelper(GCParallelTaskHelper&& other)
      : GCParallelTask(mozilla::Move(other))
    {}

  private:
    static void runTaskTyped(GCParallelTask* task) {
        static_cast<Derived*>(task)->run();
    }
};

} /* namespace js */
#endif /* gc_GCParallelTask_h */
