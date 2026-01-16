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

#include <boost/optional.hpp>

namespace mongo {

class OperationContext;

namespace admission::execution_control {
enum class MONGO_MOD_PUBLIC OperationType { kRead = 0, kWrite };
class ScopedLowPriorityBackgroundTask;
};  // namespace admission::execution_control

namespace ec = admission::execution_control;

/**
 * Stores state and statistics related to execution control for a given transactional context.
 */
class MONGO_MOD_PUBLIC ExecutionAdmissionContext : public AdmissionContext {
public:
    ExecutionAdmissionContext() = default;
    ExecutionAdmissionContext(const ExecutionAdmissionContext& other);
    ExecutionAdmissionContext& operator=(const ExecutionAdmissionContext& other);

    /**
     * Retrieve the ExecutionAdmissionContext decoration from the provided opCtx.
     */
    static ExecutionAdmissionContext& get(OperationContext* opCtx);

    /**
     * Deprioritization heuristic. Returns true if an operation should be de-prioritized based on
     * the number of ticket acquisitions.
     */
    static bool shouldDeprioritize(std::int32_t admissions);

    /**
     * Returns the total count of delinquent acquisitions across both read and write operations.
     */
    int64_t getDelinquentAcquisitions() const {
        return _readDelinquencyStats.totalDelinquentAcquisitions.loadRelaxed() +
            _writeDelinquencyStats.totalDelinquentAcquisitions.loadRelaxed();
    }

    /**
     * Returns total delinquency duration (ms) across both read and write operations.
     */
    int64_t getTotalAcquisitionDelinquencyMillis() const {
        return _readDelinquencyStats.totalAcquisitionDelinquencyMillis.loadRelaxed() +
            _writeDelinquencyStats.totalAcquisitionDelinquencyMillis.loadRelaxed();
    }

    /**
     * Returns the maximum delinquency duration (ms) observed for any single acquisition.
     */
    int64_t getMaxAcquisitionDelinquencyMillis() const {
        return std::max(_readDelinquencyStats.maxAcquisitionDelinquencyMillis.loadRelaxed(),
                        _writeDelinquencyStats.maxAcquisitionDelinquencyMillis.loadRelaxed());
    }

    /**
     * Marks this operation as having been heuristically deprioritized.
     */
    void priorityLowered() {
        _priorityLowered.store(true);
    }

    /**
     * Returns true if this operation was ever heuristically deprioritized.
     */
    bool getPriorityLowered() const {
        return _priorityLowered.loadRelaxed();
    }

    /**
     * Sets the current operation type (read or write) for stats.
     */
    void setOperationType(ec::OperationType o) {
        _opType = o;
    }

    /**
     * Returns the current operation type.
     */
    ec::OperationType getOperationType() const {
        return _opType;
    }

    /**
     * Returns whether this operation is considered as background task.
     */
    bool isBackgroundTask() const {
        return _isBackgroundTask.loadRelaxed();
    }

    /**
     * Records that a ticket was acquired. Increments totalAdmissions for the current bucket.
     */
    void recordExecutionAcquisition();

    /**
     * Records the time spent waiting in queue before acquiring a ticket.
     */
    void recordExecutionWaitedAcquisition(Microseconds queueTimeMicros);

    /**
     * Records the time spent processing while holding a ticket.
     */
    void recordExecutionRelease(Microseconds processedTimeMicros);

    /**
     * Records that a ticket was held past the delinquency threshold.
     */
    void recordDelinquentAcquisition(Milliseconds delay);

    /**
     * Represents the finalized stats from an operation, ready to be accumulated into global stats.
     */
    struct FinalizedStats {
        // Stats recorded at finalization time (CPU, elapsed, load shed) - only depend on short/long
        // running classification, not read/write type.
        ec::OperationFinalizedStats shortRunning;
        ec::OperationFinalizedStats longRunning;

        // Per-acquisition execution stats by bucket (short/long) and type (read/write).
        ec::OperationExecutionStats readShort;
        ec::OperationExecutionStats readLong;
        ec::OperationExecutionStats writeShort;
        ec::OperationExecutionStats writeLong;

        // This stats are owned by the ticket holder and will be aggregated into the global stats by
        // the ticketing system. That is done because at the end of the operation we know the
        // priority of the operation and to which bucket (low or normal) it belongs.
        ec::DelinquencyStats readDelinquency;
        ec::DelinquencyStats writeDelinquency;

        // Whether this operation was deprioritized.
        bool wasDeprioritized = false;

        // Whether this operation was in a multi-document transaction.
        bool wasInMultiDocTxn = false;

