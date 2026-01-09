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

}  // namespace mongo
