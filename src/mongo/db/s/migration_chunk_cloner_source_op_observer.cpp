/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#include "mongo/db/s/migration_chunk_cloner_source_op_observer.h"

#include "mongo/db/s/database_sharding_state.h"
#include "mongo/db/s/migration_chunk_cloner_source.h"
#include "mongo/logv2/log.h"
#include "mongo/s/chunk_manager.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

namespace mongo {

// static
void MigrationChunkClonerSourceOpObserver::assertIntersectingChunkHasNotMoved(
    OperationContext* opCtx,
    const CollectionMetadata& metadata,
    const BSONObj& shardKey,
    const LogicalTime& atClusterTime) {
    // We can assume the simple collation because shard keys do not support non-simple collations.
    auto cmAtTimeOfWrite =
        ChunkManager::makeAtTime(*metadata.getChunkManager(), atClusterTime.asTimestamp());
    auto chunk = cmAtTimeOfWrite.findIntersectingChunkWithSimpleCollation(shardKey);

    // Throws if the chunk has moved since the timestamp of the running transaction's atClusterTime
    // read concern parameter.
    chunk.throwIfMoved();
}

// static
void MigrationChunkClonerSourceOpObserver::assertNoMovePrimaryInProgress(
    OperationContext* opCtx, const NamespaceString& nss) {
    if (!nss.isNormalCollection() && nss.coll() != "system.views" &&
        !nss.isTimeseriesBucketsCollection()) {
        return;
    }

    // TODO SERVER-58222: evaluate whether this is safe or whether acquiring the lock can block.
    AllowLockAcquisitionOnTimestampedUnitOfWork allowLockAcquisition(opCtx->lockState());
    Lock::DBLock dblock(opCtx, nss.dbName(), MODE_IS);

    const auto scopedDss =
        DatabaseShardingState::assertDbLockedAndAcquireShared(opCtx, nss.dbName());
    if (scopedDss->isMovePrimaryInProgress()) {
        LOGV2(4908600, "assertNoMovePrimaryInProgress", logAttrs(nss));

        uasserted(ErrorCodes::MovePrimaryInProgress,
                  "movePrimary is in progress for namespace " + nss.toStringForErrorMsg());
    }
}

void MigrationChunkClonerSourceOpObserver::onTransactionPrepareNonPrimary(
    OperationContext* opCtx,
    const LogicalSessionId& lsid,
    const std::vector<repl::OplogEntry>& statements,
    const repl::OpTime& prepareOpTime) {
    opCtx->recoveryUnit()->registerChange(
        std::make_unique<LogTransactionOperationsForShardingHandler>(
            lsid, statements, prepareOpTime));
}

}  // namespace mongo
