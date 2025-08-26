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

#pragma once
#include "mongo/bson/oid.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/local_catalog/collection.h"
#include "mongo/db/local_catalog/shard_role_catalog/collection_metadata.h"
#include "mongo/db/local_catalog/shard_role_catalog/collection_sharding_runtime.h"

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

}  // namespace mongo
