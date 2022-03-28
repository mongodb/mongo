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

namespace {
constexpr int64_t kPlaceholderTimestampForTesting = 0;
constexpr int64_t kPlaceholderTimeRemainingForTesting = 0;
constexpr auto TEMP_VALUE = "placeholder";

}  // namespace

namespace mongo {

namespace {
constexpr auto kNoDate = Date_t::min();

int64_t getElapsedTimeSeconds(const AtomicWord<Date_t>& startTime,
                              const AtomicWord<Date_t>& endTime,
                              ClockSource* clock) {
    auto start = startTime.load();
    if (start == kNoDate) {
        return 0;
    }
    auto end = endTime.load();
    if (end == kNoDate) {
        end = clock->now();
    }
    return durationCount<Seconds>(end - start);
}
}  // namespace

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
      _clockSource{clockSource},
      _observer{std::move(observer)},
      _cumulativeMetrics{cumulativeMetrics},
      _deregister{_cumulativeMetrics->registerInstanceMetrics(_observer.get())},
      _startTime{startTime},
      _copyingStartTime{kNoDate},
      _copyingEndTime{kNoDate},
      _approxDocumentsToCopy{0},
      _documentsCopied{0},
      _approxBytesToCopy{0},
      _bytesCopied{0},
      _insertsApplied{0},
      _updatesApplied{0},
      _deletesApplied{0},
      _oplogEntriesApplied{0},
      _criticalSectionStartTime{kNoDate},
      _criticalSectionEndTime{kNoDate},
      _writesDuringCriticalSection{0} {}

ShardingDataTransformInstanceMetrics::~ShardingDataTransformInstanceMetrics() {
    if (_deregister) {
        _deregister();
    }
}

int64_t ShardingDataTransformInstanceMetrics::getHighEstimateRemainingTimeMillis() const {
    return kPlaceholderTimeRemainingForTesting;
}

int64_t ShardingDataTransformInstanceMetrics::getLowEstimateRemainingTimeMillis() const {
    return kPlaceholderTimeRemainingForTesting;
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
    builder.append(kOpTimeElapsed, getOperationRunningTimeSecs());

    switch (_role) {
        case Role::kCoordinator:
            builder.append(kAllShardsHighestRemainingOperationTimeEstimatedSecs, TEMP_VALUE);
            builder.append(kAllShardsLowestRemainingOperationTimeEstimatedSecs, TEMP_VALUE);
            builder.append(kCoordinatorState, getStateString());
            builder.append(kApplyTimeElapsed, TEMP_VALUE);
            builder.append(kCopyTimeElapsed, getCopyingElapsedTimeSecs());
            builder.append(kCriticalSectionTimeElapsed, getCriticalSectionElapsedTimeSecs());
            break;
        case Role::kDonor:
            builder.append(kDonorState, getStateString());
            builder.append(kCriticalSectionTimeElapsed, getCriticalSectionElapsedTimeSecs());
            builder.append(kCountWritesDuringCriticalSection, _writesDuringCriticalSection.load());
            builder.append(kCountReadsDuringCriticalSection, _readsDuringCriticalSection.load());
            break;
        case Role::kRecipient:
            builder.append(kRecipientState, getStateString());
            builder.append(kApplyTimeElapsed, TEMP_VALUE);
            builder.append(kCopyTimeElapsed, getCopyingElapsedTimeSecs());
            builder.append(kRemainingOpTimeEstimated, TEMP_VALUE);
            builder.append(kApproxDocumentsToCopy, _approxDocumentsToCopy.load());
            builder.append(kApproxBytesToCopy, _approxBytesToCopy.load());
            builder.append(kBytesCopied, _bytesCopied.load());
            builder.append(kCountWritesToStashCollections, _writesToStashCollections.load());
            builder.append(kInsertsApplied, _insertsApplied.load());
            builder.append(kUpdatesApplied, _updatesApplied.load());
            builder.append(kDeletesApplied, _deletesApplied.load());
            builder.append(kOplogEntriesApplied, _oplogEntriesApplied.load());
            builder.append(kOplogEntriesFetched, TEMP_VALUE);
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

void ShardingDataTransformInstanceMetrics::onDocumentsCopied(int64_t documentCount,
                                                             int64_t totalDocumentsSizeBytes) {
    _documentsCopied.addAndFetch(documentCount);
    _bytesCopied.addAndFetch(totalDocumentsSizeBytes);
}

void ShardingDataTransformInstanceMetrics::setDocumentsToCopyCounts(
    int64_t documentCount, int64_t totalDocumentsSizeBytes) {
    _approxDocumentsToCopy.store(documentCount);
    _approxBytesToCopy.store(totalDocumentsSizeBytes);
}

void ShardingDataTransformInstanceMetrics::onInsertApplied() {
    _insertsApplied.addAndFetch(1);
}

void ShardingDataTransformInstanceMetrics::onUpdateApplied() {
    _updatesApplied.addAndFetch(1);
}

void ShardingDataTransformInstanceMetrics::onDeleteApplied() {
    _deletesApplied.addAndFetch(1);
}

void ShardingDataTransformInstanceMetrics::onOplogEntriesApplied(int64_t numEntries) {
    _oplogEntriesApplied.addAndFetch(numEntries);
}

void ShardingDataTransformInstanceMetrics::onWriteDuringCriticalSection() {
    _writesDuringCriticalSection.addAndFetch(1);
}

void ShardingDataTransformInstanceMetrics::onCriticalSectionBegin() {
    _criticalSectionStartTime.store(_clockSource->now());
}

void ShardingDataTransformInstanceMetrics::onCriticalSectionEnd() {
    _criticalSectionEndTime.store(_clockSource->now());
}

inline int64_t ShardingDataTransformInstanceMetrics::getOperationRunningTimeSecs() const {
    return durationCount<Seconds>(_clockSource->now() - _startTime);
}

int64_t ShardingDataTransformInstanceMetrics::getCriticalSectionElapsedTimeSecs() const {
    return getElapsedTimeSeconds(_criticalSectionStartTime, _criticalSectionEndTime, _clockSource);
}

int64_t ShardingDataTransformInstanceMetrics::getCopyingElapsedTimeSecs() const {
    return getElapsedTimeSeconds(_copyingStartTime, _copyingEndTime, _clockSource);
}

void ShardingDataTransformInstanceMetrics::onWriteToStashedCollections() {
    _writesToStashCollections.fetchAndAdd(1);
}

void ShardingDataTransformInstanceMetrics::onReadDuringCriticalSection() {
    _readsDuringCriticalSection.fetchAndAdd(1);
}

void ShardingDataTransformInstanceMetrics::accumulateValues(int64_t insertsApplied,
                                                            int64_t updatesApplied,
                                                            int64_t deletesApplied) {
    _insertsApplied.fetchAndAdd(insertsApplied);
    _updatesApplied.fetchAndAdd(updatesApplied);
    _deletesApplied.fetchAndAdd(deletesApplied);
}

}  // namespace mongo
