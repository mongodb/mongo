/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef js_SliceBudget_h
#define js_SliceBudget_h

#include "mozilla/Assertions.h"
#include "mozilla/TimeStamp.h"
#include "mozilla/Variant.h"

#include <stdint.h>

#include "jstypes.h"

namespace js {

struct JS_PUBLIC_API TimeBudget {
  const int64_t budget;
  mozilla::TimeStamp deadline;  // Calculated when SliceBudget is constructed.

  explicit TimeBudget(int64_t milliseconds) : budget(milliseconds) {}
  explicit TimeBudget(mozilla::TimeDuration duration)
      : TimeBudget(duration.ToMilliseconds()) {}
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
  static const intptr_t UnlimitedCounter = INTPTR_MAX;
  static const intptr_t DefaultStepsPerTimeCheck = 1000;

  mozilla::Variant<TimeBudget, WorkBudget, UnlimitedBudget> budget;
  int64_t stepsPerTimeCheck = DefaultStepsPerTimeCheck;

  int64_t counter;

  SliceBudget() : budget(UnlimitedBudget()), counter(UnlimitedCounter) {}

  bool checkOverBudget();

 public:
  // Use to create an unlimited budget.
  static SliceBudget unlimited() { return SliceBudget(); }

  // Instantiate as SliceBudget(TimeBudget(n)).
  explicit SliceBudget(TimeBudget time,
                       int64_t stepsPerTimeCheck = DefaultStepsPerTimeCheck);

  // Instantiate as SliceBudget(WorkBudget(n)).
  explicit SliceBudget(WorkBudget work);

  explicit SliceBudget(mozilla::TimeDuration time)
      : SliceBudget(TimeBudget(time.ToMilliseconds())) {}

  // Register having performed the given number of steps (counted against a
  // work budget, or progress towards the next time or callback check).
  void step(uint64_t steps = 1) {
    MOZ_ASSERT(steps > 0);
    counter -= steps;
  }

  // Do enough steps to force an "expensive" (time and/or callback) check on
  // the next call to isOverBudget. Useful when switching between major phases
  // of an operation like a cycle collection.
  void stepAndForceCheck() {
    if (!isUnlimited()) {
      counter = 0;
    }
  }

  bool isOverBudget() { return counter <= 0 && checkOverBudget(); }

  bool isWorkBudget() const { return budget.is<WorkBudget>(); }
  bool isTimeBudget() const { return budget.is<TimeBudget>(); }
  bool isUnlimited() const { return budget.is<UnlimitedBudget>(); }

  int64_t timeBudget() const { return budget.as<TimeBudget>().budget; }
  int64_t workBudget() const { return budget.as<WorkBudget>().budget; }

  int describe(char* buffer, size_t maxlen) const;
};

}  // namespace js

#endif /* js_SliceBudget_h */
