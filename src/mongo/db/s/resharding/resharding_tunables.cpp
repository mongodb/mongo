// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/s/resharding/resharding_tunables.h"

#include "mongo/db/s/resharding/resharding_server_parameters_gen.h"

// Sets the tunable `Name` to the current value of the server parameter `param`, unless it was
// explicitly specified. Operates on the `tunables` argument of the enclosing function.
#define FILL_IF_UNSET(Name, param)        \
    if (!tunables.get##Name()) {          \
        tunables.set##Name(param.load()); \
    }

namespace mongo {
namespace resharding {

void fillDefaults(ReshardingTunables& tunables) {
    FILL_IF_UNSET(CollectionClonerMaxStalenessSeconds,
                  gReshardingCollectionClonerMaxStalenessSeconds);
    FILL_IF_UNSET(CollectionClonerBatchSizeCount, gReshardingCollectionClonerBatchSizeCount);
    FILL_IF_UNSET(CollectionClonerWriteThreadCount, gReshardingCollectionClonerWriteThreadCount);
    FILL_IF_UNSET(TxnClonerProgressBatchSize, gReshardingTxnClonerProgressBatchSize);
    FILL_IF_UNSET(OplogFetcherInsertBatchLimitOperations,
                  gReshardingOplogFetcherInsertBatchLimitOperations);
    FILL_IF_UNSET(OplogFetcherInsertBatchLimitBytes, gReshardingOplogFetcherInsertBatchLimitBytes);
    FILL_IF_UNSET(OplogFetcherSleepMillisBeforeCriticalSection,
                  gReshardingOplogFetcherSleepMillisBeforeCriticalSection);
    FILL_IF_UNSET(OplogFetcherSleepMillisDuringCriticalSection,
                  gReshardingOplogFetcherSleepMillisDuringCriticalSection);
    FILL_IF_UNSET(OplogFetcherTargetPrimaryDuringCriticalSection,
                  gReshardingOplogFetcherTargetPrimaryDuringCriticalSection);
    FILL_IF_UNSET(OplogBatchTaskCount, gReshardingOplogBatchTaskCount);
    FILL_IF_UNSET(OplogBatchLimitOperations, gReshardingOplogBatchLimitOperations);
    FILL_IF_UNSET(OplogBatchLimitBytes, gReshardingOplogBatchLimitBytes);
    FILL_IF_UNSET(OplogApplierMaxLockRequestTimeoutMillis,
                  gReshardingOplogApplierMaxLockRequestTimeoutMillis);
    FILL_IF_UNSET(RemainingReshardingOperationTimePrepareThresholdMillis,
                  gRemainingReshardingOperationTimePrepareThresholdMillis);
    FILL_IF_UNSET(RemainingTimeEstimateBasedOnMovingAverage,
                  gReshardingRemainingTimeEstimateBasedOnMovingAverage);
    FILL_IF_UNSET(ExponentialMovingAverageTimeToFetchAndApplySmoothingFactor,
                  gReshardingExponentialMovingAverageTimeToFetchAndApplySmoothingFactor);
    FILL_IF_UNSET(ExponentialMovingAverageTimeToFetchAndApplyIntervalMillis,
                  gReshardingExponentialMovingAverageTimeToFetchAndApplyIntervalMillis);
    FILL_IF_UNSET(VerificationChangeStreamsEventsBatchSizeLimit,
                  gReshardingVerificationChangeStreamsEventsBatchSizeLimit);
    FILL_IF_UNSET(VerificationChangeStreamsEventsBatchTimeLimitMillis,
                  gReshardingVerificationChangeStreamsEventsBatchTimeLimitMillis);
    FILL_IF_UNSET(VerificationChangeStreamsSleepMS, gReshardingVerificationChangeStreamsSleepMS);
    FILL_IF_UNSET(VerificationChangeStreamsGetMoreMaxTimeMS,
                  gReshardingVerificationChangeStreamsGetMoreMaxTimeMS);
    FILL_IF_UNSET(WaitForReplicationIntervalMilliseconds,
                  gReshardingWaitForReplicationIntervalMilliseconds);
    FILL_IF_UNSET(WaitForReplicationTimeoutSeconds, gReshardingWaitForReplicationTimeoutSeconds);
    FILL_IF_UNSET(MaxReplicationLagSecondsBeforeBuildingIndexes,
                  gReshardingMaxReplicationLagSecondsBeforeBuildingIndexes);
    FILL_IF_UNSET(AbortUnpreparedTransactionsUponPreparingToBlockWrites,
                  gReshardingAbortUnpreparedTransactionsUponPreparingToBlockWrites);
    FILL_IF_UNSET(IndexVerification, gReshardingIndexVerification);
    FILL_IF_UNSET(CollectionOptionsVerification, gReshardingCollectionOptionsVerification);
    FILL_IF_UNSET(MinimumOperationDurationMillis, gReshardingMinimumOperationDurationMillis);
    FILL_IF_UNSET(DelayBeforeRemainingOperationTimeQueryMillis,
                  gReshardingDelayBeforeRemainingOperationTimeQueryMillis);
    FILL_IF_UNSET(MaxDelayBetweenRemainingOperationTimeQueriesMillis,
                  gReshardingMaxDelayBetweenRemainingOperationTimeQueriesMillis);
    FILL_IF_UNSET(CriticalSectionTimeoutMillis, gReshardingCriticalSectionTimeoutMillis);
    FILL_IF_UNSET(VerificationDeltaWaitRemainingCriticalSectionPercent,
                  gReshardingVerificationDeltaWaitRemainingCriticalSectionPercent);
    FILL_IF_UNSET(RemainingReshardingOperationTimeThresholdMillis,
                  gRemainingReshardingOperationTimeThresholdMillis);
    FILL_IF_UNSET(RemainingTimeEstimateAccountsForDonorReplicationLag,
                  gReshardingRemainingTimeEstimateAccountsForDonorReplicationLag);
    FILL_IF_UNSET(RemainingTimeEstimateAccountsForRecipientReplicationLag,
                  gReshardingRemainingTimeEstimateAccountsForRecipientReplicationLag);
    FILL_IF_UNSET(RequireExplicitCommit, gReshardingRequireExplicitCommit);
    FILL_IF_UNSET(CoordinatorQuiescePeriodMillis, gReshardingCoordinatorQuiescePeriodMillis);
    FILL_IF_UNSET(FetchDocumentsToCopyTimeoutSecs, gReshardingFetchDocumentsToCopyTimeoutSecs);
    FILL_IF_UNSET(DocumentVerification, gReshardingDocumentVerification);
    FILL_IF_UNSET(DocumentValidationMaxCollectionSizeBytes,
                  gReshardingDocumentValidationMaxCollectionSizeBytes);
    FILL_IF_UNSET(CollectionSizeEstimationTimeoutMS, gReshardingCollectionSizeEstimationTimeoutMS);
}

#undef FILL_IF_UNSET

}  // namespace resharding
}  // namespace mongo
