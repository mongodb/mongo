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

#include "mongo/db/s/sharding_data_transform_cumulative_metrics_field_name_provider.h"

namespace mongo {

namespace {
using Provider = ShardingDataTransformCumulativeMetricsFieldNameProvider;
using Placeholder = ShardingDataTransformCumulativeMetricsFieldNamePlaceholder;
constexpr auto kCountStarted = "countStarted";
constexpr auto kCountSucceeded = "countSucceeded";
constexpr auto kCountFailed = "countFailed";
constexpr auto kCountCanceled = "countCanceled";
constexpr auto kLastOpEndingChunkImbalance = "lastOpEndingChunkImbalance";
constexpr auto kDocumentsCopied = "documentsCopied";
constexpr auto kBytesCopied = "bytesCopied";
constexpr auto kCountWritesToStashCollections = "countWritesToStashCollections";
constexpr auto kCountWritesDuringCriticalSection = "countWritesDuringCriticalSection";
constexpr auto kCountReadsDuringCriticalSection = "countReadsDuringCriticalSection";
constexpr auto kCoordinatorAllShardsLowestRemainingOperationTimeEstimatedMillis =
    "coordinatorAllShardsLowestRemainingOperationTimeEstimatedMillis";
constexpr auto kCoordinatorAllShardsHighestRemainingOperationTimeEstimatedMillis =
    "coordinatorAllShardsHighestRemainingOperationTimeEstimatedMillis";
constexpr auto kRecipientRemainingOperationTimeEstimatedMillis =
    "recipientRemainingOperationTimeEstimatedMillis";
constexpr auto kCollectionCloningTotalRemoteBatchRetrievalTimeMillis =
    "collectionCloningTotalRemoteBatchRetrievalTimeMillis";
constexpr auto kCollectionCloningTotalRemoteBatchesRetrieved =
    "collectionCloningTotalRemoteBatchesRetrieved";
constexpr auto kCollectionCloningTotalLocalInsertTimeMillis =
    "collectionCloningTotalLocalInsertTimeMillis";
constexpr auto kCollectionCloningTotalLocalInserts = "collectionCloningTotalLocalInserts";
}  // namespace

StringData Provider::getForCountStarted() const {
    return kCountStarted;
}
StringData Provider::getForCountSucceeded() const {
    return kCountSucceeded;
}
StringData Provider::getForCountFailed() const {
    return kCountFailed;
}
StringData Provider::getForCountCanceled() const {
    return kCountCanceled;
}
StringData Provider::getForLastOpEndingChunkImbalance() const {
    return kLastOpEndingChunkImbalance;
}
StringData Provider::getForCountWritesToStashCollections() const {
    return kCountWritesToStashCollections;
}
StringData Provider::getForCountWritesDuringCriticalSection() const {
    return kCountWritesDuringCriticalSection;
}
StringData Provider::getForCountReadsDuringCriticalSection() const {
    return kCountReadsDuringCriticalSection;
}
StringData Provider::getForCoordinatorAllShardsLowestRemainingOperationTimeEstimatedMillis() const {
    return kCoordinatorAllShardsLowestRemainingOperationTimeEstimatedMillis;
}
StringData Provider::getForCoordinatorAllShardsHighestRemainingOperationTimeEstimatedMillis()
    const {
    return kCoordinatorAllShardsHighestRemainingOperationTimeEstimatedMillis;
}
StringData Provider::getForRecipientRemainingOperationTimeEstimatedMillis() const {
    return kRecipientRemainingOperationTimeEstimatedMillis;
}
StringData Provider::getForCollectionCloningTotalRemoteBatchRetrievalTimeMillis() const {
    return kCollectionCloningTotalRemoteBatchRetrievalTimeMillis;
}
StringData Provider::getForCollectionCloningTotalRemoteBatchesRetrieved() const {
    return kCollectionCloningTotalRemoteBatchesRetrieved;
}
StringData Provider::getForCollectionCloningTotalLocalInsertTimeMillis() const {
    return kCollectionCloningTotalLocalInsertTimeMillis;
}
StringData Provider::getForCollectionCloningTotalLocalInserts() const {
    return kCollectionCloningTotalLocalInserts;
}

StringData Placeholder::getForDocumentsProcessed() const {
    return kDocumentsCopied;
}
StringData Placeholder::getForBytesWritten() const {
    return kBytesCopied;
}

}  // namespace mongo
