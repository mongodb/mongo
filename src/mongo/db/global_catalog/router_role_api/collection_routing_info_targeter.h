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

#include "mongo/base/status_with.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobj_comparator_interface.h"
#include "mongo/bson/oid.h"
#include "mongo/bson/simple_bsonobj_comparator.h"
#include "mongo/db/global_catalog/catalog_cache/catalog_cache.h"
#include "mongo/db/global_catalog/chunk_manager.h"
#include "mongo/db/global_catalog/ddl/cannot_implicitly_create_collection_info.h"
#include "mongo/db/global_catalog/router_role_api/ns_targeter.h"
#include "mongo/db/global_catalog/router_role_api/router_role.h"
#include "mongo/db/global_catalog/shard_key_pattern.h"
#include "mongo/db/global_catalog/type_chunk.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/db/sharding_environment/shard_id.h"
#include "mongo/db/timeseries/timeseries_gen.h"
#include "mongo/db/versioning_protocol/chunk_version.h"
#include "mongo/db/versioning_protocol/stale_exception.h"
#include "mongo/s/write_ops/batched_command_request.h"

#include <map>
#include <set>
#include <vector>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {

/**
 * NSTargeter based on a CollectionRoutingInfo implementation. Wraps all exception codepaths and
 * returns NamespaceNotFound status on applicable failures.
 *
 * Must be initialized before use, and initialization may fail.
 */
class CollectionRoutingInfoTargeter : public NSTargeter {
public:
    enum class LastErrorType {
        kCouldNotTarget,
        kStaleShardVersion,
        kStaleDbVersion,
        kCannotImplicitlyCreateCollection
    };

    /**
     * Initializes the targeter with the latest routing information for the namespace, which means
     * it may have to block and load information from the config server.
     *
     * If 'nss' is a tracked time-series collection, replaces this value with namespace string of a
     * time-series buckets collection.
     *
     * If 'expectedEpoch' is specified, the targeter will throws 'StaleEpoch' exception if the epoch
     * for 'nss' ever becomes different from 'expectedEpoch'. Otherwise, the targeter will continue
     * targeting even if the collection gets dropped and recreated.
     */
    CollectionRoutingInfoTargeter(OperationContext* opCtx,
                                  const NamespaceString& nss,
                                  boost::optional<OID> expectedEpoch = boost::none);

    /* Initializes the targeter with a custom CollectionRoutingInfo cri, in order to support
     * using a custom (synthetic) routing table */
    CollectionRoutingInfoTargeter(const NamespaceString& nss, const CollectionRoutingInfo& cri);

    const NamespaceString& getNS() const override;

    ShardEndpoint targetInsert(OperationContext* opCtx, const BSONObj& doc) const override;

    /**
     * Attempts to target an update request by shard key and returns a vector of shards to target.
     */
    TargetingResult targetUpdate(OperationContext* opCtx,
                                 const BatchItemRef& itemRef) const override;

    /**
     * Attempts to target an delete request by shard key and returns a vector of shards to target.
     */
    TargetingResult targetDelete(OperationContext* opCtx,
                                 const BatchItemRef& itemRef) const override;

    std::vector<ShardEndpoint> targetAllShards(OperationContext* opCtx) const override;

    void noteCouldNotTarget() override;

    void noteStaleCollVersionResponse(OperationContext* opCtx,
                                      const StaleConfigInfo& staleInfo) override;

    void noteStaleDbVersionResponse(OperationContext* opCtx,
                                    const StaleDbRoutingVersion& staleInfo) override;

    /**
     * Returns if _lastError is StaleConfig type.
     */
    bool hasStaleShardResponse() override;


    void noteCannotImplicitlyCreateCollectionResponse(
        OperationContext* optCtx, const CannotImplicitlyCreateCollectionInfo& createInfo) override;

    /**
     * Replaces the targeting information with the latest information from the cache.  If this
     * information is stale WRT the noted stale responses or a remote refresh is needed due
     * to a targeting failure, will contact the config servers to reload the metadata.
     *
     * Return true if the metadata was different after this reload.
     *
     * Also see NSTargeter::refreshIfNeeded().
     */
    bool refreshIfNeeded(OperationContext* opCtx) override;

    /**
     * Creates a collection if there was a prior CannotImplicitlyCreateCollection error thrown.
     *
     * Return true if a collection was created and false if the collection already existed, throwing
     * on any errors.
     *
     * Also see NSTargeter::createCollectionIfNeeded().
     */
    bool createCollectionIfNeeded(OperationContext* opCtx) override;

