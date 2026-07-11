// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/shard_role/initialize_auto_get_helper.h"

#include "mongo/db/topology/sharding_state.h"

namespace mongo {

std::pair<ShardVersion, boost::optional<DatabaseVersion>> resolveShardRoleVersions(
    OperationContext* opCtx,
    const CollectionRoutingInfo& cri,
    const ShardId& myShardId,
    const boost::optional<LogicalTime>& placementConflictTime) {
    const bool isTracked = cri.hasRoutingTable();

    auto shardVersion = isTracked ? cri.getShardVersion(myShardId) : ShardVersion::UNTRACKED();
    if (placementConflictTime) {
        // TODO (SERVER-115178): Remove placementConflictTime stamping once v9.0 branches out.
        shardVersion.setPlacementConflictTime_DEPRECATED(*placementConflictTime);
    }

    // For UNTRACKED collections, the collection will only be potentially considered local if this
    // shard is the dbPrimary shard.
    //
    // If the routing info tells this shard is, then attach the DatabaseVersion to validate that.
    // For the opposite case, where the routing info says that this shard is not the dbPrimary
    // shard, we cannot attach the DatabaseVersion because the protocol does not allow a way to
    // express that, so it won't be validated. If the routing info was stale, this will potentially
    // result in executing a correct but sub-optimal query plan (only this time, because the next
    // executions will see an updated routing info as a side effect of this execution having
    // targeted a "remotely" and therefore will choose the optimal plan).
    const bool isDbPrimaryShard = cri.getDbPrimaryShardId() == myShardId;
    auto dbVersion = !isTracked && isDbPrimaryShard
        ? boost::optional<DatabaseVersion>(cri.getDbVersion())
        : boost::none;
    if (placementConflictTime && dbVersion) {
        // TODO (SERVER-115178): Remove placementConflictTime stamping once v9.0 branches out.
        dbVersion->setPlacementConflictTime_DEPRECATED(*placementConflictTime);
    }

    return {std::move(shardVersion), std::move(dbVersion)};
}

std::vector<ScopedSetShardRole> createScopedShardRoles(
    OperationContext* opCtx,
    const stdx::unordered_map<NamespaceString, CollectionRoutingInfo>& criMap,
    const std::vector<NamespaceString>& nssList,
    const boost::optional<LogicalTime>& placementConflictTime,
    const NamespaceString& mainNss) {
    std::vector<ScopedSetShardRole> scopedShardRoles;
    scopedShardRoles.reserve(nssList.size());
    const auto myShardId = ShardingState::get(opCtx)->shardId();

    for (const auto& nss : nssList) {
        const auto nssCri = criMap.find(nss);
        tassert(8322004,
                "Must be an entry in criMap for namespace " + nss.toStringForErrorMsg(),
                nssCri != criMap.end());

        auto [shardVersion, dbVersion] =
            resolveShardRoleVersions(opCtx, nssCri->second, myShardId, placementConflictTime);

        try {
            // Versioning checks are safely disabled for UNTRACKED collections when no
            // DatabaseVersion is set, as the resulting query plan remains correct in case of stale
            // routing info. For more details, see comment above regarding UNTRACKED collections.
            const auto disableCheckVersioningCorrectness =
                !dbVersion && shardVersion == ShardVersion::UNTRACKED() ? true : false;
            scopedShardRoles.emplace_back(
                opCtx, nss, shardVersion, dbVersion, disableCheckVersioningCorrectness);
        } catch (const ExceptionFor<ErrorCodes::IllegalChangeToExpectedDatabaseVersion>&) {
            // Only one can be correct. Check css and the new one.
            const auto scopedDss = DatabaseShardingState::acquire(opCtx, nss.dbName());
            scopedDss->checkDbVersionOrThrow(opCtx);
            scopedDss->checkDbVersionOrThrow(opCtx, *dbVersion);
            MONGO_UNREACHABLE_TASSERT(10825600);
        } catch (const ExceptionFor<ErrorCodes::IllegalChangeToExpectedShardVersion>&) {
            // During FCV transitions, a timeseries collection can concurrently be upgraded to
            // viewless. If the command handler set the shard version for the buckets namespace and
            // the CRI now shows a different version (because a concurrent FCV upgrade switched the
            // tracked namespace from the buckets namespace to the collection namespace), convert to
            // a retryable error. If not, throw the original error.
            // TODO SERVER-117477 remove this logic once 9.0 becomes last LTS and all timeseries
            // collection are viewless.
            // (Ignore FCV check): This code is backward compatible.
            if (gFeatureFlagCreateViewlessTimeseriesCollections.isEnabledAndIgnoreFCVUnsafe() &&
                mainNss.isTimeseriesBucketsCollection()) {
                uasserted(ErrorCodes::InterruptedDueToTimeseriesUpgradeDowngrade,
                          fmt::format(
                              "Operation on collection '{}' was interrupted due to a time-series "
                              "metadata change during aggregation resolution. Retry the operation.",
                              nss.toStringForErrorMsg()));
            }
            throw;
        }
    }
    return scopedShardRoles;
}

}  // namespace mongo
