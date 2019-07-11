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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kQuery

#include "mongo/platform/basic.h"

#include "mongo/db/pipeline/process_interface_shardsvr.h"

#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/collection_catalog.h"
#include "mongo/db/catalog/database_holder.h"
#include "mongo/db/catalog/document_validation.h"
#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/curop.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/exec/shard_filterer_impl.h"
#include "mongo/db/ops/write_ops_exec.h"
#include "mongo/db/ops/write_ops_gen.h"
#include "mongo/db/pipeline/document_source_internal_shard_filter.h"
#include "mongo/db/pipeline/sharded_agg_helpers.h"
#include "mongo/db/query/query_knobs_gen.h"
#include "mongo/db/s/collection_sharding_state.h"
#include "mongo/db/s/sharding_state.h"
#include "mongo/s/catalog_cache.h"
#include "mongo/s/cluster_commands_helpers.h"
#include "mongo/s/grid.h"
#include "mongo/s/query/document_source_merge_cursors.h"
#include "mongo/s/write_ops/cluster_write.h"
#include "mongo/util/log.h"

namespace mongo {

using boost::intrusive_ptr;
using std::shared_ptr;
using std::string;
using std::unique_ptr;
using write_ops::Insert;
using write_ops::Update;
using write_ops::UpdateOpEntry;

namespace {

// Attaches the write concern to the given batch request. If it looks like 'writeConcern' has
// been default initialized to {w: 0, wtimeout: 0} then we do not bother attaching it.
void attachWriteConcern(BatchedCommandRequest* request, const WriteConcernOptions& writeConcern) {
    if (!writeConcern.wMode.empty() || writeConcern.wNumNodes > 0) {
        request->setWriteConcern(writeConcern.toBSON());
    }
}

}  // namespace

void MongoInterfaceShardServer::checkRoutingInfoEpochOrThrow(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    const NamespaceString& nss,
    ChunkVersion targetCollectionVersion) const {
    auto catalogCache = Grid::get(expCtx->opCtx)->catalogCache();
    return catalogCache->checkEpochOrThrow(nss, targetCollectionVersion);
}

std::pair<std::vector<FieldPath>, bool>
MongoInterfaceShardServer::collectDocumentKeyFieldsForHostedCollection(OperationContext* opCtx,
                                                                       const NamespaceString& nss,
                                                                       UUID uuid) const {
    invariant(serverGlobalParams.clusterRole == ClusterRole::ShardServer);

    const auto metadata = [opCtx, &nss]() -> ScopedCollectionMetadata {
        Lock::DBLock dbLock(opCtx, nss.db(), MODE_IS);
        Lock::CollectionLock collLock(opCtx, nss, MODE_IS);
        return CollectionShardingState::get(opCtx, nss)->getCurrentMetadata();
    }();

    if (!metadata->isSharded() || !metadata->uuidMatches(uuid)) {
        // An unsharded collection can still become sharded so is not final. If the uuid doesn't
        // match the one stored in the ScopedCollectionMetadata, this implies that the collection
        // has been dropped and recreated as sharded. We don't know what the old document key fields
        // might have been in this case so we return just _id.
        return {{"_id"}, false};
    }

    // Unpack the shard key. Collection is now sharded so the document key fields will never change,
    // mark as final.
    return {_shardKeyToDocumentKeyFields(metadata->getKeyPatternFields()), true};
}

Status MongoInterfaceShardServer::insert(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                         const NamespaceString& ns,
                                         std::vector<BSONObj>&& objs,
                                         const WriteConcernOptions& wc,
                                         boost::optional<OID> targetEpoch) {
    BatchedCommandResponse response;
    BatchWriteExecStats stats;

    BatchedCommandRequest insertCommand(
        buildInsertOp(ns, std::move(objs), expCtx->bypassDocumentValidation));

    // If applicable, attach a write concern to the batched command request.
    attachWriteConcern(&insertCommand, wc);

    ClusterWriter::write(expCtx->opCtx, insertCommand, &stats, &response, targetEpoch);

    return response.toStatus();
}

StatusWith<MongoProcessInterface::UpdateResult> MongoInterfaceShardServer::update(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    const NamespaceString& ns,
    BatchedObjects&& batch,
    const WriteConcernOptions& wc,
    bool upsert,
    bool multi,
    boost::optional<OID> targetEpoch) {
    BatchedCommandResponse response;
    BatchWriteExecStats stats;

    BatchedCommandRequest updateCommand(buildUpdateOp(expCtx, ns, std::move(batch), upsert, multi));

    // If applicable, attach a write concern to the batched command request.
    attachWriteConcern(&updateCommand, wc);

    ClusterWriter::write(expCtx->opCtx, updateCommand, &stats, &response, targetEpoch);

    if (auto status = response.toStatus(); status != Status::OK()) {
        return status;
    }
    return {{response.getN(), response.getNModified()}};
}

unique_ptr<Pipeline, PipelineDeleter> MongoInterfaceShardServer::attachCursorSourceToPipeline(
    const boost::intrusive_ptr<ExpressionContext>& expCtx, Pipeline* ownedPipeline) {
    std::unique_ptr<Pipeline, PipelineDeleter> pipeline(ownedPipeline,
                                                        PipelineDeleter(expCtx->opCtx));

    invariant(pipeline->getSources().empty() ||
              !dynamic_cast<DocumentSourceMergeCursors*>(pipeline->getSources().front().get()));

    // $lookup on a sharded collection is not allowed in a transaction. We assume that if we're in
    // a transaction, the foreign collection is unsharded. Otherwise, we may access the catalog
    // cache, and attempt to do a network request while holding locks.
    // TODO: SERVER-39162 allow $lookup in sharded transactions.
    const bool inTxn = expCtx->opCtx->inMultiDocumentTransaction();

    const bool isSharded = [&]() {
        if (inTxn || !ShardingState::get(expCtx->opCtx)->enabled()) {
            // Sharding isn't enabled or we're in a transaction. In either case we assume it's
            // unsharded.
            return false;
        } else if (expCtx->ns.db() == "local") {
            // This may be a change stream examining the oplog. We know the oplog (or any local
            // collections for that matter) will never be sharded.
            return false;
        }
        return uassertStatusOK(getCollectionRoutingInfoForTxnCmd(expCtx->opCtx, expCtx->ns)).cm() !=
            nullptr;
    }();

    if (isSharded) {
        const bool foreignShardedAllowed =
            getTestCommandsEnabled() && internalQueryAllowShardedLookup.load();
        if (foreignShardedAllowed) {
            // For a sharded collection we may have to establish cursors on a remote host.
            return sharded_agg_helpers::targetShardsAndAddMergeCursors(expCtx, pipeline.release());
        }

        uasserted(51069, "Cannot run $lookup with sharded foreign collection");
    }

    // Perform a "local read", the same as if we weren't a shard server.

    // TODO SERVER-39015 we should do a shard version check here after we acquire a lock within
    // this function, to be sure the collection didn't become sharded between the time we checked
    // whether it's sharded and the time we took the lock.

    return attachCursorSourceToPipelineForLocalRead(expCtx, pipeline.release());
}

std::unique_ptr<ShardFilterer> MongoInterfaceShardServer::getShardFilterer(
    const boost::intrusive_ptr<ExpressionContext>& expCtx) const {
    const bool aggNsIsCollection = expCtx->uuid != boost::none;
    auto shardingMetadata = CollectionShardingState::get(expCtx->opCtx, expCtx->ns)
                                ->getOrphansFilter(expCtx->opCtx, aggNsIsCollection);
    return std::make_unique<ShardFiltererImpl>(std::move(shardingMetadata));
}

}  // namespace mongo
