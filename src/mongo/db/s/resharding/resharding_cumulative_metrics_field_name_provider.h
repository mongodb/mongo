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

#pragma once

#include "mongo/db/s/sharding_data_transform_cumulative_metrics_field_name_provider.h"

namespace mongo {

class ReshardingCumulativeMetricsFieldNameProvider
    : public ShardingDataTransformCumulativeMetricsFieldNameProvider {
public:
    virtual StringData getForDocumentsProcessed() const override;
    virtual StringData getForBytesWritten() const override;
    StringData getForOplogEntriesFetched() const;
    StringData getForOplogEntriesApplied() const;
    StringData getForInsertsApplied() const;
    StringData getForUpdatesApplied() const;
    StringData getForDeletesApplied() const;
    StringData getForOplogFetchingTotalRemoteBatchRetrievalTimeMillis() const;
    StringData getForOplogFetchingTotalRemoteBatchesRetrieved() const;
    StringData getForOplogFetchingTotalLocalInsertTimeMillis() const;
    StringData getForOplogFetchingTotalLocalInserts() const;
    StringData getForOplogApplyingTotalLocalBatchRetrievalTimeMillis() const;
    StringData getForOplogApplyingTotalLocalBatchesRetrieved() const;
    StringData getForOplogApplyingTotalLocalBatchApplyTimeMillis() const;
    StringData getForOplogApplyingTotalLocalBatchesApplied() const;
    StringData getForCountInstancesInCoordinatorState1Initializing() const;
    StringData getForCountInstancesInCoordinatorState2PreparingToDonate() const;
    StringData getForCountInstancesInCoordinatorState3Cloning() const;
    StringData getForCountInstancesInCoordinatorState4Applying() const;
    StringData getForCountInstancesInCoordinatorState5BlockingWrites() const;
    StringData getForCountInstancesInCoordinatorState6Aborting() const;
    StringData getForCountInstancesInCoordinatorState7Committing() const;
    StringData getForCountInstancesInRecipientState1AwaitingFetchTimestamp() const;
    StringData getForCountInstancesInRecipientState2CreatingCollection() const;
    StringData getForCountInstancesInRecipientState3Cloning() const;
    StringData getForCountInstancesInRecipientState4Applying() const;
    StringData getForCountInstancesInRecipientState5Error() const;
    StringData getForCountInstancesInRecipientState6StrictConsistency() const;
    StringData getForCountInstancesInRecipientState7Done() const;
    StringData getForCountInstancesInDonorState1PreparingToDonate() const;
    StringData getForCountInstancesInDonorState2DonatingInitialData() const;
    StringData getForCountInstancesInDonorState3DonatingOplogEntries() const;
    StringData getForCountInstancesInDonorState4PreparingToBlockWrites() const;
    StringData getForCountInstancesInDonorState5Error() const;
    StringData getForCountInstancesInDonorState6BlockingWrites() const;
    StringData getForCountInstancesInDonorState7Done() const;
};

}  // namespace mongo
