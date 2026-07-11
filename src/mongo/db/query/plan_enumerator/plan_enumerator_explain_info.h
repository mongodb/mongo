// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/util/modules.h"

namespace mongo {

// The information in this struct is used for classic-engine explains.
struct PlanEnumeratorExplainInfo {
    bool hitIndexedOrLimit = false;
    bool hitIndexedAndLimit = false;
    bool hitScanLimit = false;
    bool prunedAnyIndexes = false;

    void merge(PlanEnumeratorExplainInfo other) {
        hitIndexedOrLimit = hitIndexedOrLimit || other.hitIndexedOrLimit;
        hitIndexedAndLimit = hitIndexedAndLimit || other.hitIndexedAndLimit;
        hitScanLimit = hitScanLimit || other.hitScanLimit;
        prunedAnyIndexes = prunedAnyIndexes || other.prunedAnyIndexes;
    }
};
}  // namespace mongo
