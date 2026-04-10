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
#include "mongo/db/server_feature_flags_gen.h"
#include "mongo/idl/generic_argument_gen.h"
#include "mongo/logv2/log.h"

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
      _readNonDeprioritizableStats(other._readNonDeprioritizableStats),
      _readDeprioritizableStats(other._readDeprioritizableStats),
      _writeNonDeprioritizableStats(other._writeNonDeprioritizableStats),
      _writeDeprioritizableStats(other._writeDeprioritizableStats),
      _nonDeprioritizableFinalStats(other._nonDeprioritizableFinalStats),
      _deprioritizableFinalStats(other._deprioritizableFinalStats),
      _priorityLowered(other._priorityLowered.loadRelaxed()),
      _markedNonDeprioritizable(other._markedNonDeprioritizable.loadRelaxed()),
      _taskType(other._taskType.loadRelaxed()),
      _opType(other._opType),
      _statsFinalized(other._statsFinalized.loadRelaxed()),
      _inMultiDocTxn(other._inMultiDocTxn.loadRelaxed()),
      _wasInMultiDocTxn(other._wasInMultiDocTxn.loadRelaxed()) {}

ExecutionAdmissionContext& ExecutionAdmissionContext::operator=(
    const ExecutionAdmissionContext& other) {
    AdmissionContext::operator=(other);
    _readDelinquencyStats = other._readDelinquencyStats;
    _writeDelinquencyStats = other._writeDelinquencyStats;
    _readNonDeprioritizableStats = other._readNonDeprioritizableStats;
    _readDeprioritizableStats = other._readDeprioritizableStats;
    _writeNonDeprioritizableStats = other._writeNonDeprioritizableStats;
    _writeDeprioritizableStats = other._writeDeprioritizableStats;
    _nonDeprioritizableFinalStats = other._nonDeprioritizableFinalStats;
    _deprioritizableFinalStats = other._deprioritizableFinalStats;
    _priorityLowered.store(other._priorityLowered.loadRelaxed());
    _markedNonDeprioritizable.store(other._markedNonDeprioritizable.loadRelaxed());
    _taskType.store(other._taskType.loadRelaxed());
    _opType = other._opType;
    _statsFinalized.store(other._statsFinalized.loadRelaxed());
    _inMultiDocTxn.store(other._inMultiDocTxn.loadRelaxed());
    _wasInMultiDocTxn.store(other._wasInMultiDocTxn.loadRelaxed());
    return *this;
}

void ExecutionAdmissionContext::writeAsMetadata(OperationContext* opCtx, BSONObjBuilder* builder) {
    // We use last LTS when uninitialized to prevent attaching a field that an old binary will not
    // understand. This may result in normal priority being used for commands that specify some
    // other priority during startup.
    // TODO (SERVER-122847): Remove the feature flag check.
    if (gExecutionControlRemoteSpecification.isEnabledUseLastLTSFCVWhenUninitialized(
            VersionContext::getDecoration(opCtx),
            serverGlobalParams.featureCompatibility.acquireFCVSnapshot())) {
        switch (getPriority()) {
            case AdmissionContext::Priority::kNormal:
                break;
            case AdmissionContext::Priority::kLow:
                builder->append(
                    GenericArguments::kExecutionAdmissionContextTypeFieldName,
                    idlSerialize(admission::execution_control::ExecutionAdmissionTypeEnum::kLow));
                return;
            case AdmissionContext::Priority::kExempt:
                builder->append(
                    GenericArguments::kExecutionAdmissionContextTypeFieldName,
                    idlSerialize(
                        admission::execution_control::ExecutionAdmissionTypeEnum::kExempt));
                return;
            case AdmissionContext::Priority::kPrioritiesCount:
            default:
                MONGO_UNREACHABLE_TASSERT(12137301);
        }
        switch (getTaskType()) {
            case TaskType::Default:
                break;
            case TaskType::Background:
                builder->append(
                    GenericArguments::kExecutionAdmissionContextTypeFieldName,
                    idlSerialize(
                        admission::execution_control::ExecutionAdmissionTypeEnum::kBackground));
                return;
            case TaskType::NonDeprioritizable:
                builder->append(GenericArguments::kExecutionAdmissionContextTypeFieldName,
                                idlSerialize(admission::execution_control::
                                                 ExecutionAdmissionTypeEnum::kNonDeprioritizable));
                return;
            default:
                MONGO_UNREACHABLE_TASSERT(12137302);
        }
    }
}