    /**
     * Returns the number of shards on which the collection has any chunks.
     *
     * To be only used for logging/metrics which do not need to be always correct. The returned
     * value may be incorrect when this targeter is at point-in-time (it will reflect the 'latest'
     * number of shards, rather than the one at the point-in-time).
     */
    int getAproxNShardsOwningChunks() const override;

    bool isTargetedCollectionSharded() const override;

    bool isTrackedTimeSeriesBucketsNamespace() const override;

    bool isTrackedTimeSeriesNamespace() const override;

    bool timeseriesNamespaceNeedsRewrite(const NamespaceString& nss) const;

    RoutingContext& getRoutingCtx() const;

    const CollectionRoutingInfo& getRoutingInfo() const;

    static BSONObj extractBucketsShardKeyFromTimeseriesDoc(
        const BSONObj& doc,
        const ShardKeyPattern& pattern,
        const TimeseriesOptions& timeseriesOptions);

    /**
     * This returns "does the query have an _id field" and "is the _id field querying for a direct
     * value like _id : 3 and not _id : { $gt : 3 }"
     *
     * If the query does not use the collection default collation, the _id field cannot contain
     * strings, objects, or arrays.
     *
     * Ex: { _id : 1 } => true
     *     { foo : <anything>, _id : 1 } => true
     *     { _id : { $lt : 30 } } => false
     *     { foo : <anything> } => false
     */
    static bool isExactIdQuery(OperationContext* opCtx,
                               const NamespaceString& nss,
                               const BSONObj& query,
                               const BSONObj& collation,
                               const ChunkManager& cm);

private:
    // Maximum number of database creation attempts, which may fail due to a concurrent drop.
    static const size_t kMaxDatabaseCreationAttempts;

    std::unique_ptr<RoutingContext> _init(OperationContext* opCtx, bool refresh);

    /**
     * Returns a CanonicalQuery if parsing succeeds.
     *
     * Returns !OK with message if query could not be canonicalized.
     *
     * If 'collation' is empty, we use the collection default collation for targeting.
     */
    static StatusWith<std::unique_ptr<CanonicalQuery>> _canonicalize(
        OperationContext* opCtx,
        boost::intrusive_ptr<mongo::ExpressionContext> expCtx,
        const NamespaceString& nss,
        const BSONObj& query,
        const BSONObj& collation,
        const ChunkManager& cm);

    static bool _isExactIdQuery(const CanonicalQuery& query, const ChunkManager& cm);

    bool _isTimeseriesLogicalOperation(OperationContext* opCtx) const;

    /**
     * Returns a vector of ShardEndpoints for a potentially multi-shard query.
     *
     * Uses the collation specified on the CanonicalQuery for targeting. If there is no query
     * collation, uses the collection default.
     *
     * Returns !OK with message if query could not be targeted.
     */
    StatusWith<std::vector<ShardEndpoint>> _targetQuery(const CanonicalQuery& query) const;

    /**
     * Returns a vector of ShardEndpoints for a potentially multi-shard query.
     *
     * This method is an alternative to _targetQuery that is intended to be used for multi:true
     * upsert queries only.
     *
     * This method attempts to extract a shard key from the CanonicalQuery and then attempts target
     * a single shard using this shard key.
     *
     * Returns !OK with message if a shard key could not be extracted or a single shard could not
     * be targeted due to collation.
     */
    StatusWith<std::vector<ShardEndpoint>> _targetQueryForMultiUpsert(
        const CanonicalQuery& query) const;

    /**
     * Returns a ShardEndpoint for an exact shard key query.
     *
     * Also has the side effect of updating the chunks stats with an estimate of the amount of
     * data targeted at this shard key.
     *
     * If 'collation' is empty, we use the collection default collation for targeting.
     */
    StatusWith<ShardEndpoint> _targetShardKey(const BSONObj& shardKey,
                                              const BSONObj& collation) const;

    // Full namespace of the collection for this targeter
    NamespaceString _nss;

    // Set to true when the request was originally targeting the view of a tracked timeseries
    // collection and the namespace got converted to the underlying buckets collection. Note: this
    // will only be true if the timeseries collection is tracked in the global catalog.
    //
    // TODO SERVER-106874 remove this parameter once 9.0 becomes last LTS. By then we will only have
    // viewless timeseries so nss conversion will not be needed anymore
    bool _nssConvertedToTimeseriesBuckets = false;

    // Stores last error occurred
    boost::optional<LastErrorType> _lastError;

    // Set to the epoch of the namespace we are targeting. If we ever refresh the catalog cache
    // and find a new epoch, we immediately throw a StaleEpoch exception.
    boost::optional<OID> _targetEpoch;

    std::unique_ptr<RoutingContext> _routingCtx;

    // The latest loaded routing cache entry
    CollectionRoutingInfo _cri;
};

}  // namespace mongo
