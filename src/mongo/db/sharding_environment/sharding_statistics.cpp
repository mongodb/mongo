/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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
}

}  // namespace mongo