void ExecutionAdmissionContext::setFromMetadata(
    OperationContext* opCtx,
    const boost::optional<admission::execution_control::ExecutionAdmissionTypeEnum>& type) {
    invariant(getPriority() == AdmissionContext::Priority::kNormal);
    invariant(getTaskType() == TaskType::Default);
    if (!type) {
        return;
    }
    switch (*type) {
        case admission::execution_control::ExecutionAdmissionTypeEnum::kExempt:
            _priority.store(AdmissionContext::Priority::kExempt);
            return;
        case admission::execution_control::ExecutionAdmissionTypeEnum::kLow:
            _priority.store(AdmissionContext::Priority::kLow);
            return;
        case admission::execution_control::ExecutionAdmissionTypeEnum::kBackground:
            setTaskType(opCtx, TaskType::Background);
            return;
        case admission::execution_control::ExecutionAdmissionTypeEnum::kNonDeprioritizable:
            setTaskType(opCtx, TaskType::NonDeprioritizable);
            return;
        default:
            MONGO_UNREACHABLE_TASSERT(12137304);
    }
}

boost::optional<ExecutionAdmissionContext::FinalizedStats> ExecutionAdmissionContext::finalizeStats(
    int64_t cpuUsageMicros, int64_t elapsedMicros) {
    bool expectedNotFinalized = false;
    if (!_statsFinalized.compareAndSwap(&expectedNotFinalized, true)) {
        // Stats have already been finalized, return none to indicate no action needed.
        return boost::none;
    }

    // Append CPU, elapsed time, and load-shed stats only if the operation is not composed solely of
    // exempted admissions.
    if (getAdmissions() > getExemptedAdmissions()) {
        // Record CPU and elapsed time to the appropriate finalized stats bucket
        // (deprioritizable/non-deprioritizable). This is independent of operation type (read/write)
        // since the type may have changed during the operation's lifetime.
        auto& stats = _isDeprioritizable(true /* isFinalization */) ? _deprioritizableFinalStats
                                                                    : _nonDeprioritizableFinalStats;

        stats.totalOpsFinished.fetchAndAddRelaxed(1);
        stats.totalCPUUsageMicros.fetchAndAddRelaxed(cpuUsageMicros);
        stats.totalElapsedTimeMicros.fetchAndAddRelaxed(elapsedMicros);

        // Record final CPU, elapsed time and number of admissions to the load shed bucket.
        if (getLoadShed()) {
            stats.totalCPUUsageLoadShed.fetchAndAddRelaxed(cpuUsageMicros);
            stats.totalElapsedTimeMicrosLoadShed.fetchAndAddRelaxed(elapsedMicros);
            stats.totalOpsLoadShed.fetchAndAddRelaxed(1);
            stats.totalAdmissionsLoadShed.fetchAndAddRelaxed(getAdmissions());
            stats.totalQueuedTimeMicrosLoadShed.fetchAndAddRelaxed(totalTimeQueuedMicros().count());
        }
    }

    // Take a snapshot of all stats.
    FinalizedStats result;
    result.nonDeprioritizable = _nonDeprioritizableFinalStats;
    result.deprioritizable = _deprioritizableFinalStats;
    result.readNonDeprioritizable = _readNonDeprioritizableStats;
    result.readDeprioritizable = _readDeprioritizableStats;
    result.writeNonDeprioritizable = _writeNonDeprioritizableStats;
    result.writeDeprioritizable = _writeDeprioritizableStats;
    result.readDelinquency = _readDelinquencyStats;
    result.writeDelinquency = _writeDelinquencyStats;
    result.wasDeprioritized = getPriorityLowered();
    result.wasMarkedNonDeprioritizable = getMarkedNonDeprioritizable();
    result.wasInMultiDocTxn = _wasInMultiDocTxn.loadRelaxed();

    return result;
}

void ExecutionAdmissionContext::setTaskType(OperationContext* opCtx, TaskType newType) {
    const auto currentType = getTaskType();
    const auto currentPrio = getPriority();

    dassert(currentType == newType || currentType == TaskType::Default);
    dassert(currentPrio == AdmissionContext::Priority::kExempt ||
            currentPrio == AdmissionContext::Priority::kNormal);
    dassert(!opCtx->inMultiDocumentTransaction());

    _taskType.store(newType);
    if (newType == TaskType::NonDeprioritizable) {
        markedNonDeprioritizable();
    }

    // There is no need for mutex here, because the thread <-> operation context (and
    // ExecutionAdmissionContext) is a 1 to 1 relation.
    // Nobody should update the ExecutionAdmissionContext's counter from a different thread (sharing
    // opCtx is prohibited).
    const auto recursionCount = ++_scopedTaskTypeModifierRecursion;
    if (recursionCount < 1) {
        LOGV2_WARNING(12096800,
                      "Inconsistency in _scopedTaskTypeModifierRecursion count. Resetting it to 1.",
                      "recursionCount"_attr = recursionCount);
        dassert(false);
        _scopedTaskTypeModifierRecursion = 1;
    }

    LOGV2_DEBUG(12043501,
                1,
                "Changing task type on ExecutionAdmissionContext",
                "oldValue"_attr = to_string(currentType),
                "newValue"_attr = to_string(newType));
}

