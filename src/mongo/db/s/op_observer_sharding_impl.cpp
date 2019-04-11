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
#include "mongo/db/s/collection_sharding_runtime.h"
#include "mongo/db/s/migration_source_manager.h"

namespace mongo {
namespace {

const auto getIsMigrating = OperationContext::declareDecoration<bool>();

/**
 * Write operations do shard version checking, but do not perform orphan document filtering. Because
 * of this, if an update operation runs as part of a 'readConcern:snapshot' transaction, it might
 * get routed to a shard which no longer owns the chunk being written to. In such cases, throw a
 * MigrationConflict exception to indicate that the transaction needs to be rolled-back and
 * restarted.
 */
void assertIntersectingChunkHasNotMoved(OperationContext* opCtx,
                                        CollectionShardingRuntime* csr,
                                        const BSONObj& doc) {
    if (!repl::ReadConcernArgs::get(opCtx).getArgsAtClusterTime())
        return;

    const auto metadata = csr->getOrphansFilter(opCtx);
    if (!metadata->isSharded())
        return;

    // We can assume the simple collation because shard keys do not support non-simple collations.
    auto chunk = metadata->getChunkManager()->findIntersectingChunkWithSimpleCollation(
        metadata->extractDocumentKey(doc));

    // Throws if the chunk has moved since the timestamp of the running transaction's atClusterTime
    // read concern parameter.
    chunk.throwIfMoved();
}

bool isMigratingWithCSRLock(CollectionShardingRuntime* csr,
                            CollectionShardingRuntime::CSRLock& csrLock,
                            BSONObj const& docToDelete) {
    auto msm = MigrationSourceManager::get(csr, csrLock);
    return msm && msm->getCloner()->isDocumentInMigratingChunk(docToDelete);
}

}  // namespace

bool OpObserverShardingImpl::isMigrating(OperationContext* opCtx,
                                         NamespaceString const& nss,
                                         BSONObj const& docToDelete) {
    auto csr = CollectionShardingRuntime::get(opCtx, nss);
    auto csrLock = CollectionShardingRuntime::CSRLock::lock(opCtx, csr);
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
                                                  const bool fromMigrate,
                                                  const bool inMultiDocumentTransaction) {
    auto* const csr = (nss == NamespaceString::kSessionTransactionsTableNamespace || fromMigrate)
        ? nullptr
        : CollectionShardingRuntime::get(opCtx, nss);

    if (!csr) {
        return;
    }

    csr->checkShardVersionOrThrow(opCtx);

    if (inMultiDocumentTransaction) {
        assertIntersectingChunkHasNotMoved(opCtx, csr, insertedDoc);
        return;
    }

    auto csrLock = CollectionShardingRuntime::CSRLock::lock(opCtx, csr);
    auto msm = MigrationSourceManager::get(csr, csrLock);
    if (msm) {
        msm->getCloner()->onInsertOp(opCtx, insertedDoc, opTime);
    }
}

void OpObserverShardingImpl::shardObserveUpdateOp(OperationContext* opCtx,
                                                  const NamespaceString nss,
                                                  boost::optional<BSONObj> preImageDoc,
                                                  const BSONObj& postImageDoc,
                                                  const repl::OpTime& opTime,
                                                  const repl::OpTime& prePostImageOpTime,
                                                  const bool inMultiDocumentTransaction) {
    auto* const csr = CollectionShardingRuntime::get(opCtx, nss);
    csr->checkShardVersionOrThrow(opCtx);

    if (inMultiDocumentTransaction) {
        assertIntersectingChunkHasNotMoved(opCtx, csr, postImageDoc);
        return;
    }

    auto csrLock = CollectionShardingRuntime::CSRLock::lock(opCtx, csr);
    auto msm = MigrationSourceManager::get(csr, csrLock);
    if (msm) {
        msm->getCloner()->onUpdateOp(opCtx, preImageDoc, postImageDoc, opTime, prePostImageOpTime);
    }
}

void OpObserverShardingImpl::shardObserveDeleteOp(OperationContext* opCtx,
                                                  const NamespaceString nss,
                                                  const BSONObj& documentKey,
                                                  const repl::OpTime& opTime,
                                                  const repl::OpTime& preImageOpTime,
                                                  const bool inMultiDocumentTransaction) {
    auto* const csr = CollectionShardingRuntime::get(opCtx, nss);
    csr->checkShardVersionOrThrow(opCtx);

    if (inMultiDocumentTransaction) {
        assertIntersectingChunkHasNotMoved(opCtx, csr, documentKey);
        return;
    }

    auto csrLock = CollectionShardingRuntime::CSRLock::lock(opCtx, csr);
    auto msm = MigrationSourceManager::get(csr, csrLock);

    if (msm && getIsMigrating(opCtx)) {
        msm->getCloner()->onDeleteOp(opCtx, documentKey, opTime, preImageOpTime);
    }
}

void OpObserverShardingImpl::shardObserveTransactionPrepareOrUnpreparedCommit(
    OperationContext* opCtx,
    const std::vector<repl::ReplOperation>& stmts,
    const repl::OpTime& prepareOrCommitOptime) {

    std::set<NamespaceString> namespacesTouchedByTransaction;

    for (const auto& stmt : stmts) {
        const auto& nss = stmt.getNss();

        invariant(opCtx->lockState()->isCollectionLockedForMode(nss, MODE_IS));

        auto csr = CollectionShardingRuntime::get(opCtx, nss);
        auto csrLock = CollectionShardingRuntime::CSRLock::lock(opCtx, csr);
        auto msm = MigrationSourceManager::get(csr, csrLock);
        if (!msm) {
            continue;
        }

        if (namespacesTouchedByTransaction.find(nss) == namespacesTouchedByTransaction.end()) {
            msm->getCloner()->onTransactionPrepareOrUnpreparedCommit(opCtx, prepareOrCommitOptime);
            namespacesTouchedByTransaction.insert(nss);
        }


        const auto& opType = stmt.getOpType();

        // We pass an empty opTime to observers because retryable write history doesn't care about
        // writes in transactions.
        if (opType == repl::OpTypeEnum::kInsert) {
            msm->getCloner()->onInsertOp(opCtx, stmt.getObject(), {});
        } else if (opType == repl::OpTypeEnum::kUpdate) {
            if (auto updateDoc = stmt.getObject2()) {
                msm->getCloner()->onUpdateOp(
                    opCtx, stmt.getPreImageDocumentKey(), *updateDoc, {}, {});
            }
        } else if (opType == repl::OpTypeEnum::kDelete) {
            if (isMigratingWithCSRLock(csr, csrLock, stmt.getObject())) {
                msm->getCloner()->onDeleteOp(
                    opCtx, getDocumentKey(opCtx, nss, stmt.getObject()), {}, {});
            }
        }
    }
}

}  // namespace mongo
