// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/util/modules.h"

namespace mongo::parallelism_planner {

class ParallelismPlannerContext {
public:
    /**
     * 'maxDoP' is the plan-wide degree of parallelism: the maximum number of workers any part of
     * the plan may run on.
     *
     * A maxDoP of 1 means the plan is serial and the parallelism planner should not run at all.
     */
    explicit ParallelismPlannerContext(int maxDoP) : _maxDoP(maxDoP) {}

    int maxDoP() const {
        return _maxDoP;
    }

private:
    int _maxDoP;
};

}  // namespace mongo::parallelism_planner