        void clearDelinquencyStats() {
            readDelinquency = ec::DelinquencyStats{};
            writeDelinquency = ec::DelinquencyStats{};
            readShort.delinquencyStats = ec::DelinquencyStats{};
            readLong.delinquencyStats = ec::DelinquencyStats{};
            writeShort.delinquencyStats = ec::DelinquencyStats{};
            writeLong.delinquencyStats = ec::DelinquencyStats{};
        }
    };

    /**
     * Finalizes the operation's stats by recording CPU/elapsed time and returning a snapshot of all
     * accumulated stats. Returns boost::none if stats have already been finalized.
     */
    boost::optional<FinalizedStats> finalizeStats(int64_t cpuUsageMicros, int64_t elapsedMicros);

    /**
     * Sets the inMultiDocTxn flag to the provided value.
     */
    void setInMultiDocTxn(bool inMultiDocTxn) {
        _inMultiDocTxn.store(inMultiDocTxn);

        if (inMultiDocTxn) {
            _wasInMultiDocTxn.store(true);
        }
    }

private:
    friend class ec::ScopedLowPriorityBackgroundTask;

    /**
     * Returns true if this operation should be classified as "long running" based on admission
     * count, priority flags, and exemption status.
     *
     * 'isFinalization' ensures the admission count is adjusted (decremented) to reflect the state
     * at the time of the deprioritization decision, rather than the final total.
     */
    bool _isLongRunning(bool isFinalization = false) const;

    /**
     * Returns a reference to the appropriate OperationExecutionStats based on the current operation
     * type (read/write) and stats bucket (short/long).
     */
    ec::OperationExecutionStats& _getOperationExecutionStats();

    /**
     * Returns true if stats should be recorded for this operation. Operations with exempted
     * admissions (internal) are excluded.
     */
    bool _shouldRecordStats();

    // Delinquency stats by operation type.
    ec::DelinquencyStats _readDelinquencyStats;
    ec::DelinquencyStats _writeDelinquencyStats;

    /**
     * Stats for read and write short/long operations. Whether they're short or long will be
     * determined by the execution contol heuristic. These stats will be accumulated regardless of
     * whether deprioritization is active, that way, some external source could measure the running
     * state of the workload and evaluate whether the activation or not of the de-prioritization
     * will be helpfull at any given time, specially under heavy load.
     */
    ec::OperationExecutionStats _readShortStats;
    ec::OperationExecutionStats _readLongStats;
    ec::OperationExecutionStats _writeShortStats;
    ec::OperationExecutionStats _writeLongStats;

    /**
     * Stats recorded at finalization time (CPU, elapsed, load shed). These are independent of
     * read/write operation type and only depend on short/long running classification.
     */
    ec::OperationFinalizedStats _shortRunningFinalStats;
    ec::OperationFinalizedStats _longRunningFinalStats;

    // True if this operation was ever heuristically deprioritized.
    Atomic<bool> _priorityLowered{false};

    // True if this operation is considered as background task, e.g. index builds, range deletions,
    // and TTL deletions.
    Atomic<bool> _isBackgroundTask{false};

    // Current operation type (read or write).
    ec::OperationType _opType{ec::OperationType::kRead};

    // True if finalizeStats() has been called. Prevents double-counting stats.
    Atomic<bool> _statsFinalized{false};

    // True if the operation was in a multi-document transaction at the time of the ticket
    // acquisition.
    Atomic<bool> _inMultiDocTxn{false};

    // True if the operation was ever in a multi-document transaction. Once set to true, stays true.
    Atomic<bool> _wasInMultiDocTxn{false};
};

namespace admission::execution_control {

/**
 * RAII object to set the background task flag on the ExecutionAdmissionContext decoration and
 * restore it to its original value upon destruction.
 */
class MONGO_MOD_PUBLIC ScopedLowPriorityBackgroundTask {
public:
    ScopedLowPriorityBackgroundTask(OperationContext* opCtx) : _opCtx(opCtx) {
        ExecutionAdmissionContext::get(_opCtx)._isBackgroundTask.store(true);
    }

    ~ScopedLowPriorityBackgroundTask() {
        ExecutionAdmissionContext::get(_opCtx)._isBackgroundTask.store(false);
    }

    ScopedLowPriorityBackgroundTask(const ScopedLowPriorityBackgroundTask&) = delete;
    ScopedLowPriorityBackgroundTask& operator=(const ScopedLowPriorityBackgroundTask&) = delete;

private:
    OperationContext* _opCtx;
};

};  // namespace admission::execution_control

}  // namespace mongo
