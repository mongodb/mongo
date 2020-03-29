/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

using StaleShardVersionMap = std::map<ShardId, ChunkVersion>;

/**
 * NSTargeter based on a ChunkManager implementation. Wraps all exception codepaths and returns
 * NamespaceNotFound status on applicable failures.
 *
 * Must be initialized before use, and initialization may fail.
 */
class ChunkManagerTargeter : public NSTargeter {
public:
    enum class UpdateType { kReplacement, kOpStyle, kUnknown };

    /**
     * Initializes the targeter with the latest routing information for the namespace, which means
     * it may have to block and load information from the config server.
     *
     * If 'expectedEpoch' is specified, the targeter will throws 'StaleEpoch' exception if the epoch
     * for 'nss' ever becomes different from 'expectedEpoch'. Otherwise, the targeter will continue
     * targeting even if the collection gets dropped and recreated.
     */
    ChunkManagerTargeter(OperationContext* opCtx,
                         const NamespaceString& nss,
                         boost::optional<OID> expectedEpoch = boost::none);

    const NamespaceString& getNS() const override;

    ShardEndpoint targetInsert(OperationContext* opCtx, const BSONObj& doc) const override;

    std::vector<ShardEndpoint> targetUpdate(
        OperationContext* opCtx, const write_ops::UpdateOpEntry& updateOp) const override;

    std::vector<ShardEndpoint> targetDelete(
        OperationContext* opCtx, const write_ops::DeleteOpEntry& deleteOp) const override;

    std::vector<ShardEndpoint> targetAllShards(OperationContext* opCtx) const override;

    void noteCouldNotTarget() override;

    void noteStaleShardResponse(const ShardEndpoint& endpoint,
                                const StaleConfigInfo& staleInfo) override;

    void noteStaleDbResponse(const ShardEndpoint& endpoint,
                             const StaleDbRoutingVersion& staleInfo) override;

    /**
     * Replaces the targeting information with the latest information from the cache.  If this
     * information is stale WRT the noted stale responses or a remote refresh is needed due
     * to a targeting failure, will contact the config servers to reload the metadata.
     *
     * Reports wasChanged = true if the metadata is different after this reload.
     *
     * Also see NSTargeter::refreshIfNeeded().
     */
    void refreshIfNeeded(OperationContext* opCtx, bool* wasChanged) override;

    bool endpointIsConfigServer() const override;

    int getNShardsOwningChunks() const override;

private:
    void _init(OperationContext* opCtx);

    /**
     * Performs an actual refresh from the config server.
     */
    void _refreshShardVersionNow(OperationContext* opCtx);

    /**
     * Performs an actual refresh from the config server.
     */
    void _refreshDbVersionNow(OperationContext* opCtx);

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
    StatusWith<ShardEndpoint> _targetShardKey(const BSONObj& shardKey,
                                              const BSONObj& collation,
                                              long long estDataSize) const;

    // Full namespace of the collection for this targeter
    const NamespaceString _nss;

    // Stores whether we need to check the remote server on refresh
    bool _needsTargetingRefresh;

    // The latest loaded routing cache entry
    boost::optional<CachedCollectionRoutingInfo> _routingInfo;

    // Set to the epoch of the namespace we are targeting. If we ever refresh the catalog cache and
    // find a new epoch, we immediately throw a StaleEpoch exception.
    boost::optional<OID> _targetEpoch;

    // Map of shard->remote shard version reported from stale errors
    StaleShardVersionMap _remoteShardVersions;

    // remote db version reported from stale errors
    boost::optional<DatabaseVersion> _remoteDbVersion;
};

}  // namespace mongo
