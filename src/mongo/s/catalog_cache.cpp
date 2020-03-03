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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kSharding

#define LOGV2_FOR_CATALOG_REFRESH(ID, DLEVEL, MESSAGE, ...) \
    LOGV2_DEBUG_OPTIONS(                                    \
        ID, DLEVEL, {logv2::LogComponent::kShardingCatalogRefresh}, MESSAGE, ##__VA_ARGS__)

#include "mongo/platform/basic.h"

#include "mongo/s/catalog_cache.h"

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/logical_clock.h"
#include "mongo/db/query/collation/collator_factory_interface.h"
#include "mongo/db/repl/optime_with.h"
#include "mongo/logv2/log.h"
#include "mongo/s/catalog/type_collection.h"
#include "mongo/s/catalog/type_database.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/database_version_helpers.h"
#include "mongo/s/grid.h"
#include "mongo/s/mongos_server_parameters_gen.h"
#include "mongo/s/stale_exception.h"
#include "mongo/util/concurrency/with_lock.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/timer.h"

namespace mongo {
const OperationContext::Decoration<bool> operationShouldBlockBehindCatalogCacheRefresh =
    OperationContext::declareDecoration<bool>();

namespace {

// How many times to try refreshing the routing info if the set of chunks loaded from the config
// server is found to be inconsistent.
const int kMaxInconsistentRoutingInfoRefreshAttempts = 3;

/**
 * Returns whether two shard versions have a matching epoch.
 */
bool shardVersionsHaveMatchingEpoch(boost::optional<ChunkVersion> wanted,
                                    const ChunkVersion& received) {
    return wanted && wanted->epoch() == received.epoch();
};

/**
 * Given an (optional) initial routing table and a set of changed chunks returned by the catalog
 * cache loader, produces a new routing table with the changes applied.
 *
 * If the collection is no longer sharded returns nullptr. If the epoch has changed, expects that
 * the 'collectionChunksList' contains the full contents of the chunks collection for that namespace
 * so that the routing table can be built from scratch.
 *
 * Throws ConflictingOperationInProgress if the chunk metadata was found to be inconsistent (not
 * containing all the necessary chunks, contains overlaps or chunks' epoch values are not the same
 * as that of the collection). Since this situation may be transient, due to the collection being
 * dropped or having its shard key refined concurrently, the caller must retry the reload up to some
 * configurable number of attempts.
 */
std::shared_ptr<RoutingTableHistory> refreshCollectionRoutingInfo(
    OperationContext* opCtx,
    const NamespaceString& nss,
    std::shared_ptr<RoutingTableHistory> existingRoutingInfo,
    StatusWith<CatalogCacheLoader::CollectionAndChangedChunks> swCollectionAndChangedChunks) {
    if (swCollectionAndChangedChunks == ErrorCodes::NamespaceNotFound) {
        return nullptr;
    }

    const auto collectionAndChunks = uassertStatusOK(std::move(swCollectionAndChangedChunks));

    auto chunkManager = [&] {
        // If we have routing info already and it's for the same collection epoch, we're updating.
        // Otherwise, we're making a whole new routing table.
        if (existingRoutingInfo &&
            existingRoutingInfo->getVersion().epoch() == collectionAndChunks.epoch) {

            return existingRoutingInfo->makeUpdated(collectionAndChunks.changedChunks);
        }
        auto defaultCollator = [&]() -> std::unique_ptr<CollatorInterface> {
            if (!collectionAndChunks.defaultCollation.isEmpty()) {
                // The collation should have been validated upon collection creation
                return uassertStatusOK(CollatorFactoryInterface::get(opCtx->getServiceContext())
                                           ->makeFromBSON(collectionAndChunks.defaultCollation));
            }
            return nullptr;
        }();
        return RoutingTableHistory::makeNew(nss,
                                            collectionAndChunks.uuid,
                                            KeyPattern(collectionAndChunks.shardKeyPattern),
                                            std::move(defaultCollator),
                                            collectionAndChunks.shardKeyIsUnique,
                                            collectionAndChunks.epoch,
                                            collectionAndChunks.changedChunks);
    }();

    std::set<ShardId> shardIds;
    chunkManager->getAllShardIds(&shardIds);
    for (const auto& shardId : shardIds) {
        uassertStatusOK(Grid::get(opCtx)->shardRegistry()->getShard(opCtx, shardId));
    }
    return chunkManager;
}

}  // namespace

CatalogCache::CatalogCache(CatalogCacheLoader& cacheLoader) : _cacheLoader(cacheLoader) {}

CatalogCache::~CatalogCache() = default;

StatusWith<CachedDatabaseInfo> CatalogCache::getDatabase(OperationContext* opCtx,
                                                         StringData dbName) {
    invariant(!opCtx->lockState() || !opCtx->lockState()->isLocked(),
              "Do not hold a lock while refreshing the catalog cache. Doing so would potentially "
              "hold the lock during a network call, and can lead to a deadlock as described in "
              "SERVER-37398.");
    try {
        while (true) {
            stdx::unique_lock<Latch> ul(_mutex);

            auto& dbEntry = _databases[dbName];
            if (!dbEntry) {
                dbEntry = std::make_shared<DatabaseInfoEntry>();
            }

            if (dbEntry->needsRefresh) {
                auto refreshNotification = dbEntry->refreshCompletionNotification;
                if (!refreshNotification) {
                    refreshNotification = (dbEntry->refreshCompletionNotification =
                                               std::make_shared<Notification<Status>>());
                    _scheduleDatabaseRefresh(ul, dbName.toString(), dbEntry);
                }

                // Wait on the notification outside of the mutex.
                ul.unlock();
                uassertStatusOK(refreshNotification->get(opCtx));

                // Once the refresh is complete, loop around to get the refreshed cache entry.
                continue;
            }

            if (dbEntry->mustLoadShardedCollections) {
                // If this is the first time we are loading info for this database, also load the
                // sharded collections.
                // TODO (SERVER-34061): Stop loading sharded collections when loading a database.

                const auto dbNameCopy = dbName.toString();
                repl::OpTime collLoadConfigOptime;
                const std::vector<CollectionType> collections =
                    uassertStatusOK(Grid::get(opCtx)->catalogClient()->getCollections(
                        opCtx, &dbNameCopy, &collLoadConfigOptime));

                CollectionInfoMap collectionEntries;
                for (const auto& coll : collections) {
                    if (coll.getDropped()) {
                        continue;
                    }
                    collectionEntries[coll.getNs().ns()] =
                        std::make_shared<CollectionRoutingInfoEntry>();
                }
                _collectionsByDb[dbName] = std::move(collectionEntries);
                dbEntry->mustLoadShardedCollections = false;
            }

            auto primaryShard = uassertStatusOKWithContext(
                Grid::get(opCtx)->shardRegistry()->getShard(opCtx, dbEntry->dbt->getPrimary()),
                str::stream() << "could not find the primary shard for database " << dbName);
            return {CachedDatabaseInfo(*dbEntry->dbt, std::move(primaryShard))};
        }
    } catch (const DBException& ex) {
        return ex.toStatus();
    }
}

StatusWith<CachedCollectionRoutingInfo> CatalogCache::getCollectionRoutingInfo(
    OperationContext* opCtx, const NamespaceString& nss) {
    return _getCollectionRoutingInfo(opCtx, nss).statusWithInfo;
}

CatalogCache::RefreshResult CatalogCache::_getCollectionRoutingInfoWithForcedRefresh(
    OperationContext* opCtx, const NamespaceString& nss) {
    setOperationShouldBlockBehindCatalogCacheRefresh(opCtx, true);
    _createOrGetCollectionEntryAndMarkAsNeedsRefresh(nss);
    return _getCollectionRoutingInfo(opCtx, nss);
}

CatalogCache::RefreshResult CatalogCache::_getCollectionRoutingInfo(OperationContext* opCtx,
                                                                    const NamespaceString& nss) {
    return _getCollectionRoutingInfoAt(opCtx, nss, boost::none);
}


StatusWith<CachedCollectionRoutingInfo> CatalogCache::getCollectionRoutingInfoAt(
    OperationContext* opCtx, const NamespaceString& nss, Timestamp atClusterTime) {
    return _getCollectionRoutingInfoAt(opCtx, nss, atClusterTime).statusWithInfo;
}

CatalogCache::RefreshResult CatalogCache::_getCollectionRoutingInfoAt(
    OperationContext* opCtx, const NamespaceString& nss, boost::optional<Timestamp> atClusterTime) {
    invariant(!opCtx->lockState() || !opCtx->lockState()->isLocked(),
              "Do not hold a lock while refreshing the catalog cache. Doing so would potentially "
              "hold the lock during a network call, and can lead to a deadlock as described in "
              "SERVER-37398.");
    // This default value can cause a single unnecessary extra refresh if this thread did do the
    // refresh but the refresh failed, or if the database or collection was not found, but only if
    // the caller is getCollectionRoutingInfoWithRefresh with the parameter
    // forceRefreshFromThisThread set to true
    RefreshAction refreshActionTaken(RefreshAction::kDidNotPerformRefresh);
    while (true) {
        const auto swDbInfo = getDatabase(opCtx, nss.db());
        if (!swDbInfo.isOK()) {
            return {swDbInfo.getStatus(), refreshActionTaken};
        }

        const auto dbInfo = std::move(swDbInfo.getValue());

        stdx::unique_lock<Latch> ul(_mutex);

        const auto itDb = _collectionsByDb.find(nss.db());
        if (itDb == _collectionsByDb.end()) {
            return {CachedCollectionRoutingInfo(nss, dbInfo, nullptr), refreshActionTaken};
        }

        const auto itColl = itDb->second.find(nss.ns());
        if (itColl == itDb->second.end()) {
            return {CachedCollectionRoutingInfo(nss, dbInfo, nullptr), refreshActionTaken};
        }

        auto& collEntry = itColl->second;

        if (collEntry->needsRefresh &&
            (!gEnableFinerGrainedCatalogCacheRefresh || collEntry->epochHasChanged ||
             operationShouldBlockBehindCatalogCacheRefresh(opCtx))) {
            auto refreshNotification = collEntry->refreshCompletionNotification;
            if (!refreshNotification) {
                refreshNotification = (collEntry->refreshCompletionNotification =
                                           std::make_shared<Notification<Status>>());
                _scheduleCollectionRefresh(ul, collEntry, nss, 1);
                refreshActionTaken = RefreshAction::kPerformedRefresh;
            }

            // Wait on the notification outside of the mutex
            ul.unlock();

            auto refreshStatus = [&]() {
                Timer t;
                ON_BLOCK_EXIT([&] { _stats.totalRefreshWaitTimeMicros.addAndFetch(t.micros()); });

                try {
                    const Milliseconds kReportingInterval{250};
                    while (!refreshNotification->waitFor(opCtx, kReportingInterval)) {
                        _stats.totalRefreshWaitTimeMicros.addAndFetch(t.micros());
                        t.reset();
                    }

                    return refreshNotification->get(opCtx);
                } catch (const DBException& ex) {
                    return ex.toStatus();
                }
            }();

            if (!refreshStatus.isOK()) {
                return {refreshStatus, refreshActionTaken};
            }

            // Once the refresh is complete, loop around to get the latest value
            continue;
        }

        auto cm = std::make_shared<ChunkManager>(collEntry->routingInfo, atClusterTime);

        return {CachedCollectionRoutingInfo(nss, dbInfo, std::move(cm)), refreshActionTaken};
    }
}

StatusWith<CachedDatabaseInfo> CatalogCache::getDatabaseWithRefresh(OperationContext* opCtx,
                                                                    StringData dbName) {
    invalidateDatabaseEntry(dbName);
    return getDatabase(opCtx, dbName);
}

StatusWith<CachedCollectionRoutingInfo> CatalogCache::getCollectionRoutingInfoWithRefresh(
    OperationContext* opCtx, const NamespaceString& nss, bool forceRefreshFromThisThread) {
    auto refreshResult = _getCollectionRoutingInfoWithForcedRefresh(opCtx, nss);
    // We want to ensure that we don't join an in-progress refresh because that
    // could violate causal consistency for this client. We don't need to actually perform the
    // refresh ourselves but we do need the refresh to begin *after* this function is
    // called, so calling it twice is enough regardless of what happens the
    // second time. See SERVER-33954 for reasoning.
    if (forceRefreshFromThisThread &&
        refreshResult.actionTaken == RefreshAction::kDidNotPerformRefresh) {
        refreshResult = _getCollectionRoutingInfoWithForcedRefresh(opCtx, nss);
    }
    return refreshResult.statusWithInfo;
}

StatusWith<CachedCollectionRoutingInfo> CatalogCache::getShardedCollectionRoutingInfoWithRefresh(
    OperationContext* opCtx, const NamespaceString& nss) {
    auto routingInfoStatus = _getCollectionRoutingInfoWithForcedRefresh(opCtx, nss).statusWithInfo;
    if (routingInfoStatus.isOK() && !routingInfoStatus.getValue().cm()) {
        return {ErrorCodes::NamespaceNotSharded,
                str::stream() << "Collection " << nss.ns() << " is not sharded."};
    }

    return routingInfoStatus;
}

void CatalogCache::onStaleDatabaseVersion(const StringData dbName,
                                          const DatabaseVersion& databaseVersion) {
    stdx::lock_guard<Latch> lg(_mutex);

    const auto itDbEntry = _databases.find(dbName);
    if (itDbEntry == _databases.end()) {
        // The database was dropped.
        return;
    } else if (itDbEntry->second->needsRefresh) {
        // Refresh has been scheduled for the database already
        return;
    } else if (!itDbEntry->second->dbt ||
               databaseVersion::equal(itDbEntry->second->dbt->getVersion(), databaseVersion)) {
        // If the versions match, the cached database info is stale, so mark it as needs refresh.
        LOGV2(
            22642, "Marking cached database entry for '{dbName}' as stale", "dbName"_attr = dbName);
        itDbEntry->second->needsRefresh = true;
    }
}

void CatalogCache::onStaleShardVersion(CachedCollectionRoutingInfo&& ccriToInvalidate,
                                       const ShardId& staleShardId) {
    _stats.countStaleConfigErrors.addAndFetch(1);

    // Ensure the move constructor of CachedCollectionRoutingInfo is invoked in order to clear the
    // input argument so it can't be used anymore
    auto ccri(ccriToInvalidate);

    if (!ccri._cm) {
        // We received StaleShardVersion for a collection we thought was unsharded. The collection
        // must have become sharded.
        onEpochChange(ccri._nss);
        return;
    }

    // We received StaleShardVersion for a collection we thought was sharded. Either a migration
    // occurred to or from a shard we contacted, or the collection was dropped.
    stdx::lock_guard<Latch> lg(_mutex);

    const auto nss = ccri._cm->getns();
    const auto itDb = _collectionsByDb.find(nss.db());
    if (itDb == _collectionsByDb.end()) {
        // The database was dropped.
        return;
    }

    auto itColl = itDb->second.find(nss.ns());
    if (itColl == itDb->second.end()) {
        // The collection was dropped.
    } else if (itColl->second->routingInfo->getVersion() == ccri._cm->getVersion()) {
        // If the versions match, the last version of the routing information that we used is no
        // longer valid, so trigger a refresh.
        itColl->second->needsRefresh = true;
        itColl->second->routingInfo->setShardStale(staleShardId);
    }
}

void CatalogCache::setOperationShouldBlockBehindCatalogCacheRefresh(OperationContext* opCtx,
                                                                    bool shouldBlock) {
    if (gEnableFinerGrainedCatalogCacheRefresh) {
        operationShouldBlockBehindCatalogCacheRefresh(opCtx) = shouldBlock;
    }
};

void CatalogCache::invalidateShardOrEntireCollectionEntryForShardedCollection(
    OperationContext* opCtx,
    const NamespaceString& nss,
    boost::optional<ChunkVersion> wantedVersion,
    const ChunkVersion& receivedVersion,
    boost::optional<ShardId> shardId) {
    if (shardId && shardVersionsHaveMatchingEpoch(wantedVersion, receivedVersion)) {
        _createOrGetCollectionEntryAndMarkShardStale(nss, *shardId);
    } else {
        _createOrGetCollectionEntryAndMarkEpochStale(nss);
    }
};

void CatalogCache::onEpochChange(const NamespaceString& nss) {
    _createOrGetCollectionEntryAndMarkEpochStale(nss);
};

void CatalogCache::checkEpochOrThrow(const NamespaceString& nss,
                                     ChunkVersion targetCollectionVersion,
                                     const ShardId& shardId) const {
    stdx::lock_guard<Latch> lg(_mutex);
    const auto itDb = _collectionsByDb.find(nss.db());
    uassert(StaleConfigInfo(nss, targetCollectionVersion, boost::none, shardId),
            str::stream() << "could not act as router for " << nss.ns()
                          << ", no entry for database " << nss.db(),
            itDb != _collectionsByDb.end());

    auto itColl = itDb->second.find(nss.ns());
    uassert(StaleConfigInfo(nss, targetCollectionVersion, boost::none, shardId),
            str::stream() << "could not act as router for " << nss.ns()
                          << ", no entry for collection.",
            itColl != itDb->second.end());

    uassert(StaleConfigInfo(nss, targetCollectionVersion, boost::none, shardId),
            str::stream() << "could not act as router for " << nss.ns() << ", wanted "
                          << targetCollectionVersion.toString()
                          << ", but found the collection was unsharded",
            itColl->second->routingInfo);

    auto foundVersion = itColl->second->routingInfo->getVersion();
    uassert(StaleConfigInfo(nss, targetCollectionVersion, foundVersion, shardId),
            str::stream() << "could not act as router for " << nss.ns() << ", wanted "
                          << targetCollectionVersion.toString() << ", but found "
                          << foundVersion.toString(),
            foundVersion.epoch() == targetCollectionVersion.epoch());
}

void CatalogCache::invalidateDatabaseEntry(const StringData dbName) {
    stdx::lock_guard<Latch> lg(_mutex);
    auto itDbEntry = _databases.find(dbName);
    if (itDbEntry == _databases.end()) {
        // The database was dropped.
        return;
    }
    itDbEntry->second->needsRefresh = true;
}

void CatalogCache::invalidateShardForShardedCollection(const NamespaceString& nss,
                                                       const ShardId& staleShardId) {
    _createOrGetCollectionEntryAndMarkShardStale(nss, staleShardId);
}

void CatalogCache::invalidateEntriesThatReferenceShard(const ShardId& shardId) {
    stdx::lock_guard<Latch> lg(_mutex);

    LOGV2(22643,
          "Starting to invalidate databases and collections with data on shard: {shardId}",
          "shardId"_attr = shardId);

    // Invalidate databases with this shard as their primary.
    for (const auto& [dbNs, dbInfoEntry] : _databases) {
        LOGV2_DEBUG(22644,
                    3,
                    "Checking if database {dbNs}has primary shard: {shardId}",
                    "dbNs"_attr = dbNs,
                    "shardId"_attr = shardId);
        if (!dbInfoEntry->needsRefresh && dbInfoEntry->dbt->getPrimary() == shardId) {
            LOGV2_DEBUG(22645,
                        3,
                        "Database {dbNs}has primary shard {shardId}, invalidating cache entry",
                        "dbNs"_attr = dbNs,
                        "shardId"_attr = shardId);
            dbInfoEntry->needsRefresh = true;
        }
    }

    // Invalidate collections which contain data on this shard.
    for (const auto& [db, collInfoMap] : _collectionsByDb) {
        for (const auto& [collNs, collRoutingInfoEntry] : collInfoMap) {

            LOGV2_DEBUG(22646,
                        3,
                        "Checking if {collNs}has data on shard: {shardId}",
                        "collNs"_attr = collNs,
                        "shardId"_attr = shardId);

            if (!collRoutingInfoEntry->needsRefresh) {
                // The set of shards on which this collection contains chunks.
                std::set<ShardId> shardsOwningDataForCollection;
                collRoutingInfoEntry->routingInfo->getAllShardIds(&shardsOwningDataForCollection);

                if (shardsOwningDataForCollection.find(shardId) !=
                    shardsOwningDataForCollection.end()) {
                    LOGV2_DEBUG(22647,
                                3,
                                "{collNs}has data on shard {shardId}, invalidating cache entry",
                                "collNs"_attr = collNs,
                                "shardId"_attr = shardId);

                    collRoutingInfoEntry->needsRefresh = true;
                    collRoutingInfoEntry->routingInfo->setShardStale(shardId);
                }
            }
        }
    }

    LOGV2(22648,
          "Finished invalidating databases and collections with data on shard: {shardId}",
          "shardId"_attr = shardId);
}

void CatalogCache::purgeCollection(const NamespaceString& nss) {
    stdx::lock_guard<Latch> lg(_mutex);

    auto itDb = _collectionsByDb.find(nss.db());
    if (itDb == _collectionsByDb.end()) {
        return;
    }

    itDb->second.erase(nss.ns());
}

void CatalogCache::purgeDatabase(StringData dbName) {
    stdx::lock_guard<Latch> lg(_mutex);
    _databases.erase(dbName);
    _collectionsByDb.erase(dbName);
}

void CatalogCache::purgeAllDatabases() {
    stdx::lock_guard<Latch> lg(_mutex);
    _databases.clear();
    _collectionsByDb.clear();
}

void CatalogCache::report(BSONObjBuilder* builder) const {
    BSONObjBuilder cacheStatsBuilder(builder->subobjStart("catalogCache"));

    size_t numDatabaseEntries;
    size_t numCollectionEntries{0};
    {
        stdx::lock_guard<Latch> ul(_mutex);
        numDatabaseEntries = _databases.size();
        for (const auto& entry : _collectionsByDb) {
            numCollectionEntries += entry.second.size();
        }
    }

    cacheStatsBuilder.append("numDatabaseEntries", static_cast<long long>(numDatabaseEntries));
    cacheStatsBuilder.append("numCollectionEntries", static_cast<long long>(numCollectionEntries));

    _stats.report(&cacheStatsBuilder);
}

void CatalogCache::_scheduleDatabaseRefresh(WithLock lk,
                                            const std::string& dbName,
                                            std::shared_ptr<DatabaseInfoEntry> dbEntry) {
    const auto onRefreshCompleted = [this, t = Timer(), dbName, dbEntry](
                                        const StatusWith<DatabaseType>& swDbt) {
        // TODO (SERVER-34164): Track and increment stats for database refreshes.
        if (!swDbt.isOK()) {
            LOGV2_OPTIONS(24100,
                          {logv2::LogComponent::kShardingCatalogRefresh},
                          "Refresh for database {dbName} took {t_millis} ms and "
                          "failed{causedBy_swDbt_getStatus}",
                          "dbName"_attr = dbName,
                          "t_millis"_attr = t.millis(),
                          "causedBy_swDbt_getStatus"_attr = causedBy(redact(swDbt.getStatus())));
            return;
        }

        const auto dbVersionAfterRefresh = swDbt.getValue().getVersion();
        const int logLevel =
            (!dbEntry->dbt ||
             (dbEntry->dbt &&
              !databaseVersion::equal(dbVersionAfterRefresh, dbEntry->dbt->getVersion())))
            ? 0
            : 1;
        LOGV2_FOR_CATALOG_REFRESH(
            24101,
            logSeverityV1toV2(logLevel).toInt(),
            "Refresh for database {dbName} from version "
            "{dbEntry_dbt_dbEntry_dbt_getVersion_BSONObj} to version {dbVersionAfterRefresh} "
            "took {t_millis} ms",
            "dbName"_attr = dbName,
            "dbEntry_dbt_dbEntry_dbt_getVersion_BSONObj"_attr =
                (dbEntry->dbt ? dbEntry->dbt->getVersion().toBSON() : BSONObj()),
            "dbVersionAfterRefresh"_attr = dbVersionAfterRefresh.toBSON(),
            "t_millis"_attr = t.millis());
    };

    // Invoked if getDatabase resulted in error or threw and exception
    const auto onRefreshFailed =
        [ this, dbName, dbEntry, onRefreshCompleted ](WithLock, const Status& status) noexcept {
        onRefreshCompleted(status);

        // Clear the notification so the next 'getDatabase' kicks off a new refresh attempt.
        dbEntry->refreshCompletionNotification->set(status);
        dbEntry->refreshCompletionNotification = nullptr;

        if (status == ErrorCodes::NamespaceNotFound) {
            // The refresh found that the database was dropped, so remove its entry from the cache.
            _databases.erase(dbName);
            _collectionsByDb.erase(dbName);
            return;
        }
    };

    const auto refreshCallback = [ this, dbName, dbEntry, onRefreshFailed, onRefreshCompleted ](
        OperationContext * opCtx, StatusWith<DatabaseType> swDbt) noexcept {
        stdx::lock_guard<Latch> lg(_mutex);

        if (!swDbt.isOK()) {
            onRefreshFailed(lg, swDbt.getStatus());
            return;
        }

        onRefreshCompleted(swDbt);

        dbEntry->needsRefresh = false;
        dbEntry->refreshCompletionNotification->set(Status::OK());
        dbEntry->refreshCompletionNotification = nullptr;

        dbEntry->dbt = std::move(swDbt.getValue());
    };

    LOGV2_FOR_CATALOG_REFRESH(
        24102,
        1,
        "Refreshing cached database entry for {dbName}; current cached database info is "
        "{dbEntry_dbt_dbEntry_dbt_BSONObj}",
        "dbName"_attr = dbName,
        "dbEntry_dbt_dbEntry_dbt_BSONObj"_attr =
            (dbEntry->dbt ? dbEntry->dbt->toBSON() : BSONObj()));

    try {
        _cacheLoader.getDatabase(dbName, refreshCallback);
    } catch (const DBException& ex) {
        const auto status = ex.toStatus();

        onRefreshFailed(lk, status);
    }
}

void CatalogCache::_scheduleCollectionRefresh(WithLock lk,
                                              std::shared_ptr<CollectionRoutingInfoEntry> collEntry,
                                              NamespaceString const& nss,
                                              int refreshAttempt) {
    const auto existingRoutingInfo = collEntry->routingInfo;

    // If we have an existing chunk manager, the refresh is considered "incremental", regardless of
    // how many chunks are in the differential
    const bool isIncremental(existingRoutingInfo);

    if (isIncremental) {
        _stats.numActiveIncrementalRefreshes.addAndFetch(1);
        _stats.countIncrementalRefreshesStarted.addAndFetch(1);
    } else {
        _stats.numActiveFullRefreshes.addAndFetch(1);
        _stats.countFullRefreshesStarted.addAndFetch(1);
    }

    // Invoked when one iteration of getChunksSince has completed, whether with success or error
    const auto onRefreshCompleted = [this, t = Timer(), nss, isIncremental, existingRoutingInfo](
                                        const Status& status,
                                        RoutingTableHistory* routingInfoAfterRefresh) {
        if (isIncremental) {
            _stats.numActiveIncrementalRefreshes.subtractAndFetch(1);
        } else {
            _stats.numActiveFullRefreshes.subtractAndFetch(1);
        }

        if (!status.isOK()) {
            _stats.countFailedRefreshes.addAndFetch(1);

            LOGV2_OPTIONS(
                24103,
                {logv2::LogComponent::kShardingCatalogRefresh},
                "Refresh for collection {nss} took {t_millis} ms and failed{causedBy_status}",
                "nss"_attr = nss,
                "t_millis"_attr = t.millis(),
                "causedBy_status"_attr = causedBy(redact(status)));
        } else if (routingInfoAfterRefresh) {
            const int logLevel =
                (!existingRoutingInfo ||
                 (existingRoutingInfo &&
                  routingInfoAfterRefresh->getVersion() != existingRoutingInfo->getVersion()))
                ? 0
                : 1;
            LOGV2_FOR_CATALOG_REFRESH(
                24104,
                logSeverityV1toV2(logLevel).toInt(),
                "Refresh for collection "
                "{nss}{existingRoutingInfo_from_version_existingRoutingInfo_getVersion} to version "
                "{routingInfoAfterRefresh_getVersion} took {t_millis} ms",
                "nss"_attr = nss.toString(),
                "existingRoutingInfo_from_version_existingRoutingInfo_getVersion"_attr =
                    (existingRoutingInfo
                         ? (" from version " + existingRoutingInfo->getVersion().toString())
                         : ""),
                "routingInfoAfterRefresh_getVersion"_attr =
                    routingInfoAfterRefresh->getVersion().toString(),
                "t_millis"_attr = t.millis());
        } else {
            LOGV2_OPTIONS(
                24105,
                {logv2::LogComponent::kShardingCatalogRefresh},
                "Refresh for collection {nss} took {t_millis} ms and found the collection is not "
                "sharded",
                "nss"_attr = nss,
                "t_millis"_attr = t.millis());
        }
    };

    // Invoked if getChunksSince resulted in error or threw an exception
    const auto onRefreshFailed = [ this, collEntry, nss, refreshAttempt, onRefreshCompleted ](
        WithLock lk, const Status& status) noexcept {
        onRefreshCompleted(status, nullptr);

        // It is possible that the metadata is being changed concurrently, so retry the
        // refresh again
        if (status == ErrorCodes::ConflictingOperationInProgress &&
            refreshAttempt < kMaxInconsistentRoutingInfoRefreshAttempts) {
            _scheduleCollectionRefresh(lk, collEntry, nss, refreshAttempt + 1);
        } else {
            // Leave needsRefresh to true so that any subsequent get attempts will kick off
            // another round of refresh
            collEntry->refreshCompletionNotification->set(status);
            collEntry->refreshCompletionNotification = nullptr;
        }
    };

    const auto refreshCallback =
        [ this, collEntry, nss, existingRoutingInfo, onRefreshFailed, onRefreshCompleted ](
            OperationContext * opCtx,
            StatusWith<CatalogCacheLoader::CollectionAndChangedChunks> swCollAndChunks) noexcept {
        std::shared_ptr<RoutingTableHistory> newRoutingInfo;
        try {
            newRoutingInfo = refreshCollectionRoutingInfo(
                opCtx, nss, std::move(existingRoutingInfo), std::move(swCollAndChunks));

            onRefreshCompleted(Status::OK(), newRoutingInfo.get());
        } catch (const DBException& ex) {
            stdx::lock_guard<Latch> lg(_mutex);
            onRefreshFailed(lg, ex.toStatus());
            return;
        }

        stdx::lock_guard<Latch> lg(_mutex);

        collEntry->epochHasChanged = false;
        collEntry->needsRefresh = false;
        collEntry->refreshCompletionNotification->set(Status::OK());
        collEntry->refreshCompletionNotification = nullptr;

        setOperationShouldBlockBehindCatalogCacheRefresh(opCtx, false);

        if (!newRoutingInfo) {
            // The refresh found that collection was dropped, so remove it from our cache.
            auto itDb = _collectionsByDb.find(nss.db());
            if (itDb == _collectionsByDb.end()) {
                // The entire database was dropped.
                return;
            }
            itDb->second.erase(nss.ns());
            return;
        }
        collEntry->routingInfo = std::move(newRoutingInfo);
    };

    const ChunkVersion startingCollectionVersion =
        (existingRoutingInfo ? existingRoutingInfo->getVersion() : ChunkVersion::UNSHARDED());

    LOGV2_FOR_CATALOG_REFRESH(
        24106,
        1,
        "Refreshing chunks for collection {nss}; current collection version is "
        "{startingCollectionVersion}",
        "nss"_attr = nss,
        "startingCollectionVersion"_attr = startingCollectionVersion);

    try {
        _cacheLoader.getChunksSince(nss, startingCollectionVersion, refreshCallback);
    } catch (const DBException& ex) {
        const auto status = ex.toStatus();

        // ConflictingOperationInProgress errors trigger retry of the catalog cache reload logic. If
        // we failed to schedule the asynchronous reload, there is no point in doing another
        // attempt.
        invariant(status != ErrorCodes::ConflictingOperationInProgress);

        onRefreshFailed(lk, status);
    }

    // The routing info for this collection shouldn't change, as other threads may try to use the
    // CatalogCache while we are waiting for the refresh to complete.
    invariant(collEntry->routingInfo.get() == existingRoutingInfo.get());
}

void CatalogCache::_createOrGetCollectionEntryAndMarkEpochStale(const NamespaceString& nss) {
    stdx::lock_guard<Latch> lg(_mutex);
    auto optionalRoutingInfoEntry = _createOrGetCollectionEntry(lg, nss);
    if (!optionalRoutingInfoEntry) {
        return;
    }

    optionalRoutingInfoEntry->needsRefresh = true;
    optionalRoutingInfoEntry->epochHasChanged = true;
}

void CatalogCache::_createOrGetCollectionEntryAndMarkShardStale(const NamespaceString& nss,
                                                                const ShardId& staleShardId) {
    stdx::lock_guard<Latch> lg(_mutex);
    auto optionalRoutingInfoEntry = _createOrGetCollectionEntry(lg, nss);
    if (!optionalRoutingInfoEntry) {
        return;
    }

    optionalRoutingInfoEntry->needsRefresh = true;
    if (optionalRoutingInfoEntry->routingInfo) {
        optionalRoutingInfoEntry->routingInfo->setShardStale(staleShardId);
    }
}

void CatalogCache::_createOrGetCollectionEntryAndMarkAsNeedsRefresh(const NamespaceString& nss) {
    stdx::lock_guard<Latch> lg(_mutex);
    auto optionalRoutingInfoEntry = _createOrGetCollectionEntry(lg, nss);
    if (!optionalRoutingInfoEntry) {
        return;
    }

    optionalRoutingInfoEntry->needsRefresh = true;
}

boost::optional<CatalogCache::CollectionRoutingInfoEntry&>
CatalogCache::_createOrGetCollectionEntry(WithLock wl, const NamespaceString& nss) {
    auto itDb = _collectionsByDb.find(nss.db());
    if (itDb == _collectionsByDb.end()) {
        return boost::none;
    }

    if (itDb->second.find(nss.ns()) == itDb->second.end()) {
        itDb->second[nss.ns()] = std::make_shared<CollectionRoutingInfoEntry>();
    }

    return *itDb->second[nss.ns()];
}

void CatalogCache::Stats::report(BSONObjBuilder* builder) const {
    builder->append("countStaleConfigErrors", countStaleConfigErrors.load());

    builder->append("totalRefreshWaitTimeMicros", totalRefreshWaitTimeMicros.load());

    builder->append("numActiveIncrementalRefreshes", numActiveIncrementalRefreshes.load());
    builder->append("countIncrementalRefreshesStarted", countIncrementalRefreshesStarted.load());

    builder->append("numActiveFullRefreshes", numActiveFullRefreshes.load());
    builder->append("countFullRefreshesStarted", countFullRefreshesStarted.load());

    builder->append("countFailedRefreshes", countFailedRefreshes.load());
}

CachedDatabaseInfo::CachedDatabaseInfo(DatabaseType dbt, std::shared_ptr<Shard> primaryShard)
    : _dbt(std::move(dbt)), _primaryShard(std::move(primaryShard)) {}

const ShardId& CachedDatabaseInfo::primaryId() const {
    return _dbt.getPrimary();
}

bool CachedDatabaseInfo::shardingEnabled() const {
    return _dbt.getSharded();
}

DatabaseVersion CachedDatabaseInfo::databaseVersion() const {
    return _dbt.getVersion();
}

CachedCollectionRoutingInfo::CachedCollectionRoutingInfo(NamespaceString nss,
                                                         CachedDatabaseInfo db,
                                                         std::shared_ptr<ChunkManager> cm)
    : _nss(std::move(nss)), _db(std::move(db)), _cm(std::move(cm)) {}

}  // namespace mongo
