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

#include "mongo/db/logical_time.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/router_role/router_role.h"
#include "mongo/db/shard_role/shard_catalog/operation_sharding_state.h"
#include "mongo/db/shard_role/shard_role_loop.h"
#include "mongo/db/sharding_environment/grid.h"
#include "mongo/db/sharding_environment/shard_id.h"
#include "mongo/s/transaction_router.h"
#include "mongo/util/modules.h"

#include <utility>

#include <boost/optional/optional.hpp>

namespace mongo {

/**
 * Derives the (ShardVersion, DatabaseVersion) pair that a ScopedSetShardRole on 'myShardId' should
 * carry for the collection described by 'cri'. This is the single source of truth for the
 * tracked/untracked rules:
 *   - tracked collection:   ShardVersion = cri.getShardVersion(myShardId), no DatabaseVersion.
 *   - untracked collection: ShardVersion = UNTRACKED(); DatabaseVersion attached only when this
 *                           shard is the dbPrimary (an untracked collection only lives there).
 *
 * Inside a multi-document transaction, 'placementConflictTime' is stamped onto both versions so the
 * receiving shard can detect a placement change against the transaction's read snapshot. Outside a
 * transaction it is boost::none and nothing is stamped.
 */
MONGO_MOD_PUBLIC
std::pair<ShardVersion, boost::optional<DatabaseVersion>> resolveShardRoleVersions(
    OperationContext* opCtx,
    const CollectionRoutingInfo& cri,
    const ShardId& myShardId,
    const boost::optional<LogicalTime>& placementConflictTime);

/**
 * Function which produces an vector of 'ScopedShardRole' objects for the namespaces in 'nssList'
 * using the routing information in 'criMap'.
 */
MONGO_MOD_PUBLIC
std::vector<ScopedSetShardRole> createScopedShardRoles(
    OperationContext* opCtx,
    const stdx::unordered_map<NamespaceString, CollectionRoutingInfo>& criMap,
    const std::vector<NamespaceString>& nssList,
    const boost::optional<LogicalTime>& placementConflictTime,
    const NamespaceString& mainNss);

/**
 * Helper that constructs a ShardRole CollectionAcquisition using 'initAutoGetFn'.
 * Returns whether any namespaces in 'secondaryExecNssList' are non local.
 */
template <typename F>
MONGO_MOD_PUBLIC bool initializeAutoGet(
    OperationContext* opCtx,
    const NamespaceString& nss,
    const std::vector<NamespaceStringOrUUID>& secondaryExecNssList,
    F&& initAutoGetFn) {
    bool isAnySecondaryCollectionNotLocal = false;
    auto* grid = Grid::get(opCtx->getServiceContext());
    if (grid->isInitialized() && grid->isShardingInitialized() &&
        OperationShardingState::get(opCtx).isVersioned(opCtx, nss)) {
        std::vector<NamespaceString> secondaryExecNssListJustNss;
        for (const auto& nsOrUuid : secondaryExecNssList) {
            tassert(8322005, "Expected NamespaceString, not UUID", nsOrUuid.isNamespaceString());
            auto asNss = nsOrUuid.nss();
            // Do not add the main nss to the list of secondary namespaces, as doing so may
            // result in trying to set the shard version twice for the same collection.
            if (asNss != nss) {
                secondaryExecNssListJustNss.emplace_back(asNss);
            }
        }

        sharding::router::MultiCollectionRouter multiCollectionRouter(opCtx,
                                                                      secondaryExecNssListJustNss);
        multiCollectionRouter.route(
            "initializeAutoGet",
            [&](OperationContext* opCtx,
                const stdx::unordered_map<NamespaceString, CollectionRoutingInfo>& criMap) {
                // Figure out if all of 'secondaryExecNssListJustNss' are local. This is useful
                // because we can pushdown $lookup to SBE if:
                // - All secondary collections are tracked and local to this shard.
                // - All secondary collections are untracked and this shard is the primary shard.
                // Note that it is implied that the main collection is local to this shard. This
                // is because if the main collection is not local to this shard, we would have to
                // read remotely, which would inhibit the pushdown of $lookup to SBE.
                isAnySecondaryCollectionNotLocal =
                    multiCollectionRouter.isAnyCollectionNotLocal(opCtx, criMap);

                const auto placementConflictTime = [&] {
                    const auto txnRouter = TransactionRouter::get(opCtx);
                    return txnRouter && opCtx->inMultiDocumentTransaction()
                        ? txnRouter.getPlacementConflictTime()
                        : boost::none;
                }();

                shard_role_loop::withStaleShardRetry(opCtx, [&]() {
                    auto scopedShardRoles = createScopedShardRoles(
                        opCtx, criMap, secondaryExecNssListJustNss, placementConflictTime, nss);
                    initAutoGetFn();
                });
            });
    } else {
        initAutoGetFn();
    }

    return isAnySecondaryCollectionNotLocal;
}

}  // namespace mongo
