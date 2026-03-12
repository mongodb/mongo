/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef js_SliceBudget_h
#define js_SliceBudget_h

#include "mozilla/Assertions.h"
#include "mozilla/Atomics.h"
#include "mozilla/TimeStamp.h"
#include "mozilla/Variant.h"

#include <stdint.h>

#include "jstypes.h"

namespace JS {

struct JS_PUBLIC_API TimeBudget {
  const mozilla::TimeDuration budget;
  mozilla::TimeStamp deadline;  // Calculated when SliceBudget is constructed.

  explicit TimeBudget(mozilla::TimeDuration duration) : budget(duration) {}
  explicit TimeBudget(int64_t milliseconds)
      : budget(mozilla::TimeDuration::FromMilliseconds(milliseconds)) {}

  void setDeadlineFromNow();
};

struct JS_PUBLIC_API WorkBudget {
  const int64_t budget;

  explicit WorkBudget(int64_t work) : budget(work) {}
};

struct UnlimitedBudget {};

/*
 * This class describes a limit to the amount of work to be performed in a GC
 * slice, so that we can return to the mutator without pausing for too long. The
 * budget may be based on a deadline time or an amount of work to be performed,
 * or may be unlimited.
 *
 * To reduce the number of gettimeofday calls, we only check the time every 1000
 * operations.
 */
class JS_PUBLIC_API SliceBudget {
 public:
  using InterruptRequestFlag = mozilla::Atomic<bool, mozilla::Relaxed>;

 private:
  static constexpr int64_t UnlimitedCounter = INT64_MAX;

  // Most calls to isOverBudget will only check the counter value. Every N
  // steps, do a more "expensive" check -- look at the current time and/or
  // check the atomic interrupt flag.
  static constexpr int64_t StepsPerExpensiveCheck = 1000;

  // How many steps to count before checking the time and possibly the interrupt
  // flag.
  int64_t counter = StepsPerExpensiveCheck;

  // External flag to request the current slice to be interrupted
  // (and return isOverBudget() early.) Applies only to time-based budgets.
  InterruptRequestFlag* interruptRequested = nullptr;

  // Configuration
  mozilla::Variant<TimeBudget, WorkBudget, UnlimitedBudget> budget;

  // This SliceBudget is considered interrupted from the time isOverBudget()
  // finds the interrupt flag set.
  bool interrupted = false;

 public:
  // Whether this slice is running in (predicted to be) idle time.
  // Only used for recording in the profile.
  bool idle = false;

  // Whether this slice was given an extended budget, larger than
  // the predicted idle time.
  bool extended = false;

 private:
  explicit SliceBudget(InterruptRequestFlag* irqPtr)
      : counter(irqPtr ? StepsPerExpensiveCheck : UnlimitedCounter),
        interruptRequested(irqPtr),
        budget(UnlimitedBudget()) {}

  bool checkOverBudget();

 public:
  // Use to create an unlimited budget.
  static SliceBudget unlimited() { return SliceBudget(nullptr); }

  // Instantiate as SliceBudget(TimeBudget(n)).
  explicit SliceBudget(TimeBudget time,
                       InterruptRequestFlag* interrupt = nullptr);

  explicit SliceBudget(mozilla::TimeDuration duration,
                       InterruptRequestFlag* interrupt = nullptr)
      : SliceBudget(TimeBudget(duration.ToMilliseconds()), interrupt) {}

  // Instantiate as SliceBudget(WorkBudget(n)).
  explicit SliceBudget(WorkBudget work);

  // Register having performed the given number of steps (counted against a
  // work budget, or progress towards the next time or callback check).
  void step(uint64_t steps = 1) {
    MOZ_ASSERT(steps > 0);
    counter -= steps;
  }

  // Force an "expensive" (time) check on the next call to isOverBudget. Useful
  // when switching between major phases of an operation like a cycle
  // collection.
  void forceCheck() {
    if (isTimeBudget()) {
      counter = 0;
    }
  }

  bool isOverBudget() { return counter <= 0 && checkOverBudget(); }

  bool isWorkBudget() const { return budget.is<WorkBudget>(); }
  bool isTimeBudget() const { return budget.is<TimeBudget>(); }
  bool isUnlimited() const { return budget.is<UnlimitedBudget>(); }

  mozilla::TimeDuration timeBudgetDuration() const {
    return budget.as<TimeBudget>().budget;
  }
  int64_t timeBudget() const { return timeBudgetDuration().ToMilliseconds(); }
  int64_t workBudget() const { return budget.as<WorkBudget>().budget; }

  mozilla::TimeStamp deadline() const {
    return budget.as<TimeBudget>().deadline;
  }

  int describe(char* buffer, size_t maxlen) const;
};

}  // namespace JS

#endif /* js_SliceBudget_h */
