// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/platform/atomic.h"
#include "mongo/util/duration.h"
#include "mongo/util/histogram.h"
#include "mongo/util/modules.h"

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>


[[MONGO_MOD_PUBLIC]];

namespace mongo::admission::execution_control {
using namespace std::literals::string_view_literals;

// Forward declaration to avoid circular dependency.
enum class OperationType;

/**
 * Identifies which stats bucket an operation's metrics should be recorded to.
 */
enum class ExecutionStatsBucket {
    kNonDeprioritizable,  // Operations that don't meet deprioritization criteria.
    kDeprioritizable      // Operations that meet deprioritization criteria.
};

/**
 * Histogram tracking the distribution of completed operations by their total admission count.
 *
 * Each bucket counts operations that had a specific range of ticket acquisitions during their
 * lifetime. This helps identify the distribution of deprioritizable vs non-deprioritizable
 * operations.
 *
 * Bucket ranges: 1-2, 3-4, 5-8, 9-16, 17-32, 33-64, 65-128, 129-256, 257-512, 513-1024, 1025+
 */
class AdmissionsHistogram {
public:
    static constexpr size_t kNumBuckets = 11;

    static constexpr std::array<std::string_view, kNumBuckets> kBucketNames = {"1-2"sv,
                                                                               "3-4"sv,
                                                                               "5-8"sv,
                                                                               "9-16"sv,
                                                                               "17-32"sv,
                                                                               "33-64"sv,
                                                                               "65-128"sv,
                                                                               "129-256"sv,
                                                                               "257-512"sv,
                                                                               "513-1024"sv,
                                                                               "1025+"sv};

    AdmissionsHistogram() = default;

    /**
     * Records a completed operation with the given number of admissions.
     */
    void record(int32_t admissions);

    /**
     * Appends the histogram to a BSON builder.
     */
    void appendStats(BSONObjBuilder& b) const;

private:
    /**
     * Returns the bucket index for a given admission count.
     */
    size_t _getBucketIndex(int32_t admissions);

    std::array<Atomic<int64_t>, kNumBuckets> _buckets{};
};

/**
 * Histogram tracking the distribution of per-operation queue wait times for a single ticket queue.
 *
 * It's a wrapper around the Histogram<t> for a convenient use of appendStats and a sanitization
 * point for recording a new datapoint.
 */
class QueueWaitTimeHistogram {
public:
    // Lower-bound partitions (microseconds). The implicit first bucket below the smallest
    // partition captures the "did not wait" (0us) samples.
    static std::vector<int64_t> partitions() {
        return {1,       10,      25,        50,        100,       250,       500,
                1'000,   2'500,   5'000,     10'000,    25'000,    50'000,    100'000,
                250'000, 500'000, 1'000'000, 2'500'000, 5'000'000, 10'000'000};
    }

    QueueWaitTimeHistogram() : _hist(partitions()) {}

    void record(Microseconds queueWaitTime);
    void appendStats(BSONArrayBuilder& arr) const;

private:
    Histogram<int64_t> _hist;
};

/**
 * Recollect the number of operations that hold the ticket longer than a defined threshold.
 */
class DelinquencyStats {
public:
    DelinquencyStats() = default;
    DelinquencyStats(int64_t, int64_t, int64_t);
    DelinquencyStats(const DelinquencyStats& other);
    DelinquencyStats& operator=(const DelinquencyStats& other);
    DelinquencyStats& operator+=(const DelinquencyStats& other);

    void appendStats(BSONObjBuilder& b) const;

    Atomic<int64_t> totalDelinquentAcquisitions{0};
    Atomic<int64_t> totalAcquisitionDelinquencyMillis{0};
    Atomic<int64_t> maxAcquisitionDelinquencyMillis{0};
};

/**
 * Stats recorded at operation finalization time (CPU usage, elapsed time, load shed info).
 * These are independent of read/write operation type and only depend on deprioritizable/not
 * deprioritizable classification.
 */
class OperationFinalizedStats {
public:
    OperationFinalizedStats() = default;
    OperationFinalizedStats(const OperationFinalizedStats& other);
    OperationFinalizedStats& operator=(const OperationFinalizedStats& other);
    OperationFinalizedStats& operator+=(const OperationFinalizedStats& other);

    void appendStats(BSONObjBuilder& b) const;

    Atomic<int64_t> totalCPUUsageMicros{0};
    Atomic<int64_t> totalElapsedTimeMicros{0};
    Atomic<int64_t> totalOpsFinished{0};
    Atomic<int64_t> totalOpsLoadShed{0};
    Atomic<int64_t> totalCPUUsageLoadShed{0};
    Atomic<int64_t> totalElapsedTimeMicrosLoadShed{0};
    Atomic<int64_t> totalAdmissionsLoadShed{0};
    Atomic<int64_t> totalQueuedTimeMicrosLoadShed{0};
};

/**
 * Recollect information about deprioritizable and non-deprioritizable operations on the server.
 * These stats are recorded per-acquisition and depend on both read/write type and
 * deprioritizable/non-deprioritizable classification.
 */
class OperationExecutionStats {
public:
    OperationExecutionStats() = default;
    OperationExecutionStats(const OperationExecutionStats& other);
    OperationExecutionStats& operator=(const OperationExecutionStats& other);
    OperationExecutionStats& operator+=(const OperationExecutionStats& other);

    void appendStats(BSONObjBuilder& b) const;

    Atomic<int64_t> totalTimeQueuedMicros{0};
    Atomic<int64_t> totalTimeProcessingMicros{0};
    Atomic<int64_t> totalAdmissions{0};
    Atomic<int64_t> totalNormalPriorityAdmissions{0};
    Atomic<int64_t> totalLowPriorityAdmissions{0};
    DelinquencyStats delinquencyStats;
};

/**
 * Aggregated execution stats for the ticketing system.
 * Contains both per-acquisition stats (by read/write and deprioritizable/non-deprioritizable) and
 * finalized stats (by deprioritizable/non-deprioritizable only).
 */
struct AggregatedExecutionStats {
    // Finalized stats (CPU, elapsed, load shed) - only depend on deprioritizable classification.
    OperationFinalizedStats nonDeprioritizable;
    OperationFinalizedStats deprioritizable;

    // Per-acquisition stats by bucket (deprioritizable/non-deprioritizable) and type (read/write).
    OperationExecutionStats readNonDeprioritizable;
    OperationExecutionStats readDeprioritizable;
    OperationExecutionStats writeNonDeprioritizable;
    OperationExecutionStats writeDeprioritizable;
};

}  // namespace mongo::admission::execution_control
