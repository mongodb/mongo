// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/sharding_environment/sharding_statistics.h"

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/feature_flag.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/db/sharding_environment/sharding_feature_flags_gen.h"
#include "mongo/util/decorable.h"

#include <utility>

namespace mongo {
namespace {

const auto getShardingStatistics = ServiceContext::declareDecoration<ShardingStatistics>();

}  // namespace

ShardingStatistics& ShardingStatistics::get(ServiceContext* serviceContext) {
    return getShardingStatistics(serviceContext);
}

ShardingStatistics& ShardingStatistics::get(OperationContext* opCtx) {
    return get(opCtx->getServiceContext());
}

void ShardingStatistics::report(BSONObjBuilder* builder) const {
    builder->append("countStaleConfigErrors", countStaleConfigErrors.loadRelaxed());

    builder->append("countDonorMoveChunkStarted", countDonorMoveChunkStarted.loadRelaxed());
    builder->append("countDonorMoveChunkCommitted", countDonorMoveChunkCommitted.loadRelaxed());
    builder->append("countDonorMoveChunkAborted", countDonorMoveChunkAborted.loadRelaxed());
    builder->append("totalDonorMoveChunkTimeMillis", totalDonorMoveChunkTimeMillis.loadRelaxed());
    builder->append("totalDonorChunkCloneTimeMillis", totalDonorChunkCloneTimeMillis.loadRelaxed());
    builder->append("totalCriticalSectionCommitTimeMillis",
                    totalCriticalSectionCommitTimeMillis.loadRelaxed());
    builder->append("totalCriticalSectionTimeMillis", totalCriticalSectionTimeMillis.loadRelaxed());
    builder->append("totalRecipientCriticalSectionTimeMillis",
                    totalRecipientCriticalSectionTimeMillis.loadRelaxed());
    builder->append("countDocsClonedOnRecipient", countDocsClonedOnRecipient.loadRelaxed());
    builder->append("countBytesClonedOnRecipient", countBytesClonedOnRecipient.loadRelaxed());
    builder->append("countDocsClonedOnCatchUpOnRecipient",
                    countDocsClonedOnCatchUpOnRecipient.loadRelaxed());
    builder->append("countBytesClonedOnCatchUpOnRecipient",
                    countBytesClonedOnCatchUpOnRecipient.loadRelaxed());
    builder->append("countDocsClonedOnDonor", countDocsClonedOnDonor.loadRelaxed());
    builder->append("countBytesClonedOnDonor", countBytesClonedOnDonor.loadRelaxed());
    builder->append("countRecipientMoveChunkStarted", countRecipientMoveChunkStarted.loadRelaxed());
    builder->append("countDocsDeletedByRangeDeleter", countDocsDeletedByRangeDeleter.loadRelaxed());
    builder->append("countBytesDeletedByRangeDeleter",
                    countBytesDeletedByRangeDeleter.loadRelaxed());
    // Computed at read time so a deletion currently stalled on ticket admission shows its
    // in-progress wait growing in real time, not just once it is granted a ticket.
    builder->append("rangeDeleterTimeQueuedForTicketsMicros",
                    rangeDeleterTicketQueueTime.totalMicros());
    builder->append("rangeDeleterTimeProcessingWithTicketsMicros",
                    rangeDeleterTicketProcessingTime.totalMicros());
    builder->append("rangeDeleterTicketAdmissions", rangeDeleterTicketAdmissions.loadRelaxed());
    builder->append("rangeDeleterLowPriorityTicketAdmissions",
                    rangeDeleterLowPriorityTicketAdmissions.loadRelaxed());
    builder->append("rangeDeleterQueuedForTickets", rangeDeleterTicketQueueTime.currentCount());
    builder->append("countRangeDeletionTasksPreservingMaxKeyOrphans",
                    countRangeDeletionTasksPreservingMaxKeyOrphans.loadRelaxed());
    builder->append("countDonorMoveChunkLockTimeout", countDonorMoveChunkLockTimeout.loadRelaxed());
    builder->append("countDonorMoveChunkAbortConflictingIndexOperation",
                    countDonorMoveChunkAbortConflictingIndexOperation.loadRelaxed());
    builder->append("unfinishedMigrationFromPreviousPrimary",
                    unfinishedMigrationFromPreviousPrimary.loadRelaxed());
    builder->append("unauthorizedDirectShardOps", unauthorizedDirectShardOperations.loadRelaxed());
    builder->append("countTransitionToDedicatedConfigServerStarted",
                    countTransitionToDedicatedConfigServerStarted.loadRelaxed());
    builder->append("countTransitionToDedicatedConfigServerCompleted",
                    countTransitionToDedicatedConfigServerCompleted.loadRelaxed());
    builder->append("countTransitionFromDedicatedConfigServerCompleted",
                    countTransitionFromDedicatedConfigServerCompleted.loadRelaxed());
    builder->append("countFlushReshardingStateChangeTotalShardingMetadataRefreshes",
                    countFlushReshardingStateChangeTotalShardingMetadataRefreshes.loadRelaxed());
    builder->append(
        "countFlushReshardingStateChangeSuccessfulShardingMetadataRefreshes",
        countFlushReshardingStateChangeSuccessfulShardingMetadataRefreshes.loadRelaxed());
    builder->append("countFlushReshardingStateChangeFailedShardingMetadataRefreshes",
                    countFlushReshardingStateChangeFailedShardingMetadataRefreshes.loadRelaxed());
    builder->append("countHitsOfCompoundWildcardIndexesWithShardKeyPrefix",
                    countHitsOfCompoundWildcardIndexesWithShardKeyPrefix.loadRelaxed());
    builder->append("chunkMigrationWaitedOnReclaimedPreparedTxns",
                    chunkMigrationWaitedOnReclaimedPreparedTxns.loadRelaxed());
    builder->append("chunkMigrationWaitForReclaimedPreparedTxnsMillis",
                    chunkMigrationWaitForReclaimedPreparedTxnsMillis.loadRelaxed());
    builder->append("maxKeyOrphanScanComplete", maxKeyOrphanScanComplete.loadRelaxed());
    builder->append("maxKeyOrphanScanFoundUnownedMaxKey",
                    maxKeyOrphanScanFoundUnownedMaxKey.loadRelaxed());
    builder->append("maxKeyOrphanScanUnownedAlertEmitted",
                    maxKeyOrphanScanUnownedAlertEmitted.loadRelaxed());
    builder->append("maxKeyOrphanScanErrors", maxKeyOrphanScanErrors.loadRelaxed());
    builder->append("maxKeyOrphanScanFoundOwnedMaxKey",
                    maxKeyOrphanScanFoundOwnedMaxKey.loadRelaxed());
    builder->append("maxKeyOrphanScanOwnedAlertEmitted",
                    maxKeyOrphanScanOwnedAlertEmitted.loadRelaxed());
    builder->append("maxKeyZoneScanComplete", maxKeyZoneScanComplete.loadRelaxed());
    builder->append("maxKeyZoneScanFoundBuggyZone", maxKeyZoneScanFoundBuggyZone.loadRelaxed());
    builder->append("maxKeyZoneScanAlertEmitted", maxKeyZoneScanAlertEmitted.loadRelaxed());
    builder->append("maxKeyZoneScanErrors", maxKeyZoneScanErrors.loadRelaxed());

    {
        BSONObjBuilder databaseCriticalSectionBuilder{
            builder->subobjStart("databaseCriticalSectionStatistics")};
        databaseCriticalSectionStatistics.report(databaseCriticalSectionBuilder);
        databaseCriticalSectionBuilder.doneFast();
    }
    {
        BSONObjBuilder collectionCriticalSectionBuilder{
            builder->subobjStart("collectionCriticalSectionStatistics")};
        collectionCriticalSectionStatistics.report(collectionCriticalSectionBuilder);
        collectionCriticalSectionBuilder.doneFast();
    }
    {
        BSONObjBuilder subobj{builder->subobjStart("databaseShardingMetadataStatistics")};
        databaseShardingMetadataStatistics.report(subobj);
        subobj.doneFast();
    }
    {
        BSONObjBuilder subobj{builder->subobjStart("collectionShardingMetadataStatistics")};
        collectionShardingMetadataStatistics.report(subobj);
        subobj.doneFast();
    }
    {
        BSONObjBuilder subobj{builder->subobjStart("chunkOperationsStatistics")};
        chunkOperationsStatistics.report(subobj);
        subobj.doneFast();
    }
}
}  // namespace mongo
