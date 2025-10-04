/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "gc/GCParallelTask.h"

#include "mozilla/Maybe.h"
#include "mozilla/TimeStamp.h"

#include "gc/GCContext.h"
#include "gc/GCInternals.h"
#include "gc/ParallelWork.h"
#include "vm/HelperThreadState.h"
#include "vm/Runtime.h"
#include "vm/Time.h"

using namespace js;
using namespace js::gc;

using mozilla::Maybe;
using mozilla::TimeDuration;
using mozilla::TimeStamp;

js::GCParallelTask::~GCParallelTask() {
  // The LinkedListElement destructor will remove us from any list we are part
  // of without synchronization, so ensure that doesn't happen.
  MOZ_DIAGNOSTIC_ASSERT(!isInList());

  // Only most-derived classes' destructors may do the join: base class
  // destructors run after those for derived classes' members, so a join in a
  // base class can't ensure that the task is done using the members. All we
  // can do now is check that someone has previously stopped the task.
  assertIdle();
}

static bool ShouldMeasureTaskStartDelay() {
  // We use many tasks during GC so randomly sample a small fraction for the
  // purposes of recording telemetry.
  return (rand() % 100) == 0;
}

void js::GCParallelTask::startWithLockHeld(AutoLockHelperThreadState& lock) {
  MOZ_ASSERT(CanUseExtraThreads());
  MOZ_ASSERT(HelperThreadState().isInitialized(lock));
  assertIdle();

  maybeQueueTime_ = TimeStamp();
  if (ShouldMeasureTaskStartDelay()) {
    maybeQueueTime_ = TimeStamp::Now();
  }

  dispatchedToThreadPool = false;

  gc->dispatchOrQueueParallelTask(this, lock);
}

void js::GCParallelTask::start() {
  if (!CanUseExtraThreads()) {
    runFromMainThread();
    return;
  }

  AutoLockHelperThreadState lock;
  startWithLockHeld(lock);
}

void js::GCParallelTask::startOrRunIfIdle(AutoLockHelperThreadState& lock) {
  if (wasStarted(lock)) {
    return;
  }

  // Join the previous invocation of the task. This will return immediately
  // if the thread has never been started.
  joinWithLockHeld(lock);

  if (!CanUseExtraThreads()) {
    runFromMainThread(lock);
    return;
  }

  startWithLockHeld(lock);
}

void js::GCParallelTask::cancelAndWait() {
  MOZ_ASSERT(!isCancelled());
  cancel_ = true;
  join();
  cancel_ = false;
}

void js::GCParallelTask::join(Maybe<TimeStamp> deadline) {
  AutoLockHelperThreadState lock;
  joinWithLockHeld(lock, deadline);
}

void js::GCParallelTask::joinWithLockHeld(AutoLockHelperThreadState& lock,
                                          Maybe<TimeStamp> deadline) {
  // Task has not been started; there's nothing to do.
  if (isIdle(lock)) {
    return;
  }

  if (lock.hasQueuedTasks()) {
    // Unlock to allow task dispatch without lock held, otherwise we could wait
    // forever.
    AutoUnlockHelperThreadState unlock(lock);
  }

  if (isNotYetRunning(lock) && !dispatchedToThreadPool &&
      deadline.isNothing()) {
    // If the task was dispatched but has not yet started then cancel the task
    // and run it from the main thread. This stops us from blocking here when
    // the helper threads are busy with other tasks.
    MOZ_ASSERT(isInList());
    MOZ_ASSERT_IF(isDispatched(lock), gc->dispatchedParallelTasks != 0);

    remove();
    runFromMainThread(lock);
  } else {
    // Otherwise wait for the task to complete.
    joinNonIdleTask(deadline, lock);
  }

  if (isIdle(lock)) {
    recordDuration();
  }
}

void GCParallelTask::recordDuration() {
  if (phaseKind != gcstats::PhaseKind::NONE) {
    gc->stats().recordParallelPhase(phaseKind, duration_);
  }
}

void js::GCParallelTask::joinNonIdleTask(Maybe<TimeStamp> deadline,
                                         AutoLockHelperThreadState& lock) {
  MOZ_ASSERT(!isIdle(lock));

  while (!isFinished(lock)) {
    TimeDuration timeout = TimeDuration::Forever();
    if (deadline) {
      TimeStamp now = TimeStamp::Now();
      if (*deadline <= now) {
        break;
      }
      timeout = *deadline - now;
    }

    HelperThreadState().wait(lock, timeout);
  }

  if (isFinished(lock)) {
    setIdle(lock);
  }
}

