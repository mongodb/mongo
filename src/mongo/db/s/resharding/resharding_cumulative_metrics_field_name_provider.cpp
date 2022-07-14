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

#include "mongo/db/s/resharding/resharding_cumulative_metrics_field_name_provider.h"

namespace mongo {

namespace {
using Provider = ReshardingCumulativeMetricsFieldNameProvider;
constexpr auto kDocumentsCopied = "documentsCopied";
constexpr auto kBytesCopied = "bytesCopied";
constexpr auto kOplogEntriesFetched = "oplogEntriesFetched";
constexpr auto kOplogEntriesApplied = "oplogEntriesApplied";
constexpr auto kInsertsApplied = "insertsApplied";
constexpr auto kUpdatesApplied = "updatesApplied";
constexpr auto kDeletesApplied = "deletesApplied";
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
constexpr auto kCountInstancesInCoordinatorState1Initializing =
    "countInstancesInCoordinatorState1Initializing";
constexpr auto kCountInstancesInCoordinatorState2PreparingToDonate =
    "countInstancesInCoordinatorState2PreparingToDonate";
constexpr auto kCountInstancesInCoordinatorState3Cloning =
    "countInstancesInCoordinatorState3Cloning";
constexpr auto kCountInstancesInCoordinatorState4Applying =
    "countInstancesInCoordinatorState4Applying";
constexpr auto kCountInstancesInCoordinatorState5BlockingWrites =
    "countInstancesInCoordinatorState5BlockingWrites";
constexpr auto kCountInstancesInCoordinatorState6Aborting =
    "countInstancesInCoordinatorState6Aborting";
constexpr auto kCountInstancesInCoordinatorState7Committing =
    "countInstancesInCoordinatorState7Committing";
constexpr auto kCountInstancesInRecipientState1AwaitingFetchTimestamp =
    "countInstancesInRecipientState1AwaitingFetchTimestamp";
constexpr auto kCountInstancesInRecipientState2CreatingCollection =
    "countInstancesInRecipientState2CreatingCollection";
constexpr auto kCountInstancesInRecipientState3Cloning = "countInstancesInRecipientState3Cloning";
constexpr auto kCountInstancesInRecipientState4Applying = "countInstancesInRecipientState4Applying";
constexpr auto kCountInstancesInRecipientState5Error = "countInstancesInRecipientState5Error";
constexpr auto kCountInstancesInRecipientState6StrictConsistency =
    "countInstancesInRecipientState6StrictConsistency";
constexpr auto kCountInstancesInRecipientState7Done = "countInstancesInRecipientState7Done";
constexpr auto kCountInstancesInDonorState1PreparingToDonate =
    "countInstancesInDonorState1PreparingToDonate";
constexpr auto kCountInstancesInDonorState2DonatingInitialData =
    "countInstancesInDonorState2DonatingInitialData";
constexpr auto kCountInstancesInDonorState3DonatingOplogEntries =
    "countInstancesInDonorState3DonatingOplogEntries";
constexpr auto kCountInstancesInDonorState4PreparingToBlockWrites =
    "countInstancesInDonorState4PreparingToBlockWrites";
constexpr auto kCountInstancesInDonorState5Error = "countInstancesInDonorState5Error";
constexpr auto kCountInstancesInDonorState6BlockingWrites =
    "countInstancesInDonorState6BlockingWrites";
constexpr auto kCountInstancesInDonorState7Done = "countInstancesInDonorState7Done";
}  // namespace

StringData Provider::getForDocumentsProcessed() const {
    return kDocumentsCopied;
}
StringData Provider::getForBytesWritten() const {
    return kBytesCopied;
}
StringData Provider::getForOplogEntriesFetched() const {
    return kOplogEntriesFetched;
}
StringData Provider::getForOplogEntriesApplied() const {
    return kOplogEntriesApplied;
}
StringData Provider::getForInsertsApplied() const {
    return kInsertsApplied;
}
StringData Provider::getForUpdatesApplied() const {
    return kUpdatesApplied;
}
StringData Provider::getForDeletesApplied() const {
    return kDeletesApplied;
}
StringData Provider::getForOplogFetchingTotalRemoteBatchRetrievalTimeMillis() const {
    return kOplogFetchingTotalRemoteBatchRetrievalTimeMillis;
}
StringData Provider::getForOplogFetchingTotalRemoteBatchesRetrieved() const {
    return kOplogFetchingTotalRemoteBatchesRetrieved;
}
StringData Provider::getForOplogFetchingTotalLocalInsertTimeMillis() const {
    return kOplogFetchingTotalLocalInsertTimeMillis;
}
StringData Provider::getForOplogFetchingTotalLocalInserts() const {
    return kOplogFetchingTotalLocalInserts;
}
StringData Provider::getForOplogApplyingTotalLocalBatchRetrievalTimeMillis() const {
    return kOplogApplyingTotalLocalBatchRetrievalTimeMillis;
}
StringData Provider::getForOplogApplyingTotalLocalBatchesRetrieved() const {
    return kOplogApplyingTotalLocalBatchesRetrieved;
}
StringData Provider::getForOplogApplyingTotalLocalBatchApplyTimeMillis() const {
    return kOplogApplyingTotalLocalBatchApplyTimeMillis;
}
StringData Provider::getForOplogApplyingTotalLocalBatchesApplied() const {
    return kOplogApplyingTotalLocalBatchesApplied;
}
StringData Provider::getForCountInstancesInCoordinatorState1Initializing() const {
    return kCountInstancesInCoordinatorState1Initializing;
}
StringData Provider::getForCountInstancesInCoordinatorState2PreparingToDonate() const {
    return kCountInstancesInCoordinatorState2PreparingToDonate;
}
StringData Provider::getForCountInstancesInCoordinatorState3Cloning() const {
    return kCountInstancesInCoordinatorState3Cloning;
}
StringData Provider::getForCountInstancesInCoordinatorState4Applying() const {
    return kCountInstancesInCoordinatorState4Applying;
}
StringData Provider::getForCountInstancesInCoordinatorState5BlockingWrites() const {
    return kCountInstancesInCoordinatorState5BlockingWrites;
}
StringData Provider::getForCountInstancesInCoordinatorState6Aborting() const {
    return kCountInstancesInCoordinatorState6Aborting;
}
StringData Provider::getForCountInstancesInCoordinatorState7Committing() const {
    return kCountInstancesInCoordinatorState7Committing;
}
StringData Provider::getForCountInstancesInRecipientState1AwaitingFetchTimestamp() const {
    return kCountInstancesInRecipientState1AwaitingFetchTimestamp;
}
StringData Provider::getForCountInstancesInRecipientState2CreatingCollection() const {
    return kCountInstancesInRecipientState2CreatingCollection;
}
StringData Provider::getForCountInstancesInRecipientState3Cloning() const {
    return kCountInstancesInRecipientState3Cloning;
}
StringData Provider::getForCountInstancesInRecipientState4Applying() const {
    return kCountInstancesInRecipientState4Applying;
}
StringData Provider::getForCountInstancesInRecipientState5Error() const {
    return kCountInstancesInRecipientState5Error;
}
StringData Provider::getForCountInstancesInRecipientState6StrictConsistency() const {
    return kCountInstancesInRecipientState6StrictConsistency;
}
StringData Provider::getForCountInstancesInRecipientState7Done() const {
    return kCountInstancesInRecipientState7Done;
}
StringData Provider::getForCountInstancesInDonorState1PreparingToDonate() const {
    return kCountInstancesInDonorState1PreparingToDonate;
}
StringData Provider::getForCountInstancesInDonorState2DonatingInitialData() const {
    return kCountInstancesInDonorState2DonatingInitialData;
}
StringData Provider::getForCountInstancesInDonorState3DonatingOplogEntries() const {
    return kCountInstancesInDonorState3DonatingOplogEntries;
}
StringData Provider::getForCountInstancesInDonorState4PreparingToBlockWrites() const {
    return kCountInstancesInDonorState4PreparingToBlockWrites;
}
StringData Provider::getForCountInstancesInDonorState5Error() const {
    return kCountInstancesInDonorState5Error;
}
StringData Provider::getForCountInstancesInDonorState6BlockingWrites() const {
    return kCountInstancesInDonorState6BlockingWrites;
}
StringData Provider::getForCountInstancesInDonorState7Done() const {
    return kCountInstancesInDonorState7Done;
}

}  // namespace mongo
