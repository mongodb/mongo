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


bool ExecutionAdmissionContext::shouldDeprioritize(ExecutionAdmissionContext* admCtx) {
    // If the op is eligible, downgrade it if it has yielded enough times to meet the threshold.
    return admCtx->getAdmissions() >=
        admission::execution_control::gHeuristicNumAdmissionsDeprioritizeThreshold.load();
}

ExecutionAdmissionContext& ExecutionAdmissionContext::get(OperationContext* opCtx) {
    return contextDecoration(opCtx);
}

ExecutionAdmissionContext::ExecutionAdmissionContext(const ExecutionAdmissionContext& other)
    : AdmissionContext(other),
      _readDelinquencyStats(other._readDelinquencyStats),
      _writeDelinquencyStats(other._writeDelinquencyStats) {}

ExecutionAdmissionContext& ExecutionAdmissionContext::operator=(
    const ExecutionAdmissionContext& other) {
    AdmissionContext::operator=(other);
    _readDelinquencyStats = other._readDelinquencyStats;
    _writeDelinquencyStats = other._writeDelinquencyStats;
    return *this;
}

admission::execution_control::OperationExecutionStats&
ExecutionAdmissionContext::_getOperationExecutionStats() {
    auto dePrioritized = ExecutionAdmissionContext::shouldDeprioritize(this) ||
        getPriority() == AdmissionContext::Priority::kLow;
    switch (getOperationType()) {
        case admission::execution_control::OperationType::kRead:
            return dePrioritized ? _readShortStats : _readLongStats;

        case admission::execution_control::OperationType::kWrite:
            return dePrioritized ? _writeShortStats : _writeLongStats;
    }

    MONGO_UNREACHABLE;
}

void ExecutionAdmissionContext::recordExecutionAcquisition() {
    if (!_isUserOperation())
        return;

    _recordExecutionAcquisition(_getOperationExecutionStats());
}

void ExecutionAdmissionContext::recordExecutionWaitedAcquisition(Microseconds queueTimeMicros) {
    if (!_isUserOperation())
        return;

    _recordExecutionTimeQueued(queueTimeMicros, _getOperationExecutionStats());
}

void ExecutionAdmissionContext::recordExecutionRelease(Microseconds processedTimeMicros) {
    if (!_isUserOperation())
        return;

    _recordExecutionProcessedTime(processedTimeMicros, _getOperationExecutionStats());
}

void ExecutionAdmissionContext::recordOperationLoadShed() {
    AdmissionContext::recordOperationLoadShed();
    _recordExecutionShed(_getOperationExecutionStats());
}

void ExecutionAdmissionContext::recordExecutionCPUUsageAndElapsedTime(int64_t cpuUsageMicros,
                                                                      int64_t elapsedMicros) {
    if (!_isUserOperation())
        return;

    _recordExecutionCPUUsageAndElapsedTimeShed(
        cpuUsageMicros, elapsedMicros, _getOperationExecutionStats());
    if (getLoadShed()) {
        recordOperationLoadShed();
    }
}

void ExecutionAdmissionContext::recordDelinquentAcquisition(Milliseconds delay) {
    _recordDelinquentAcquisition(delay, _getDelinquencyStats());
    _recordDelinquentAcquisition(delay, _getOperationExecutionStats().delinquencyStats);
}

admission::execution_control::DelinquencyStats& ExecutionAdmissionContext::_getDelinquencyStats() {
    return (getOperationType() == admission::execution_control::OperationType::kRead)
        ? _readDelinquencyStats
        : _writeDelinquencyStats;
}

bool ExecutionAdmissionContext::_isUserOperation() {
    // We want the long/short running stats to reflect the behavior of user workloads, by preventing
    // counting commands with exempted acquisitions, we remove noise comming from internal
    // operations.
    return getExemptedAdmissions() == 0;
}

void ExecutionAdmissionContext::_recordDelinquentAcquisition(
    Milliseconds delay, admission::execution_control::DelinquencyStats& stats) {
    const int64_t delayMs = delay.count();
    stats.totalDelinquentAcquisitions.fetchAndAddRelaxed(1);
    stats.totalAcquisitionDelinquencyMillis.fetchAndAddRelaxed(delayMs);
    stats.maxAcquisitionDelinquencyMillis.storeRelaxed(
        std::max(stats.maxAcquisitionDelinquencyMillis.loadRelaxed(), delayMs));
}

void ExecutionAdmissionContext::_recordExecutionTimeQueued(
    Microseconds queueTimeMillis, admission::execution_control::OperationExecutionStats& stats) {
    stats.totalTimeQueuedMicros.fetchAndAddRelaxed(queueTimeMillis.count());
}

void ExecutionAdmissionContext::_recordExecutionAcquisition(
    admission::execution_control::OperationExecutionStats& stats) {
    if (!_isUserOperation())
        return;

    auto admissions = getAdmissions();
    if (admissions == 1) {
        stats.newAdmissions.fetchAndAddRelaxed(1);
    }
    stats.totalAdmissions.fetchAndAddRelaxed(1);
}

void ExecutionAdmissionContext::_recordExecutionProcessedTime(
    Microseconds processedTimeMillis,
    admission::execution_control::OperationExecutionStats& stats) {
    stats.totalTimeProcessingMicros.fetchAndAddRelaxed(processedTimeMillis.count());
}

void ExecutionAdmissionContext::_recordExecutionShed(
    admission::execution_control::OperationExecutionStats& stats) {
    stats.newAdmissionsLoadShed.fetchAndAddRelaxed(1);
    stats.totalAdmissionsLoadShed.fetchAndAddRelaxed(getAdmissions() - getExemptedAdmissions());
    stats.totalQueuedTimeMicrosLoadShed.fetchAndAddRelaxed(totalTimeQueuedMicros().count());
}

void ExecutionAdmissionContext::_recordExecutionCPUUsageAndElapsedTime(
    int64_t cpuUsageMicros,
    int64_t elapsedMicros,
    admission::execution_control::OperationExecutionStats& stats) {
    stats.totalCPUUsageMicros.fetchAndAddRelaxed(cpuUsageMicros);
    stats.totalElapsedTimeMicros.fetchAndAddRelaxed(elapsedMicros);
}

void ExecutionAdmissionContext::_recordExecutionCPUUsageAndElapsedTimeShed(
    int64_t cpuUsage,
    int64_t elapsedMicros,
    admission::execution_control::OperationExecutionStats& stats) {
    stats.totalCPUUsageLoadShed.fetchAndAddRelaxed(cpuUsage);
    stats.totalElapsedTimeMicrosLoadShed.fetchAndAddRelaxed(elapsedMicros);
}
}  // namespace mongo
