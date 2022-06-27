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


#include "mongo/platform/basic.h"

#include "mongo/db/s/op_observer_sharding_impl.h"

#include "mongo/db/catalog_raii.h"
#include "mongo/db/repl/oplog_entry.h"
#include "mongo/db/s/collection_sharding_runtime.h"
#include "mongo/db/s/database_sharding_state.h"
#include "mongo/db/s/migration_chunk_cloner_source_legacy.h"
#include "mongo/db/s/migration_source_manager.h"
#include "mongo/db/s/resharding/resharding_util.h"
#include "mongo/db/s/sharding_write_router.h"
#include "mongo/logv2/log.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding


namespace mongo {
namespace {

const auto getIsMigrating = OperationContext::declareDecoration<bool>();

/**
 * Write operations do shard version checking, but if an update operation runs as part of a
 * 'readConcern:snapshot' transaction, the router could have used the metadata at the snapshot
 * time and yet set the latest shard version on the request. This is why the write can get routed
 * to a shard which no longer owns the chunk being written to. In such cases, throw a
 * MigrationConflict exception to indicate that the transaction needs to be rolled-back and
 * restarted.
 */
void assertIntersectingChunkHasNotMoved(OperationContext* opCtx,
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

bool isMigratingWithCSRLock(CollectionShardingRuntime* csr,
                            CollectionShardingRuntime::CSRLock& csrLock,
                            BSONObj const& docToDelete) {
    auto cloner = MigrationSourceManager::getCurrentCloner(csr, csrLock);
    return cloner && cloner->isDocumentInMigratingChunk(docToDelete);
}

void assertMovePrimaryInProgress(OperationContext* opCtx, NamespaceString const& nss) {
    if (!nss.isNormalCollection() && nss.coll() != "system.views" &&
        !nss.isTimeseriesBucketsCollection()) {
        return;
    }

    // TODO SERVER-58222: evaluate whether this is safe or whether acquiring the lock can block.
    AllowLockAcquisitionOnTimestampedUnitOfWork allowLockAcquisition(opCtx->lockState());
    Lock::DBLock dblock(opCtx, nss.dbName(), MODE_IS);
    auto dss = DatabaseShardingState::get(opCtx, nss.db().toString());
    if (!dss) {
        return;
    }

    auto dssLock = DatabaseShardingState::DSSLock::lockShared(opCtx, dss);
    auto mpsm = dss->getMovePrimarySourceManager(dssLock);

    if (mpsm) {
        LOGV2(4908600, "assertMovePrimaryInProgress", "namespace"_attr = nss.toString());

        uasserted(ErrorCodes::MovePrimaryInProgress,
                  "movePrimary is in progress for namespace " + nss.toString());
    }
}

}  // namespace

bool OpObserverShardingImpl::isMigrating(OperationContext* opCtx,
                                         NamespaceString const& nss,
                                         BSONObj const& docToDelete) {
    auto csr = CollectionShardingRuntime::get(opCtx, nss);
    auto csrLock = CollectionShardingRuntime::CSRLock::lockShared(opCtx, csr);
    return isMigratingWithCSRLock(csr, csrLock, docToDelete);
}

void OpObserverShardingImpl::shardObserveAboutToDelete(OperationContext* opCtx,
                                                       NamespaceString const& nss,
                                                       BSONObj const& docToDelete) {
    getIsMigrating(opCtx) = isMigrating(opCtx, nss, docToDelete);
}

void OpObserverShardingImpl::shardObserveInsertOp(OperationContext* opCtx,
                                                  const NamespaceString nss,
                                                  const BSONObj& insertedDoc,
                                                  const repl::OpTime& opTime,
                                                  const ShardingWriteRouter& shardingWriteRouter,
                                                  const bool fromMigrate,
                                                  const bool inMultiDocumentTransaction) {
    if (nss == NamespaceString::kSessionTransactionsTableNamespace || fromMigrate)
        return;

    auto css = shardingWriteRouter.getCollectionShardingState();
    auto* const csr = CollectionShardingRuntime::get(css);
    csr->checkShardVersionOrThrow(opCtx);

    auto metadata = csr->getCurrentMetadataIfKnown();
    if (!metadata || !metadata->isSharded()) {
        assertMovePrimaryInProgress(opCtx, nss);
        return;
    }

    if (inMultiDocumentTransaction) {
        const auto atClusterTime = repl::ReadConcernArgs::get(opCtx).getArgsAtClusterTime();

        if (atClusterTime) {
            const auto shardKey =
                metadata->getShardKeyPattern().extractShardKeyFromDocThrows(insertedDoc);
            assertIntersectingChunkHasNotMoved(opCtx, *metadata, shardKey, *atClusterTime);
        }

        return;
    }

    auto csrLock = CollectionShardingRuntime::CSRLock::lockShared(opCtx, csr);
    auto cloner = MigrationSourceManager::getCurrentCloner(csr, csrLock);
    if (cloner) {
        cloner->onInsertOp(opCtx, insertedDoc, opTime);
    }
}

void OpObserverShardingImpl::shardObserveUpdateOp(OperationContext* opCtx,
                                                  const NamespaceString nss,
                                                  boost::optional<BSONObj> preImageDoc,
                                                  const BSONObj& postImageDoc,
                                                  const repl::OpTime& opTime,
                                                  const ShardingWriteRouter& shardingWriteRouter,
                                                  const repl::OpTime& prePostImageOpTime,
                                                  const bool inMultiDocumentTransaction) {
    auto css = shardingWriteRouter.getCollectionShardingState();
    auto* const csr = CollectionShardingRuntime::get(css);
    csr->checkShardVersionOrThrow(opCtx);

    auto metadata = csr->getCurrentMetadataIfKnown();
    if (!metadata || !metadata->isSharded()) {
        assertMovePrimaryInProgress(opCtx, nss);
        return;
    }

    if (inMultiDocumentTransaction) {
        const auto atClusterTime = repl::ReadConcernArgs::get(opCtx).getArgsAtClusterTime();

        if (atClusterTime) {
            const auto shardKey =
                metadata->getShardKeyPattern().extractShardKeyFromDocThrows(postImageDoc);
            assertIntersectingChunkHasNotMoved(opCtx, *metadata, shardKey, *atClusterTime);
        }

        return;
    }

    auto csrLock = CollectionShardingRuntime::CSRLock::lockShared(opCtx, csr);
    auto cloner = MigrationSourceManager::getCurrentCloner(csr, csrLock);
    if (cloner) {
        cloner->onUpdateOp(opCtx, preImageDoc, postImageDoc, opTime, prePostImageOpTime);
    }
}

void OpObserverShardingImpl::shardObserveDeleteOp(OperationContext* opCtx,
                                                  const NamespaceString nss,
                                                  const BSONObj& documentKey,
                                                  const repl::OpTime& opTime,
                                                  const ShardingWriteRouter& shardingWriteRouter,
                                                  const repl::OpTime& preImageOpTime,
                                                  const bool inMultiDocumentTransaction) {
    auto css = shardingWriteRouter.getCollectionShardingState();
    auto* const csr = CollectionShardingRuntime::get(css);
    csr->checkShardVersionOrThrow(opCtx);

    auto metadata = csr->getCurrentMetadataIfKnown();
    if (!metadata || !metadata->isSharded()) {
        assertMovePrimaryInProgress(opCtx, nss);
        return;
    }

    if (inMultiDocumentTransaction) {
        const auto atClusterTime = repl::ReadConcernArgs::get(opCtx).getArgsAtClusterTime();

        if (atClusterTime) {
            const auto shardKey =
                metadata->getShardKeyPattern().extractShardKeyFromDocumentKeyThrows(documentKey);
            assertIntersectingChunkHasNotMoved(opCtx, *metadata, shardKey, *atClusterTime);
        }

        return;
    }

    auto csrLock = CollectionShardingRuntime::CSRLock::lockShared(opCtx, csr);
    auto cloner = MigrationSourceManager::getCurrentCloner(csr, csrLock);

    if (cloner && getIsMigrating(opCtx)) {
        cloner->onDeleteOp(opCtx, documentKey, opTime, preImageOpTime);
    }
}

void OpObserverShardingImpl::shardObserveTransactionPrepareOrUnpreparedCommit(
    OperationContext* opCtx,
    const std::vector<repl::ReplOperation>& stmts,
    const repl::OpTime& prepareOrCommitOptime) {

    opCtx->recoveryUnit()->registerChange(
        std::make_unique<LogTransactionOperationsForShardingHandler>(
            *opCtx->getLogicalSessionId(), stmts, prepareOrCommitOptime));
}

}  // namespace mongo
