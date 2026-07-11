// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/oid.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/database_name.h"
#include "mongo/db/versioning_protocol/chunk_version.h"
#include "mongo/util/modules.h"

#include <string>
#include <vector>

#include <boost/optional/optional.hpp>

namespace mongo {

class ChunkType;
class NamespaceString;
class OperationContext;

class ShardCollectionType;
class ShardDatabaseType;

namespace shardmetadatautil {

/**
 * Structure representing the generated query and sort order for a chunk diffing operation.
 */
struct [[MONGO_MOD_NEEDS_REPLACEMENT]] QueryAndSort {
    const BSONObj query;
    const BSONObj sort;
};

/**
 * Returns the query needed to find incremental changes to the chunks collection on a shard server.
 *
 * The query has to find all the chunks $gte the current max version. Currently, any splits, merges
 * and moves will increment the current max version. Querying by lastmod is essential because we
 * want to use the {lastmod} index on the chunks collection. This makes potential cursor yields to
 * apply split/merge/move updates safe: updates always move or insert documents at the end of the
 * index (because the document updates always have higher lastmod), so changed always come *after*
 * our current cursor position and are seen when the cursor recommences.
 *
 * The sort must be by ascending version so that the updates can be applied in-memory in order. This
 * is important because it is possible for a cursor to read updates to the same _id document twice,
 * due to the yield described above. If updates are applied in ascending version order, the newer
 * update is applied last.
 */
[[MONGO_MOD_NEEDS_REPLACEMENT]] QueryAndSort createShardChunkDiffQuery(
    const ChunkVersion& collectionPlacementVersion);

/**
 * Writes a persisted signal to indicate that it is once again safe to read from the chunks
 * collection for 'nss' and updates the collection's collection placement version to
 * 'refreshedVersion'. It is essential to call this after updating the chunks collection so that
 * secondaries know they can safely use the chunk metadata again.
 *
 * It is safe to call this multiple times: it's an idempotent action.
 *
 * refreshedVersion - the new collection placement version for the completed refresh.
 *
 * Note: if there is no document present in the collections collection for 'nss', nothing is
 * updated.
 */
[[MONGO_MOD_NEEDS_REPLACEMENT]] Status unsetPersistedRefreshFlags(
    OperationContext* opCtx, const NamespaceString& nss, const ChunkVersion& refreshedVersion);

/**
 * Represents a subset of a collection's config.cache.collections entry that relates to refresh
 * state.
 */
struct [[MONGO_MOD_NEEDS_REPLACEMENT]] RefreshState {
    bool operator==(const RefreshState& other) const;

    std::string toString() const;

    // The current generation of the collection.
    CollectionGeneration generation;

    // Whether a refresh is currently in progress.
    bool refreshing;

