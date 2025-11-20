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

#include "mongo/db/global_catalog/router_role_api/router_role.h"
#include "mongo/db/local_catalog/shard_role_api/shard_role_loop.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/sharding_environment/grid.h"
#include "mongo/db/topology/sharding_state.h"
#include "mongo/s/transaction_router.h"
#include "mongo/util/modules.h"

namespace mongo {
/**
 * Function which produces an vector of 'ScopedShardRole' objects for the namespaces in 'nssList'
 * using the routing information in 'criMap'.
 */
inline std::vector<ScopedSetShardRole> createScopedShardRoles(
    OperationContext* opCtx,
    const stdx::unordered_map<NamespaceString, CollectionRoutingInfo>& criMap,
    const std::vector<NamespaceString>& nssList) {
    std::vector<ScopedSetShardRole> scopedShardRoles;
    scopedShardRoles.reserve(nssList.size());
    const auto myShardId = ShardingState::get(opCtx)->shardId();
    const auto placementConflictTime = [&] {
        const auto txnRouter = TransactionRouter::get(opCtx);
        return txnRouter && opCtx->inMultiDocumentTransaction()
            ? txnRouter.getPlacementConflictTime()
            : boost::none;
    }();

    for (const auto& nss : nssList) {
        const auto nssCri = criMap.find(nss);
        tassert(8322004,
                "Must be an entry in criMap for namespace " + nss.toStringForErrorMsg(),
                nssCri != criMap.end());

        bool isTracked = nssCri->second.hasRoutingTable();

        auto shardVersion = [&] {
            auto sv =
                isTracked ? nssCri->second.getShardVersion(myShardId) : ShardVersion::UNTRACKED();
            if (placementConflictTime) {
                sv.setPlacementConflictTime(*placementConflictTime);
            }
            return sv;
        }();

        // For UNTRACKED collections, the collection will only be potentially considered local if
        // this shard is the dbPrimary shard.
        //
        // If the routing info tells this shard is, then attach the DatabaseVersion to validate
        // that. For the opposite case, where the routing info says that this shard is not the
        // dbPrimary shard, we cannot attach the DatabaseVersion because the protocol does not allow
        // a way to express that, so it won't be validated. If the routing info was stale, this will
        // potentially result in executing a correct but sub-optimal query plan (only this time,
        // because the next executions will see an updated routing info as a side effect of this
        // execution having targeted a "remotely" and therefore will choose the optimal plan).
        const bool isDbPrimaryShard = nssCri->second.getDbPrimaryShardId() == myShardId;
        auto dbVersion = !isTracked && isDbPrimaryShard
            ? boost::optional<DatabaseVersion>(nssCri->second.getDbVersion())
            : boost::none;

        if (placementConflictTime && dbVersion) {
            dbVersion->setPlacementConflictTime(*placementConflictTime);
        }

        try {
            scopedShardRoles.emplace_back(opCtx, nss, shardVersion, dbVersion);
        } catch (const ExceptionFor<ErrorCodes::IllegalChangeToExpectedDatabaseVersion>&) {
            // Only one can be correct. Check css and the new one.
            const auto scopedDss = DatabaseShardingState::acquire(opCtx, nss.dbName());
            scopedDss->checkDbVersionOrThrow(opCtx);
            scopedDss->checkDbVersionOrThrow(opCtx, *dbVersion);
            MONGO_UNREACHABLE_TASSERT(10825600);
        }
    }
    return scopedShardRoles;
}

/**
 * Helper that constructs a ShardRole CollectionAcquisition using 'initAutoGetFn'.
 * Returns whether any namespaces in 'secondaryExecNssList' are non local.
 */
template <typename F>
bool initializeAutoGet(OperationContext* opCtx,
                       const NamespaceString& nss,
                       const std::vector<NamespaceStringOrUUID>& secondaryExecNssList,
                       F&& initAutoGetFn) {
    bool isAnySecondaryCollectionNotLocal = false;
    auto* grid = Grid::get(opCtx->getServiceContext());
    if (grid->isInitialized() && grid->isShardingInitialized() &&
        OperationShardingState::get(opCtx).isComingFromRouter(opCtx)) {
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

        sharding::router::MultiCollectionRouter multiCollectionRouter(opCtx->getServiceContext(),
                                                                      secondaryExecNssListJustNss);
        multiCollectionRouter.route(
            opCtx,
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

                shard_role_loop::withStaleShardRetry(opCtx, [&]() {
                    auto scopedShardRoles =
                        createScopedShardRoles(opCtx, criMap, secondaryExecNssListJustNss);
                    initAutoGetFn();
                });
            });
    } else {
        initAutoGetFn();
    }

    return isAnySecondaryCollectionNotLocal;
}
}  // namespace mongo
