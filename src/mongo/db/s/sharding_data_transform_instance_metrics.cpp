/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include "mongo/db/s/sharding_data_transform_instance_metrics.h"
#include "mongo/db/s/sharding_data_transform_metrics_observer.h"
#include "mongo/util/duration.h"

namespace mongo {

ShardingDataTransformInstanceMetrics::ShardingDataTransformInstanceMetrics(
    UUID instanceId,
    BSONObj originalCommand,
    NamespaceString sourceNs,
    Role role,
    Date_t startTime,
    ClockSource* clockSource,
    ShardingDataTransformCumulativeMetrics* cumulativeMetrics)
    : ShardingDataTransformInstanceMetrics{
          std::move(instanceId),
          std::move(originalCommand),
          std::move(sourceNs),
          role,
          startTime,
          clockSource,
          cumulativeMetrics,
          std::make_unique<ShardingDataTransformMetricsObserver>(this)} {}

ShardingDataTransformInstanceMetrics::ShardingDataTransformInstanceMetrics(
    UUID instanceId,
    BSONObj originalCommand,
    NamespaceString sourceNs,
    Role role,
    Date_t startTime,
    ClockSource* clockSource,
    ShardingDataTransformCumulativeMetrics* cumulativeMetrics,
    ObserverPtr observer)
    : _instanceId{std::move(instanceId)},
      _originalCommand{std::move(originalCommand)},
      _sourceNs{std::move(sourceNs)},
      _role{role},
      _startTime{startTime},
      _clockSource{clockSource},
      _observer{std::move(observer)},
      _cumulativeMetrics{cumulativeMetrics},
      _deregister{_cumulativeMetrics->registerInstanceMetrics(_observer.get())},
      _copyingStartTime{kNoDate},
      _copyingEndTime{kNoDate},
      _approxDocumentsToCopy{0},
      _documentsCopied{0},
      _approxBytesToCopy{0},
      _bytesCopied{0},
      _coordinatorHighEstimateRemainingTimeMillis{Milliseconds{0}},
      _coordinatorLowEstimateRemainingTimeMillis{Milliseconds{0}},
      _criticalSectionStartTime{kNoDate},
      _criticalSectionEndTime{kNoDate},
      _writesDuringCriticalSection{0} {}

ShardingDataTransformInstanceMetrics::~ShardingDataTransformInstanceMetrics() {
    if (_deregister) {
        _deregister();
    }
}

Milliseconds ShardingDataTransformInstanceMetrics::getHighEstimateRemainingTimeMillis() const {
    switch (_role) {
        case Role::kRecipient:
            return getRecipientHighEstimateRemainingTimeMillis();
        case Role::kCoordinator:
            return Milliseconds{_coordinatorHighEstimateRemainingTimeMillis.load()};
        case Role::kDonor:
            break;
    }
    MONGO_UNREACHABLE;
}

Milliseconds ShardingDataTransformInstanceMetrics::getLowEstimateRemainingTimeMillis() const {
    switch (_role) {
        case Role::kRecipient:
            return getHighEstimateRemainingTimeMillis();
        case Role::kCoordinator:
            return _coordinatorLowEstimateRemainingTimeMillis.load();
        case Role::kDonor:
            break;
    }
    MONGO_UNREACHABLE;
}

Date_t ShardingDataTransformInstanceMetrics::getStartTimestamp() const {
    return _startTime;
}

const UUID& ShardingDataTransformInstanceMetrics::getInstanceId() const {
    return _instanceId;
}

ShardingDataTransformInstanceMetrics::Role ShardingDataTransformInstanceMetrics::getRole() const {
    return _role;
}

std::string ShardingDataTransformInstanceMetrics::createOperationDescription() const noexcept {
    return fmt::format("ShardingDataTransformMetrics{}Service {}",
                       ShardingDataTransformMetrics::getRoleName(_role),
                       _instanceId.toString());
}

StringData ShardingDataTransformInstanceMetrics::getStateString() const noexcept {
    return "Unknown";
}

BSONObj ShardingDataTransformInstanceMetrics::reportForCurrentOp() const noexcept {

    BSONObjBuilder builder;
    builder.append(kType, "op");
    builder.append(kDescription, createOperationDescription());
    builder.append(kOp, "command");
    builder.append(kNamespace, _sourceNs.toString());
    builder.append(kOriginatingCommand, _originalCommand);
    builder.append(kOpTimeElapsed, getOperationRunningTimeSecs().count());

    switch (_role) {
        case Role::kCoordinator:
            builder.append(kAllShardsHighestRemainingOperationTimeEstimatedSecs,
                           durationCount<Seconds>(getHighEstimateRemainingTimeMillis()));
            builder.append(kAllShardsLowestRemainingOperationTimeEstimatedSecs,
                           durationCount<Seconds>(getLowEstimateRemainingTimeMillis()));
            builder.append(kCoordinatorState, getStateString());
            builder.append(kCopyTimeElapsed, getCopyingElapsedTimeSecs().count());
            builder.append(kCriticalSectionTimeElapsed,
                           getCriticalSectionElapsedTimeSecs().count());
            break;
        case Role::kDonor:
            builder.append(kDonorState, getStateString());
            builder.append(kCriticalSectionTimeElapsed,
                           getCriticalSectionElapsedTimeSecs().count());
            builder.append(kCountWritesDuringCriticalSection, _writesDuringCriticalSection.load());
            builder.append(kCountReadsDuringCriticalSection, _readsDuringCriticalSection.load());
            break;
        case Role::kRecipient:
            builder.append(kRecipientState, getStateString());
            builder.append(kCopyTimeElapsed, getCopyingElapsedTimeSecs().count());
            builder.append(kRemainingOpTimeEstimated,
                           durationCount<Seconds>(getHighEstimateRemainingTimeMillis()));
            builder.append(kApproxDocumentsToCopy, _approxDocumentsToCopy.load());
            builder.append(kApproxBytesToCopy, _approxBytesToCopy.load());
            builder.append(kBytesCopied, _bytesCopied.load());
            builder.append(kCountWritesToStashCollections, _writesToStashCollections.load());
            builder.append(kDocumentsCopied, _documentsCopied.load());
            break;
        default:
            MONGO_UNREACHABLE;
    }

    return builder.obj();
}

void ShardingDataTransformInstanceMetrics::onCopyingBegin() {
    _copyingStartTime.store(_clockSource->now());
}

void ShardingDataTransformInstanceMetrics::onCopyingEnd() {
    _copyingEndTime.store(_clockSource->now());
}

void ShardingDataTransformInstanceMetrics::restoreCopyingBegin(Date_t date) {
    _copyingStartTime.store(date);
}

void ShardingDataTransformInstanceMetrics::restoreCopyingEnd(Date_t date) {
    _copyingEndTime.store(date);
}

Date_t ShardingDataTransformInstanceMetrics::getCopyingBegin() const {
    return _copyingStartTime.load();
}

Date_t ShardingDataTransformInstanceMetrics::getCopyingEnd() const {
    return _copyingEndTime.load();
}

void ShardingDataTransformInstanceMetrics::onDocumentsCopied(int64_t documentCount,
                                                             int64_t totalDocumentsSizeBytes,
                                                             Milliseconds elapsed) {
    _documentsCopied.addAndFetch(documentCount);
    _bytesCopied.addAndFetch(totalDocumentsSizeBytes);
    _cumulativeMetrics->onInsertsDuringCloning(documentCount, totalDocumentsSizeBytes, elapsed);
}

int64_t ShardingDataTransformInstanceMetrics::getDocumentsCopiedCount() const {
    return _documentsCopied.load();
}

int64_t ShardingDataTransformInstanceMetrics::getBytesCopiedCount() const {
    return _bytesCopied.load();
}

int64_t ShardingDataTransformInstanceMetrics::getApproxBytesToCopyCount() const {
    return _approxBytesToCopy.load();
}

void ShardingDataTransformInstanceMetrics::restoreDocumentsCopied(int64_t documentCount,
                                                                  int64_t totalDocumentsSizeBytes) {
    _documentsCopied.store(documentCount);
    _bytesCopied.store(totalDocumentsSizeBytes);
}

void ShardingDataTransformInstanceMetrics::setDocumentsToCopyCounts(
    int64_t documentCount, int64_t totalDocumentsSizeBytes) {
    _approxDocumentsToCopy.store(documentCount);
    _approxBytesToCopy.store(totalDocumentsSizeBytes);
}

void ShardingDataTransformInstanceMetrics::setCoordinatorHighEstimateRemainingTimeMillis(
    Milliseconds milliseconds) {
    _coordinatorHighEstimateRemainingTimeMillis.store(milliseconds);
}

void ShardingDataTransformInstanceMetrics::setCoordinatorLowEstimateRemainingTimeMillis(
    Milliseconds milliseconds) {
    _coordinatorLowEstimateRemainingTimeMillis.store(milliseconds);
}

void ShardingDataTransformInstanceMetrics::onLocalInsertDuringOplogFetching(Milliseconds elapsed) {
    _cumulativeMetrics->onLocalInsertDuringOplogFetching(elapsed);
}

void ShardingDataTransformInstanceMetrics::onBatchRetrievedDuringOplogApplying(
    Milliseconds elapsed) {
    _cumulativeMetrics->onBatchRetrievedDuringOplogApplying(elapsed);
}

void ShardingDataTransformInstanceMetrics::onWriteDuringCriticalSection() {
    _writesDuringCriticalSection.addAndFetch(1);
    _cumulativeMetrics->onWriteDuringCriticalSection();
}

void ShardingDataTransformInstanceMetrics::onCriticalSectionBegin() {
    _criticalSectionStartTime.store(_clockSource->now());
}

void ShardingDataTransformInstanceMetrics::onCriticalSectionEnd() {
    _criticalSectionEndTime.store(_clockSource->now());
}

Seconds ShardingDataTransformInstanceMetrics::getOperationRunningTimeSecs() const {
    return duration_cast<Seconds>(_clockSource->now() - _startTime);
}

Seconds ShardingDataTransformInstanceMetrics::getCopyingElapsedTimeSecs() const {
    return getElapsed<Seconds>(_copyingStartTime, _copyingEndTime, _clockSource);
}

Seconds ShardingDataTransformInstanceMetrics::getCriticalSectionElapsedTimeSecs() const {
    return getElapsed<Seconds>(_criticalSectionStartTime, _criticalSectionEndTime, _clockSource);
}

void ShardingDataTransformInstanceMetrics::onWriteToStashedCollections() {
    _writesToStashCollections.fetchAndAdd(1);
    _cumulativeMetrics->onWriteToStashedCollections();
}

void ShardingDataTransformInstanceMetrics::onReadDuringCriticalSection() {
    _readsDuringCriticalSection.fetchAndAdd(1);
    _cumulativeMetrics->onWriteDuringCriticalSection();
}

void ShardingDataTransformInstanceMetrics::accumulateWritesToStashCollections(
    int64_t writesToStashCollections) {
    _writesToStashCollections.fetchAndAdd(writesToStashCollections);
}

void ShardingDataTransformInstanceMetrics::onCloningTotalRemoteBatchRetrieval(
    Milliseconds elapsed) {
    _cumulativeMetrics->onCloningTotalRemoteBatchRetrieval(elapsed);
}

void ShardingDataTransformInstanceMetrics::onOplogLocalBatchApplied(Milliseconds elapsed) {
    _cumulativeMetrics->onOplogLocalBatchApplied(elapsed);
}

ShardingDataTransformCumulativeMetrics*
ShardingDataTransformInstanceMetrics::getCumulativeMetrics() {
    return _cumulativeMetrics;
}

ClockSource* ShardingDataTransformInstanceMetrics::getClockSource() const {
    return _clockSource;
}

void ShardingDataTransformInstanceMetrics::onStarted() {
    _cumulativeMetrics->onStarted();
}

void ShardingDataTransformInstanceMetrics::onSuccess() {
    _cumulativeMetrics->onSuccess();
}

void ShardingDataTransformInstanceMetrics::onFailure() {
    _cumulativeMetrics->onFailure();
}

void ShardingDataTransformInstanceMetrics::onCanceled() {
    _cumulativeMetrics->onCanceled();
}

void ShardingDataTransformInstanceMetrics::setLastOpEndingChunkImbalance(int64_t imbalanceCount) {
    _cumulativeMetrics->setLastOpEndingChunkImbalance(imbalanceCount);
}

}  // namespace mongo
