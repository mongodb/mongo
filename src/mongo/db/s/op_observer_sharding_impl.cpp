
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

#include "mongo/db/s/collection_sharding_runtime.h"
#include "mongo/db/s/migration_source_manager.h"

namespace mongo {
namespace {
const auto getIsMigrating = OperationContext::declareDecoration<bool>();

void assertIntersectingChunkHasNotMoved(OperationContext* opCtx,
                                        CollectionShardingRuntime* csr,
                                        const BSONObj& doc) {
    auto metadata = csr->getMetadataForOperation(opCtx);
    if (!metadata->isSharded()) {
        return;
    }

    // We can assume the simple collation because shard keys do not support non-simple collations.
    auto chunk = metadata->getChunkManager()->findIntersectingChunkWithSimpleCollation(
        metadata->extractDocumentKey(doc));

    // Throws if the chunk has moved since the timestamp of the running transaction's atClusterTime
    // read concern parameter.
    chunk.throwIfMoved();
}
}

bool OpObserverShardingImpl::isMigrating(OperationContext* opCtx,
                                         NamespaceString const& nss,
                                         BSONObj const& docToDelete) {
    auto css = CollectionShardingRuntime::get(opCtx, nss);
    auto msm = MigrationSourceManager::get(css);
    return msm && msm->getCloner()->isDocumentInMigratingChunk(docToDelete);
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
    auto* const css = (nss == NamespaceString::kSessionTransactionsTableNamespace || fromMigrate)
        ? nullptr
        : CollectionShardingRuntime::get(opCtx, nss);
    if (css) {
        css->checkShardVersionOrThrow(opCtx);

        auto msm = MigrationSourceManager::get(css);
        if (msm) {
            msm->getCloner()->onInsertOp(opCtx, insertedDoc, opTime);
        }

        if (inMultiDocumentTransaction &&
            repl::ReadConcernArgs::get(opCtx).getArgsAtClusterTime()) {
            assertIntersectingChunkHasNotMoved(opCtx, css, insertedDoc);
        }
    }
}

void OpObserverShardingImpl::shardObserveUpdateOp(OperationContext* opCtx,
                                                  const NamespaceString nss,
                                                  const BSONObj& updatedDoc,
                                                  const repl::OpTime& opTime,
                                                  const repl::OpTime& prePostImageOpTime,
                                                  const bool inMultiDocumentTransaction) {
    auto* const css = CollectionShardingRuntime::get(opCtx, nss);
    css->checkShardVersionOrThrow(opCtx);

    auto msm = MigrationSourceManager::get(css);
    if (msm) {
        msm->getCloner()->onUpdateOp(opCtx, updatedDoc, opTime, prePostImageOpTime);
    }

    if (inMultiDocumentTransaction && repl::ReadConcernArgs::get(opCtx).getArgsAtClusterTime()) {
        assertIntersectingChunkHasNotMoved(opCtx, css, updatedDoc);
    }
}

void OpObserverShardingImpl::shardObserveDeleteOp(OperationContext* opCtx,
                                                  const NamespaceString nss,
                                                  const BSONObj& documentKey,
                                                  const repl::OpTime& opTime,
                                                  const repl::OpTime& preImageOpTime,
                                                  const bool inMultiDocumentTransaction) {
    auto& isMigrating = getIsMigrating(opCtx);
    auto* const css = CollectionShardingRuntime::get(opCtx, nss);
    css->checkShardVersionOrThrow(opCtx);

    auto msm = MigrationSourceManager::get(css);
    if (msm && isMigrating) {
        msm->getCloner()->onDeleteOp(opCtx, documentKey, opTime, preImageOpTime);
    }

    if (inMultiDocumentTransaction && repl::ReadConcernArgs::get(opCtx).getArgsAtClusterTime()) {
        assertIntersectingChunkHasNotMoved(opCtx, css, documentKey);
    }
}

}  // namespace mongo
