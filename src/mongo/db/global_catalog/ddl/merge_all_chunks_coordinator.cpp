/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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

#include "mongo/db/global_catalog/ddl/merge_all_chunks_coordinator.h"

#include "mongo/bson/bsontypes.h"
#include "mongo/bson/simple_bsonobj_comparator.h"
#include "mongo/client/read_preference.h"
#include "mongo/db/generic_argument_util.h"
#include "mongo/db/global_catalog/ddl/merge_chunk_request_gen.h"
#include "mongo/db/global_catalog/type_chunk_range.h"
#include "mongo/db/shard_role/shard_catalog/collection_sharding_runtime.h"
#include "mongo/db/shard_role/shard_catalog/shard_filtering_metadata_refresh.h"
#include "mongo/db/sharding_environment/grid.h"
#include "mongo/db/topology/shard_registry.h"
#include "mongo/db/write_concern_options.h"
#include "mongo/logv2/log.h"
#include "mongo/util/future_util.h"
#include "mongo/util/str.h"

#include <boost/none.hpp>
#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

namespace mongo {

namespace {
static inline IDLParserContext kIdlParserCtx{"MergeAllChunksOnShardResponse"};
}  // namespace

MergeAllChunksCoordinator::MergeAllChunksCoordinator(ShardingCoordinatorService* service,
                                                     const BSONObj& initialStateDoc)
    : ChunkOperationShardingCoordinator(service, "MergeAllChunksCoordinator", initialStateDoc),
      _request(_doc.getShardsvrMergeAllChunksOnShardRequest()) {}

void MergeAllChunksCoordinator::checkIfOptionsConflict(const BSONObj& doc) const {
    const auto otherDoc = MergeAllChunksCoordinatorDocument::parse(
        doc, IDLParserContext("MergeAllChunksCoordinatorDocument"));

    const auto& selfReq = _request.toBSON();
    const auto& otherReq = otherDoc.getShardsvrMergeAllChunksOnShardRequest().toBSON();

    uassert(ErrorCodes::ConflictingOperationInProgress,
            str::stream() << "Another merge all chunks operation for namespace "
                          << nss().toStringForErrorMsg()
                          << " is being executed with different parameters: " << redact(selfReq)
                          << " vs " << redact(otherReq),
            SimpleBSONObjComparator::kInstance.evaluate(selfReq == otherReq));
}

void MergeAllChunksCoordinator::appendCommandInfo(BSONObjBuilder* cmdInfoBuilder) const {
    cmdInfoBuilder->appendElements(_request.toBSON());
}

bool MergeAllChunksCoordinator::isInCriticalSection(Phase phase) const {
    return false;
}

ExecutorFuture<void> MergeAllChunksCoordinator::_acquireLocksAsync(
    OperationContext* opCtx,
    std::shared_ptr<executor::ScopedTaskExecutor> executor,
    const CancellationToken& token) {
    return AsyncTry([this, anchor = shared_from_this()] {
               auto opCtxHolder = makeOperationContext(/*deprioritizable=*/true);
               auto* newOpCtx = opCtxHolder.get();

               // Span the whole shard key space so the registration acts as a
               // namespace-wide guard (see class comment for details).
               const auto chunkRange = ChunkRange(kMinBSONKey, kMaxBSONKey);
               _scopedSplitMergeChunk.emplace(uassertStatusOK(
                   ActiveMigrationsRegistry::get(newOpCtx).registerSplitOrMergeChunk(
                       newOpCtx, nss(), chunkRange)));
           })
        .until([this, anchor = shared_from_this()](Status status) {
            if (!status.isOK()) {
                LOGV2_WARNING(12117914,
                              "ActiveMigrationsRegistry lock acquisition attempt failed",
                              logv2::DynamicAttributes{getCoordinatorLogAttrs(),
                                                       "error"_attr = redact(status)});
            }
            return !_recoveredFromDisk || status.isOK();
        })
        .withBackoffBetweenIterations(kExponentialBackoff)
        .on(**executor, token);
}

void MergeAllChunksCoordinator::_releaseLocks(OperationContext* opCtx) {
    _scopedSplitMergeChunk.reset();
}

ExecutorFuture<void> MergeAllChunksCoordinator::_runImpl(
    std::shared_ptr<executor::ScopedTaskExecutor> executor,
    const CancellationToken& token) noexcept {
    return ExecutorFuture<void>(**executor).then([this, anchor = shared_from_this()] {
        auto opCtxHolder = makeOperationContext(/*deprioritizable=*/true);
        auto* opCtx = opCtxHolder.get();
        LOGV2(12118000,
              "Running merge all chunks on shard operation",
              logAttrs(nss()),
              "shard"_attr = _request.getShard());

        // Because this is a non-authoritative update, we must mark the CSR metadata as
        // kNonAuthoritative so that the following refresh will fetch the metadata from the config
        // server. Leaving it kAuthoritative would short-circuit the refresh against the durable
        // shard catalog and keep the CSR pinned to the pre-mergeAllChunks version. This must be
        // done before starting the operation to ensure the CSR is left as kNonAuthoritative in case
        // of an unexpected failure.
        // TODO (SERVER-125786) The clearFilteringMetadata_nonAuthoritative should go away
        // once mergeAllChunks becomes authoritative.
        {
            auto scopedCsr = CollectionShardingRuntime::acquireExclusive(opCtx, nss());
            scopedCsr->clearFilteringMetadata_nonAuthoritative(opCtx);
        }

        ConfigSvrCommitMergeAllChunksOnShard configRequest(nss());
        configRequest.setDbName(DatabaseName::kAdmin);
        configRequest.setShard(_request.getShard());
        configRequest.setMaxNumberOfChunksToMerge(_request.getMaxNumberOfChunksToMerge());
        configRequest.setMaxTimeProcessingChunksMS(_request.getMaxTimeProcessingChunksMS());
        configRequest.setWriteConcern(defaultMajorityWriteConcernDoNotUse());

        auto cmdResponse =
            uassertStatusOK(Grid::get(opCtx)->shardRegistry()->getConfigShard()->runCommand(
                opCtx,
                ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                DatabaseName::kAdmin,
                configRequest.toBSON(),
                Shard::RetryPolicy::kIdempotent));

        uassertStatusOK(Shard::CommandResponse::getEffectiveStatus(cmdResponse));

        _response = MergeAllChunksOnShardResponse::parse(cmdResponse.response, kIdlParserCtx);

        // Update the shard catalog filtering metadata to reflect the new shard
        // version produced by the config server merge.
        uassertStatusOK(FilteringMetadataCache::get(opCtx)->onCollectionPlacementVersionMismatch(
            opCtx, nss(), _response->getShardVersion()));

        LOGV2(12118001,
              "Completed merge all chunks on shard operation",
              logAttrs(nss()),
              "shard"_attr = _request.getShard(),
              "numMergedChunks"_attr = _response->getNumMergedChunks());
    });
}

}  // namespace mongo
