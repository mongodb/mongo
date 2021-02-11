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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

#define LOGV2_FOR_CATALOG_REFRESH(ID, DLEVEL, MESSAGE, ...) \
    LOGV2_DEBUG_OPTIONS(                                    \
        ID, DLEVEL, {logv2::LogComponent::kShardingCatalogRefresh}, MESSAGE, ##__VA_ARGS__)

#include "mongo/platform/basic.h"

#include "mongo/s/catalog_cache.h"

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/query/collation/collator_factory_interface.h"
#include "mongo/db/repl/optime_with.h"
#include "mongo/logv2/log.h"
#include "mongo/s/catalog/type_collection.h"
#include "mongo/s/catalog/type_database.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/database_version.h"
#include "mongo/s/grid.h"
#include "mongo/s/is_mongos.h"
#include "mongo/s/mongod_and_mongos_server_parameters_gen.h"
#include "mongo/s/stale_exception.h"
#include "mongo/util/concurrency/with_lock.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/timer.h"

namespace mongo {
namespace {

MONGO_FAIL_POINT_DEFINE(blockCollectionCacheLookup);

// How many times to try refreshing the routing info if the set of chunks loaded from the config
// server is found to be inconsistent.
const int kMaxInconsistentRoutingInfoRefreshAttempts = 3;

const int kDatabaseCacheSize = 10000;
const int kCollectionCacheSize = 10000;

const OperationContext::Decoration<bool> operationShouldBlockBehindCatalogCacheRefresh =
    OperationContext::declareDecoration<bool>();

const OperationContext::Decoration<bool> operationBlockedBehindCatalogCacheRefresh =
    OperationContext::declareDecoration<bool>();

}  // namespace

CachedDatabaseInfo::CachedDatabaseInfo(DatabaseTypeValueHandle&& dbt) : _dbt(std::move(dbt)){};

DatabaseType CachedDatabaseInfo::getDatabaseType() const {
    return *_dbt;
}

const ShardId& CachedDatabaseInfo::primaryId() const {
    return _dbt->getPrimary();
}

bool CachedDatabaseInfo::shardingEnabled() const {
    return _dbt->getSharded();
}

DatabaseVersion CachedDatabaseInfo::databaseVersion() const {
    return _dbt->getVersion();
}

CatalogCache::CatalogCache(ServiceContext* const service, CatalogCacheLoader& cacheLoader)
    : _cacheLoader(cacheLoader),
      _executor(std::make_shared<ThreadPool>([] {
          ThreadPool::Options options;
          options.poolName = "CatalogCache";
          options.minThreads = 0;
          options.maxThreads = 6;
          return options;
      }())),
      _databaseCache(service, *_executor, _cacheLoader),
      _collectionCache(service, *_executor, _cacheLoader) {
    _executor->startup();
}

CatalogCache::~CatalogCache() {
    // The executor is used by the Database and Collection caches,
    // so it must be joined, before these caches are destroyed,
    // per the contract of ReadThroughCache.
    _executor->shutdown();
    _executor->join();
}

StatusWith<CachedDatabaseInfo> CatalogCache::getDatabase(OperationContext* opCtx,
                                                         StringData dbName,
                                                         bool allowLocks) {
    if (!allowLocks) {
        invariant(
            !opCtx->lockState() || !opCtx->lockState()->isLocked(),
            "Do not hold a lock while refreshing the catalog cache. Doing so would potentially "
            "hold the lock during a network call, and can lead to a deadlock as described in "
            "SERVER-37398.");
    }

    try {
        auto dbEntry = _databaseCache.acquire(opCtx, dbName, CacheCausalConsistency::kLatestKnown);
        uassert(ErrorCodes::NamespaceNotFound,
                str::stream() << "database " << dbName << " not found",
                dbEntry);

        return {CachedDatabaseInfo(std::move(dbEntry))};
    } catch (const DBException& ex) {
        return ex.toStatus();
    }
}

StatusWith<ChunkManager> CatalogCache::_getCollectionRoutingInfoAt(
    OperationContext* opCtx,
    const NamespaceString& nss,
    boost::optional<Timestamp> atClusterTime,
    bool allowLocks) {
    if (!allowLocks) {
        invariant(!opCtx->lockState() || !opCtx->lockState()->isLocked(),
                  "Do not hold a lock while refreshing the catalog cache. Doing so would "
                  "potentially hold "
                  "the lock during a network call, and can lead to a deadlock as described in "
                  "SERVER-37398.");
    }

    try {
        const auto swDbInfo = getDatabase(opCtx, nss.db(), allowLocks);
        if (!swDbInfo.isOK()) {
            if (swDbInfo == ErrorCodes::NamespaceNotFound) {
                LOGV2_FOR_CATALOG_REFRESH(
                    4947103,
                    2,
                    "Invalidating cached collection entry because its database has been dropped",
                    "namespace"_attr = nss);
                invalidateCollectionEntry_LINEARIZABLE(nss);
            }
            return swDbInfo.getStatus();
        }

        const auto dbInfo = std::move(swDbInfo.getValue());

        const auto cacheConsistency = gEnableFinerGrainedCatalogCacheRefresh &&
                !operationShouldBlockBehindCatalogCacheRefresh(opCtx)
            ? CacheCausalConsistency::kLatestCached
            : CacheCausalConsistency::kLatestKnown;

        auto collEntryFuture = _collectionCache.acquireAsync(nss, cacheConsistency);

        if (allowLocks) {
            // When allowLocks is true we may be holding a lock, so we don't
            // want to block the current thread: if the future is ready let's
            // use it, otherwise return an error

            if (collEntryFuture.isReady()) {
                setOperationShouldBlockBehindCatalogCacheRefresh(opCtx, false);
                return ChunkManager(dbInfo.primaryId(),
                                    dbInfo.databaseVersion(),
                                    collEntryFuture.get(opCtx),
                                    atClusterTime);
            } else {
                return Status{ErrorCodes::StaleShardVersion,
                              "Routing info refresh did not complete"};
            }
        }

        // From this point we can guarantee that allowLocks is false

        operationBlockedBehindCatalogCacheRefresh(opCtx) = true;

        size_t acquireTries = 0;
        Timer t;

        while (true) {
            try {
                auto collEntry = collEntryFuture.get(opCtx);
                _stats.totalRefreshWaitTimeMicros.addAndFetch(t.micros());

                setOperationShouldBlockBehindCatalogCacheRefresh(opCtx, false);

                return ChunkManager(dbInfo.primaryId(),
                                    dbInfo.databaseVersion(),
                                    std::move(collEntry),
                                    atClusterTime);
            } catch (ExceptionFor<ErrorCodes::ConflictingOperationInProgress>& ex) {
                LOGV2_FOR_CATALOG_REFRESH(5310501,
                                          0,
                                          "Collection refresh failed",
                                          "namespace"_attr = nss,
                                          "exception"_attr = redact(ex));
                _stats.totalRefreshWaitTimeMicros.addAndFetch(t.micros());
                acquireTries++;
                if (acquireTries == kMaxInconsistentRoutingInfoRefreshAttempts) {
                    return ex.toStatus();
                }
            } catch (ExceptionFor<ErrorCodes::BadValue>& ex) {
                // TODO SERVER-53283: Remove once 5.0 has branched out.
                // This would happen when the query to config.chunks fails because the index
                // specified in the 'hint' provided by ConfigServerCatalogCache loader does no
                // longer exist because it was dropped as part of the FCV upgrade/downgrade process
                // to/from 5.0.
                LOGV2_FOR_CATALOG_REFRESH(5310502,
                                          0,
                                          "Collection refresh failed",
                                          "namespace"_attr = nss,
                                          "exception"_attr = redact(ex));
                _stats.totalRefreshWaitTimeMicros.addAndFetch(t.micros());
                acquireTries++;
                if (acquireTries == kMaxInconsistentRoutingInfoRefreshAttempts) {
                    return ex.toStatus();
                }
            } catch (ExceptionFor<ErrorCodes::QueryPlanKilled>& ex) {
                // TODO SERVER-53283: Remove once 5.0 has branched out.
                // This would happen when the query to config.chunks is killed because the index it
                // relied on has been dropped while the query was ongoing.
                LOGV2_FOR_CATALOG_REFRESH(5310503,
                                          0,
                                          "Collection refresh failed",
                                          "namespace"_attr = nss,
                                          "exception"_attr = redact(ex));
                _stats.totalRefreshWaitTimeMicros.addAndFetch(t.micros());
                acquireTries++;
                if (acquireTries == kMaxInconsistentRoutingInfoRefreshAttempts) {
                    return ex.toStatus();
                }
            }

            collEntryFuture = _collectionCache.acquireAsync(nss, cacheConsistency);
            t.reset();
        }
    } catch (const DBException& ex) {
        return ex.toStatus();
    }
}

StatusWith<ChunkManager> CatalogCache::getCollectionRoutingInfo(OperationContext* opCtx,
                                                                const NamespaceString& nss,
                                                                bool allowLocks) {
    return _getCollectionRoutingInfoAt(opCtx, nss, boost::none, allowLocks);
}

StatusWith<ChunkManager> CatalogCache::getCollectionRoutingInfoAt(OperationContext* opCtx,
                                                                  const NamespaceString& nss,
                                                                  Timestamp atClusterTime) {
    return _getCollectionRoutingInfoAt(opCtx, nss, atClusterTime, false);
}

StatusWith<CachedDatabaseInfo> CatalogCache::getDatabaseWithRefresh(OperationContext* opCtx,
                                                                    StringData dbName) {
    _databaseCache.invalidate(dbName);
    return getDatabase(opCtx, dbName);
}

StatusWith<ChunkManager> CatalogCache::getCollectionRoutingInfoWithRefresh(
    OperationContext* opCtx, const NamespaceString& nss) {
    _collectionCache.invalidate(nss);
    setOperationShouldBlockBehindCatalogCacheRefresh(opCtx, true);
    return getCollectionRoutingInfo(opCtx, nss);
}

ChunkManager CatalogCache::getShardedCollectionRoutingInfo(OperationContext* opCtx,
                                                           const NamespaceString& nss) {
    auto cm = uassertStatusOK(getCollectionRoutingInfo(opCtx, nss));

    uassert(ErrorCodes::NamespaceNotSharded,
            str::stream() << "Expected collection " << nss << " to be sharded",
            cm.isSharded());

    return cm;
}

StatusWith<ChunkManager> CatalogCache::getShardedCollectionRoutingInfoWithRefresh(
    OperationContext* opCtx, const NamespaceString& nss) {
    auto routingInfoStatus = getCollectionRoutingInfoWithRefresh(opCtx, nss);
    if (routingInfoStatus.isOK() && !routingInfoStatus.getValue().isSharded()) {
        return {ErrorCodes::NamespaceNotSharded,
                str::stream() << "Collection " << nss.ns() << " is not sharded."};
    }
    return routingInfoStatus;
}

void CatalogCache::onStaleDatabaseVersion(const StringData dbName,
                                          const boost::optional<DatabaseVersion>& databaseVersion) {
    if (databaseVersion) {
        const auto version =
            ComparableDatabaseVersion::makeComparableDatabaseVersion(databaseVersion.get());
        LOGV2_FOR_CATALOG_REFRESH(4899101,
                                  2,
                                  "Registering new database version",
                                  "db"_attr = dbName,
                                  "version"_attr = version.toBSONForLogging());
        _databaseCache.advanceTimeInStore(dbName, version);
    } else {
        _databaseCache.invalidate(dbName);
    }
}

void CatalogCache::setOperationShouldBlockBehindCatalogCacheRefresh(OperationContext* opCtx,
                                                                    bool shouldBlock) {
    if (gEnableFinerGrainedCatalogCacheRefresh) {
        operationShouldBlockBehindCatalogCacheRefresh(opCtx) = shouldBlock;
    }
}

void CatalogCache::invalidateShardOrEntireCollectionEntryForShardedCollection(
    const NamespaceString& nss,
    const boost::optional<ChunkVersion>& wantedVersion,
    const ShardId& shardId) {
    _stats.countStaleConfigErrors.addAndFetch(1);

    auto collectionEntry = _collectionCache.peekLatestCached(nss);

    const auto newChunkVersion = wantedVersion
        ? ComparableChunkVersion::makeComparableChunkVersion(*wantedVersion)
        : ComparableChunkVersion::makeComparableChunkVersionForForcedRefresh();

    const bool timeAdvanced = _collectionCache.advanceTimeInStore(nss, newChunkVersion);

    if (timeAdvanced && collectionEntry && collectionEntry->optRt) {
        // Shards marked stale will be reset on the next refresh.
        // We can mark the shard stale only if the time advanced, otherwise no refresh would happen
        // and the shard will remain marked stale.
        // Even if a concurrent refresh is happening this is still the old collectionEntry,
        // so it is safe to call setShardStale.
        collectionEntry->optRt->setShardStale(shardId);
    }
}

void CatalogCache::checkEpochOrThrow(const NamespaceString& nss,
                                     const ChunkVersion& targetCollectionVersion,
                                     const ShardId& shardId) {
    uassert(StaleConfigInfo(nss, targetCollectionVersion, boost::none, shardId),
            str::stream() << "could not act as router for " << nss.ns()
                          << ", no entry for database " << nss.db(),
            _databaseCache.peekLatestCached(nss.db()));

    auto collectionValueHandle = _collectionCache.peekLatestCached(nss);
    uassert(StaleConfigInfo(nss, targetCollectionVersion, boost::none, shardId),
            str::stream() << "could not act as router for " << nss.ns()
                          << ", no entry for collection.",
            collectionValueHandle);

    uassert(StaleConfigInfo(nss, targetCollectionVersion, boost::none, shardId),
            str::stream() << "could not act as router for " << nss.ns() << ", wanted "
                          << targetCollectionVersion.toString()
                          << ", but found the collection was unsharded",
            collectionValueHandle->optRt);

    auto foundVersion = collectionValueHandle->optRt->getVersion();
    uassert(StaleConfigInfo(nss, targetCollectionVersion, foundVersion, shardId),
            str::stream() << "could not act as router for " << nss.ns() << ", wanted "
                          << targetCollectionVersion.toString() << ", but found "
                          << foundVersion.toString(),
            foundVersion.epoch() == targetCollectionVersion.epoch());
}

void CatalogCache::invalidateEntriesThatReferenceShard(const ShardId& shardId) {
    LOGV2_DEBUG(4997600,
                1,
                "Invalidating databases and collections referencing a specific shard",
                "shardId"_attr = shardId);

    _databaseCache.invalidateCachedValueIf(
        [&](const DatabaseType& dbt) { return dbt.getPrimary() == shardId; });

    // Invalidate collections which contain data on this shard.
    _collectionCache.invalidateCachedValueIf([&](const OptionalRoutingTableHistory& ort) {
        if (!ort.optRt)
            return false;
        const auto& rt = *ort.optRt;

        std::set<ShardId> shardIds;
        rt.getAllShardIds(&shardIds);

        LOGV2_DEBUG(22647,
                    3,
                    "Invalidating cached collection {namespace} that has data "
                    "on shard {shardId}",
                    "Invalidating cached collection",
                    "namespace"_attr = rt.nss(),
                    "shardId"_attr = shardId);
        return shardIds.find(shardId) != shardIds.end();
    });

    LOGV2(22648,
          "Finished invalidating databases and collections with data on shard: {shardId}",
          "Finished invalidating databases and collections that reference specific shard",
          "shardId"_attr = shardId);
}

void CatalogCache::purgeDatabase(StringData dbName) {
    _databaseCache.invalidate(dbName);
    _collectionCache.invalidateKeyIf(
        [&](const NamespaceString& nss) { return nss.db() == dbName; });
}

void CatalogCache::purgeAllDatabases() {
    _databaseCache.invalidateAll();
    _collectionCache.invalidateAll();
}

void CatalogCache::report(BSONObjBuilder* builder) const {
    BSONObjBuilder cacheStatsBuilder(builder->subobjStart("catalogCache"));

    const size_t numDatabaseEntries = _databaseCache.getCacheInfo().size();
    const size_t numCollectionEntries = _collectionCache.getCacheInfo().size();

    cacheStatsBuilder.append("numDatabaseEntries", static_cast<long long>(numDatabaseEntries));
    cacheStatsBuilder.append("numCollectionEntries", static_cast<long long>(numCollectionEntries));

    _stats.report(&cacheStatsBuilder);
    _collectionCache.reportStats(&cacheStatsBuilder);
}

void CatalogCache::checkAndRecordOperationBlockedByRefresh(OperationContext* opCtx,
                                                           mongo::LogicalOp opType) {
    if (!isMongos() || !operationBlockedBehindCatalogCacheRefresh(opCtx)) {
        return;
    }

    auto& opsBlockedByRefresh = _stats.operationsBlockedByRefresh;

    opsBlockedByRefresh.countAllOperations.fetchAndAddRelaxed(1);

    switch (opType) {
        case LogicalOp::opInsert:
            opsBlockedByRefresh.countInserts.fetchAndAddRelaxed(1);
            break;
        case LogicalOp::opQuery:
            opsBlockedByRefresh.countQueries.fetchAndAddRelaxed(1);
            break;
        case LogicalOp::opUpdate:
            opsBlockedByRefresh.countUpdates.fetchAndAddRelaxed(1);
            break;
        case LogicalOp::opDelete:
            opsBlockedByRefresh.countDeletes.fetchAndAddRelaxed(1);
            break;
        case LogicalOp::opCommand:
            opsBlockedByRefresh.countCommands.fetchAndAddRelaxed(1);
            break;
        default:
            MONGO_UNREACHABLE;
    }
}

void CatalogCache::invalidateDatabaseEntry_LINEARIZABLE(const StringData& dbName) {
    _databaseCache.invalidate(dbName);
}

void CatalogCache::invalidateCollectionEntry_LINEARIZABLE(const NamespaceString& nss) {
    _collectionCache.invalidate(nss);
}

void CatalogCache::Stats::report(BSONObjBuilder* builder) const {
    builder->append("countStaleConfigErrors", countStaleConfigErrors.load());

    builder->append("totalRefreshWaitTimeMicros", totalRefreshWaitTimeMicros.load());

    if (isMongos()) {
        BSONObjBuilder operationsBlockedByRefreshBuilder(
            builder->subobjStart("operationsBlockedByRefresh"));

        operationsBlockedByRefreshBuilder.append(
            "countAllOperations", operationsBlockedByRefresh.countAllOperations.load());
        operationsBlockedByRefreshBuilder.append("countInserts",
                                                 operationsBlockedByRefresh.countInserts.load());
        operationsBlockedByRefreshBuilder.append("countQueries",
                                                 operationsBlockedByRefresh.countQueries.load());
        operationsBlockedByRefreshBuilder.append("countUpdates",
                                                 operationsBlockedByRefresh.countUpdates.load());
        operationsBlockedByRefreshBuilder.append("countDeletes",
                                                 operationsBlockedByRefresh.countDeletes.load());
        operationsBlockedByRefreshBuilder.append("countCommands",
                                                 operationsBlockedByRefresh.countCommands.load());

        operationsBlockedByRefreshBuilder.done();
    }
}

CatalogCache::DatabaseCache::DatabaseCache(ServiceContext* service,
                                           ThreadPoolInterface& threadPool,
                                           CatalogCacheLoader& catalogCacheLoader)
    : ReadThroughCache(_mutex,
                       service,
                       threadPool,
                       [this](OperationContext* opCtx,
                              const std::string& dbName,
                              const ValueHandle& db,
                              const ComparableDatabaseVersion& previousDbVersion) {
                           return _lookupDatabase(opCtx, dbName, db, previousDbVersion);
                       },
                       kDatabaseCacheSize),
      _catalogCacheLoader(catalogCacheLoader) {}

CatalogCache::DatabaseCache::LookupResult CatalogCache::DatabaseCache::_lookupDatabase(
    OperationContext* opCtx,
    const std::string& dbName,
    const DatabaseTypeValueHandle& previousDbType,
    const ComparableDatabaseVersion& previousDbVersion) {
    // TODO (SERVER-34164): Track and increment stats for database refreshes

    LOGV2_FOR_CATALOG_REFRESH(24102, 2, "Refreshing cached database entry", "db"_attr = dbName);

    Timer t{};
    try {
        auto newDb = _catalogCacheLoader.getDatabase(dbName).get();
        uassertStatusOKWithContext(
            Grid::get(opCtx)->shardRegistry()->getShard(opCtx, newDb.getPrimary()),
            str::stream() << "The primary shard for database " << dbName << " does not exist");

        const bool mustReplaceEntryDueToUpgradeOrDowngrade = previousDbType &&
            (previousDbType->getVersion().getTimestamp().is_initialized() !=
             newDb.getVersion().getTimestamp().is_initialized());

        auto newDbVersion = mustReplaceEntryDueToUpgradeOrDowngrade
            ? ComparableDatabaseVersion::makeComparableDatabaseVersionForForcedRefresh(
                  newDb.getVersion())
            : ComparableDatabaseVersion::makeComparableDatabaseVersion(newDb.getVersion());

        LOGV2_FOR_CATALOG_REFRESH(24101,
                                  1,
                                  "Refreshed cached database entry",
                                  "db"_attr = dbName,
                                  "newDbVersion"_attr = newDbVersion.toBSONForLogging(),
                                  "oldDbVersion"_attr = previousDbVersion.toBSONForLogging(),
                                  "duration"_attr = Milliseconds(t.millis()));
        return CatalogCache::DatabaseCache::LookupResult(std::move(newDb), std::move(newDbVersion));
    } catch (const DBException& ex) {
        LOGV2_FOR_CATALOG_REFRESH(24100,
                                  1,
                                  "Error refreshing cached database entry",
                                  "db"_attr = dbName,
                                  "duration"_attr = Milliseconds(t.millis()),
                                  "error"_attr = redact(ex));
        if (ex.code() == ErrorCodes::NamespaceNotFound) {
            return CatalogCache::DatabaseCache::LookupResult(boost::none, previousDbVersion);
        }
        throw;
    }
}

CatalogCache::CollectionCache::CollectionCache(ServiceContext* service,
                                               ThreadPoolInterface& threadPool,
                                               CatalogCacheLoader& catalogCacheLoader)
    : ReadThroughCache(_mutex,
                       service,
                       threadPool,
                       [this](OperationContext* opCtx,
                              const NamespaceString& nss,
                              const ValueHandle& collectionHistory,
                              const ComparableChunkVersion& previousChunkVersion) {
                           return _lookupCollection(
                               opCtx, nss, collectionHistory, previousChunkVersion);
                       },
                       kCollectionCacheSize),
      _catalogCacheLoader(catalogCacheLoader) {}

void CatalogCache::CollectionCache::reportStats(BSONObjBuilder* builder) const {
    _stats.report(builder);
}

void CatalogCache::CollectionCache::_updateRefreshesStats(const bool isIncremental,
                                                          const bool add) {
    if (add) {
        if (isIncremental) {
            _stats.numActiveIncrementalRefreshes.addAndFetch(1);
            _stats.countIncrementalRefreshesStarted.addAndFetch(1);
        } else {
            _stats.numActiveFullRefreshes.addAndFetch(1);
            _stats.countFullRefreshesStarted.addAndFetch(1);
        }
    } else {
        if (isIncremental) {
            _stats.numActiveIncrementalRefreshes.subtractAndFetch(1);
        } else {
            _stats.numActiveFullRefreshes.subtractAndFetch(1);
        }
    }
}

void CatalogCache::CollectionCache::Stats::report(BSONObjBuilder* builder) const {
    builder->append("numActiveIncrementalRefreshes", numActiveIncrementalRefreshes.load());
    builder->append("countIncrementalRefreshesStarted", countIncrementalRefreshesStarted.load());

    builder->append("numActiveFullRefreshes", numActiveFullRefreshes.load());
    builder->append("countFullRefreshesStarted", countFullRefreshesStarted.load());

    builder->append("countFailedRefreshes", countFailedRefreshes.load());
}

CatalogCache::CollectionCache::LookupResult CatalogCache::CollectionCache::_lookupCollection(
    OperationContext* opCtx,
    const NamespaceString& nss,
    const RoutingTableHistoryValueHandle& existingHistory,
    const ComparableChunkVersion& previousVersion) {
    const bool isIncremental(existingHistory && existingHistory->optRt);
    _updateRefreshesStats(isIncremental, true);
    blockCollectionCacheLookup.pauseWhileSet(opCtx);

    Timer t{};
    try {
        auto lookupVersion =
            isIncremental ? existingHistory->optRt->getVersion() : ChunkVersion::UNSHARDED();

        LOGV2_FOR_CATALOG_REFRESH(4619900,
                                  1,
                                  "Refreshing cached collection",
                                  "namespace"_attr = nss,
                                  "currentVersion"_attr = previousVersion.toBSONForLogging());

        auto collectionAndChunks = _catalogCacheLoader.getChunksSince(nss, lookupVersion).get();

        bool mustReplaceEntryDueToUpgradeOrDowngrade = false;
        auto newRoutingHistory = [&] {
            // If we have routing info already and it's for the same collection epoch, we're
            // updating. Otherwise, we're making a whole new routing table.
            if (isIncremental &&
                existingHistory->optRt->getVersion().epoch() == collectionAndChunks.epoch) {
                if (existingHistory->optRt->getVersion().getTimestamp().is_initialized() !=
                    collectionAndChunks.creationTime.is_initialized()) {
                    mustReplaceEntryDueToUpgradeOrDowngrade = true;
                    return existingHistory->optRt
                        ->makeUpdatedReplacingTimestamp(collectionAndChunks.creationTime)
                        .makeUpdated(collectionAndChunks.reshardingFields,
                                     collectionAndChunks.allowMigrations,
                                     collectionAndChunks.changedChunks);
                } else {
                    return existingHistory->optRt->makeUpdated(collectionAndChunks.reshardingFields,
                                                               collectionAndChunks.allowMigrations,
                                                               collectionAndChunks.changedChunks);
                }
            }

            auto defaultCollator = [&]() -> std::unique_ptr<CollatorInterface> {
                if (!collectionAndChunks.defaultCollation.isEmpty()) {
                    // The collation should have been validated upon collection creation
                    return uassertStatusOK(
                        CollatorFactoryInterface::get(opCtx->getServiceContext())
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
                                                collectionAndChunks.creationTime,
                                                std::move(collectionAndChunks.reshardingFields),
                                                collectionAndChunks.allowMigrations,
                                                collectionAndChunks.changedChunks);
        }();

        newRoutingHistory.setAllShardsRefreshed();

        // Check that the shards all match with what is on the config server
        std::set<ShardId> shardIds;
        newRoutingHistory.getAllShardIds(&shardIds);
        for (const auto& shardId : shardIds) {
            uassertStatusOKWithContext(Grid::get(opCtx)->shardRegistry()->getShard(opCtx, shardId),
                                       str::stream() << "Collection " << nss
                                                     << " references shard which does not exist");
        }

        auto newVersion = (mustReplaceEntryDueToUpgradeOrDowngrade)
            ? ComparableChunkVersion::makeComparableChunkVersionForForcedRefresh(
                  newRoutingHistory.getVersion())
            : ComparableChunkVersion::makeComparableChunkVersion(newRoutingHistory.getVersion());

        invariant(isIncremental == false ||
                      (existingHistory->optRt->getVersion().epoch() !=
                           newRoutingHistory.getVersion().epoch() ||
                       existingHistory->optRt->getVersion().isOlderThan(
                           newRoutingHistory.getVersion())) ||
                      (newRoutingHistory.sameAllowMigrations(*existingHistory->optRt) &&
                       newRoutingHistory.sameReshardingFields(*existingHistory->optRt)),
                  "Unconsistent routing table info value for collections of the same version");

        LOGV2_FOR_CATALOG_REFRESH(4619901,
                                  isIncremental || newVersion != previousVersion ? 0 : 1,
                                  "Refreshed cached collection",
                                  "namespace"_attr = nss,
                                  "newVersion"_attr = newVersion.toBSONForLogging(),
                                  "oldVersion"_attr = previousVersion.toBSONForLogging(),
                                  "duration"_attr = Milliseconds(t.millis()));
        _updateRefreshesStats(isIncremental, false);

        return LookupResult(OptionalRoutingTableHistory(std::move(newRoutingHistory)),
                            std::move(newVersion));
    } catch (const DBException& ex) {
        _stats.countFailedRefreshes.addAndFetch(1);
        _updateRefreshesStats(isIncremental, false);

        if (ex.code() == ErrorCodes::NamespaceNotFound) {
            LOGV2_FOR_CATALOG_REFRESH(4619902,
                                      0,
                                      "Collection has found to be unsharded after refresh",
                                      "namespace"_attr = nss,
                                      "duration"_attr = Milliseconds(t.millis()));

            return LookupResult(
                OptionalRoutingTableHistory(),
                ComparableChunkVersion::makeComparableChunkVersion(ChunkVersion::UNSHARDED()));
        }

        LOGV2_FOR_CATALOG_REFRESH(4619903,
                                  0,
                                  "Error refreshing cached collection",
                                  "namespace"_attr = nss,
                                  "duration"_attr = Milliseconds(t.millis()),
                                  "error"_attr = redact(ex));

        throw;
    }
}

}  // namespace mongo
