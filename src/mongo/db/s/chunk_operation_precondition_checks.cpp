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

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/oid.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/global_catalog/type_chunk.h"
#include "mongo/db/local_catalog/catalog_raii.h"
#include "mongo/db/local_catalog/lock_manager/lock_manager_defs.h"
#include "mongo/db/local_catalog/shard_role_catalog/collection_metadata.h"
#include "mongo/db/local_catalog/shard_role_catalog/collection_sharding_runtime.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/sharding_environment/shard_id.h"
#include "mongo/db/topology/sharding_state.h"
#include "mongo/db/versioning_protocol/chunk_version.h"
#include "mongo/db/versioning_protocol/shard_version.h"
#include "mongo/db/versioning_protocol/shard_version_factory.h"
#include "mongo/db/versioning_protocol/stale_exception.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#include <utility>

#include <absl/container/node_hash_map.h>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {

namespace {

// This shard version is used as the received version in StaleConfigInfo since we do not have
// information about the received version of the operation.
ShardVersion ShardVersionPlacementIgnored() {
    return ShardVersionFactory::make(ChunkVersion::IGNORED());
}

}  // namespace

CollectionMetadata checkCollectionIdentity(OperationContext* opCtx,
                                           const NamespaceString& nss,
                                           const boost::optional<OID>& expectedEpoch,
                                           const boost::optional<Timestamp>& expectedTimestamp,
                                           const CollectionPtr& collection,
                                           const CollectionShardingRuntime& csr) {
    const auto shardId = ShardingState::get(opCtx)->shardId();
    auto optMetadata = csr.getCurrentMetadataIfKnown();

    uassert(StaleConfigInfo(nss,
                            ShardVersionPlacementIgnored() /* receivedVersion */,
                            boost::none /* wantedVersion */,
                            shardId),
            str::stream() << "Collection " << nss.toStringForErrorMsg() << " needs to be recovered",
            optMetadata);

    auto metadata = *optMetadata;

    uassert(StaleConfigInfo(nss,
                            ShardVersionPlacementIgnored() /* receivedVersion */,
                            ShardVersion::UNSHARDED() /* wantedVersion */,
                            shardId),
            str::stream() << "Collection " << nss.toStringForErrorMsg() << " is not sharded",
            metadata.isSharded());

    uassert(ErrorCodes::NamespaceNotFound,
            "The collection was not found locally even though it is marked as sharded.",
            collection);

    const auto placementVersion = metadata.getShardPlacementVersion();
    const auto shardVersion = ShardVersionFactory::make(metadata);

    uassert(StaleConfigInfo(nss,
                            ShardVersionPlacementIgnored() /* receivedVersion */,
                            shardVersion /* wantedVersion */,
                            shardId),
            str::stream() << "Collection " << nss.toStringForErrorMsg()
                          << " has changed since operation was sent (sent epoch: " << expectedEpoch
                          << ", current epoch: " << placementVersion.epoch() << ")",
            (!expectedEpoch || expectedEpoch == placementVersion.epoch()) &&
                (!expectedTimestamp || expectedTimestamp == placementVersion.getTimestamp()));

    uassert(StaleConfigInfo(nss,
                            ShardVersionPlacementIgnored() /* receivedVersion */,
                            shardVersion /* wantedVersion */,
                            shardId),
            str::stream() << "Shard does not contain any chunks for collection.",
            placementVersion.majorVersion() > 0);

    return metadata;
}

CollectionMetadata checkCollectionIdentity(OperationContext* opCtx,
                                           const NamespaceString& nss,
                                           const boost::optional<OID>& expectedEpoch,
                                           const boost::optional<Timestamp>& expectedTimestamp) {
    AutoGetCollection collection(opCtx, nss, MODE_IS);
    const auto scopedCsr =
        CollectionShardingRuntime::assertCollectionLockedAndAcquireShared(opCtx, nss);
    return checkCollectionIdentity(
        opCtx, nss, expectedEpoch, expectedTimestamp, *collection, *scopedCsr);
}


void checkShardKeyPattern(OperationContext* opCtx,
                          const NamespaceString& nss,
                          const CollectionMetadata& metadata,
                          const ChunkRange& chunkRange) {
    const auto shardId = ShardingState::get(opCtx)->shardId();
    const auto& keyPattern = metadata.getKeyPattern();
    const auto shardVersion = ShardVersionFactory::make(metadata);

    uassert(StaleConfigInfo(nss,
                            ShardVersionPlacementIgnored() /* receivedVersion */,
                            shardVersion /* wantedVersion */,
                            shardId),
            str::stream() << "The range " << chunkRange.toString()
                          << " is not valid for collection " << nss.toStringForErrorMsg()
                          << " with key pattern " << keyPattern.toString(),
            metadata.isValidKey(chunkRange.getMin()) && metadata.isValidKey(chunkRange.getMax()));
}

void checkChunkMatchesRange(OperationContext* opCtx,
                            const NamespaceString& nss,
                            const CollectionMetadata& metadata,
                            const ChunkRange& chunkRange) {
    const auto shardId = ShardingState::get(opCtx)->shardId();
    const auto shardVersion = ShardVersionFactory::make(metadata);

    ChunkType existingChunk;
    uassert(StaleConfigInfo(nss,
                            ShardVersionPlacementIgnored() /* receivedVersion */,
                            shardVersion /* wantedVersion */,
                            shardId),
            str::stream() << "Range with bounds " << chunkRange.toString()
                          << " is not owned by this shard.",
            metadata.getNextChunk(chunkRange.getMin(), &existingChunk) &&
                existingChunk.getMin().woCompare(chunkRange.getMin()) == 0);

    uassert(StaleConfigInfo(nss,
                            ShardVersionPlacementIgnored() /* receivedVersion */,
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
    const auto shardVersion = ShardVersionFactory::make(metadata);

    ChunkType existingChunk;
    uassert(StaleConfigInfo(nss,
                            ShardVersionPlacementIgnored() /* receivedVersion */,
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
    const auto shardVersion = ShardVersionFactory::make(metadata);

    ChunkType existingChunk;
    BSONObj minKey = chunkRange.getMin();
    do {
        uassert(StaleConfigInfo(nss,
                                ShardVersionPlacementIgnored() /* receivedVersion */,
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
                        ShardVersionPlacementIgnored() /* receivedVersion */,
                        shardVersion /* wantedVersion */,
                        shardId),
        str::stream() << "Shard does not contain a sequence of chunks that exactly fills the range "
                      << chunkRange.toString(),
        existingChunk.getMax().woCompare(chunkRange.getMax()) == 0);
}

}  // namespace mongo
