/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/db/router_role/routing_cache/catalog_cache.h"
#include "mongo/s/write_ops/write_command_ref.h"
#include "mongo/util/modules.h"

#include <vector>

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {

struct MONGO_MOD_PUBLIC TargetOpResult {
    TargetOpResult() = default;

    TargetOpResult(std::vector<ShardEndpoint> endpoints) : endpoints(std::move(endpoints)) {}

    TargetOpResult(std::vector<ShardEndpoint> endpoints,
                   bool useTwoPhaseWriteProtocol,
                   bool isNonTargetedRetryableWriteWithId)
        : endpoints(std::move(endpoints)),
          useTwoPhaseWriteProtocol(useTwoPhaseWriteProtocol),
          isNonTargetedRetryableWriteWithId(isNonTargetedRetryableWriteWithId) {}

    std::vector<ShardEndpoint> endpoints;
    bool useTwoPhaseWriteProtocol = false;
    bool isNonTargetedRetryableWriteWithId = false;
};

MONGO_MOD_PUBLIC BSONObj extractBucketsShardKeyFromTimeseriesDoc(
    const BSONObj& doc, const ShardKeyPattern& pattern, const TimeseriesOptions& timeseriesOptions);

MONGO_MOD_PUBLIC bool isExactIdQuery(OperationContext* opCtx,
                                     const NamespaceString& nss,
                                     const BSONObj& query,
                                     const BSONObj& collation,
                                     bool isSharded,
                                     const CollatorInterface* defaultCollator);

MONGO_MOD_PUBLIC ShardEndpoint targetInsert(OperationContext* opCtx,
                                            const NamespaceString& nss,
                                            const CollectionRoutingInfo& cri,
                                            bool isViewfulTimeseries,
                                            const BSONObj& doc);

/**
 * Attempts to target an update request by shard key and returns a vector of shards to target.
 */
MONGO_MOD_PUBLIC TargetOpResult targetUpdate(OperationContext* opCtx,
                                             const NamespaceString& nss,
                                             const CollectionRoutingInfo& cri,
                                             bool isViewfulTimeseries,
                                             const WriteOpRef& itemRef);

/**
 * Attempts to target an delete request by shard key and returns a vector of shards to target.
 */
MONGO_MOD_PUBLIC TargetOpResult targetDelete(OperationContext* opCtx,
                                             const NamespaceString& nss,
                                             const CollectionRoutingInfo& cri,
                                             bool isViewfulTimeseries,
                                             const WriteOpRef& itemRef);

MONGO_MOD_PUBLIC std::vector<ShardEndpoint> targetAllShards(OperationContext* opCtx,
                                                            const CollectionRoutingInfo& cri);

/**
 * Returns a CanonicalQuery if parsing succeeds.
 *
 * Returns !OK with message if query could not be canonicalized.
 *
 * If 'collation' is empty, we use the collection default collation for targeting.
 */
StatusWith<std::unique_ptr<CanonicalQuery>> canonicalizeFindQuery(
    OperationContext* opCtx,
    boost::intrusive_ptr<mongo::ExpressionContext> expCtx,
    const NamespaceString& nss,
    const BSONObj& query,
    const BSONObj& collation,
    const CollectionRoutingInfo& cri);

/**
 * Returns a vector of ShardEndpoints for a potentially multi-shard query.
 *
 * Uses the collation specified on the CanonicalQuery for targeting. If there is no query
 * collation, uses the collection default. If 'bypassIsFieldHashedCheck' is true, it skips
 * checking if the shard key was hashed and assumes that any non-collatable shard key was not
 * hashed from a collatable type.
 *
 * Returns !OK with message if query could not be targeted.
 */
StatusWith<std::vector<ShardEndpoint>> targetQuery(const CollectionRoutingInfo& cri,
                                                   const CanonicalQuery& query,
                                                   bool bypassIsFieldHashedCheck);

/**
 * Returns a vector of ShardEndpoints for a potentially multi-shard query.
 *
 * This method is an alternative to targetQuery() that is intended to be used for multi:true
 * upsert queries only.
 *
 * This method attempts to extract a shard key from the CanonicalQuery and then attempts target
 * a single shard using this shard key.
 *
 * Returns !OK with message if a shard key could not be extracted or a single shard could not
 * be targeted due to collation.
 */
StatusWith<std::vector<ShardEndpoint>> targetQueryForMultiUpsert(const CollectionRoutingInfo& cri,
                                                                 const CanonicalQuery& query);

/**
 * Returns a ShardEndpoint for an exact shard key query.
 *
 * Also has the side effect of updating the chunks stats with an estimate of the amount of
 * data targeted at this shard key.
 *
 * If 'collation' is empty, we use the collection default collation for targeting.
 */
StatusWith<ShardEndpoint> targetShardKey(const CollectionRoutingInfo& cri,
                                         const BSONObj& shardKey,
                                         const BSONObj& collation);

/**
 * Returns the number of shards on which the collection has any chunks.
 *
 * To be only used for logging/metrics which do not need to be always correct. The returned
 * value may be incorrect when this targeter is at point-in-time (it will reflect the 'latest'
 * number of shards, rather than the one at the point-in-time).
 */
MONGO_MOD_PUBLIC int getAproxNShardsOwningChunks(const CollectionRoutingInfo& cri);

MONGO_MOD_PUBLIC bool isTrackedTimeSeriesBucketsNamespace(const CollectionRoutingInfo& cri);

MONGO_MOD_PUBLIC bool isTrackedTimeSeriesNamespace(const CollectionRoutingInfo& cri);

}  // namespace mongo
