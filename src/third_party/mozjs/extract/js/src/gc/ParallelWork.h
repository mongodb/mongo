/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef gc_ParallelWork_h
#define gc_ParallelWork_h

#include "mozilla/Maybe.h"

#include <algorithm>

#include "gc/GCParallelTask.h"
#include "gc/GCRuntime.h"
#include "js/SliceBudget.h"
#include "vm/HelperThreads.h"

namespace js {

namespace gcstats {
enum class PhaseKind : uint8_t;
}

namespace gc {

template <typename WorkItem>
using ParallelWorkFunc = size_t (*)(GCRuntime*, const WorkItem&);

// A GCParallelTask task that executes WorkItems from a WorkItemIterator.
//
// The WorkItemIterator class must supply done(), next() and get() methods. The
// get() method must return WorkItems objects.
template <typename WorkItem, typename WorkItemIterator>
class ParallelWorker : public GCParallelTask {
 public:
  using WorkFunc = ParallelWorkFunc<WorkItem>;

  ParallelWorker(GCRuntime* gc, gcstats::PhaseKind phaseKind, GCUse use,
                 WorkFunc func, WorkItemIterator& work,
                 const JS::SliceBudget& budget, AutoLockHelperThreadState& lock)
      : GCParallelTask(gc, phaseKind, use),
        func_(func),
        work_(work),
        budget_(budget),
        item_(work.get()) {
    // Consume a work item on creation so that we can stop creating workers if
    // the number of workers exceeds the number of work items.
    work.next();
  }

  void run(AutoLockHelperThreadState& lock) {
    AutoUnlockHelperThreadState unlock(lock);

    for (;;) {
      size_t steps = func_(gc, item_);
      budget_.step(std::max(steps, size_t(1)));
      if (budget_.isOverBudget()) {
        break;
      }

      AutoLockHelperThreadState lock;
      if (work().done()) {
        break;
      }

      item_ = work().get();
      work().next();
    }
  }

 private:
  WorkItemIterator& work() { return work_.ref(); }

  // A function to execute work items on the helper thread.
  WorkFunc func_;

  // An iterator which produces work items to execute.
  HelperThreadLockData<WorkItemIterator&> work_;

  // The budget that determines how long to run for.
  JS::SliceBudget budget_;

  // The next work item to process.
  WorkItem item_;
};

static constexpr size_t MaxParallelWorkers = 8;

// An RAII class that starts a number of ParallelWorkers and waits for them to
// finish.
template <typename WorkItem, typename WorkItemIterator>
class MOZ_RAII AutoRunParallelWork {
 public:
  using Worker = ParallelWorker<WorkItem, WorkItemIterator>;
  using WorkFunc = ParallelWorkFunc<WorkItem>;

  AutoRunParallelWork(GCRuntime* gc, WorkFunc func,
                      gcstats::PhaseKind phaseKind, GCUse use,
                      WorkItemIterator& work, const JS::SliceBudget& budget,
                      AutoLockHelperThreadState& lock)
      : gc(gc), phaseKind(phaseKind), lock(lock), tasksStarted(0) {
    size_t workerCount = gc->parallelWorkerCount();
    MOZ_ASSERT(workerCount <= MaxParallelWorkers);
    MOZ_ASSERT_IF(workerCount == 0, work.done());

    for (size_t i = 0; i < workerCount && !work.done(); i++) {
      tasks[i].emplace(gc, phaseKind, use, func, work, budget, lock);
      gc->startTask(*tasks[i], lock);
      tasksStarted++;
    }
  }

  ~AutoRunParallelWork() {
    gHelperThreadLock.assertOwnedByCurrentThread();

    for (size_t i = 0; i < tasksStarted; i++) {
      gc->joinTask(*tasks[i], lock);
    }
    for (size_t i = tasksStarted; i < MaxParallelWorkers; i++) {
      MOZ_ASSERT(tasks[i].isNothing());
    }
  }

 private:
  GCRuntime* gc;
  gcstats::PhaseKind phaseKind;
  AutoLockHelperThreadState& lock;
  size_t tasksStarted;
  mozilla::Maybe<Worker> tasks[MaxParallelWorkers];
};

} /* namespace gc */
} /* namespace js */

#endif /* gc_ParallelWork_h */
