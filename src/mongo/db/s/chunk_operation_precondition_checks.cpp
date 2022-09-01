/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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
#include "mongo/db/catalog_raii.h"
#include "mongo/db/s/collection_sharding_runtime.h"
#include "mongo/db/s/operation_sharding_state.h"
#include "mongo/db/s/sharding_state.h"

namespace mongo {

CollectionMetadata checkCollectionIdentity(OperationContext* opCtx,
                                           const NamespaceString& nss,
                                           const OID& expectedEpoch,
                                           const boost::optional<Timestamp>& expectedTimestamp) {
    AutoGetCollection collection(opCtx, nss, MODE_IS);

    const auto shardId = ShardingState::get(opCtx)->shardId();
    auto* const csr = CollectionShardingRuntime::get(opCtx, nss);
    const auto csrLock = CollectionShardingRuntime::CSRLock::lockExclusive(opCtx, csr);
    auto optMetadata = csr->getCurrentMetadataIfKnown();

    uassert(StaleConfigInfo(nss,
                            ShardVersion::IGNORED() /* receivedVersion */,
                            boost::none /* wantedVersion */,
                            shardId),
            str::stream() << "Collection " << nss.ns() << " needs to be recovered",
            optMetadata);

    auto metadata = *optMetadata;

    uassert(StaleConfigInfo(nss,
                            ShardVersion::IGNORED() /* receivedVersion */,
                            ShardVersion::UNSHARDED() /* wantedVersion */,
                            shardId),
            str::stream() << "Collection " << nss.ns() << " is not sharded",
            metadata.isSharded());

    uassert(ErrorCodes::NamespaceNotFound,
            "The collection was not found locally even though it is marked as sharded.",
            collection);

    const auto placementVersion = metadata.getShardVersion();
    const auto shardVersion =
        ShardVersion(placementVersion, CollectionIndexes(placementVersion, boost::none));

    uassert(StaleConfigInfo(nss,
                            ShardVersion::IGNORED() /* receivedVersion */,
                            shardVersion /* wantedVersion */,
                            shardId),
            str::stream() << "Collection " << nss.ns()
                          << " has changed since operation was sent (sent epoch: " << expectedEpoch
                          << ", current epoch: " << shardVersion.epoch() << ")",
            expectedEpoch == shardVersion.epoch() &&
                (!expectedTimestamp || expectedTimestamp == shardVersion.getTimestamp()));

    uassert(StaleConfigInfo(nss,
                            ShardVersion::IGNORED() /* receivedVersion */,
                            shardVersion /* wantedVersion */,
                            shardId),
            str::stream() << "Shard does not contain any chunks for collection.",
            placementVersion.majorVersion() > 0);

    return metadata;
}

void checkShardKeyPattern(OperationContext* opCtx,
                          const NamespaceString& nss,
                          const CollectionMetadata& metadata,
                          const ChunkRange& chunkRange) {
    const auto shardId = ShardingState::get(opCtx)->shardId();
    const auto& keyPattern = metadata.getKeyPattern();
    const auto placementVersion = metadata.getShardVersion();
    const auto shardVersion =
        ShardVersion(placementVersion, CollectionIndexes(placementVersion, boost::none));

    uassert(StaleConfigInfo(nss,
                            ShardVersion::IGNORED() /* receivedVersion */,
                            shardVersion /* wantedVersion */,
                            shardId),
            str::stream() << "The range " << chunkRange.toString()
                          << " is not valid for collection " << nss.ns() << " with key pattern "
                          << keyPattern.toString(),
            metadata.isValidKey(chunkRange.getMin()) && metadata.isValidKey(chunkRange.getMax()));
}

void checkChunkMatchesRange(OperationContext* opCtx,
                            const NamespaceString& nss,
                            const CollectionMetadata& metadata,
                            const ChunkRange& chunkRange) {
    const auto shardId = ShardingState::get(opCtx)->shardId();
    const auto placementVersion = metadata.getShardVersion();
    const auto shardVersion =
        ShardVersion(placementVersion, CollectionIndexes(placementVersion, boost::none));

    ChunkType existingChunk;
    uassert(StaleConfigInfo(nss,
                            ShardVersion::IGNORED() /* receivedVersion */,
                            shardVersion /* wantedVersion */,
                            shardId),
            str::stream() << "Range with bounds " << chunkRange.toString()
                          << " is not owned by this shard.",
            metadata.getNextChunk(chunkRange.getMin(), &existingChunk) &&
                existingChunk.getMin().woCompare(chunkRange.getMin()) == 0);

    uassert(StaleConfigInfo(nss,
                            ShardVersion::IGNORED() /* receivedVersion */,
                            shardVersion /* wantedVersion */,
                            shardId),
            str::stream() << "Chunk bounds " << chunkRange.toString() << " do not exist.",
            existingChunk.getRange() == chunkRange);
}

void checkRangeWithinChunk(OperationContext* opCtx,
                           const NamespaceString& nss,
                           const CollectionMetadata& metadata,
                           const ChunkRange& chunkRange) {
    const auto shardId = ShardingState::get(opCtx)->shardId();
    const auto placementVersion = metadata.getShardVersion();
    const auto shardVersion =
        ShardVersion(placementVersion, CollectionIndexes(placementVersion, boost::none));

    ChunkType existingChunk;
    uassert(StaleConfigInfo(nss,
                            ShardVersion::IGNORED() /* receivedVersion */,
                            shardVersion /* wantedVersion */,
                            shardId),
            str::stream() << "Range with bounds " << chunkRange.toString()
                          << " is not contained within a chunk owned by this shard.",
            metadata.getNextChunk(chunkRange.getMin(), &existingChunk) &&
                existingChunk.getRange().covers(chunkRange));
}

void checkRangeOwnership(OperationContext* opCtx,
                         const NamespaceString& nss,
                         const CollectionMetadata& metadata,
                         const ChunkRange& chunkRange) {
    const auto shardId = ShardingState::get(opCtx)->shardId();
    const auto placementVersion = metadata.getShardVersion();
    const auto shardVersion =
        ShardVersion(placementVersion, CollectionIndexes(placementVersion, boost::none));

    ChunkType existingChunk;
    BSONObj minKey = chunkRange.getMin();
    do {
        uassert(StaleConfigInfo(nss,
                                ShardVersion::IGNORED() /* receivedVersion */,
                                shardVersion /* wantedVersion */,
                                shardId),
                str::stream() << "Range with bounds " << chunkRange.toString()
                              << " is not owned by this shard.",
                metadata.getNextChunk(minKey, &existingChunk) &&
                    existingChunk.getMin().woCompare(minKey) == 0);
        minKey = existingChunk.getMax();
    } while (existingChunk.getMax().woCompare(chunkRange.getMax()) < 0);
    uassert(
        StaleConfigInfo(nss,
                        ShardVersion::IGNORED() /* receivedVersion */,
                        shardVersion /* wantedVersion */,
                        shardId),
        str::stream() << "Shard does not contain a sequence of chunks that exactly fills the range "
                      << chunkRange.toString(),
        existingChunk.getMax().woCompare(chunkRange.getMax()) == 0);
}

}  // namespace mongo
