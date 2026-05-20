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

#pragma once

#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/sharding_environment/shard_id.h"
#include "mongo/util/modules.h"

MONGO_MOD_PARENT_PRIVATE;
namespace mongo {
namespace shard_catalog_commit {

/**
 * Shard id for the placeholder chunk when a collection is tracked on a shard but owns no real
 * chunks (see commitCreateCollectionChunklessLocally).
 */
inline const ShardId kChunklessPlaceholderShardId{"__chunkless_placeholder__"};

/**
 * Fetches the latest collection metadata and owned chunks from the global catalog, persists them
 * to the shard catalog (config.shard.catalog.collections and config.shard.catalog.chunks), removes
 * any stale chunks whose shard key bounds don't match the refined key pattern, writes an oplog
 * entry to invalidate collection metadata on secondaries, and updates the in-memory
 * CollectionShardingRuntime (CSR) with the new routing information.
 */
void commitRefineShardKeyLocally(OperationContext* opCtx, const NamespaceString& nss);

/**
 * Deletes the collection and chunk metadata from the shard catalog
 * (config.shard.catalog.collections and config.shard.catalog.chunks), writes an oplog entry to
 * invalidate collection metadata on secondaries, and clears the in-memory CollectionShardingRuntime
 * (CSR) for the dropped collection.
 */
void commitDropCollectionLocally(OperationContext* opCtx,
                                 const NamespaceString& nss,
                                 const UUID& uuid);

/**
 * Deletes only chunk metadata from the shard catalog (config.shard.catalog.chunks) and does nothing
 * else. Only used by rename in the event of rename overwriting an existing collection.
 */
void commitDropOfStaleChunksForRename(OperationContext* opCtx, const UUID& uuid);

/**
 * Modifies the shard catalog for both fromNss and toNss in order to durably persist the decision to
 * rename the collection.
 * The command will invalidate the collection metadata for both namespaces and clear the in-memory
 * state in order to repopulate it on the next query.
 */
void commitRenameOfCollectionMetadata(OperationContext* opCtx,
                                      const NamespaceString& fromNss,
                                      const boost::optional<UUID>& fromUUID,
                                      const NamespaceString& toNss,
                                      const boost::optional<UUID>& targetUUID,
                                      const boost::optional<UUID>& newTargetUUID,
                                      bool isUpgrading,
                                      bool isDbPrimaryShard);
/**
 * Performs the local persistence of up-to-date collection metadata and chunk information for a
 * sharded collection on the shard. Specifically:
 *   1. Removes any existing chunk entries for the specified collection from the shard catalog
 *      (config.shard.catalog.chunks).
 *   2. Fetches the latest collection metadata and owned chunk entries from the global catalog.
 *   3. Persists the collection metadata and owned chunks to the shard catalog collections and
 * chunks namespaces (config.shard.catalog.collections and config.shard.catalog.chunks).
 *   4. Writes an oplog entry to invalidate collection metadata on secondaries.
 *   5. Updates the in-memory CollectionShardingRuntime (CSR) to reflect the new filtering
 * information.
 */
void commitCollectionMetadataLocally(OperationContext* opCtx,
                                     const NamespaceString& nss,
                                     bool isDbPrimaryShard = false);

/**
 * Fetches the collection metadata from the global catalog, removes any existing chunk entries for
 * this collection UUID, persists a chunkless placeholder chunk and the updated collection document
 * to the shard catalog, writes an oplog entry to invalidate secondaries, and refreshes the
 * in-memory CollectionShardingRuntime. Used for shards that participate in a tracked collection
 * but own no chunks.
 */
void commitChunklessCollectionLocally(OperationContext* opCtx, const NamespaceString& nss);

}  // namespace shard_catalog_commit
}  // namespace mongo
