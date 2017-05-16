/**
 *    Copyright (C) 2017 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#pragma once

#include <string>
#include <vector>

#include "mongo/base/status.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/oid.h"

namespace mongo {

struct ChunkVersion;
class ChunkType;
class CollectionMetadata;
class NamespaceString;
class OperationContext;
class ShardCollectionType;
template <typename T>
class StatusWith;

/**
 * Function helpers to locally, using a DBDirectClient, read and write sharding metadata on a shard.
 */
namespace shardmetadatautil {

/**
 * Structure representing the generated query and sort order for a chunk diffing operation.
 */
struct QueryAndSort {
    const BSONObj query;
    const BSONObj sort;
};

/**
 * Subset of the shard's collections collection related to refresh state.
 */
struct RefreshState {
    bool operator==(RefreshState& other);

    OID epoch;
    bool refreshing;
    long long sequenceNumber;
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
QueryAndSort createShardChunkDiffQuery(const ChunkVersion& collectionVersion);

/**
 * Writes a persisted signal to indicate that the chunks collection is being updated. It is
 * essential to call this before updating the chunks collection for 'nss' so that secondaries do not
 * use incomplete metadata.
 *
 * It is safe to call this multiple times: it's an idempotent action.
 *
 * Don't forget to call refreshPersistedSignalFinish after the chunks update is finished!
 *
 * Note: if there is no document present in the collections collection for 'nss', nothing is
 * updated.
 */
Status setPersistedRefreshFlags(OperationContext* opCtx, const NamespaceString& nss);

/**
 * Writes a persisted signal to indicate that it is once again safe to read from the chunks
 * collection for 'nss'. It is essential to call this after updating the chunks collection so that
 * secondaries know they can safely use the chunk metadata again.
 *
 * Note: if there is no document present in the collections collection for 'nss', nothing is
 * updated.
 */
Status unsetPersistedRefreshFlags(OperationContext* opCtx, const NamespaceString& nss);

/**
 * Reads the persisted refresh signal for 'nss' and returns those settings.
 */
StatusWith<RefreshState> getPersistedRefreshFlags(OperationContext* opCtx,
                                                  const NamespaceString& nss);

/**
 * Reads the shard server's collections collection entry identified by 'nss'.
 */
StatusWith<ShardCollectionType> readShardCollectionsEntry(OperationContext* opCtx,
                                                          const NamespaceString& nss);

/**
 * Updates the collections collection entry matching 'query' with 'update' using local write
 * concern.
 *
 * Uses the $set operator on the update so that updates can be applied without resetting everything.
 *
 * If 'upsert' is true, $setOnInsert is used to initialize the 'refreshSequenceNumber'.
 * upsert should only be used when applying updates from the config server, where the update never
 * includes the 'refreshing' and 'refreshSequenceNumber' fields.
 *
 * 'inc' contains fields and the desired increments, e.g. {'refreshSequenceNumber': 1}. Never use
 * 'upsert' true with a non-empty 'inc' object.
 */
Status updateShardCollectionsEntry(OperationContext* opCtx,
                                   const BSONObj& query,
                                   const BSONObj& update,
                                   const BSONObj& inc,
                                   const bool upsert);

/**
 * Reads the shard server's chunks collection corresponding to 'nss' for chunks matching 'query',
 * returning at most 'limit' chunks in 'sort' order. 'epoch' populates the returned chunks' version
 * fields, because we do not yet have UUIDs to replace epoches nor UUIDs associated with namespaces.
 */
StatusWith<std::vector<ChunkType>> readShardChunks(OperationContext* opCtx,
                                                   const NamespaceString& nss,
                                                   const BSONObj& query,
                                                   const BSONObj& sort,
                                                   boost::optional<long long> limit,
                                                   const OID& epoch);

/**
 * Takes a vector of 'chunks' and updates the shard's chunks collection for 'nss'. Any chunk
 * documents in config.chunks.ns that overlap with a chunk in 'chunks' is removed as the updated
 * chunk document is inserted. If the epoch of a chunk in 'chunks' does not match 'currEpoch',
 * a ConflictingOperationInProgress error is returned and no more updates are applied.
 *
 * Note: two threads running this function in parallel for the same collection can corrupt the
 * collection data!
 *
 * nss - the regular collection namespace for which chunk metadata is being updated.
 * chunks - chunks retrieved from the config server, sorted in ascending chunk version order
 * currEpoch - what this shard server expects the collection epoch to be.
 *
 * Returns:
 * - ConflictingOperationInProgress if the chunk version epoch of any chunk in 'chunks' is different
 *   than 'currEpoch'
 * - Other errors if unable to do local writes/reads to the config.chunks.ns collection.
 */
Status updateShardChunks(OperationContext* opCtx,
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
Status dropChunksAndDeleteCollectionsEntry(OperationContext* opCtx, const NamespaceString& nss);

}  // namespace shardmetadatautil
}  // namespace mongo
