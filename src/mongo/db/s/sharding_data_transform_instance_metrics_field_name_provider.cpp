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

#include "mongo/db/s/sharding_data_transform_instance_metrics_field_name_provider.h"

namespace mongo {
namespace {
constexpr auto kType = "type";
constexpr auto kDescription = "desc";
constexpr auto kNamespace = "ns";
constexpr auto kOp = "op";
constexpr auto kOriginatingCommand = "originatingCommand";
constexpr auto kOpTimeElapsed = "totalOperationTimeElapsedSecs";
constexpr auto kCriticalSectionTimeElapsed = "totalCriticalSectionTimeElapsedSecs";
constexpr auto kRemainingOpTimeEstimated = "remainingOperationTimeEstimatedSecs";
constexpr auto kCopyTimeElapsed = "totalCopyTimeElapsedSecs";
constexpr auto kCountWritesToStashCollections = "countWritesToStashCollections";
constexpr auto kCountWritesDuringCriticalSection = "countWritesDuringCriticalSection";
constexpr auto kCountReadsDuringCriticalSection = "countReadsDuringCriticalSection";
constexpr auto kCoordinatorState = "coordinatorState";
constexpr auto kDonorState = "donorState";
constexpr auto kRecipientState = "recipientState";
constexpr auto kAllShardsLowestRemainingOperationTimeEstimatedSecs =
    "allShardsLowestRemainingOperationTimeEstimatedSecs";
constexpr auto kAllShardsHighestRemainingOperationTimeEstimatedSecs =
    "allShardsHighestRemainingOperationTimeEstimatedSecs";
}  // namespace

StringData ShardingDataTransformInstanceMetricsFieldNameProvider::getForType() const {
    return kType;
}

StringData ShardingDataTransformInstanceMetricsFieldNameProvider::getForDescription() const {
    return kDescription;
}

StringData ShardingDataTransformInstanceMetricsFieldNameProvider::getForNamespace() const {
    return kNamespace;
}

StringData ShardingDataTransformInstanceMetricsFieldNameProvider::getForOp() const {
    return kOp;
}

StringData ShardingDataTransformInstanceMetricsFieldNameProvider::getForOriginatingCommand() const {
    return kOriginatingCommand;
}

StringData ShardingDataTransformInstanceMetricsFieldNameProvider::getForOpTimeElapsed() const {
    return kOpTimeElapsed;
}

StringData ShardingDataTransformInstanceMetricsFieldNameProvider::getForCriticalSectionTimeElapsed()
    const {
    return kCriticalSectionTimeElapsed;
}

StringData ShardingDataTransformInstanceMetricsFieldNameProvider::getForRemainingOpTimeEstimated()
    const {
    return kRemainingOpTimeEstimated;
}

StringData ShardingDataTransformInstanceMetricsFieldNameProvider::getForCopyTimeElapsed() const {
    return kCopyTimeElapsed;
}

StringData
ShardingDataTransformInstanceMetricsFieldNameProvider::getForCountWritesToStashCollections() const {
    return kCountWritesToStashCollections;
}

StringData
ShardingDataTransformInstanceMetricsFieldNameProvider::getForCountWritesDuringCriticalSection()
    const {
    return kCountWritesDuringCriticalSection;
}

StringData
ShardingDataTransformInstanceMetricsFieldNameProvider::getForCountReadsDuringCriticalSection()
    const {
    return kCountReadsDuringCriticalSection;
}

StringData ShardingDataTransformInstanceMetricsFieldNameProvider::getForCoordinatorState() const {
    return kCoordinatorState;
}

StringData ShardingDataTransformInstanceMetricsFieldNameProvider::getForDonorState() const {
    return kDonorState;
}

StringData ShardingDataTransformInstanceMetricsFieldNameProvider::getForRecipientState() const {
    return kRecipientState;
}

StringData ShardingDataTransformInstanceMetricsFieldNameProvider::
    getForAllShardsLowestRemainingOperationTimeEstimatedSecs() const {
    return kAllShardsLowestRemainingOperationTimeEstimatedSecs;
}
StringData ShardingDataTransformInstanceMetricsFieldNameProvider::
    getForAllShardsHighestRemainingOperationTimeEstimatedSecs() const {
    return kAllShardsHighestRemainingOperationTimeEstimatedSecs;
}
}  // namespace mongo
