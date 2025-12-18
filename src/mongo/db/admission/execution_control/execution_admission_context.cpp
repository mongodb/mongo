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

#include "mongo/db/admission/execution_control/execution_admission_context.h"

#include "mongo/db/admission/execution_control/execution_control_heuristic_parameters_gen.h"
#include "mongo/db/operation_context.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kStorage

namespace mongo {

namespace {
const auto contextDecoration = OperationContext::declareDecoration<ExecutionAdmissionContext>();
}  // namespace

bool ExecutionAdmissionContext::shouldDeprioritize(std::int32_t admissions) {
    // If the op is eligible, downgrade it if it has yielded enough times to meet the threshold.
    return admissions >= ec::gHeuristicNumAdmissionsDeprioritizeThreshold.load();
}

ExecutionAdmissionContext& ExecutionAdmissionContext::get(OperationContext* opCtx) {
    return contextDecoration(opCtx);
}

ExecutionAdmissionContext::ExecutionAdmissionContext(const ExecutionAdmissionContext& other)
    : AdmissionContext(other),
      _readDelinquencyStats(other._readDelinquencyStats),
      _writeDelinquencyStats(other._writeDelinquencyStats),
      _readShortStats(other._readShortStats),
      _readLongStats(other._readLongStats),
      _writeShortStats(other._writeShortStats),
      _writeLongStats(other._writeLongStats),
      _shortRunningFinalStats(other._shortRunningFinalStats),
      _longRunningFinalStats(other._longRunningFinalStats),
      _priorityLowered(other._priorityLowered.loadRelaxed()),
      _opType(other._opType) {}

ExecutionAdmissionContext& ExecutionAdmissionContext::operator=(
    const ExecutionAdmissionContext& other) {
    AdmissionContext::operator=(other);
    _readDelinquencyStats = other._readDelinquencyStats;
    _writeDelinquencyStats = other._writeDelinquencyStats;
    _readShortStats = other._readShortStats;
    _readLongStats = other._readLongStats;
    _writeShortStats = other._writeShortStats;
    _writeLongStats = other._writeLongStats;
    _shortRunningFinalStats = other._shortRunningFinalStats;
    _longRunningFinalStats = other._longRunningFinalStats;
    _priorityLowered.store(other._priorityLowered.loadRelaxed());
    _opType = other._opType;
    return *this;
}

ExecutionAdmissionContext::FinalizedStats ExecutionAdmissionContext::finalizeStats(
    int64_t cpuUsageMicros, int64_t elapsedMicros) {
    // Append CPU, elapsed time, and load-shed stats only if the operation is not composed solely of
    // exempted admissions.
    if (getAdmissions() > getExemptedAdmissions()) {
        // Record CPU and elapsed time to the appropriate finalized stats bucket (short/long
        // running). This is independent of operation type (read/write) since the type may have
        // changed during the operation's lifetime.
        auto& stats = _isLongRunning(true /* isFinalization */) ? _longRunningFinalStats
                                                                : _shortRunningFinalStats;

        stats.totalCPUUsageMicros.fetchAndAddRelaxed(cpuUsageMicros);
        stats.totalElapsedTimeMicros.fetchAndAddRelaxed(elapsedMicros);

        // Record final CPU, elapsed time and number of admissions to the load shed bucket.
        if (getLoadShed()) {
            stats.totalCPUUsageLoadShed.fetchAndAddRelaxed(cpuUsageMicros);
            stats.totalElapsedTimeMicrosLoadShed.fetchAndAddRelaxed(elapsedMicros);
            stats.newAdmissionsLoadShed.fetchAndAddRelaxed(1);
            stats.totalAdmissionsLoadShed.fetchAndAddRelaxed(getAdmissions());
            stats.totalQueuedTimeMicrosLoadShed.fetchAndAddRelaxed(totalTimeQueuedMicros().count());
        }
    }

    // Take a snapshot of all stats.
    FinalizedStats result;
    result.shortRunning = _shortRunningFinalStats;
    result.longRunning = _longRunningFinalStats;
    result.readShort = _readShortStats;
    result.readLong = _readLongStats;
    result.writeShort = _writeShortStats;
    result.writeLong = _writeLongStats;
    result.readDelinquency = _readDelinquencyStats;
    result.writeDelinquency = _writeDelinquencyStats;
    result.wasDeprioritized = getPriorityLowered();

    return result;
}

void ExecutionAdmissionContext::recordExecutionAcquisition() {
    if (!_shouldRecordStats()) {
        return;
    }

    auto& stats = _getOperationExecutionStats();
    if (getAdmissions() == 1) {
        stats.newAdmissions.fetchAndAddRelaxed(1);
    }
    stats.totalAdmissions.fetchAndAddRelaxed(1);
}

void ExecutionAdmissionContext::recordExecutionWaitedAcquisition(Microseconds queueTimeMicros) {
    if (!_shouldRecordStats()) {
        return;
    }

    auto& stats = _getOperationExecutionStats();
    stats.totalTimeQueuedMicros.fetchAndAddRelaxed(queueTimeMicros.count());
}

void ExecutionAdmissionContext::recordExecutionRelease(Microseconds processedTimeMicros) {
    if (!_shouldRecordStats()) {
        return;
    }

    auto& stats = _getOperationExecutionStats();
    stats.totalTimeProcessingMicros.fetchAndAddRelaxed(processedTimeMicros.count());
}

void ExecutionAdmissionContext::recordDelinquentAcquisition(Milliseconds delay) {
    auto recordDelinquentAcquisition = [](Milliseconds delay, ec::DelinquencyStats& stats) {
        const int64_t delayMs = delay.count();
        stats.totalDelinquentAcquisitions.fetchAndAddRelaxed(1);
        stats.totalAcquisitionDelinquencyMillis.fetchAndAddRelaxed(delayMs);
        stats.maxAcquisitionDelinquencyMillis.storeRelaxed(
            std::max(stats.maxAcquisitionDelinquencyMillis.loadRelaxed(), delayMs));
    };

    auto& ticketHolderStats = getOperationType() == ec::OperationType::kRead
        ? _readDelinquencyStats
        : _writeDelinquencyStats;
    recordDelinquentAcquisition(delay, ticketHolderStats);

    auto& operationStats = _getOperationExecutionStats().delinquencyStats;
    recordDelinquentAcquisition(delay, operationStats);
}

bool ExecutionAdmissionContext::_isLongRunning(bool isFinalization) const {
    // An operation is considered "long running" if any of these conditions are true:
    //   1. The deprioritization heuristic says it should be deprioritized (based on admissions).
    //      Note that this is called AFTER the number of admissions was successfully accounted for,
    //      so we need to decrease the number of admissions to match the state at decision time.
    //   2. The operation was heuristically demoted at some point (priorityLowered flag).
    //   3. The operation has an inherently low priority.
    int32_t admissions = getAdmissions();
    if (isHoldingTicket() || isFinalization) {
        admissions -= 1;
    }
    return shouldDeprioritize(admissions) || getPriorityLowered() ||
        getPriority() == AdmissionContext::Priority::kLow;
}

ec::OperationExecutionStats& ExecutionAdmissionContext::_getOperationExecutionStats() {
    const bool isLongRunning = _isLongRunning();

    switch (getOperationType()) {
        case ec::OperationType::kRead:
            return isLongRunning ? _readLongStats : _readShortStats;

        case ec::OperationType::kWrite:
            return isLongRunning ? _writeLongStats : _writeShortStats;
    }

    MONGO_UNREACHABLE;
}

bool ExecutionAdmissionContext::_shouldRecordStats() {
    return getPriority() != AdmissionContext::Priority::kExempt;
}

}  // namespace mongo
