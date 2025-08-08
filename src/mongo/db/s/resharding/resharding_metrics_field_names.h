/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

namespace mongo {
namespace resharding_metrics {
namespace field_names {

constexpr auto kType = "type";
constexpr auto kDescription = "desc";
constexpr auto kNamespace = "ns";
constexpr auto kOp = "op";
constexpr auto kOriginatingCommand = "originatingCommand";
constexpr auto kOpTimeElapsed = "totalOperationTimeElapsedSecs";
constexpr auto kRemainingOpTimeEstimated = "remainingOperationTimeEstimatedSecs";
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

constexpr auto kApproxDocumentsToCopy = "approxDocumentsToCopy";
constexpr auto kApproxBytesToCopy = "approxBytesToCopy";
constexpr auto kDocumentsCopied = "documentsCopied";
constexpr auto kBytesCopied = "bytesCopied";

constexpr auto kIsSameKeyResharding = "isSameKeyResharding";
constexpr auto kIndexesToBuild = "indexesToBuild";
constexpr auto kIndexesBuilt = "indexesBuilt";
constexpr auto kIndexBuildTimeElapsed = "indexBuildTimeElapsedSecs";

constexpr auto kOplogEntriesFetched = "oplogEntriesFetched";
constexpr auto kOplogEntriesApplied = "oplogEntriesApplied";
constexpr auto kInsertsApplied = "insertsApplied";
constexpr auto kUpdatesApplied = "updatesApplied";
constexpr auto kDeletesApplied = "deletesApplied";

constexpr auto kCountStarted = "countStarted";
constexpr auto kCountSucceeded = "countSucceeded";
constexpr auto kCountFailed = "countFailed";
constexpr auto kCountCanceled = "countCanceled";
constexpr auto kCountSameKeyStarted = "countSameKeyStarted";
constexpr auto kCountSameKeySucceeded = "countSameKeySucceeded";
constexpr auto kCountSameKeyFailed = "countSameKeyFailed";
constexpr auto kCountSameKeyCanceled = "countSameKeyCanceled";

constexpr auto kLastOpEndingChunkImbalance = "lastOpEndingChunkImbalance";
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

constexpr auto kOplogFetchingTotalRemoteBatchRetrievalTimeMillis =
    "oplogFetchingTotalRemoteBatchRetrievalTimeMillis";
constexpr auto kOplogFetchingTotalRemoteBatchesRetrieved =
    "oplogFetchingTotalRemoteBatchesRetrieved";
constexpr auto kOplogFetchingTotalLocalInsertTimeMillis = "oplogFetchingTotalLocalInsertTimeMillis";
constexpr auto kOplogFetchingTotalLocalInserts = "oplogFetchingTotalLocalInserts";
constexpr auto kOplogApplyingTotalLocalBatchRetrievalTimeMillis =
    "oplogApplyingTotalLocalBatchRetrievalTimeMillis";
constexpr auto kOplogApplyingTotalLocalBatchesRetrieved = "oplogApplyingTotalLocalBatchesRetrieved";
constexpr auto kOplogApplyingTotalLocalBatchApplyTimeMillis =
    "oplogApplyingTotalLocalBatchApplyTimeMillis";
constexpr auto kOplogApplyingTotalLocalBatchesApplied = "oplogApplyingTotalLocalBatchesApplied";

}  // namespace field_names
}  // namespace resharding_metrics
}  // namespace mongo
