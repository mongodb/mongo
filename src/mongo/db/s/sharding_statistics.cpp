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

#include "mongo/db/s/sharding_statistics.h"

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/feature_flag.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/s/sharding_feature_flags_gen.h"

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
    builder->append("countStaleConfigErrors", countStaleConfigErrors.load());

    builder->append("countDonorMoveChunkStarted", countDonorMoveChunkStarted.load());
    builder->append("totalDonorChunkCloneTimeMillis", totalDonorChunkCloneTimeMillis.load());
    builder->append("totalCriticalSectionCommitTimeMillis",
                    totalCriticalSectionCommitTimeMillis.load());
    builder->append("totalCriticalSectionTimeMillis", totalCriticalSectionTimeMillis.load());
    builder->append("totalRecipientCriticalSectionTimeMillis",
                    totalRecipientCriticalSectionTimeMillis.load());
    builder->append("countDocsClonedOnRecipient", countDocsClonedOnRecipient.load());
    builder->append("countDocsClonedOnDonor", countDocsClonedOnDonor.load());
    builder->append("countRecipientMoveChunkStarted", countRecipientMoveChunkStarted.load());
    builder->append("countDocsDeletedOnDonor", countDocsDeletedOnDonor.load());
    builder->append("countDonorMoveChunkLockTimeout", countDonorMoveChunkLockTimeout.load());
    builder->append("countDonorMoveChunkAbortConflictingIndexOperation",
                    countDonorMoveChunkAbortConflictingIndexOperation.load());
    builder->append("unfinishedMigrationFromPreviousPrimary",
                    unfinishedMigrationFromPreviousPrimary.load());
    // (Ignore FCV check): This feature flag doesn't have any upgrade/downgrade concerns.
    if (mongo::feature_flags::gConcurrencyInChunkMigration.isEnabledAndIgnoreFCVUnsafe())
        builder->append("chunkMigrationConcurrency", chunkMigrationConcurrencyCnt.load());
}

}  // namespace mongo
