/**
 * Copyright (C) 2013 10gen Inc.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License, version 3,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 *
 * As a special exception, the copyright holders give permission to link the
 * code of portions of this program with the OpenSSL library under certain
 * conditions as described in each individual source file and distribute
 * linked combinations including the program with the OpenSSL library. You
 * must comply with the GNU Affero General Public License in all respects
 * for all of the code used other than as permitted herein. If you modify
 * file(s) with this exception, you may extend this exception to your
 * version of the file(s), but you are not obligated to do so. If you do not
 * wish to do so, delete this exception statement from your version. If you
 * delete this exception statement from all source files in the program,
 * then also delete it in the license file.
 */

#pragma once

#include <map>

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobj_comparator_interface.h"
#include "mongo/bson/simple_bsonobj_comparator.h"
#include "mongo/db/namespace_string.h"
#include "mongo/s/catalog_cache.h"
#include "mongo/s/ns_targeter.h"

namespace mongo {

struct TargeterStats {
    // Map of chunk shard minKey -> approximate delta. This is used for deciding whether a chunk
    // might need splitting or not.
    BSONObjIndexedMap<int> chunkSizeDelta{
        SimpleBSONObjComparator::kInstance.makeBSONObjIndexedMap<int>()};
};

/**
 * NSTargeter based on a ChunkManager implementation. Wraps all exception codepaths and returns
 * NamespaceNotFound status on applicable failures.
 *
 * Must be initialized before use, and initialization may fail.
 */
class ChunkManagerTargeter : public NSTargeter {
public:
    enum class UpdateType { kReplacement, kOpStyle, kUnknown };

    ChunkManagerTargeter(const NamespaceString& nss, TargeterStats* stats);

    /**
     * Initializes the ChunkManagerTargeter with the latest targeting information for the
     * namespace.  May need to block and load information from a remote config server.
     *
     * Returns !OK if the information could not be initialized.
     */
    Status init(OperationContext* opCtx);

    const NamespaceString& getNS() const override;

    // Returns ShardKeyNotFound if document does not have a full shard key.
    StatusWith<ShardEndpoint> targetInsert(OperationContext* opCtx,
                                           const BSONObj& doc) const override;

    // Returns ShardKeyNotFound if the update can't be targeted without a shard key.
    StatusWith<std::vector<ShardEndpoint>> targetUpdate(
        OperationContext* opCtx, const write_ops::UpdateOpEntry& updateDoc) const override;

    // Returns ShardKeyNotFound if the delete can't be targeted without a shard key.
    StatusWith<std::vector<ShardEndpoint>> targetDelete(
        OperationContext* opCtx, const write_ops::DeleteOpEntry& deleteDoc) const override;

    StatusWith<std::vector<ShardEndpoint>> targetCollection() const override;

    StatusWith<std::vector<ShardEndpoint>> targetAllShards(OperationContext* opCtx) const override;

    void noteCouldNotTarget() override;

    void noteStaleResponse(const ShardEndpoint& endpoint, const BSONObj& staleInfo) override;

    /**
     * Replaces the targeting information with the latest information from the cache.  If this
     * information is stale WRT the noted stale responses or a remote refresh is needed due
     * to a targeting failure, will contact the config servers to reload the metadata.
     *
     * Reports wasChanged = true if the metadata is different after this reload.
     *
     * Also see NSTargeter::refreshIfNeeded().
     */
    Status refreshIfNeeded(OperationContext* opCtx, bool* wasChanged) override;

private:
    using ShardVersionMap = std::map<ShardId, ChunkVersion>;

    /**
     * Performs an actual refresh from the config server.
     */
    Status _refreshNow(OperationContext* opCtx);

    /**
     * Attempts to route an update operation by extracting an exact shard key from the given query
     * and/or update expression. Should only be called on sharded collections, and with a valid
     * updateType. Returns not-OK if the user's query is invalid, if the extracted shard key exceeds
     * the maximum allowed length, or if the update could not be targeted by exact shard key.
     */
    StatusWith<std::vector<ShardEndpoint>> _targetUpdateByShardKey(OperationContext* opCtx,
                                                                   const UpdateType updateType,
                                                                   const BSONObj query,
                                                                   const BSONObj collation,
                                                                   const BSONObj updateExpr,
                                                                   const bool isUpsert) const;

    /**
     * Returns a vector of ShardEndpoints where a document might need to be placed.
     *
     * Returns !OK with message if replacement could not be targeted
     *
     * If 'collation' is empty, we use the collection default collation for targeting.
     */
    StatusWith<std::vector<ShardEndpoint>> _targetDoc(OperationContext* opCtx,
                                                      const BSONObj& doc,
                                                      const BSONObj& collation) const;

    /**
     * Returns a vector of ShardEndpoints for a potentially multi-shard query.
     *
     * Returns !OK with message if query could not be targeted.
     *
     * If 'collation' is empty, we use the collection default collation for targeting.
     */
    StatusWith<std::vector<ShardEndpoint>> _targetQuery(OperationContext* opCtx,
                                                        const BSONObj& query,
                                                        const BSONObj& collation) const;

    /**
     * Returns a ShardEndpoint for an exact shard key query.
     *
     * Also has the side effect of updating the chunks stats with an estimate of the amount of
     * data targeted at this shard key.
     *
     * If 'collation' is empty, we use the collection default collation for targeting.
     */
    ShardEndpoint _targetShardKey(const BSONObj& shardKey,
                                  const BSONObj& collation,
                                  long long estDataSize) const;

    // Full namespace of the collection for this targeter
    const NamespaceString _nss;

    // Stores whether we need to check the remote server on refresh
    bool _needsTargetingRefresh;

    // Represents only the view and not really part of the targeter state. This is not owned here.
    TargeterStats* _stats;

    // The latest loaded routing cache entry
    boost::optional<CachedCollectionRoutingInfo> _routingInfo;

    // Map of shard->remote shard version reported from stale errors
    ShardVersionMap _remoteShardVersions;
};

}  // namespace mongo