    // The collection placement version after the last complete refresh. Indicates change if
    // refreshing has started and finished since last loaded.
    ChunkVersion lastRefreshedCollectionPlacementVersion;
};

/**
 * Reads the persisted refresh signal for 'nss' and returns those settings.
 */
[[MONGO_MOD_NEEDS_REPLACEMENT]] StatusWith<RefreshState> getPersistedRefreshFlags(
    OperationContext* opCtx, const NamespaceString& nss);

/**
 * Reads the shard server's collections collection entry identified by 'nss'.
 */
[[MONGO_MOD_NEEDS_REPLACEMENT]] StatusWith<ShardCollectionType> readShardCollectionsEntry(
    OperationContext* opCtx, const NamespaceString& nss);

/**
 * Reads the shard server's databases collection entry identified by 'dbName'.
 */
[[MONGO_MOD_NEEDS_REPLACEMENT]] StatusWith<ShardDatabaseType> readShardDatabasesEntry(
    OperationContext* opCtx, const DatabaseName& dbName);

/**
 * Updates the collections collection entry matching 'query' with 'update' using local write
 * concern.
 *
 * If 'upsert' is true, expects 'lastRefreshedCollectionPlacementVersion' to be absent in the
 * update: these refreshing fields should only be added to an existing document.
 */
[[MONGO_MOD_NEEDS_REPLACEMENT]] Status updateShardCollectionsEntry(OperationContext* opCtx,
                                                                   const BSONObj& query,
                                                                   const BSONObj& update,
                                                                   bool upsert);

/**
 * Updates the databases collection entry matching 'query' with 'update' using local write
 * concern.
 *
 * Uses the $set operator on the update so that updates can be applied without resetting everything.
 * 'inc' can be used to specify fields and their increments: it will be assigned to the $inc
 * operator.
 *
 * 'inc' should not specify 'upsert' true.
 */
[[MONGO_MOD_NEEDS_REPLACEMENT]] Status updateShardDatabasesEntry(OperationContext* opCtx,
                                                                 const BSONObj& query,
                                                                 const BSONObj& update,
                                                                 const BSONObj& inc,
                                                                 bool upsert);

/**
 * Reads the shard server's chunks collection corresponding to 'nss' for chunks matching 'query',
 * returning at most 'limit' chunks in 'sort' order. 'epoch' populates the returned chunks' version
 * fields, because we do not yet have UUIDs to replace epochs nor UUIDs associated with namespaces.
 */
[[MONGO_MOD_NEEDS_REPLACEMENT]] StatusWith<std::vector<ChunkType>> readShardChunks(
    OperationContext* opCtx,
    const NamespaceString& nss,
    const BSONObj& query,
    const BSONObj& sort,
    boost::optional<long long> limit,
    const OID& epoch,
    const Timestamp& timestamp);

/**
 * Takes a vector of 'chunks' and updates the shard's chunks collection for 'nss'. Any chunk
 * documents in config.cache.chunks.<ns> that overlap with a chunk in 'chunks' is removed as the
 * updated chunk document is inserted. If the epoch of a chunk in 'chunks' does not match
 * 'currEpoch', a ConflictingOperationInProgress error is returned and no more updates are applied.
 *
 * Note: two threads running this function in parallel for the same collection can corrupt the
 * collection data!
 *
 * nss - the collection namespace for which chunk metadata is being updated.
 * chunks - chunks retrieved from the config server, sorted in ascending chunk version order.
 * currEpoch - what this shard server expects the collection epoch to be.
 *
 * Returns:
 * - ConflictingOperationInProgress if the chunk version epoch of any chunk in 'chunks' is different
 *   than 'currEpoch'.
 * - Other errors if unable to do local writes/reads to the config.chunks.ns collection.
 */
[[MONGO_MOD_NEEDS_REPLACEMENT]] Status updateShardChunks(OperationContext* opCtx,
                                                         const NamespaceString& nss,
                                                         const std::vector<ChunkType>& chunks,
                                                         const OID& currEpoch);

/**
 * Deletes locally persisted chunk metadata associated with 'nss': drops the chunks collection
 * and removes the collections collection entry.
 *
 * The order is important because the secondary observes changes to the config.collections entries.
 * If the chunks were dropped first, the secondary would keep refreshing until it exceeded its
 * retries, rather than returning with a useful error message.
 */
[[MONGO_MOD_NEEDS_REPLACEMENT]] Status dropChunksAndDeleteCollectionsEntry(
    OperationContext* opCtx, const NamespaceString& nss);

/**
 * Drops locally persisted chunk metadata associated with 'nss': only drops the chunks collection.
 */
[[MONGO_MOD_NEEDS_REPLACEMENT]] void dropChunks(OperationContext* opCtx,
                                                const NamespaceString& nss);

/**
 * Deletes locally persisted database metadata associated with 'dbName': removes the databases
 * collection entry.
 */
[[MONGO_MOD_NEEDS_REPLACEMENT]] Status deleteDatabasesEntry(OperationContext* opCtx,
                                                            const DatabaseName& dbName);

}  // namespace shardmetadatautil
}  // namespace mongo
