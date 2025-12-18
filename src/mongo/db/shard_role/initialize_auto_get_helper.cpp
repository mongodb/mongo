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

#include "mongo/db/shard_role/initialize_auto_get_helper.h"

#include "mongo/db/topology/sharding_state.h"

namespace mongo {

std::vector<ScopedSetShardRole> createScopedShardRoles(
    OperationContext* opCtx,
    const stdx::unordered_map<NamespaceString, CollectionRoutingInfo>& criMap,
    const std::vector<NamespaceString>& nssList,
    const boost::optional<LogicalTime>& placementConflictTime) {
    std::vector<ScopedSetShardRole> scopedShardRoles;
    scopedShardRoles.reserve(nssList.size());
    const auto myShardId = ShardingState::get(opCtx)->shardId();

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
                sv.setPlacementConflictTime_DEPRECATED(*placementConflictTime);
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
            dbVersion->setPlacementConflictTime_DEPRECATED(*placementConflictTime);
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

}  // namespace mongo
