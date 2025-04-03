/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef gc_ParallelMarking_h
#define gc_ParallelMarking_h

#include "mozilla/Atomics.h"
#include "mozilla/DoublyLinkedList.h"
#include "mozilla/TimeStamp.h"

#include "gc/GCMarker.h"
#include "gc/GCParallelTask.h"
#include "js/HeapAPI.h"
#include "js/SliceBudget.h"
#include "threading/ConditionVariable.h"
#include "threading/ProtectedData.h"

namespace js {

class AutoLockGC;
class AutoLockHelperThreadState;
class AutoUnlockGC;

namespace gc {

class ParallelMarkTask;

// Per-runtime parallel marking state.
//
// This class is used on the main thread and coordinates parallel marking using
// several helper threads running ParallelMarkTasks.
//
// This uses a work-requesting approach. Threads mark until they run out of
// work and then add themselves to a list of waiting tasks and block. Running
// tasks with enough work may donate work to a waiting task and resume it.
class MOZ_STACK_CLASS ParallelMarker {
 public:
  explicit ParallelMarker(GCRuntime* gc);

  bool mark(SliceBudget& sliceBudget);

  using AtomicCount = mozilla::Atomic<uint32_t, mozilla::Relaxed>;
  AtomicCount& waitingTaskCountRef() { return waitingTaskCount; }
  bool hasWaitingTasks() { return waitingTaskCount != 0; }
  void donateWorkFrom(GCMarker* src);

 private:
  bool markOneColor(MarkColor color, SliceBudget& sliceBudget);

  bool hasWork(MarkColor color) const;

  void addTask(ParallelMarkTask* task, const AutoLockGC& lock);

  void addTaskToWaitingList(ParallelMarkTask* task, const AutoLockGC& lock);
#ifdef DEBUG
  bool isTaskInWaitingList(const ParallelMarkTask* task,
                           const AutoLockGC& lock) const;
#endif

  bool hasActiveTasks(const AutoLockGC& lock) const { return activeTasks; }
  void incActiveTasks(ParallelMarkTask* task, const AutoLockGC& lock);
  void decActiveTasks(ParallelMarkTask* task, const AutoLockGC& lock);

  size_t workerCount() const;

  friend class ParallelMarkTask;

  GCRuntime* const gc;

  using ParallelMarkTaskList = mozilla::DoublyLinkedList<ParallelMarkTask>;
  GCLockData<ParallelMarkTaskList> waitingTasks;
  AtomicCount waitingTaskCount;

  GCLockData<size_t> activeTasks;
};

// A helper thread task that performs parallel marking.
class alignas(TypicalCacheLineSize) ParallelMarkTask
    : public GCParallelTask,
      public mozilla::DoublyLinkedListElement<ParallelMarkTask> {
 public:
  friend class ParallelMarker;

  ParallelMarkTask(ParallelMarker* pm, GCMarker* marker, MarkColor color,
                   const SliceBudget& budget);
  ~ParallelMarkTask();

  void run(AutoLockHelperThreadState& lock) override;

  void recordDuration() override;

 private:
  void markOrRequestWork(AutoLockGC& lock);
  bool tryMarking(AutoLockGC& lock);
  bool requestWork(AutoLockGC& lock);

  void waitUntilResumed(AutoLockGC& lock);
  void resume();
  void resumeOnFinish(const AutoLockGC& lock);

  bool hasWork() const;

  // The following fields are only accessed by the marker thread:
  ParallelMarker* const pm;
  GCMarker* const marker;
  AutoSetMarkColor color;
  SliceBudget budget;
  ConditionVariable resumed;

  GCLockData<bool> isWaiting;

  // Length of time this task spent blocked waiting for work.
  MainThreadOrGCTaskData<mozilla::TimeDuration> markTime;
  MainThreadOrGCTaskData<mozilla::TimeDuration> waitTime;
};

}  // namespace gc
}  // namespace js

#endif /* gc_ParallelMarking_h */
