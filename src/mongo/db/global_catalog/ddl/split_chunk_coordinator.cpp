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

#include "mongo/db/global_catalog/ddl/split_chunk_coordinator.h"

#include "mongo/bson/simple_bsonobj_comparator.h"
#include "mongo/db/global_catalog/ddl/split_chunk.h"
#include "mongo/db/s/chunk_operation_precondition_checks.h"
#include "mongo/db/shard_role/shard_catalog/shard_filtering_metadata_refresh.h"
#include "mongo/logv2/log.h"
#include "mongo/util/future_util.h"
#include "mongo/util/str.h"

#include <boost/none.hpp>
#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

namespace mongo {

SplitChunkCoordinator::SplitChunkCoordinator(ShardingCoordinatorService* service,
                                             const BSONObj& initialStateDoc)
    : ChunkOperationShardingCoordinator(service, "SplitChunkCoordinator", initialStateDoc),
      _request(_doc.getShardsvrSplitChunkRequest()) {}

void SplitChunkCoordinator::checkIfOptionsConflict(const BSONObj& doc) const {
    const auto otherDoc = SplitChunkCoordinatorDocument::parse(
        doc, IDLParserContext("SplitChunkCoordinatorDocument"));

    const auto& selfReq = _request.toBSON();
    const auto& otherReq = otherDoc.getShardsvrSplitChunkRequest().toBSON();

    uassert(ErrorCodes::ConflictingOperationInProgress,
            str::stream() << "Another split chunk operation for namespace "
                          << nss().toStringForErrorMsg()
                          << " is being executed with different parameters: " << redact(selfReq)
                          << " vs " << redact(otherReq),
            SimpleBSONObjComparator::kInstance.evaluate(selfReq == otherReq));
}

void SplitChunkCoordinator::appendCommandInfo(BSONObjBuilder* cmdInfoBuilder) const {
    cmdInfoBuilder->appendElements(_request.toBSON());
}

bool SplitChunkCoordinator::isInCriticalSection(Phase phase) const {
    return false;
}

ExecutorFuture<void> SplitChunkCoordinator::_acquireLocksAsync(
    OperationContext* opCtx,
    std::shared_ptr<executor::ScopedTaskExecutor> executor,
    const CancellationToken& token) {
    return AsyncTry([this, anchor = shared_from_this()] {
               auto opCtxHolder = makeOperationContext(/*deprioritizable=*/true);
               auto* opCtx = opCtxHolder.get();

               auto chunkRange = ChunkRange(_request.getMin(), _request.getMax());
               _scopedSplitMergeChunk.emplace(
                   uassertStatusOK(ActiveMigrationsRegistry::get(opCtx).registerSplitOrMergeChunk(
                       opCtx, nss(), chunkRange)));
           })
        .until([this, anchor = shared_from_this()](Status status) {
            if (!status.isOK()) {
                LOGV2_WARNING(12117803,
                              "ActiveMigrationsRegistry lock acquisition attempt failed",
                              logv2::DynamicAttributes{getCoordinatorLogAttrs(),
                                                       "error"_attr = redact(status)});
            }
            return !_recoveredFromDisk || status.isOK();
        })
        .withBackoffBetweenIterations(kExponentialBackoff)
        .on(**executor, token);
}

void SplitChunkCoordinator::_releaseLocks(OperationContext* opCtx) {
    _scopedSplitMergeChunk.reset();
}

ExecutorFuture<void> SplitChunkCoordinator::_runImpl(
    std::shared_ptr<executor::ScopedTaskExecutor> executor,
    const CancellationToken& token) noexcept {
    return ExecutorFuture<void>(**executor).then([this, anchor = shared_from_this()] {
        auto opCtxHolder = makeOperationContext(/*deprioritizable=*/true);
        auto* opCtx = opCtxHolder.get();

        const auto& keyPatternObj = _request.getKeyPattern();
        auto chunkRange = ChunkRange(_request.getMin(), _request.getMax());
        uassertStatusOK(ChunkRange::validate(chunkRange));
        const auto& splitKeys = _request.getSplitKeys();
        const std::string shardName{_request.getFrom()};
        const auto& expectedCollectionEpoch = _request.getEpoch();
        const auto& expectedCollectionTimestamp = _request.getTimestamp();

        LOGV2(12117800,
              "Running split chunk operation",
              logAttrs(nss()),
              "range"_attr = chunkRange.toString());

        uassert(ErrorCodes::InvalidOptions,
                "need to provide the split points to chunk over",
                !splitKeys.empty());

        // Verify placement version, collection identity, shard key pattern and chunk range
        // are still valid.
        {
            uassertStatusOK(
                FilteringMetadataCache::get(opCtx)->onCollectionPlacementVersionMismatch(
                    opCtx, nss(), boost::none));
            const auto metadata = checkCollectionIdentity(
                opCtx, nss(), expectedCollectionEpoch, expectedCollectionTimestamp);
            checkShardKeyPattern(opCtx, nss(), metadata, chunkRange);
            checkChunkMatchesRange(opCtx, nss(), metadata, chunkRange);
        }

        uassertStatusOK(splitChunk(opCtx,
                                   nss(),
                                   keyPatternObj,
                                   chunkRange,
                                   std::vector<BSONObj>(splitKeys.begin(), splitKeys.end()),
                                   shardName,
                                   expectedCollectionEpoch,
                                   expectedCollectionTimestamp,
                                   *_scopedSplitMergeChunk));

        LOGV2(12117802,
              "Completed split chunk operation",
              logAttrs(nss()),
              "range"_attr = chunkRange.toString());
    });
}

}  // namespace mongo
