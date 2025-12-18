/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/util/modules.h"

#include <cstdint>
#include <string>


MONGO_MOD_PUBLIC;

namespace mongo::admission::execution_control {

// Forward declaration to avoid circular dependency.
enum class OperationType;

/**
 * Identifies which stats bucket an operation's metrics should be recorded to.
 */
enum class ExecutionStatsBucket {
    kShort,  // Operations that complete quickly (below admission threshold).
    kLong    // Operations that yield frequently or are explicitly low-priority.
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

    AtomicWord<int64_t> totalDelinquentAcquisitions{0};
    AtomicWord<int64_t> totalAcquisitionDelinquencyMillis{0};
    AtomicWord<int64_t> maxAcquisitionDelinquencyMillis{0};
};

/**
 * Stats recorded at operation finalization time (CPU usage, elapsed time, load shed info).
 * These are independent of read/write operation type and only depend on short/long running
 * classification.
 */
class OperationFinalizedStats {
public:
    OperationFinalizedStats() = default;
    OperationFinalizedStats(const OperationFinalizedStats& other);
    OperationFinalizedStats& operator=(const OperationFinalizedStats& other);
    OperationFinalizedStats& operator+=(const OperationFinalizedStats& other);

    void appendStats(BSONObjBuilder& b) const;

    AtomicWord<int64_t> totalCPUUsageMicros{0};
    AtomicWord<int64_t> totalElapsedTimeMicros{0};
    AtomicWord<int64_t> newAdmissionsLoadShed{0};
    AtomicWord<int64_t> totalCPUUsageLoadShed{0};
    AtomicWord<int64_t> totalElapsedTimeMicrosLoadShed{0};
    AtomicWord<int64_t> totalAdmissionsLoadShed{0};
    AtomicWord<int64_t> totalQueuedTimeMicrosLoadShed{0};
};

/**
 * Recollect information about long and short operations on the server.
 * These stats are recorded per-acquisition and depend on both read/write type and short/long
 * running classification.
 */
class OperationExecutionStats {
public:
    OperationExecutionStats() = default;
    OperationExecutionStats(const OperationExecutionStats& other);
    OperationExecutionStats& operator=(const OperationExecutionStats& other);
    OperationExecutionStats& operator+=(const OperationExecutionStats& other);

    void appendStats(BSONObjBuilder& b) const;

    AtomicWord<int64_t> totalTimeQueuedMicros{0};
    AtomicWord<int64_t> totalTimeProcessingMicros{0};
    AtomicWord<int64_t> totalAdmissions{0};
    AtomicWord<int64_t> newAdmissions{0};
    DelinquencyStats delinquencyStats;
};

/**
 * Aggregated execution stats for the ticketing system.
 * Contains both per-acquisition stats (by read/write and short/long) and finalized stats
 * (by short/long running only).
 */
struct AggregatedExecutionStats {
    // Finalized stats (CPU, elapsed, load shed) - only depend on short/long running classification.
    OperationFinalizedStats shortRunning;
    OperationFinalizedStats longRunning;

    // Per-acquisition stats by bucket (short/long) and type (read/write).
    OperationExecutionStats readShort;
    OperationExecutionStats readLong;
    OperationExecutionStats writeShort;
    OperationExecutionStats writeLong;
};

}  // namespace mongo::admission::execution_control
