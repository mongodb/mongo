// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/util/modules.h"

#include <cstddef>

namespace mongo {

/**
 * Each cache entry stores a value indicating how much "work" the plan required. This is used
 * for evaluating whether re-planning is necessary. This value is sometimes measured in 'works' and
 * other times measured in total storage engine reads.
 */
struct NumReads {
    size_t value = 0;
};
struct NumWorks {
    size_t value = 0;
};

struct PlanCacheDecisionMetrics {
    PlanCacheDecisionMetrics(NumReads reads, NumWorks works) : reads(reads), works(works) {}

    NumReads reads;
    NumWorks works;
};

}  // namespace mongo
