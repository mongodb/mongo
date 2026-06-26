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

#include "mongo/db/admission/execution_control/execution_admission_type_gen.h"
#include "mongo/db/admission/execution_control/execution_control_stats.h"
#include "mongo/db/admission/ticketing/admission_context.h"
#include "mongo/util/modules.h"

#include <cstdint>
#include <functional>

#include <boost/optional.hpp>

namespace mongo {

class OperationContext;

namespace admission::execution_control {
enum class MONGO_MOD_PUBLIC OperationType { kRead = 0, kWrite };
class ScopedTaskTypeModifierBase;
class ScopedTicketAdmissionStatsRecorder;
};  // namespace admission::execution_control

namespace ec = admission::execution_control;

/**
 * Stores state and statistics related to execution control for a given transactional context.
 */
class MONGO_MOD_PUBLIC ExecutionAdmissionContext : public AdmissionContext {
public:
    /**
     * Task type to fine tune the deprioritization
     */
    enum class TaskType {
        Default,             // The task can be deprioritized (if not exempt) based on the
                             // deprioritization heuristics
        NonDeprioritizable,  // The task should never be deprioritized (though it's not exempt, so
                             // waits for a ticket)
        Background,          // The task is considered as a background task, e.g. index builds,
                             // range deletions, and TTL deletions.
    };

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
     * Append the `executionAdmissionContextType` field to the request if there is some non-normal
     * priority or task type in the given execution admission context.
     */
    void writeAsMetadata(OperationContext* opCtx, BSONObjBuilder* builder);

