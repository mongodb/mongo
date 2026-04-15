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
 * Fetches the collection metadata and owned chunks from the global catalog, persists them to the
 * shard catalog (config.shard.catalog.collections and config.shard.catalog.chunks), writes an
 * oplog entry to invalidate collection metadata on secondaries, and updates the in-memory
 * CollectionShardingRuntime (CSR) with the new routing information.
 */
void commitCreateCollectionLocally(OperationContext* opCtx, const NamespaceString& nss);

/**
 * Fetches the collection metadata from the global catalog (without chunks), persists only the
 * collection document to the shard catalog (config.shard.catalog.collections), writes an oplog
 * entry to invalidate collection metadata on secondaries, and updates the in-memory
 * CollectionShardingRuntime (CSR) with a chunkless tracked metadata. This is used for shards that
 * participate on a tracked collection but do not own any chunks (e.g., the DB primary shard after
 * create collection).
 */
void commitCreateCollectionChunklessLocally(OperationContext* opCtx, const NamespaceString& nss);

}  // namespace shard_catalog_commit
}  // namespace mongo
