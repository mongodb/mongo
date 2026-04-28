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

#include "mongo/db/global_catalog/ddl/merge_chunks_coordinator.h"

#include "mongo/bson/simple_bsonobj_comparator.h"
#include "mongo/client/read_preference.h"
#include "mongo/db/generic_argument_util.h"
#include "mongo/db/global_catalog/ddl/merge_chunk_request_gen.h"
#include "mongo/db/global_catalog/ddl/sharding_catalog_manager.h"
#include "mongo/db/s/chunk_operation_precondition_checks.h"
#include "mongo/db/shard_role/shard_catalog/shard_filtering_metadata_refresh.h"
#include "mongo/db/sharding_environment/grid.h"
#include "mongo/db/topology/shard_registry.h"
#include "mongo/db/topology/sharding_state.h"
#include "mongo/db/versioning_protocol/chunk_version.h"
#include "mongo/db/write_concern_options.h"
#include "mongo/logv2/log.h"
#include "mongo/util/future_util.h"
#include "mongo/util/str.h"

#include <boost/none.hpp>
#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

namespace mongo {

MergeChunksCoordinator::MergeChunksCoordinator(ShardingCoordinatorService* service,
                                               const BSONObj& initialStateDoc)
    : ChunkOperationShardingCoordinator(service, "MergeChunksCoordinator", initialStateDoc),
      _request(_doc.getShardsvrMergeChunksRequest()) {}

void MergeChunksCoordinator::checkIfOptionsConflict(const BSONObj& doc) const {
    const auto otherDoc = MergeChunksCoordinatorDocument::parse(
        doc, IDLParserContext("MergeChunksCoordinatorDocument"));

    const auto& selfReq = _request.toBSON();
    const auto& otherReq = otherDoc.getShardsvrMergeChunksRequest().toBSON();

    uassert(ErrorCodes::ConflictingOperationInProgress,
            str::stream() << "Another merge chunks operation for namespace "
                          << nss().toStringForErrorMsg()
                          << " is being executed with different parameters: " << redact(selfReq)
                          << " vs " << redact(otherReq),
            SimpleBSONObjComparator::kInstance.evaluate(selfReq == otherReq));
}

void MergeChunksCoordinator::appendCommandInfo(BSONObjBuilder* cmdInfoBuilder) const {
    cmdInfoBuilder->appendElements(_request.toBSON());
}

bool MergeChunksCoordinator::isInCriticalSection(Phase phase) const {
    return false;
}

ExecutorFuture<void> MergeChunksCoordinator::_acquireLocksAsync(
    OperationContext* opCtx,
    std::shared_ptr<executor::ScopedTaskExecutor> executor,
    const CancellationToken& token) {
    return AsyncTry([this, anchor = shared_from_this()] {
               auto opCtxHolder = makeOperationContext(/*deprioritizable=*/true);
               auto* opCtx = opCtxHolder.get();

               auto bounds = _request.getBounds();
               uassertStatusOK(ChunkRange::validate(bounds));
               auto chunkRange = ChunkRange(bounds[0], bounds[1]);
               _scopedSplitMergeChunk.emplace(
                   uassertStatusOK(ActiveMigrationsRegistry::get(opCtx).registerSplitOrMergeChunk(
                       opCtx, nss(), chunkRange)));
           })
        .until([this, anchor = shared_from_this()](Status status) {
            if (!status.isOK()) {
                LOGV2_WARNING(12117902,
                              "ActiveMigrationsRegistry lock acquisition attempt failed",
                              logv2::DynamicAttributes{getCoordinatorLogAttrs(),
                                                       "error"_attr = redact(status)});
            }
            return !_recoveredFromDisk || status.isOK();
        })
        .withBackoffBetweenIterations(kExponentialBackoff)
        .on(**executor, token);
}

void MergeChunksCoordinator::_releaseLocks(OperationContext* opCtx) {
    _scopedSplitMergeChunk.reset();
}

ExecutorFuture<void> MergeChunksCoordinator::_runImpl(
    std::shared_ptr<executor::ScopedTaskExecutor> executor,
    const CancellationToken& token) noexcept {
    return ExecutorFuture<void>(**executor).then([this, anchor = shared_from_this()] {
        auto opCtxHolder = makeOperationContext(/*deprioritizable=*/true);
        auto* opCtx = opCtxHolder.get();

        auto bounds = _request.getBounds();
        uassertStatusOK(ChunkRange::validate(bounds));
        ChunkRange chunkRange(bounds[0], bounds[1]);

        LOGV2(12117900,
              "Running merge chunks operation",
              logAttrs(nss()),
              "range"_attr = chunkRange.toString());

        auto expectedEpoch = _request.getEpoch();
        auto expectedTimestamp = _request.getTimestamp();

        const auto metadataBeforeMerge = [&]() {
            uassertStatusOK(
                FilteringMetadataCache::get(opCtx)->onCollectionPlacementVersionMismatch(
                    opCtx, nss(), boost::none));
            const auto metadata =
                checkCollectionIdentity(opCtx, nss(), expectedEpoch, expectedTimestamp);
            checkShardKeyPattern(opCtx, nss(), metadata, chunkRange);
            checkRangeOwnership(opCtx, nss(), metadata, chunkRange);
            return metadata;
        }();

        auto const shardingState = ShardingState::get(opCtx);

        ConfigSvrMergeChunks configRequest{
            nss(), shardingState->shardId(), metadataBeforeMerge.getUUID(), chunkRange};
        configRequest.setEpoch(expectedEpoch);
        configRequest.setTimestamp(expectedTimestamp);
        configRequest.setWriteConcern(defaultMajorityWriteConcernDoNotUse());

        auto cmdResponse = uassertStatusOK(
            Grid::get(opCtx)->shardRegistry()->getConfigShard()->runCommandWithIndefiniteRetries(
                opCtx,
                ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                DatabaseName::kAdmin,
                configRequest.toBSON(),
                Shard::RetryPolicy::kIdempotent));

        auto chunkVersionReceived = [&]() -> boost::optional<ChunkVersion> {
            if (cmdResponse.response[ChunkVersion::kChunkVersionField]) {
                return ChunkVersion::parse(cmdResponse.response[ChunkVersion::kChunkVersionField]);
            }
            return boost::none;
        }();
        uassertStatusOK(FilteringMetadataCache::get(opCtx)->onCollectionPlacementVersionMismatch(
            opCtx, nss(), std::move(chunkVersionReceived)));

        uassertStatusOKWithContext(cmdResponse.commandStatus, "Failed to commit chunk merge");
        uassertStatusOKWithContext(cmdResponse.writeConcernStatus, "Failed to commit chunk merge");

        LOGV2(12117901,
              "Completed merge chunks operation",
              logAttrs(nss()),
              "range"_attr = chunkRange.toString());
    });
}

}  // namespace mongo