    /**
     * Set the priority or task type based on the `executionAdmissionContextType` from a remote
     * request.
     */
    void setFromMetadata(
        OperationContext* opCtx,
        const boost::optional<admission::execution_control::ExecutionAdmissionTypeEnum>& type);

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
     * Returns the ticket stats recorder currently registered for this operation, if any. Allows
     * code that runs within a recorder's scope (but in a different stack frame) to read the
     * stats accumulated so far, e.g. to attach them to a log line.
     */
    ec::ScopedTicketAdmissionStatsRecorder* getTicketStatsRecorder() const {
        return _statsRecorder;
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
     * Marks this operation as having been marked non-deprioritizable.
     */
    void markedNonDeprioritizable() {
        _markedNonDeprioritizable.store(true);
    }

    /**
     * Returns true if this operation was ever marked as being non-deprioritizable.
     */
    bool getMarkedNonDeprioritizable() const {
        return _markedNonDeprioritizable.loadRelaxed();
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
     * Returns the task type
     */
    TaskType getTaskType() const {
        return _taskType.loadRelaxed();
    }

    /**
     * Sets the task type. Most callers should prefer the scoped objects below, but this helper
     * is available for places that would like to set the value for the whole life of the opCtx.
     */
    void setTaskType(OperationContext* opCtx, TaskType newType);

    /**
     * Records that a ticket was acquired. Increments the appropriate admission counter
     * (normal or low priority) for the current bucket based on the provided priority.
     */
    void recordExecutionAcquisition(AdmissionContext::Priority priority);

    /**
     * Records that the operation started waiting in a ticket queue. The matching end of the wait
     * is recorded by recordExecutionWaitedAcquisition, which fires whether the ticket was
     * acquired or the wait was interrupted.
     */
    void recordExecutionStartQueueing();

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
        // Stats recorded at finalization time (CPU, elapsed, load shed) - only depend on
        // deprioritizable/non-deprioritizable classification, not read/write type.
        ec::OperationFinalizedStats nonDeprioritizable;
        ec::OperationFinalizedStats deprioritizable;

        // Per-acquisition execution stats by bucket (deprioritizable/non-deprioritizable) and type
        // (read/write).
        ec::OperationExecutionStats readNonDeprioritizable;
        ec::OperationExecutionStats readDeprioritizable;
        ec::OperationExecutionStats writeNonDeprioritizable;
        ec::OperationExecutionStats writeDeprioritizable;

        // This stats are owned by the ticket holder and will be aggregated into the global stats by
        // the ticketing system. That is done because at the end of the operation we know the
        // priority of the operation and to which bucket (low or normal) it belongs.
        ec::DelinquencyStats readDelinquency;
        ec::DelinquencyStats writeDelinquency;

        // Whether this operation was deprioritized.
        bool wasDeprioritized = false;

        // Whether this operation was ever marked non-deprioritizable.
        bool wasMarkedNonDeprioritizable = false;

        // Whether this operation was in a multi-document transaction.
        bool wasInMultiDocTxn = false;

        void clearDelinquencyStats() {
            readDelinquency = ec::DelinquencyStats{};
            writeDelinquency = ec::DelinquencyStats{};
            readNonDeprioritizable.delinquencyStats = ec::DelinquencyStats{};
            readDeprioritizable.delinquencyStats = ec::DelinquencyStats{};
            writeNonDeprioritizable.delinquencyStats = ec::DelinquencyStats{};
            writeDeprioritizable.delinquencyStats = ec::DelinquencyStats{};
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

    bool isLoadShedExempt() const override {
        return getTaskType() == TaskType::Background;
    }

private:
    friend class ec::ScopedTaskTypeModifierBase;
    friend class ec::ScopedTicketAdmissionStatsRecorder;

    /**
     * Returns true if this operation should be classified as "deprioritizable" based on admission
     * count, priority flags, and exemption status.
     *
     * 'isFinalization' ensures the admission count is adjusted (decremented) to reflect the state
     * at the time of the deprioritization decision, rather than the final total.
     */
    bool _isDeprioritizable(bool isFinalization = false) const;

    /**
     * Returns a reference to the appropriate OperationExecutionStats based on the current operation
     * type (read/write) and stats bucket (deprioritizable/non-deprioritizable).
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
     * Stats for read and write deprioritizable/non-deprioritizable operations. Whether they're
     * deprioritizable or not will be determined by the execution control heuristic. These stats
     * will be accumulated regardless of whether deprioritization is active.
     */
    ec::OperationExecutionStats _readNonDeprioritizableStats;
    ec::OperationExecutionStats _readDeprioritizableStats;
    ec::OperationExecutionStats _writeNonDeprioritizableStats;
    ec::OperationExecutionStats _writeDeprioritizableStats;

    /**
     * Stats recorded at finalization time (CPU, elapsed, load shed). These are independent of
     * read/write operation type and only depend on deprioritizable/non-deprioritizable
     * classification.
     */
    ec::OperationFinalizedStats _nonDeprioritizableFinalStats;
    ec::OperationFinalizedStats _deprioritizableFinalStats;

    // True if this operation was ever heuristically deprioritized.
    Atomic<bool> _priorityLowered{false};

    // True if this was ever marked as non-deprioritizable.
    Atomic<bool> _markedNonDeprioritizable{false};

    // Task type
    Atomic<TaskType> _taskType{TaskType::Default};

    // Current operation type (read or write).
    ec::OperationType _opType{ec::OperationType::kRead};

    // True if finalizeStats() has been called. Prevents double-counting stats.
    Atomic<bool> _statsFinalized{false};

    // True if the operation was in a multi-document transaction at the time of the ticket
    // acquisition.
    Atomic<bool> _inMultiDocTxn{false};

    // True if the operation was ever in a multi-document transaction. Once set to true, stays true.
    Atomic<bool> _wasInMultiDocTxn{false};

    // ScopedTaskTypeModifier recursion counter to handle interleaving task type modifier
    // destructions.
    int _scopedTaskTypeModifierRecursion{0};

    // Ticket stats recorder registered by a ScopedTicketAdmissionStatsRecorder, if any. Only
    // accessed from the operation's own thread. Deliberately not copied: the copy constructor
    // leaves it null and the assignment operator preserves the target's own recorder, because
    // the recorder's lifetime is tied to the registering scope, while copies of this context
    // are stats snapshots.
    ec::ScopedTicketAdmissionStatsRecorder* _statsRecorder{nullptr};
};

inline std::string to_string(ExecutionAdmissionContext::TaskType tt) {
    switch (tt) {
        case ExecutionAdmissionContext::TaskType::Default:
            return "Normal";
        case ExecutionAdmissionContext::TaskType::NonDeprioritizable:
            return "NonDeprioritizable";
        case ExecutionAdmissionContext::TaskType::Background:
            return "Background";
    }
    MONGO_UNREACHABLE_TASSERT(12043500);
}

namespace admission::execution_control {

/**
 * Execution ticket statistics for an operation or a unit of work.
 *
 * When adding a field, also update ScopedTicketAdmissionStatsRecorder::_record and the per-task
 * forwarding callbacks that mirror these fields into global counters (ttl_monitor.cpp,
 * index_builds_coordinator.cpp, ready_range_deletions_processor.cpp).
 */
struct MONGO_MOD_PUBLIC TicketAdmissionStats {
    // Number of times the operation started, respectively finished, waiting in a ticket queue.
    // 'startedQueueing - finishedQueueing' is 1 while the operation is waiting for a ticket and 0
    // otherwise, which tasks can expose as a "currently queued" gauge.
    int64_t startedQueueing = 0;
    int64_t finishedQueueing = 0;

    // Number of tickets taken and released. 'admissions - releases' is the number of tickets the
    // operation currently holds (1 while it is processing with a ticket, 0 otherwise), the
    // processing-time analogue of 'startedQueueing - finishedQueueing'.
    int64_t admissions = 0;
    int64_t releases = 0;

    int64_t lowPriorityAdmissions = 0;

    int64_t timeQueuedMicros = 0;
    int64_t timeProcessingMicros = 0;

    TicketAdmissionStats operator-(const TicketAdmissionStats& other) const {
        return {
            startedQueueing - other.startedQueueing,
            finishedQueueing - other.finishedQueueing,
            admissions - other.admissions,
            releases - other.releases,
            lowPriorityAdmissions - other.lowPriorityAdmissions,
            timeQueuedMicros - other.timeQueuedMicros,
            timeProcessingMicros - other.timeProcessingMicros,
        };
    }
};

/**
 * RAII object that records the operation's execution ticket events as they happen: every ticket
 * acquisition, every completed queue wait, and every release (i.e. every yield). Events are
 * accumulated into local stats — readable via stats() to attribute ticket time to a unit of work,
 * e.g. for logging — and forwarded to the 'onUpdate' callback so long-running background tasks
 * can keep global (serverStatus) counters up to date while they run instead of only at
 * completion.
 *
 * Note that a queue wait is only recorded once the ticket is granted (or the wait is
 * interrupted); the time spent in a still-in-progress wait is not visible here. Use
 * currentOp's 'currentQueue.timeQueuedMicros' for that.
 *
 * Events for exempt admissions are not recorded. At most one recorder may be registered per
 * operation at a time, and it must only be used from the operation's own thread.
 */
class MONGO_MOD_PUBLIC ScopedTicketAdmissionStatsRecorder {
public:
    /**
     * Invoked on every event with a delta where only the affected fields are nonzero, so
     * callbacks may unconditionally add all fields to their counters.
     */
    using OnUpdateFn = std::function<void(const TicketAdmissionStats& delta)>;

    ScopedTicketAdmissionStatsRecorder(OperationContext* opCtx, OnUpdateFn onUpdate);
    ~ScopedTicketAdmissionStatsRecorder();

    ScopedTicketAdmissionStatsRecorder(const ScopedTicketAdmissionStatsRecorder&) = delete;
    ScopedTicketAdmissionStatsRecorder& operator=(const ScopedTicketAdmissionStatsRecorder&) =
        delete;

    /**
     * The stats accumulated since this recorder was registered.
     */
    const TicketAdmissionStats& stats() const {
        return _stats;
    }

private:
    friend class ::mongo::ExecutionAdmissionContext;

    void _record(TicketAdmissionStats delta) {
        _stats.timeQueuedMicros += delta.timeQueuedMicros;
        _stats.timeProcessingMicros += delta.timeProcessingMicros;
        _stats.admissions += delta.admissions;
        _stats.lowPriorityAdmissions += delta.lowPriorityAdmissions;
        _stats.startedQueueing += delta.startedQueueing;
        _stats.finishedQueueing += delta.finishedQueueing;
        _stats.releases += delta.releases;
        if (_onUpdate) {
            _onUpdate(delta);
        }
    }

    OperationContext* _opCtx;
    TicketAdmissionStats _stats;
    OnUpdateFn _onUpdate;
};

/**
 * RAII-like object base to temporarily change the task type of the ExecutionAdmissionContext
 * attached to the operation context. Being a RAII-like, on the destructor it sets back the Normal
 * task mode.
 */
class MONGO_MOD_PRIVATE ScopedTaskTypeModifierBase {
public:
    ~ScopedTaskTypeModifierBase();

    ScopedTaskTypeModifierBase(const ScopedTaskTypeModifierBase&) = delete;
    ScopedTaskTypeModifierBase& operator=(const ScopedTaskTypeModifierBase&) = delete;

protected:
    ScopedTaskTypeModifierBase(OperationContext* opCtx,
                               ExecutionAdmissionContext::TaskType newType);

private:
    OperationContext* _opCtx;
};

/**
 * RAII-like object to set the task type to 'Background'
 */
class MONGO_MOD_PUBLIC ScopedTaskTypeBackground : private ScopedTaskTypeModifierBase {
public:
    ScopedTaskTypeBackground(OperationContext* opCtx);
};

/**
 * RAII-like object to set the task type to 'NonDeprioritizable'
 */
class MONGO_MOD_PUBLIC ScopedTaskTypeNonDeprioritizable : private ScopedTaskTypeModifierBase {
public:
    ScopedTaskTypeNonDeprioritizable(OperationContext* opCtx);
};

};  // namespace admission::execution_control

}  // namespace mongo
