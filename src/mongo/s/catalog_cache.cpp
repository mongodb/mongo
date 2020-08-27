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
#include "mongo/db/logical_clock.h"
#include "mongo/db/query/collation/collator_factory_interface.h"
#include "mongo/db/repl/optime_with.h"
#include "mongo/logv2/log.h"
#include "mongo/s/catalog/type_collection.h"
#include "mongo/s/catalog/type_database.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/database_version_helpers.h"
#include "mongo/s/grid.h"
#include "mongo/s/is_mongos.h"
#include "mongo/s/mongod_and_mongos_server_parameters_gen.h"
#include "mongo/s/stale_exception.h"
#include "mongo/util/concurrency/with_lock.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/timer.h"

namespace mongo {
const OperationContext::Decoration<bool> operationShouldBlockBehindCatalogCacheRefresh =
    OperationContext::declareDecoration<bool>();

const OperationContext::Decoration<bool> operationBlockedBehindCatalogCacheRefresh =
    OperationContext::declareDecoration<bool>();

namespace {

// How many times to try refreshing the routing info if the set of chunks loaded from the config
// server is found to be inconsistent.
const int kMaxInconsistentRoutingInfoRefreshAttempts = 3;

const int kDatabaseCacheSize = 10000;
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

            return existingRoutingInfo->makeUpdated(std::move(collectionAndChunks.reshardingFields),
                                                    collectionAndChunks.changedChunks);
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
                                            std::move(collectionAndChunks.reshardingFields),
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

CatalogCache::CatalogCache(ServiceContext* const service, CatalogCacheLoader& cacheLoader)
    : _cacheLoader(cacheLoader),
      _executor(std::make_shared<ThreadPool>([] {
          ThreadPool::Options options;
          options.poolName = "CatalogCache";
          options.minThreads = 0;
          options.maxThreads = 6;
          return options;
      }())),
      _databaseCache(service, *_executor, _cacheLoader) {
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
                                                         StringData dbName) {
    invariant(!opCtx->lockState() || !opCtx->lockState()->isLocked(),
              "Do not hold a lock while refreshing the catalog cache. Doing so would potentially "
              "hold the lock during a network call, and can lead to a deadlock as described in "
              "SERVER-37398.");
    try {
        // TODO SERVER-49724: Make ReadThroughCache support StringData keys
        auto dbEntry =
            _databaseCache.acquire(opCtx, dbName.toString(), CacheCausalConsistency::kLatestKnown);
        if (!dbEntry) {
            return {ErrorCodes::NamespaceNotFound,
                    str::stream() << "database " << dbName << " not found"};
        }
        const auto primaryShard = uassertStatusOKWithContext(
            Grid::get(opCtx)->shardRegistry()->getShard(opCtx, dbEntry->getPrimary()),
            str::stream() << "could not find the primary shard for database " << dbName);
        return {CachedDatabaseInfo(*dbEntry, std::move(primaryShard))};
    } catch (const DBException& ex) {
        return ex.toStatus();
    }
}

StatusWith<ChunkManager> CatalogCache::getCollectionRoutingInfo(OperationContext* opCtx,
                                                                const NamespaceString& nss) {
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


StatusWith<ChunkManager> CatalogCache::getCollectionRoutingInfoAt(OperationContext* opCtx,
                                                                  const NamespaceString& nss,
                                                                  Timestamp atClusterTime) {
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
            if (swDbInfo == ErrorCodes::NamespaceNotFound) {
                LOGV2_FOR_CATALOG_REFRESH(
                    4947102,
                    2,
                    "Invalidating cached collection entry because its database has been dropped",
                    "namespace"_attr = nss);
                purgeCollection(nss);
            }
            return {swDbInfo.getStatus(), refreshActionTaken};
        }

        const auto dbInfo = std::move(swDbInfo.getValue());

        stdx::unique_lock<Latch> ul(_mutex);

        auto collEntry = _createOrGetCollectionEntry(ul, nss);

        if (collEntry->needsRefresh &&
            (!gEnableFinerGrainedCatalogCacheRefresh || collEntry->epochHasChanged ||
             operationShouldBlockBehindCatalogCacheRefresh(opCtx))) {

            operationBlockedBehindCatalogCacheRefresh(opCtx) = true;

            auto refreshNotification = collEntry->refreshCompletionNotification;
            if (!refreshNotification) {
                refreshNotification = (collEntry->refreshCompletionNotification =
                                           std::make_shared<Notification<Status>>());
                _scheduleCollectionRefresh(ul, opCtx->getServiceContext(), collEntry, nss, 1);
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

        return {ChunkManager(dbInfo.primaryId(),
                             dbInfo.databaseVersion(),
                             collEntry->routingInfo,
                             atClusterTime),
                refreshActionTaken};
    }
}

StatusWith<CachedDatabaseInfo> CatalogCache::getDatabaseWithRefresh(OperationContext* opCtx,
                                                                    StringData dbName) {
    // TODO SERVER-49724: Make ReadThroughCache support StringData keys
    _databaseCache.invalidate(dbName.toString());
    return getDatabase(opCtx, dbName);
}

StatusWith<ChunkManager> CatalogCache::getCollectionRoutingInfoWithRefresh(
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

StatusWith<ChunkManager> CatalogCache::getShardedCollectionRoutingInfoWithRefresh(
    OperationContext* opCtx, const NamespaceString& nss) {
    auto swRoutingInfo = _getCollectionRoutingInfoWithForcedRefresh(opCtx, nss).statusWithInfo;
    if (!swRoutingInfo.isOK())
        return swRoutingInfo;

    auto cri(std::move(swRoutingInfo.getValue()));
    if (!cri.isSharded())
        return {ErrorCodes::NamespaceNotSharded,
                str::stream() << "Collection " << nss.ns() << " is not sharded."};

    return cri;
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
                                  "version"_attr = version);
        _databaseCache.advanceTimeInStore(dbName.toString(), version);
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
    ShardId shardId) {
    if (shardVersionsHaveMatchingEpoch(wantedVersion, receivedVersion)) {
        _createOrGetCollectionEntryAndMarkShardStale(nss, shardId);
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

void CatalogCache::invalidateShardForShardedCollection(const NamespaceString& nss,
                                                       const ShardId& staleShardId) {
    _createOrGetCollectionEntryAndMarkShardStale(nss, staleShardId);
}

void CatalogCache::invalidateEntriesThatReferenceShard(const ShardId& shardId) {
    LOGV2_DEBUG(4997600,
                1,
                "Invalidating databases and collections referencing a specific shard",
                "shardId"_attr = shardId);

    _databaseCache.invalidateCachedValueIf(
        [&](const DatabaseType& dbt) { return dbt.getPrimary() == shardId; });

    stdx::lock_guard<Latch> lg(_mutex);

    // Invalidate collections which contain data on this shard.
    for (const auto& [db, collInfoMap] : _collectionsByDb) {
        for (const auto& [collNs, collRoutingInfoEntry] : collInfoMap) {
            if (!collRoutingInfoEntry->needsRefresh && collRoutingInfoEntry->routingInfo) {
                // The set of shards on which this collection contains chunks.
                std::set<ShardId> shardsOwningDataForCollection;
                collRoutingInfoEntry->routingInfo->getAllShardIds(&shardsOwningDataForCollection);

                if (shardsOwningDataForCollection.find(shardId) !=
                    shardsOwningDataForCollection.end()) {
                    LOGV2_DEBUG(22647,
                                3,
                                "Invalidating cached collection {namespace} that has data "
                                "on shard {shardId}",
                                "Invalidating cached collection",
                                "namespace"_attr = collNs,
                                "shardId"_attr = shardId);

                    collRoutingInfoEntry->needsRefresh = true;
                    collRoutingInfoEntry->routingInfo->setShardStale(shardId);
                }
            }
        }
    }

    LOGV2(22648,
          "Finished invalidating databases and collections with data on shard: {shardId}",
          "Finished invalidating databases and collections that reference specific shard",
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
    _databaseCache.invalidate(dbName.toString());
    stdx::lock_guard<Latch> lg(_mutex);
    _collectionsByDb.erase(dbName);
}

void CatalogCache::purgeAllDatabases() {
    _databaseCache.invalidateAll();
    stdx::lock_guard<Latch> lg(_mutex);
    _collectionsByDb.clear();
}

void CatalogCache::report(BSONObjBuilder* builder) const {
    BSONObjBuilder cacheStatsBuilder(builder->subobjStart("catalogCache"));

    size_t numDatabaseEntries;
    size_t numCollectionEntries{0};
    {
        numDatabaseEntries = _databaseCache.getCacheInfo().size();
        stdx::lock_guard<Latch> ul(_mutex);
        for (const auto& entry : _collectionsByDb) {
            numCollectionEntries += entry.second.size();
        }
    }

    cacheStatsBuilder.append("numDatabaseEntries", static_cast<long long>(numDatabaseEntries));
    cacheStatsBuilder.append("numCollectionEntries", static_cast<long long>(numCollectionEntries));

    _stats.report(&cacheStatsBuilder);
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

void CatalogCache::_scheduleCollectionRefresh(WithLock lk,
                                              ServiceContext* service,
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

            LOGV2_OPTIONS(24103,
                          {logv2::LogComponent::kShardingCatalogRefresh},
                          "Error refreshing cached collection {namespace}; Took {duration} and "
                          "failed due to {error}",
                          "Error refreshing cached collection",
                          "namespace"_attr = nss,
                          "duration"_attr = Milliseconds(t.millis()),
                          "error"_attr = redact(status));
        } else if (routingInfoAfterRefresh) {
            const int logLevel =
                (!existingRoutingInfo ||
                 (existingRoutingInfo &&
                  routingInfoAfterRefresh->getVersion() != existingRoutingInfo->getVersion()))
                ? 0
                : 1;
            LOGV2_FOR_CATALOG_REFRESH(
                24104,
                logLevel,
                "Refreshed cached collection {namespace} to version {newVersion} from version "
                "{oldVersion}. Took {duration}",
                "Refreshed cached collection",
                "namespace"_attr = nss,
                "newVersion"_attr = routingInfoAfterRefresh->getVersion(),
                "oldVersion"_attr =
                    (existingRoutingInfo
                         ? (" from version " + existingRoutingInfo->getVersion().toString())
                         : ""),
                "duration"_attr = Milliseconds(t.millis()));
        } else {
            LOGV2_OPTIONS(24105,
                          {logv2::LogComponent::kShardingCatalogRefresh},
                          "Collection {namespace} was found to be unsharded after refresh that "
                          "took {duration}",
                          "Collection has found to be unsharded after refresh",
                          "namespace"_attr = nss,
                          "duration"_attr = Milliseconds(t.millis()));
        }
    };

    // Invoked if getChunksSince resulted in error or threw an exception
    const auto onRefreshFailed =
        [ this, service, collEntry, nss, refreshAttempt,
          onRefreshCompleted ](WithLock lk, const Status& status) noexcept {
        onRefreshCompleted(status, nullptr);

        // It is possible that the metadata is being changed concurrently, so retry the
        // refresh again
        if (status == ErrorCodes::ConflictingOperationInProgress &&
            refreshAttempt < kMaxInconsistentRoutingInfoRefreshAttempts) {
            _scheduleCollectionRefresh(lk, service, collEntry, nss, refreshAttempt + 1);
        } else {
            // Leave needsRefresh to true so that any subsequent get attempts will kick off
            // another round of refresh
            collEntry->refreshCompletionNotification->set(status);
            collEntry->refreshCompletionNotification = nullptr;
        }
    };

    const auto refreshCallback =
        [ this, service, collEntry, nss, existingRoutingInfo, onRefreshFailed, onRefreshCompleted ](
            StatusWith<CatalogCacheLoader::CollectionAndChangedChunks> swCollAndChunks) noexcept {

        ThreadClient tc("CatalogCache::collectionRefresh", service);
        auto opCtx = tc->makeOperationContext();

        std::shared_ptr<RoutingTableHistory> newRoutingInfo;
        try {
            newRoutingInfo = refreshCollectionRoutingInfo(
                opCtx.get(), nss, std::move(existingRoutingInfo), std::move(swCollAndChunks));

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

        setOperationShouldBlockBehindCatalogCacheRefresh(opCtx.get(), false);

        // TODO(SERVER-49876): remove clang-tidy NOLINT comments.
        if (existingRoutingInfo && newRoutingInfo &&  // NOLINT(bugprone-use-after-move)
            existingRoutingInfo->getVersion() ==      // NOLINT(bugprone-use-after-move)
                newRoutingInfo->getVersion()) {       // NOLINT(bugprone-use-after-move)
            // If the routingInfo hasn't changed, we need to manually reset stale shards.
            newRoutingInfo->setAllShardsRefreshed();
        }

        collEntry->routingInfo = std::move(newRoutingInfo);
    };

    const ChunkVersion startingCollectionVersion =
        (existingRoutingInfo ? existingRoutingInfo->getVersion() : ChunkVersion::UNSHARDED());

    LOGV2_FOR_CATALOG_REFRESH(
        24106,
        1,
        "Refreshing cached collection {namespace} with version {currentCollectionVersion}",
        "namespace"_attr = nss,
        "currentCollectionVersion"_attr = startingCollectionVersion);

    _cacheLoader.getChunksSince(nss, startingCollectionVersion)
        .thenRunOn(_executor)
        .getAsync(refreshCallback);

    // The routing info for this collection shouldn't change, as other threads may try to use the
    // CatalogCache while we are waiting for the refresh to complete.
    invariant(collEntry->routingInfo.get() == existingRoutingInfo.get());
}

void CatalogCache::_createOrGetCollectionEntryAndMarkEpochStale(const NamespaceString& nss) {
    stdx::lock_guard<Latch> lg(_mutex);
    auto collRoutingInfoEntry = _createOrGetCollectionEntry(lg, nss);
    collRoutingInfoEntry->needsRefresh = true;
    collRoutingInfoEntry->epochHasChanged = true;
}

void CatalogCache::_createOrGetCollectionEntryAndMarkShardStale(const NamespaceString& nss,
                                                                const ShardId& staleShardId) {
    stdx::lock_guard<Latch> lg(_mutex);
    auto collRoutingInfoEntry = _createOrGetCollectionEntry(lg, nss);
    collRoutingInfoEntry->needsRefresh = true;
    if (collRoutingInfoEntry->routingInfo) {
        collRoutingInfoEntry->routingInfo->setShardStale(staleShardId);
    }
}

void CatalogCache::_createOrGetCollectionEntryAndMarkAsNeedsRefresh(const NamespaceString& nss) {
    stdx::lock_guard<Latch> lg(_mutex);
    auto collRoutingInfoEntry = _createOrGetCollectionEntry(lg, nss);
    collRoutingInfoEntry->needsRefresh = true;
}

std::shared_ptr<CatalogCache::CollectionRoutingInfoEntry> CatalogCache::_createOrGetCollectionEntry(
    WithLock wl, const NamespaceString& nss) {
    auto& collectionsForDb = _collectionsByDb[nss.db()];
    if (!collectionsForDb.contains(nss.ns())) {
        // TODO SERVER-46199: ensure collections cache size is capped
        // currently no routine except for dropDatabase is removing cached collection entries and
        // the cache for a specific DB can grow indefinitely.
        collectionsForDb[nss.ns()] = std::make_shared<CollectionRoutingInfoEntry>();
    }

    return collectionsForDb[nss.ns()];
}

void CatalogCache::Stats::report(BSONObjBuilder* builder) const {
    builder->append("countStaleConfigErrors", countStaleConfigErrors.load());

    builder->append("totalRefreshWaitTimeMicros", totalRefreshWaitTimeMicros.load());

    builder->append("numActiveIncrementalRefreshes", numActiveIncrementalRefreshes.load());
    builder->append("countIncrementalRefreshesStarted", countIncrementalRefreshesStarted.load());

    builder->append("numActiveFullRefreshes", numActiveFullRefreshes.load());
    builder->append("countFullRefreshesStarted", countFullRefreshesStarted.load());

    builder->append("countFailedRefreshes", countFailedRefreshes.load());

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
                           return _lookupDatabase(opCtx, dbName, previousDbVersion);
                       },
                       kDatabaseCacheSize),
      _catalogCacheLoader(catalogCacheLoader) {}

CatalogCache::DatabaseCache::LookupResult CatalogCache::DatabaseCache::_lookupDatabase(
    OperationContext* opCtx,
    const std::string& dbName,
    const ComparableDatabaseVersion& previousDbVersion) {

    // TODO (SERVER-34164): Track and increment stats for database refreshes

    LOGV2_FOR_CATALOG_REFRESH(24102, 2, "Refreshing cached database entry", "db"_attr = dbName);

    Timer t{};
    try {
        auto newDb = _catalogCacheLoader.getDatabase(dbName).get();
        auto newDbVersion =
            ComparableDatabaseVersion::makeComparableDatabaseVersion(newDb.getVersion());
        LOGV2_FOR_CATALOG_REFRESH(24101,
                                  1,
                                  "Refreshed cached database entry",
                                  "db"_attr = dbName,
                                  "newDbVersion"_attr = newDbVersion,
                                  "oldDbVersion"_attr = previousDbVersion,
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

AtomicWord<uint64_t> ComparableDatabaseVersion::_localSequenceNumSource{1ULL};

ComparableDatabaseVersion ComparableDatabaseVersion::makeComparableDatabaseVersion(
    const DatabaseVersion& version) {
    return ComparableDatabaseVersion(version, _localSequenceNumSource.fetchAndAdd(1));
}

const DatabaseVersion& ComparableDatabaseVersion::getVersion() const {
    return _dbVersion;
}

uint64_t ComparableDatabaseVersion::getLocalSequenceNum() const {
    return _localSequenceNum;
}

BSONObj ComparableDatabaseVersion::toBSON() const {
    BSONObjBuilder builder;
    _dbVersion.getUuid().appendToBuilder(&builder, "uuid");
    builder.append("lastMod", _dbVersion.getLastMod());
    builder.append("localSequenceNum", std::to_string(_localSequenceNum));
    return builder.obj();
}

std::string ComparableDatabaseVersion::toString() const {
    return toBSON().toString();
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

AtomicWord<uint64_t> ComparableChunkVersion::_localSequenceNumSource{1ULL};

ComparableChunkVersion ComparableChunkVersion::makeComparableChunkVersion(
    const ChunkVersion& version) {
    return ComparableChunkVersion(version, _localSequenceNumSource.fetchAndAdd(1));
}

const ChunkVersion& ComparableChunkVersion::getVersion() const {
    return _chunkVersion;
}

uint64_t ComparableChunkVersion::getLocalSequenceNum() const {
    return _localSequenceNum;
}

BSONObj ComparableChunkVersion::toBSON() const {
    BSONObjBuilder builder;
    _chunkVersion.appendToCommand(&builder);
    builder.append("localSequenceNum", std::to_string(_localSequenceNum));
    return builder.obj();
}

std::string ComparableChunkVersion::toString() const {
    return toBSON().toString();
}

}  // namespace mongo