void ExecutionAdmissionContext::recordExecutionAcquisition(AdmissionContext::Priority priority) {
    if (!_shouldRecordStats()) {
        return;
    }

    auto& stats = _getOperationExecutionStats();
    stats.totalAdmissions.fetchAndAddRelaxed(1);
    if (priority == AdmissionContext::Priority::kLow) {
        stats.totalLowPriorityAdmissions.fetchAndAddRelaxed(1);
    } else {
        stats.totalNormalPriorityAdmissions.fetchAndAddRelaxed(1);
    }
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

bool ExecutionAdmissionContext::_isDeprioritizable(bool isFinalization) const {
    // Multi-document txns are never classified as "deprioritizable" because they are exempt from
    // deprioritization.
    if (_inMultiDocTxn.loadRelaxed()) {
        return false;
    }

    // Non-deprioritizable tasks are never classified as "deprioritizable" because they are exempt
    // from deprioritization.
    if (_taskType.loadRelaxed() == TaskType::NonDeprioritizable) {
        return false;
    }

    // Calculate the admission count at decision time. When holding a ticket or during finalization,
    // we decrement by 1 because the current admission has already been counted but the
    // deprioritization decision was made before this admission.
    int32_t admissions = getAdmissions();
    if (isHoldingTicket() || isFinalization) {
        admissions -= 1;
    }

    // An operation is considered "deprioritizable" if any of these conditions are true:
    //   1. It exceeded the admission threshold (heuristic deprioritization).
    //   2. It was explicitly deprioritized at some point (priorityLowered flag).
    //   3. It has an inherently low priority.
    //   4. It is a background task.
    return shouldDeprioritize(admissions) || getPriorityLowered() ||
        getPriority() == AdmissionContext::Priority::kLow || getTaskType() == TaskType::Background;
}

ec::OperationExecutionStats& ExecutionAdmissionContext::_getOperationExecutionStats() {
    const bool isDeprioritizable = _isDeprioritizable();

    switch (getOperationType()) {
        case ec::OperationType::kRead:
            return isDeprioritizable ? _readDeprioritizableStats : _readNonDeprioritizableStats;

        case ec::OperationType::kWrite:
            return isDeprioritizable ? _writeDeprioritizableStats : _writeNonDeprioritizableStats;
    }

    MONGO_UNREACHABLE;
}

bool ExecutionAdmissionContext::_shouldRecordStats() {
    return getPriority() != AdmissionContext::Priority::kExempt;
}

admission::execution_control::ScopedTaskTypeModifierBase::ScopedTaskTypeModifierBase(
    OperationContext* opCtx, ExecutionAdmissionContext::TaskType newType)
    : _opCtx(opCtx) {
    ExecutionAdmissionContext::get(_opCtx).setTaskType(_opCtx, newType);
}

admission::execution_control::ScopedTaskTypeModifierBase::~ScopedTaskTypeModifierBase() {
    // There is no need for mutex here, because the thread <-> operation context (and
    // ExecutionAdmissionContext) is a 1 to 1 relation.
    // Nobody should update the ExecutionAdmissionContext's counter from a different thread (sharing
    // opCtx is prohibited).
    const auto currentType = ExecutionAdmissionContext::get(_opCtx).getTaskType();
    const auto recursionCount =
        --ExecutionAdmissionContext::get(_opCtx)._scopedTaskTypeModifierRecursion;
    dassert(recursionCount >= 0);
    if (recursionCount == 0) {
        ExecutionAdmissionContext::get(_opCtx)._taskType.store(
            ExecutionAdmissionContext::TaskType::Default);
        LOGV2_DEBUG(12137201,
                    1,
                    "Resetting task type on ExecutionAdmissionContext",
                    "oldValue"_attr = to_string(currentType),
                    "newValue"_attr = to_string(ExecutionAdmissionContext::TaskType::Default));
    }
}

admission::execution_control::ScopedTaskTypeBackground::ScopedTaskTypeBackground(
    OperationContext* opCtx)
    : ScopedTaskTypeModifierBase(opCtx, ExecutionAdmissionContext::TaskType::Background) {}

admission::execution_control::ScopedTaskTypeNonDeprioritizable::ScopedTaskTypeNonDeprioritizable(
    OperationContext* opCtx)
    : ScopedTaskTypeModifierBase(opCtx, ExecutionAdmissionContext::TaskType::NonDeprioritizable) {}

}  // namespace mongo
