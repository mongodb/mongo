// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/exec/container_size_helper.h"
#include "mongo/db/pipeline/spilling/spilling_stats.h"
#include "mongo/util/duration.h"
#include "mongo/util/modules.h"

#include <set>
#include <string>

#include <absl/container/flat_hash_map.h>

namespace mongo {

// The precision of 'executionTime'. Note that 'kNanos' precision requires a precise timer which
// is also slower than the default timer.
enum class QueryExecTimerPrecision { kNoTiming = 0, kNanos, kMillis };

struct QueryExecTime {
    // Precision/unit of 'executionTimeEstimate'.
    QueryExecTimerPrecision precision = QueryExecTimerPrecision::kNoTiming;
    // Time elapsed while executing this plan.
    Nanoseconds executionTimeEstimate{0};
};

/**
 * Appends execution-time fields derived from `execTime` to `bob`:
 *   * kNoTiming -> nothing is appended,
 *   * kMillis   -> `executionTimeMillisEstimate`,
 *   * kNanos    -> `executionTimeMillisEstimate`, `executionTimeMicros`, `executionTimeNanos`.
 */
inline void appendExecutionTimeFields(BSONObjBuilder& bob, const QueryExecTime& execTime) {
    if (execTime.precision == QueryExecTimerPrecision::kMillis) {
        bob.appendNumber("executionTimeMillisEstimate",
                         durationCount<Milliseconds>(execTime.executionTimeEstimate));
    } else if (execTime.precision == QueryExecTimerPrecision::kNanos) {
        bob.appendNumber("executionTimeMillisEstimate",
                         durationCount<Milliseconds>(execTime.executionTimeEstimate));
        bob.appendNumber("executionTimeMicros",
                         durationCount<Microseconds>(execTime.executionTimeEstimate));
        bob.appendNumber("executionTimeNanos",
                         durationCount<Nanoseconds>(execTime.executionTimeEstimate));
    }
}

/**
 * A container for the summary statistics that the profiler, slow query log, and
 * other non-explain debug mechanisms may want to collect.
 */
struct [[MONGO_MOD_PUBLIC]] PlanSummaryStats {

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
    QueryExecTime executionTime;

    // Did this plan use an in-memory sort stage?
    bool hasSortStage = false;

    // Did this plan use disk space?
    bool usedDisk = false;

    // Stages that report SpillingStats.
    enum class SpillingStage {
        BUCKET_AUTO,
        GEO_NEAR,
        GRAPH_LOOKUP,
        GROUP,
        HASH_LOOKUP,
        HASH_JOIN,
        SET_WINDOW_FIELDS,
        SORT,
        TEXT_OR,
    };

    // The accumulated spilling statistics per stage type.
    absl::flat_hash_map<SpillingStage, SpillingStats> spillingStatsPerStage;

    // The amount of data we've sorted in bytes.
    size_t sortTotalDataSizeBytes = 0;

    // The number of keys that we've sorted.
    long long keysSorted = 0;

    // Did this plan failed during execution?
    bool planFailed = false;

    // The names of each index used by the plan.
    std::set<std::string> indexesUsed;

    // Was this plan a result of using the MultiPlanStage to select a winner among several
    // candidates?
    bool fromMultiPlanner = false;

    // Was this plan recovered from the cache?
    bool fromPlanCache = false;
    // Was a replan triggered during the execution of this query?
    boost::optional<std::string> replanReason;

    // Score calculated for the plan by PlanRanker. Only set if there were multiple candidate plans
    // and allPlansExecution verbosity mode is selected.
    boost::optional<double> score;
};

}  // namespace mongo
