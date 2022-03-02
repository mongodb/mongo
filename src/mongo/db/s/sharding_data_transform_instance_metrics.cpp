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

ShardingDataTransformInstanceMetrics::ShardingDataTransformInstanceMetrics(
    UUID instanceId,
    BSONObj originalCommand,
    NamespaceString sourceNs,
    Role role,
    ShardingDataTransformCumulativeMetrics* cumulativeMetrics)
    : ShardingDataTransformInstanceMetrics{
          std::move(instanceId),
          std::move(sourceNs),
          role,
          std::move(originalCommand),
          cumulativeMetrics,
          std::make_unique<ShardingDataTransformMetricsObserver>(this)} {}

ShardingDataTransformInstanceMetrics::ShardingDataTransformInstanceMetrics(
    UUID instanceId,
    NamespaceString sourceNs,
    Role role,
    BSONObj originalCommand,
    ShardingDataTransformCumulativeMetrics* cumulativeMetrics,
    ObserverPtr observer)
    : _instanceId{std::move(instanceId)},
      _sourceNs{std::move(sourceNs)},
      _role{role},
      _originalCommand{std::move(originalCommand)},
      _observer{std::move(observer)},
      _cumulativeMetrics{cumulativeMetrics},
      _deregister{_cumulativeMetrics->registerInstanceMetrics(_observer.get())},
      _placeholderUuidForTesting(UUID::gen()),
      _insertsApplied{0},
      _updatesApplied{0},
      _deletesApplied{0},
      _oplogEntriesApplied{0} {}

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

int64_t ShardingDataTransformInstanceMetrics::getStartTimestamp() const {
    return kPlaceholderTimestampForTesting;
}

const UUID& ShardingDataTransformInstanceMetrics::getUuid() const {
    return _placeholderUuidForTesting;
}

ShardingDataTransformInstanceMetrics::Role ShardingDataTransformInstanceMetrics::getRole() const {
    return _role;
}

std::string ShardingDataTransformInstanceMetrics::createOperationDescription() const noexcept {
    return fmt::format("ShardingDataTransformMetrics{}Service {}",
                       ShardingDataTransformMetrics::getRoleName(_role),
                       _instanceId.toString());
}

BSONObj ShardingDataTransformInstanceMetrics::reportForCurrentOp() const noexcept {

    BSONObjBuilder builder;
    builder.append(kType, "op");
    builder.append(kDescription, createOperationDescription());
    builder.append(kOp, "command");
    builder.append(kNamespace, _sourceNs.toString());
    builder.append(kOriginalCommand, _originalCommand);
    builder.append(kOpTimeElapsed, TEMP_VALUE);

    switch (_role) {
        case Role::kCoordinator:
            builder.append(kAllShardsHighestRemainingOperationTimeEstimatedSecs, TEMP_VALUE);
            builder.append(kAllShardsLowestRemainingOperationTimeEstimatedSecs, TEMP_VALUE);
            builder.append(kCoordinatorState, TEMP_VALUE);
            builder.append(kApplyTimeElapsed, TEMP_VALUE);
            builder.append(kCopyTimeElapsed, TEMP_VALUE);
            builder.append(kCriticalSectionTimeElapsed, TEMP_VALUE);
            break;
        case Role::kDonor:
            builder.append(kDonorState, TEMP_VALUE);
            builder.append(kCriticalSectionTimeElapsed, TEMP_VALUE);
            builder.append(kCountWritesDuringCriticalSection, TEMP_VALUE);
            builder.append(kCountReadsDuringCriticalSection, TEMP_VALUE);
            break;
        case Role::kRecipient:
            builder.append(kRecipientState, TEMP_VALUE);
            builder.append(kApplyTimeElapsed, TEMP_VALUE);
            builder.append(kCopyTimeElapsed, TEMP_VALUE);
            builder.append(kRemainingOpTimeEstimated, TEMP_VALUE);
            builder.append(kApproxDocumentsToCopy, TEMP_VALUE);
            builder.append(kApproxBytesToCopy, TEMP_VALUE);
            builder.append(kBytesCopied, TEMP_VALUE);
            builder.append(kCountWritesToStashCollections, TEMP_VALUE);
            builder.append(kInsertsApplied, _insertsApplied.load());
            builder.append(kUpdatesApplied, _updatesApplied.load());
            builder.append(kDeletesApplied, _deletesApplied.load());
            builder.append(kOplogEntriesApplied, _oplogEntriesApplied.load());
            builder.append(kOplogEntriesFetched, TEMP_VALUE);
            builder.append(kDocumentsCopied, TEMP_VALUE);
            break;
        default:
            MONGO_UNREACHABLE;
    }

    return builder.obj();
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

}  // namespace mongo
