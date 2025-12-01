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

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/util/modules.h"

#include <cstdint>
#include <string>

#pragma once

namespace mongo::admission::execution_control {
/**
 * Recollect the number of operations that hold the ticket longer than a defined threshold.
 */
class MONGO_MOD_PUBLIC DelinquencyStats {
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
 * Recollect information about long and short operations on the server.
 */
class MONGO_MOD_PUBLIC OperationExecutionStats {
public:
    OperationExecutionStats() = default;
    OperationExecutionStats(const OperationExecutionStats& other);
    OperationExecutionStats& operator=(const OperationExecutionStats& other);
    OperationExecutionStats& operator+=(const OperationExecutionStats& other);

    void appendStats(BSONObjBuilder& b) const;

    AtomicWord<int64_t> totalElapsedTimeMicros{0};
    AtomicWord<int64_t> totalTimeQueuedMicros{0};
    AtomicWord<int64_t> totalTimeProcessingMicros{0};
    AtomicWord<int64_t> totalCPUUsageMicros{0};
    AtomicWord<int64_t> totalAdmissions{0};
    AtomicWord<int64_t> newAdmissions{0};
    AtomicWord<int64_t> newAdmissionsLoadShed{0};
    AtomicWord<int64_t> totalCPUUsageLoadShed{0};
    AtomicWord<int64_t> totalElapsedTimeMicrosLoadShed{0};
    AtomicWord<int64_t> totalQueuedTimeMicrosLoadShed{0};
    AtomicWord<int64_t> totalAdmissionsLoadShed{0};
    DelinquencyStats delinquencyStats;
};

}  // namespace mongo::admission::execution_control
