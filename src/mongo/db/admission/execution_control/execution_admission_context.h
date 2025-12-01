/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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

#include "mongo/db/admission/execution_control/execution_control_stats.h"
#include "mongo/util/concurrency/admission_context.h"
#include "mongo/util/modules.h"

namespace mongo {

class OperationContext;
namespace admission::execution_control {
enum class MONGO_MOD_PUBLIC OperationType { kRead = 0, kWrite };
};  // namespace admission::execution_control

/**
 * Stores state and statistics related to execution control for a given transactional context.
 */
class MONGO_MOD_PUBLIC ExecutionAdmissionContext : public AdmissionContext {
public:
    // Deprioritization heuristic. Returns true if an operation should be de-prioritized.
    static bool shouldDeprioritize(ExecutionAdmissionContext* admCtx);

    ExecutionAdmissionContext() = default;
    ExecutionAdmissionContext(const ExecutionAdmissionContext& other);
    ExecutionAdmissionContext& operator=(const ExecutionAdmissionContext& other);

    int64_t getDelinquentAcquisitions() const {
        return _readDelinquencyStats.totalDelinquentAcquisitions.loadRelaxed() +
            _writeDelinquencyStats.totalDelinquentAcquisitions.loadRelaxed();
    }

    int64_t getTotalAcquisitionDelinquencyMillis() const {
        return _readDelinquencyStats.totalAcquisitionDelinquencyMillis.loadRelaxed() +
            _writeDelinquencyStats.totalAcquisitionDelinquencyMillis.loadRelaxed();
    }

    int64_t getMaxAcquisitionDelinquencyMillis() const {
        return std::max(_readDelinquencyStats.maxAcquisitionDelinquencyMillis.loadRelaxed(),
                        _writeDelinquencyStats.maxAcquisitionDelinquencyMillis.loadRelaxed());
    }

    void priorityLowered() {
        _priorityLowered.store(true);
    }

    /**
     * Getters for stats related to delinquency in acquiring read and write tickets.
     */
    const admission::execution_control::DelinquencyStats& readDelinquencyStats() const {
        return _readDelinquencyStats;
    }
    const admission::execution_control::DelinquencyStats& writeDelinquencyStats() const {
        return _writeDelinquencyStats;
    }

    bool getPriorityLowered() const {
        return _priorityLowered.loadRelaxed();
    }

    /**
     * Getters for stats related to short and long operation executions for read and write waiting
     * sets.
     */
    const admission::execution_control::OperationExecutionStats& readShortExecutionStats() const {
        return _readShortStats;
    }

    const admission::execution_control::OperationExecutionStats& readLongExecutionStats() const {
        return _readLongStats;
    }

    const admission::execution_control::OperationExecutionStats& writeShortExecutionStats() const {
        return _writeShortStats;
    }

    const admission::execution_control::OperationExecutionStats& writeLongExecutionStats() const {
        return _writeLongStats;
    }

    /**
     * Retrieve the ExecutionAdmissionContext decoration the provided OperationContext
     */
    static ExecutionAdmissionContext& get(OperationContext* opCtx);

    void recordOperationLoadShed();

    void setOperationType(admission::execution_control::OperationType o) {
        _opType = o;
    }

    admission::execution_control::OperationType getOperationType() const {
        return _opType;
    }

    /**
     * Indicates that an acquisition was done.
     */
    void recordExecutionAcquisition();

    /**
     * Indicates the read or write operation acquisition queue time.
     */
    void recordExecutionWaitedAcquisition(Microseconds queueTimeMicros);

    /**
     * Indicates the read or write operation processsed time.
     */
    void recordExecutionRelease(Microseconds processedTimeMicros);

    /**
     * Indicates that a read or write ticket was held for 'delay' milliseconds past due.
     */
    void recordDelinquentAcquisition(Milliseconds delay);

    /**
     * Indicates the total cpu usage and elapsed time of an entire operation.
     */
    void recordExecutionCPUUsageAndElapsedTime(int64_t cpuUsageMicros, int64_t elapsedMicros);

private:
    bool _isUserOperation();
    admission::execution_control::OperationExecutionStats& _getOperationExecutionStats();
    admission::execution_control::DelinquencyStats& _getDelinquencyStats();
    void _recordDelinquentAcquisition(Milliseconds delay,
                                      admission::execution_control::DelinquencyStats& stats);
    void _recordExecutionTimeQueued(Microseconds queueTimeMillis,
                                    admission::execution_control::OperationExecutionStats& stats);
    void _recordExecutionAcquisition(admission::execution_control::OperationExecutionStats& stats);
    void _recordExecutionProcessedTime(
        Microseconds processedTimeMillis,
        admission::execution_control::OperationExecutionStats& stats);
    void _recordExecutionShed(admission::execution_control::OperationExecutionStats& stats);
    void _recordExecutionCPUUsageAndElapsedTime(
        int64_t cpuUsageMicros,
        int64_t elapsedMicros,
        admission::execution_control::OperationExecutionStats& stats);
    void _recordExecutionCPUUsageAndElapsedTimeShed(
        int64_t cpuUsage,
        int64_t elapsedMicros,
        admission::execution_control::OperationExecutionStats& stats);

    admission::execution_control::DelinquencyStats _readDelinquencyStats;
    admission::execution_control::DelinquencyStats _writeDelinquencyStats;

    /**
     * Stats for read and write short/long operations. Whether they're short or long will be
     * determined by the execution contol heuristic. These stats will be accumulated regardless of
     * whether deprioritization is active, that way, some external source could measure the running
     * state of the workload and evaluate whether the activation or not of the de-prioritization
     * will be helpfull at any given time, specially under heavy load.
     */
    admission::execution_control::OperationExecutionStats _readShortStats;
    admission::execution_control::OperationExecutionStats _readLongStats;
    admission::execution_control::OperationExecutionStats _writeShortStats;
    admission::execution_control::OperationExecutionStats _writeLongStats;

    Atomic<bool> _priorityLowered{false};
    admission::execution_control::OperationType _opType;
};

}  // namespace mongo