void js::GCParallelTask::runFromMainThread() {
  AutoLockHelperThreadState lock;
  runFromMainThread(lock);
}

void js::GCParallelTask::runFromMainThread(AutoLockHelperThreadState& lock) {
  MOZ_ASSERT(isNotYetRunning(lock));
  MOZ_ASSERT(js::CurrentThreadCanAccessRuntime(gc->rt));

  if (lock.hasQueuedTasks()) {
    // Unlock to allow task dispatch without lock held, otherwise we can wait
    // forever.
    AutoUnlockHelperThreadState unlock(lock);
  }

  runTask(gc->rt->gcContext(), lock);
  setIdle(lock);
}

class MOZ_RAII AutoGCContext {
  JS::GCContext context;

 public:
  explicit AutoGCContext(JSRuntime* runtime) : context(runtime) {
    MOZ_RELEASE_ASSERT(TlsGCContext.init(),
                       "Failed to initialize TLS for GC context");

    MOZ_ASSERT(!TlsGCContext.get());
    TlsGCContext.set(&context);
  }

  ~AutoGCContext() {
    MOZ_ASSERT(TlsGCContext.get() == &context);
    TlsGCContext.set(nullptr);
  }

  JS::GCContext* get() { return &context; }
};

void js::GCParallelTask::runHelperThreadTask(AutoLockHelperThreadState& lock) {
  AutoGCContext gcContext(gc->rt);
  runTask(gcContext.get(), lock);
  MOZ_ASSERT(isFinished(lock));
}

void GCParallelTask::runTask(JS::GCContext* gcx,
                             AutoLockHelperThreadState& lock) {
  // Run the task from either the main thread or a helper thread.

  bool wasDispatched = isDispatched(lock);
  setRunning(lock);

  AutoSetThreadGCUse setUse(gcx, use);

  // The hazard analysis can't tell what the call to func_ will do but it's not
  // allowed to GC.
  JS::AutoSuppressGCAnalysis nogc;

  TimeStamp timeStart = TimeStamp::Now();
  run(lock);
  duration_ = TimeSince(timeStart);

  if (maybeQueueTime_) {
    TimeDuration delay = timeStart - maybeQueueTime_;
    gc->rt->metrics().GC_TASK_START_DELAY_US(delay);
  }

  setFinished(lock);
  gc->onParallelTaskEnd(wasDispatched, lock);
}

void GCParallelTask::onThreadPoolDispatch() {
  MOZ_ASSERT(!dispatchedToThreadPool);
  dispatchedToThreadPool = true;
}

void GCRuntime::dispatchOrQueueParallelTask(
    GCParallelTask* task, const AutoLockHelperThreadState& lock) {
  task->setQueued(lock);
  queuedParallelTasks.ref().insertBack(task, lock);
  maybeDispatchParallelTasks(lock);
}

void GCRuntime::maybeDispatchParallelTasks(
    const AutoLockHelperThreadState& lock) {
  MOZ_ASSERT(maxParallelThreads != 0);
  MOZ_ASSERT(dispatchedParallelTasks <= maxParallelThreads);

  while (dispatchedParallelTasks < maxParallelThreads &&
         !queuedParallelTasks.ref().isEmpty(lock)) {
    GCParallelTask* task = queuedParallelTasks.ref().popFirst(lock);
    task->setDispatched(lock);
    HelperThreadState().submitTask(task, lock);
    dispatchedParallelTasks++;
  }
}

void GCRuntime::onParallelTaskEnd(bool wasDispatched,
                                  const AutoLockHelperThreadState& lock) {
  if (wasDispatched) {
    MOZ_ASSERT(dispatchedParallelTasks != 0);
    dispatchedParallelTasks--;
  }
  maybeDispatchParallelTasks(lock);
}

bool js::GCParallelTask::isIdle() const {
  AutoLockHelperThreadState lock;
  return isIdle(lock);
}

bool js::GCParallelTask::wasStarted() const {
  AutoLockHelperThreadState lock;
  return wasStarted(lock);
}

/* static */
size_t js::gc::GCRuntime::parallelWorkerCount() const {
  return std::min(helperThreadCount.ref(), MaxParallelWorkers);
}
