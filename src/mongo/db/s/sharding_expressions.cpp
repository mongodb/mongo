/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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


#include "mongo/platform/basic.h"

#include "mongo/db/s/sharding_expressions.h"

#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/expression_visitor.h"
#include "mongo/db/pipeline/variables.h"
#include "mongo/db/s/sharding_state.h"
#include "mongo/s/grid.h"
#include "mongo/s/is_mongos.h"


namespace mongo {

Value ExpressionInternalOwningShard::evaluate(const Document& root, Variables* variables) const {
    // TODO SERVER-71519: Add support for handling stale exception from mongos with
    // enableFinerGrainedCatalogCacheRefresh.
    uassert(6868600, "$_internalOwningShard is currently not supported on mongos", !isMongos());

    Value input = _children[0]->evaluate(root, variables);
    if (input.nullish()) {
        return Value(BSONNULL);
    }

    // Retrieve the values from the incoming document.
    NamespaceString ns(getExpressionContext()->ns.tenantId(), input["ns"_sd].getStringData());
    const auto shardVersionObj = input["shardVersion"_sd].getDocument().toBson();
    const auto shardVersion = ShardVersion::parse(BSON("" << shardVersionObj).firstElement());
    const auto shardKeyVal = input["shardKeyVal"_sd].getDocument().toBson();

    // Get the 'chunkManager' from the catalog cache.
    auto opCtx = getExpressionContext()->opCtx;
    const auto catalogCache = Grid::get(opCtx)->catalogCache();
    uassert(6868602,
            "$_internalOwningShard expression only makes sense in sharded environment",
            catalogCache);

    // Setting 'allowLocks' to true when evaluating on mongod, as otherwise an invariant is thrown.
    // We can safely set it to true as there is no risk of deadlock, because the code still throws
    // when a refresh would actually need to take place.
    const auto chunkManager =
        uassertStatusOK(catalogCache->getCollectionRoutingInfo(opCtx, ns, true /* allowLocks */))
            .cm;

    // Invalidate catalog cache if the chunk manager version is stale.
    if (chunkManager.getVersion().isOlderThan(shardVersion.placementVersion())) {
        boost::optional<CollectionIndexes> collIndexes;
        ShardVersion currentShardVersion(chunkManager.getVersion(), collIndexes);
        uasserted(StaleConfigInfo(ns,
                                  currentShardVersion,
                                  boost::none /* wanted */,
                                  ShardingState::get(opCtx)->shardId()),
                  str::stream()
                      << "Sharding information of collection " << ns
                      << " is currently stale and needs to be recovered from the config server");
    }

    // Retrieve the shard id for the given shard key value.
    std::set<ShardId> shardIds;
    chunkManager.getShardIdsForRange(shardKeyVal, shardKeyVal, &shardIds);
    uassert(6868601, "The value should belong to exactly one ShardId", shardIds.size() == 1);
    const auto shardId = *(shardIds.begin());
    return Value(shardId.toString());
}

REGISTER_STABLE_EXPRESSION(_internalOwningShard, ExpressionInternalOwningShard::parse);

};  // namespace mongo
