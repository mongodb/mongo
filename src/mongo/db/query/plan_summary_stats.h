/**
 *    Copyright (C) 2018-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#pragma once

#include "mongo/util/container_size_helper.h"
#include <string>

namespace mongo {

/**
 * A container for the summary statistics that the profiler, slow query log, and
 * other non-explain debug mechanisms may want to collect.
 */
struct PlanSummaryStats {
    /**
     * Helper method to accumulate the plan summary stats from the input source.
     */
    void accumulate(const PlanSummaryStats& statsIn) {
        // Attributes replanReason and fromMultiPlanner have been intentionally skipped as they
        // always describe the left-hand side (or "local") collection.
        // Consider $lookup case. $lookup runtime plan selection may happen against the foreign
        // collection an arbitrary number of times. A single value of 'replanReason' and
        // 'fromMultiPlanner' can't really report correctly on the behavior of arbitrarily many
        // occurrences of runtime planning for a single query.

        nReturned += statsIn.nReturned;
        totalKeysExamined += statsIn.totalKeysExamined;
        totalDocsExamined += statsIn.totalDocsExamined;
        collectionScans += statsIn.collectionScans;
        collectionScansNonTailable += statsIn.collectionScansNonTailable;
        hasSortStage |= statsIn.hasSortStage;
        usedDisk |= statsIn.usedDisk;
        planFailed |= statsIn.planFailed;
        indexesUsed.insert(statsIn.indexesUsed.begin(), statsIn.indexesUsed.end());
    }

    uint64_t estimateObjectSizeInBytes() const {
        auto strSize = [](const std::string& str) {
            return str.capacity() * sizeof(std::string::value_type);
        };

        return sizeof(*this) +
            container_size_helper::estimateObjectSizeInBytes(
                   indexesUsed, strSize, false /* includeShallowSize */) +
            (replanReason ? strSize(*replanReason) : 0);
    }

    // The number of results returned by the plan.
    size_t nReturned = 0U;

    // The total number of index keys examined by the plan.
    size_t totalKeysExamined = 0U;

    // The total number of documents examined by the plan.
    size_t totalDocsExamined = 0U;

    // The number of collection scans that occur during execution. Note that more than one
    // collection scan may happen during execution (e.g. for $lookup execution).
    long long collectionScans = 0;

    // The number of collection scans that occur during execution which are nontailable. Note that
    // more than one collection scan may happen during execution (e.g. for $lookup execution).
    long long collectionScansNonTailable = 0;

    // Time elapsed while executing this plan.
    long long executionTimeMillisEstimate = 0;

    // Did this plan use an in-memory sort stage?
    bool hasSortStage = false;

    // Did this plan use disk space?
    bool usedDisk = false;

    // Did this plan failed during execution?
    bool planFailed = false;

    // The names of each index used by the plan.
    std::set<std::string> indexesUsed;

    // Was this plan a result of using the MultiPlanStage to select a winner among several
    // candidates?
    bool fromMultiPlanner = false;

    // Was a replan triggered during the execution of this query?
    std::optional<std::string> replanReason;
};

}  // namespace mongo
