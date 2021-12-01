/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef js_SliceBudget_h
#define js_SliceBudget_h

#include <stdint.h>

#include "jstypes.h"

namespace js {

struct JS_PUBLIC_API(TimeBudget)
{
    int64_t budget;

    explicit TimeBudget(int64_t milliseconds) { budget = milliseconds; }
};

struct JS_PUBLIC_API(WorkBudget)
{
    int64_t budget;

    explicit WorkBudget(int64_t work) { budget = work; }
};

/*
 * This class records how much work has been done in a given collection slice,
 * so that we can return before pausing for too long. Some slices are allowed
 * to run for unlimited time, and others are bounded. To reduce the number of
 * gettimeofday calls, we only check the time every 1000 operations.
 */
class JS_PUBLIC_API(SliceBudget)
{
    static const int64_t unlimitedDeadline = INT64_MAX;
    static const intptr_t unlimitedStartCounter = INTPTR_MAX;

    bool checkOverBudget();

    SliceBudget();

  public:
    // Memory of the originally requested budget. If isUnlimited, neither of
    // these are in use. If deadline==0, then workBudget is valid. Otherwise
    // timeBudget is valid.
    TimeBudget timeBudget;
    WorkBudget workBudget;

    int64_t deadline; /* in microseconds */
    intptr_t counter;

    static const intptr_t CounterReset = 1000;

    static const int64_t UnlimitedTimeBudget = -1;
    static const int64_t UnlimitedWorkBudget = -1;

    /* Use to create an unlimited budget. */
    static SliceBudget unlimited() { return SliceBudget(); }

    /* Instantiate as SliceBudget(TimeBudget(n)). */
    explicit SliceBudget(TimeBudget time);

    /* Instantiate as SliceBudget(WorkBudget(n)). */
    explicit SliceBudget(WorkBudget work);

    void makeUnlimited() {
        deadline = unlimitedDeadline;
        counter = unlimitedStartCounter;
    }

    void step(intptr_t amt = 1) {
        counter -= amt;
    }

    bool isOverBudget() {
        if (counter > 0)
            return false;
        return checkOverBudget();
    }

    bool isWorkBudget() const { return deadline == 0; }
    bool isTimeBudget() const { return deadline > 0 && !isUnlimited(); }
    bool isUnlimited() const { return deadline == unlimitedDeadline; }

    int describe(char* buffer, size_t maxlen) const;
};

} // namespace js

#endif /* js_SliceBudget_h */
