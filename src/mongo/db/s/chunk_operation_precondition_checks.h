// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/oid.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/shard_role/shard_catalog/collection.h"
#include "mongo/db/shard_role/shard_catalog/collection_metadata.h"
#include "mongo/db/shard_role/shard_catalog/collection_sharding_runtime.h"
#include "mongo/db/shard_role/shard_role.h"
#include "mongo/util/modules.h"

#include <vector>

[[MONGO_MOD_PUBLIC]];

namespace mongo {
/**
 * These functions should only be used by ddl operations such as split, merge, and split vector that
 * do not use the shard version protocol and instead perform manual checks.
 */

/**
 * Checks that the metadata for the collection is present in the CSR, that the collection is sharded
 * according to that metadata, and that the expected epoch and timestamp match what is present in
 * the CSR. Returns the collection metadata.
 *
 * This function takes the CSR lock and acquires a reference to the collection. Use the version
 * below if you already have both.
 *
 * Throws StaleShardVersion otherwise.
 */
CollectionMetadata checkCollectionIdentity(OperationContext* opCtx,
                                           const NamespaceString& nss,
                                           const boost::optional<OID>& expectedEpoch,
                                           const boost::optional<Timestamp>& expectedTimestamp);

/**
 * Same as above, but accepts the CSR and Collection references instead of acquiring them.
 */
CollectionMetadata checkCollectionIdentity(OperationContext* opCtx,
                                           const NamespaceString& nss,
                                           const boost::optional<OID>& expectedEpoch,
                                           const boost::optional<Timestamp>& expectedTimestamp,
                                           const CollectionPtr& collection,
                                           const CollectionShardingRuntime& csr);

/**
 * Validates the identity of an already-obtained CollectionMetadata against the expected
 * epoch/timestamp.
 */
void checkCollectionIdentity(OperationContext* opCtx,
                             const NamespaceString& nss,
                             const boost::optional<OID>& expectedEpoch,
                             const boost::optional<Timestamp>& expectedTimestamp,
                             const CollectionMetadata& metadata);

/**
 * Checks that the chunk range matches the shard key pattern in the metadata.
 *
 * Throws StaleShardVersion otherwise.
 */
void checkShardKeyPattern(OperationContext* opCtx,
                          const NamespaceString& nss,
                          const CollectionMetadata& metadata,
                          const ChunkRange& chunkRange);

/**
 * Checks that there is exactly one chunk owned by this shard whose bounds equal chunkRange.
 *
 * Thows StaleShardVersion otherwise.
 */
void checkChunkMatchesRange(OperationContext* opCtx,
                            const NamespaceString& nss,
                            const CollectionMetadata& metadata,
                            const ChunkRange& chunkRange);

/**
 * Checks that the range is contained within a single chunk. The bounds of the chunk do not
 * necessarily have to be the same as the bounds of the chunk.
 *
 * Thows StaleShardVersion otherwise.
 */
void checkRangeWithinChunk(OperationContext* opCtx,
                           const NamespaceString& nss,
                           const CollectionMetadata& metadata,
                           const ChunkRange& chunkRange);

/**
 * Checks that there is a series of chunks owned by this shard that make up the range in chunkRange.
 *
 * Throws StaleShardVersion otherwise.
 */
void checkRangeOwnership(OperationContext* opCtx,
                         const NamespaceString& nss,
                         const CollectionMetadata& metadata,
                         const ChunkRange& chunkRange);

/**
 * Validates that every entry in splitKeys is a valid split point for the given chunkRange:
 *   - each split point lies strictly inside chunkRange (or equals its upper bound),
 *   - split points are strictly increasing and not duplicated,
 *   - each split point conforms to the collection's shard-key pattern,
 *   - each split point is valid for BSON metadata storage.
 */
void validateSplitPoints(OperationContext* opCtx,
                         const NamespaceString& nss,
                         const CollectionMetadata& metadata,
                         const ChunkRange& chunkRange,
                         const std::vector<BSONObj>& splitKeys);

}  // namespace mongo
