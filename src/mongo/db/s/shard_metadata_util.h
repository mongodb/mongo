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

namespace mongo {

class BSONObj;
class ChunkType;
class CollectionMetadata;
class ShardCollectionType;
class NamespaceString;
class OID;
class OperationContext;
class ShardingCatalogClient;
template <typename T>
class StatusWith;

/**
 * Function helpers to locally, using a DBDirectClient, read and write sharding metadata on a shard.
 * Also retrieves metadata from the config server, copies of which are then persisted on the shard.
 */
namespace shardmetadatautil {

/**
 * Gets the config.collections for 'nss' entry either remotely from the config server if
 * 'isShardPrimary' is true or locally from the shard if false. Additionally updates the shard's
 * config.collections entry with the remotely retrieved metadata if 'isShardPrimary' is true.
 */
StatusWith<std::pair<BSONObj, OID>> getCollectionShardKeyAndEpoch(
    OperationContext* opCtx,
    ShardingCatalogClient* catalogClient,
    const NamespaceString& nss,
    bool isShardPrimary);

/**
 * Reads the shard server's config.collections entry identified by 'nss'.
 */
StatusWith<ShardCollectionType> readShardCollectionEntry(OperationContext* opCtx,
                                                         const NamespaceString& nss);

/**
 * Updates the config.collections entry matching 'query' with 'update' using local write concern.
 * Only the fields specified in 'update' are modified.
 * Sets upsert to true on the update operation in case the entry does not exist locally yet.
 */
Status updateCollectionEntry(OperationContext* opCtx,
                             const NamespaceString& nss,
                             const BSONObj& query,
                             const BSONObj& update);

/**
 * Two threads running this function in parallel for the same collection can corrupt the collection
 * data!
 *
 * Takes a vector of 'chunks' and updates the config.chunks.ns collection specified by 'nss'.
 * Any chunk documents in config.chunks.ns that overlap with a chunk in 'chunks' is removed
 * as the new chunk document is inserted. If the epoch of any chunk in 'chunks' does not match
 * 'currEpoch', the chunk metadata is dropped and a RemoteChangeDetected error returned.
 *
 * @nss - the regular collection namespace for which chunk metadata is being updated.
 * @chunks - chunks retrieved from the config server, sorted in ascending chunk version order
 * @currEpoch - what this shard server expects to be the collection epoch.
 *
 * Returns:
 * - RemoteChangeDetected if the chunk version epoch of any chunk in 'chunks' is different than
 * 'currEpoch'
 * - Other errors if unable to do local writes/reads to the config.chunks.ns colection.
 */
Status writeNewChunks(OperationContext* opCtx,
                      const NamespaceString& nss,
                      const std::vector<ChunkType>& chunks,
                      const OID& currEpoch);

/**
 * Locally on this shard, drops the config.chunks.ns corresponding to 'chunkMetadataNss' and then
 * deletes the config.collections entry for 'collectionEntryNss'.
 */
Status dropChunksAndDeleteCollectionsEntry(OperationContext* opCtx,
                                           const NamespaceString& chunkMetadataNss,
                                           const NamespaceString& collectionsEntryNss);

}  // namespace shardmetadatautil
}  // namespace mongo
