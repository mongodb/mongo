// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

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

struct [[MONGO_MOD_PUBLIC]] TargetOpResult {
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

[[MONGO_MOD_PUBLIC]] BSONObj extractBucketsShardKeyFromTimeseriesDoc(
    const BSONObj& doc, const ShardKeyPattern& pattern, const TimeseriesOptions& timeseriesOptions);

[[MONGO_MOD_PUBLIC]] bool isExactIdQuery(OperationContext* opCtx,
                                         const NamespaceString& nss,
                                         const BSONObj& query,
                                         const BSONObj& collation,
                                         bool isSharded,
                                         const CollatorInterface* defaultCollator);

[[MONGO_MOD_PUBLIC]] ShardEndpoint targetInsert(OperationContext* opCtx,
                                                const NamespaceString& nss,
                                                const CollectionRoutingInfo& cri,
                                                bool isViewfulTimeseries,
                                                const BSONObj& doc);

/**
 * Attempts to target an update request by shard key and returns a vector of shards to target.
 */
[[MONGO_MOD_PUBLIC]] TargetOpResult targetUpdate(OperationContext* opCtx,
                                                 const NamespaceString& nss,
                                                 const CollectionRoutingInfo& cri,
                                                 bool isViewfulTimeseries,
                                                 const WriteOpRef& itemRef);

/**
 * Attempts to target an delete request by shard key and returns a vector of shards to target.
 */
[[MONGO_MOD_PUBLIC]] TargetOpResult targetDelete(OperationContext* opCtx,
                                                 const NamespaceString& nss,
                                                 const CollectionRoutingInfo& cri,
                                                 bool isViewfulTimeseries,
                                                 const WriteOpRef& itemRef);

[[MONGO_MOD_PUBLIC]] std::vector<ShardEndpoint> targetAllShards(OperationContext* opCtx,
                                                                const CollectionRoutingInfo& cri);

}  // namespace mongo
