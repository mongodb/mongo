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

#include "mongo/db/catalog/catalog_helper.h"

#include "mongo/db/catalog/database_holder.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/catalog_shard_feature_flag_gen.h"
#include "mongo/db/s/database_sharding_state.h"
#include "mongo/db/s/operation_sharding_state.h"
#include "mongo/db/s/sharding_migration_critical_section.h"
#include "mongo/db/s/sharding_state.h"
#include "mongo/s/stale_exception.h"

namespace mongo::catalog_helper {
namespace {
MONGO_FAIL_POINT_DEFINE(setAutoGetCollectionWait);

/**
 * Defines sorting order for NamespaceStrings based on what their ResourceId would be for locking.
 */
struct ResourceIdNssComparator {
    bool operator()(const NamespaceString& lhs, const NamespaceString& rhs) const {
        return ResourceId(RESOURCE_COLLECTION, lhs) < ResourceId(RESOURCE_COLLECTION, rhs);
    }
};
}  // namespace

void assertMatchingDbVersion(OperationContext* opCtx, const StringData& dbName) {
    const auto receivedVersion = OperationShardingState::get(opCtx).getDbVersion(dbName);
    if (!receivedVersion) {
        return;
    }

    {
        auto scopedDss = DatabaseShardingState::acquire(opCtx, dbName, DSSAcquisitionMode::kShared);
        const auto critSecSignal = scopedDss->getCriticalSectionSignal(
            opCtx->lockState()->isWriteLocked() ? ShardingMigrationCriticalSection::kWrite
                                                : ShardingMigrationCriticalSection::kRead);
        uassert(
            StaleDbRoutingVersion(dbName.toString(), *receivedVersion, boost::none, critSecSignal),
            str::stream() << "The critical section for the database " << dbName
                          << " is acquired with reason: " << scopedDss->getCriticalSectionReason(),
            !critSecSignal);
    }

    const auto wantedVersion = DatabaseHolder::get(opCtx)->getDbVersion(opCtx, dbName);
    uassert(StaleDbRoutingVersion(dbName.toString(), *receivedVersion, boost::none),
            str::stream() << "No cached info for the database " << dbName,
            wantedVersion);

    uassert(StaleDbRoutingVersion(dbName.toString(), *receivedVersion, *wantedVersion),
            str::stream() << "Version mismatch for the database " << dbName,
            *receivedVersion == *wantedVersion);
}

void assertIsPrimaryShardForDb(OperationContext* opCtx, const StringData& dbName) {
    if (dbName == NamespaceString::kConfigDb) {
        // TODO SERVER-72488: Include the admin database.
        invariant(gFeatureFlagCatalogShard.isEnabledAndIgnoreFCV());
        invariant(serverGlobalParams.clusterRole == ClusterRole::ConfigServer);
        return;
    }

    uassert(ErrorCodes::IllegalOperation,
            str::stream() << "Received request without the version for the database " << dbName,
            OperationShardingState::get(opCtx).hasDbVersion());

    // Recover the database's information if necessary (not cached or not matching).
    AutoGetDb autoDb(opCtx, dbName, MODE_IS);
    invariant(autoDb.getDb());

    const auto primaryShardId = DatabaseHolder::get(opCtx)->getDbPrimary(opCtx, dbName).value();
    const auto thisShardId = ShardingState::get(opCtx)->shardId();
    uassert(ErrorCodes::IllegalOperation,
            str::stream() << "This is not the primary shard for the database " << dbName
                          << ". Expected: " << primaryShardId << " Actual: " << thisShardId,
            primaryShardId == thisShardId);
}

void acquireCollectionLocksInResourceIdOrder(
    OperationContext* opCtx,
    const NamespaceStringOrUUID& nsOrUUID,
    LockMode modeColl,
    Date_t deadline,
    const std::vector<NamespaceStringOrUUID>& secondaryNssOrUUIDs,
    std::vector<CollectionNamespaceOrUUIDLock>* collLocks) {
    invariant(collLocks->empty());
    auto catalog = CollectionCatalog::get(opCtx);

    // Use a set so that we can easily dedupe namespaces to avoid locking the same collection twice.
    std::set<NamespaceString, ResourceIdNssComparator> temp;
    std::set<NamespaceString, ResourceIdNssComparator> verifyTemp;
    do {
        // Clear the data structures when/if we loop more than once.
        collLocks->clear();
        temp.clear();
        verifyTemp.clear();

        // Create a single set with all the resolved namespaces sorted by ascending
        // ResourceId(RESOURCE_COLLECTION, nss).
        temp.insert(catalog->resolveNamespaceStringOrUUID(opCtx, nsOrUUID));
        for (const auto& secondaryNssOrUUID : secondaryNssOrUUIDs) {
            invariant(secondaryNssOrUUID.db() == nsOrUUID.db(),
                      str::stream()
                          << "Unable to acquire locks for collections across different databases ("
                          << secondaryNssOrUUID << " vs " << nsOrUUID << ")");
            temp.insert(catalog->resolveNamespaceStringOrUUID(opCtx, secondaryNssOrUUID));
        }

        // Acquire all of the locks in order. And clear the 'catalog' because the locks will access
        // a fresher one internally.
        catalog = nullptr;
        for (auto& nss : temp) {
            collLocks->emplace_back(opCtx, nss, modeColl, deadline);
        }

        // Check that the namespaces have NOT changed after acquiring locks. It's possible to race
        // with a rename collection when the given NamespaceStringOrUUID is a UUID, and consequently
        // fail to lock the correct namespace.
        //
        // The catalog reference must be refreshed to see the latest Collection data. Otherwise we
        // won't see any concurrent DDL/catalog operations.
        auto catalog = CollectionCatalog::get(opCtx);
        verifyTemp.insert(catalog->resolveNamespaceStringOrUUID(opCtx, nsOrUUID));
        for (const auto& secondaryNssOrUUID : secondaryNssOrUUIDs) {
            verifyTemp.insert(catalog->resolveNamespaceStringOrUUID(opCtx, secondaryNssOrUUID));
        }
    } while (temp != verifyTemp);
}

void setAutoGetCollectionWaitFailpointExecute(std::function<void(const BSONObj&)> callback) {
    setAutoGetCollectionWait.execute(callback);
}

}  // namespace mongo::catalog_helper
