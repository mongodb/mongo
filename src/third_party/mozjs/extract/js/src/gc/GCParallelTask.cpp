/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "gc/GCParallelTask.h"

#include "mozilla/MathAlgorithms.h"

#include "gc/ParallelWork.h"
#include "vm/HelperThreadState.h"
#include "vm/Runtime.h"
#include "vm/TraceLogging.h"

using namespace js;
using namespace js::gc;

using mozilla::TimeDuration;
using mozilla::TimeStamp;

js::GCParallelTask::~GCParallelTask() {
  // Only most-derived classes' destructors may do the join: base class
  // destructors run after those for derived classes' members, so a join in a
  // base class can't ensure that the task is done using the members. All we
  // can do now is check that someone has previously stopped the task.
  assertIdle();
}

void js::GCParallelTask::startWithLockHeld(AutoLockHelperThreadState& lock) {
  MOZ_ASSERT(CanUseExtraThreads());
  MOZ_ASSERT(HelperThreadState().isInitialized(lock));
  assertIdle();

  setDispatched(lock);
  HelperThreadState().submitTask(this, lock);
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
    AutoUnlockHelperThreadState unlock(lock);
    runFromMainThread();
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

void js::GCParallelTask::join() {
  AutoLockHelperThreadState lock;
  joinWithLockHeld(lock);
}

void js::GCParallelTask::joinWithLockHeld(AutoLockHelperThreadState& lock) {
  // Task has not been started; there's nothing to do.
  if (isIdle(lock)) {
    return;
  }

  // If the task was dispatched but has not yet started then cancel the task and
  // run it from the main thread. This stops us from blocking here when the
  // helper threads are busy with other tasks.
  if (isDispatched(lock)) {
    cancelDispatchedTask(lock);
    AutoUnlockHelperThreadState unlock(lock);
    runFromMainThread();
    return;
  }

  joinRunningOrFinishedTask(lock);
}

void js::GCParallelTask::joinRunningOrFinishedTask(
    AutoLockHelperThreadState& lock) {
  MOZ_ASSERT(isRunning(lock) || isFinished(lock));

  // Wait for the task to run to completion.
  while (!isFinished(lock)) {
    HelperThreadState().wait(lock);
  }

  setIdle(lock);
}

void js::GCParallelTask::cancelDispatchedTask(AutoLockHelperThreadState& lock) {
  MOZ_ASSERT(isDispatched(lock));
  MOZ_ASSERT(isInList());
  remove();
  setIdle(lock);
}

static inline TimeDuration TimeSince(TimeStamp prev) {
  TimeStamp now = ReallyNow();
  // Sadly this happens sometimes.
  MOZ_ASSERT(now >= prev);
  if (now < prev) {
    now = prev;
  }
  return now - prev;
}

void js::GCParallelTask::runFromMainThread() {
  assertIdle();
  MOZ_ASSERT(js::CurrentThreadCanAccessRuntime(gc->rt));
  AutoLockHelperThreadState lock;
  runTask(lock);
}

void js::GCParallelTask::runHelperThreadTask(AutoLockHelperThreadState& lock) {
  TraceLoggerThread* logger = TraceLoggerForCurrentThread();
  AutoTraceLog logCompile(logger, TraceLogger_GC);

  setRunning(lock);

  AutoSetHelperThreadContext usesContext(lock);
  AutoSetContextRuntime ascr(gc->rt);
  gc::AutoSetThreadIsPerformingGC performingGC;
  runTask(lock);

  setFinished(lock);
}

void GCParallelTask::runTask(AutoLockHelperThreadState& lock) {
  // Run the task from either the main thread or a helper thread.

  // The hazard analysis can't tell what the call to func_ will do but it's not
  // allowed to GC.
  JS::AutoSuppressGCAnalysis nogc;

  TimeStamp timeStart = ReallyNow();
  run(lock);
  duration_ = TimeSince(timeStart);
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
