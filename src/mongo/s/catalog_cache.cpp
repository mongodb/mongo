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

#include "mongo/s/catalog_cache.h"

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/curop.h"
#include "mongo/db/query/collation/collator_factory_interface.h"
#include "mongo/db/repl/optime_with.h"
#include "mongo/logv2/log.h"
#include "mongo/s/catalog/type_collection.h"
#include "mongo/s/catalog/type_database_gen.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/grid.h"
#include "mongo/s/is_mongos.h"
#include "mongo/s/mongod_and_mongos_server_parameters_gen.h"
#include "mongo/s/shard_cannot_refresh_due_to_locks_held_exception.h"
#include "mongo/s/stale_exception.h"
#include "mongo/util/concurrency/with_lock.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/timer.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding


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

std::shared_ptr<RoutingTableHistory> createUpdatedRoutingTableHistory(
    OperationContext* opCtx,
    const NamespaceString& nss,
    bool isIncremental,
    const RoutingTableHistoryValueHandle& existingHistory,
    const CatalogCacheLoader::CollectionAndChangedChunks& collectionAndChunks) {
    // If a refresh doesn't find new information -> re-use the existing RoutingTableHistory
    if (isIncremental && collectionAndChunks.changedChunks.size() == 1 &&
        collectionAndChunks.changedChunks[0].getVersion() == existingHistory->optRt->getVersion()) {

        invariant(collectionAndChunks.allowMigrations == existingHistory->optRt->allowMigrations(),
                  str::stream() << "allowMigrations field of " << nss
                                << " collection changed without changing the collection version "
                                << existingHistory->optRt->getVersion().toString()
                                << ". Old value: " << existingHistory->optRt->allowMigrations()
                                << ", new value: " << collectionAndChunks.allowMigrations);

        const auto& oldReshardingFields = existingHistory->optRt->getReshardingFields();
        const auto& newReshardingFields = collectionAndChunks.reshardingFields;
        invariant(
            [&] {
                if (oldReshardingFields && newReshardingFields)
                    return oldReshardingFields->toBSON().woCompare(newReshardingFields->toBSON()) ==
                        0;
                else
                    return !oldReshardingFields && !newReshardingFields;
            }(),
            str::stream() << "reshardingFields field of " << nss
                          << " collection changed without changing the collection version "
                          << existingHistory->optRt->getVersion().toString()
                          << ". Old value: " << oldReshardingFields->toBSON()
                          << ", new value: " << newReshardingFields->toBSON());

        return existingHistory->optRt;
    }

    const auto maxChunkSize = [&]() -> boost::optional<uint64_t> {
        if (!collectionAndChunks.allowAutoSplit) {
            // maxChunkSize = 0 is an invalid chunkSize so we use it to detect noAutoSplit
            // on the steady-state path in incrementChunkOnInsertOrUpdate(...)
            return 0;
        }
        if (collectionAndChunks.maxChunkSizeBytes) {
            invariant(collectionAndChunks.maxChunkSizeBytes.value() > 0);
            return uint64_t(*collectionAndChunks.maxChunkSizeBytes);
        }
        return boost::none;
    }();

    auto newRoutingHistory = [&] {
        // If we have routing info already and it's for the same collection, we're updating.
        // Otherwise, we are making a whole new routing table.
        if (isIncremental &&
            existingHistory->optRt->getVersion().isSameCollection(
                {collectionAndChunks.epoch, collectionAndChunks.timestamp})) {
            return existingHistory->optRt->makeUpdated(collectionAndChunks.timeseriesFields,
                                                       collectionAndChunks.reshardingFields,
                                                       maxChunkSize,
                                                       collectionAndChunks.allowMigrations,
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
                                            *collectionAndChunks.uuid,
                                            KeyPattern(collectionAndChunks.shardKeyPattern),
                                            std::move(defaultCollator),
                                            collectionAndChunks.shardKeyIsUnique,
                                            collectionAndChunks.epoch,
                                            collectionAndChunks.timestamp,
                                            collectionAndChunks.timeseriesFields,
                                            std::move(collectionAndChunks.reshardingFields),
                                            maxChunkSize,
                                            collectionAndChunks.allowMigrations,
                                            collectionAndChunks.changedChunks);
    }();

    return std::make_shared<RoutingTableHistory>(std::move(newRoutingHistory));
}

}  // namespace

AtomicWord<uint64_t> ComparableDatabaseVersion::_disambiguatingSequenceNumSource{1ULL};
AtomicWord<uint64_t> ComparableDatabaseVersion::_forcedRefreshSequenceNumSource{1ULL};

ComparableDatabaseVersion ComparableDatabaseVersion::makeComparableDatabaseVersion(
    const boost::optional<DatabaseVersion>& version) {
    return ComparableDatabaseVersion(version,
                                     _disambiguatingSequenceNumSource.fetchAndAdd(1),
                                     _forcedRefreshSequenceNumSource.load());
}

ComparableDatabaseVersion
ComparableDatabaseVersion::makeComparableDatabaseVersionForForcedRefresh() {
    return ComparableDatabaseVersion(boost::none /* version */,
                                     _disambiguatingSequenceNumSource.fetchAndAdd(1),
                                     _forcedRefreshSequenceNumSource.addAndFetch(2) - 1);
}

void ComparableDatabaseVersion::setDatabaseVersion(const DatabaseVersion& version) {
    _dbVersion = version;
}

std::string ComparableDatabaseVersion::toString() const {
    BSONObjBuilder builder;
    if (_dbVersion)
        builder.append("dbVersion"_sd, _dbVersion->toBSON());
    else
        builder.append("dbVersion"_sd, "None");

    builder.append("disambiguatingSequenceNum"_sd,
                   static_cast<int64_t>(_disambiguatingSequenceNum));

    builder.append("forcedRefreshSequenceNum"_sd, static_cast<int64_t>(_forcedRefreshSequenceNum));

    return builder.obj().toString();
}

bool ComparableDatabaseVersion::operator==(const ComparableDatabaseVersion& other) const {
    if (_forcedRefreshSequenceNum != other._forcedRefreshSequenceNum)
        return false;  // Values created on two sides of a forced refresh sequence number are always
                       // considered different
    if (_forcedRefreshSequenceNum == 0)
        return true;  // Only default constructed values have _forcedRefreshSequenceNum == 0 and
                      // they are always equal

    // Relying on the boost::optional<DatabaseVersion>::operator== comparison
    return _dbVersion == other._dbVersion;
}

bool ComparableDatabaseVersion::operator<(const ComparableDatabaseVersion& other) const {
    if (_forcedRefreshSequenceNum < other._forcedRefreshSequenceNum)
        return true;  // Values created on two sides of a forced refresh sequence number are always
                      // considered different
    if (_forcedRefreshSequenceNum > other._forcedRefreshSequenceNum)
        return false;  // Values created on two sides of a forced refresh sequence number are always
                       // considered different
    if (_forcedRefreshSequenceNum == 0)
        return false;  // Only default constructed values have _forcedRefreshSequenceNum == 0 and
                       // they are always equal

    // If both versions are valid we rely on the underlying DatabaseVersion comparison
    if (_dbVersion && other._dbVersion)
        return _dbVersion < other._dbVersion;

    // Finally, we do a disambiguating sequence number comparison
    return _disambiguatingSequenceNum < other._disambiguatingSequenceNum;
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

    Timer t{};
    ScopeGuard finishTiming([&] {
        CurOp::get(opCtx)->debug().catalogCacheDatabaseLookupMillis += Milliseconds(t.millis());
    });

    try {
        auto dbEntry = _databaseCache.acquire(opCtx, dbName, CacheCausalConsistency::kLatestKnown);
        uassert(ErrorCodes::NamespaceNotFound,
                str::stream() << "database " << dbName << " not found",
                dbEntry);

        return dbEntry;
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

        Timer curOpTimer{};
        ScopeGuard finishTiming([&] {
            CurOp::get(opCtx)->debug().catalogCacheCollectionLookupMillis +=
                Milliseconds(curOpTimer.millis());
        });

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
                return ChunkManager(dbInfo->getPrimary(),
                                    dbInfo->getVersion(),
                                    collEntryFuture.get(opCtx),
                                    atClusterTime);
            } else {
                return Status{ShardCannotRefreshDueToLocksHeldInfo(nss),
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

                return ChunkManager(dbInfo->getPrimary(),
                                    dbInfo->getVersion(),
                                    std::move(collEntry),
                                    atClusterTime);
            } catch (const DBException& ex) {
                _stats.totalRefreshWaitTimeMicros.addAndFetch(t.micros());
                bool isCatalogCacheRetriableError = ex.isA<ErrorCategory::SnapshotError>() ||
                    ex.code() == ErrorCodes::ConflictingOperationInProgress ||
                    ex.code() == ErrorCodes::QueryPlanKilled;
                if (!isCatalogCacheRetriableError) {
                    return ex.toStatus();
                }

                LOGV2_FOR_CATALOG_REFRESH(4086500,
                                          0,
                                          "Collection refresh failed",
                                          "namespace"_attr = nss,
                                          "exception"_attr = redact(ex));
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
    _databaseCache.advanceTimeInStore(
        dbName, ComparableDatabaseVersion::makeComparableDatabaseVersionForForcedRefresh());
    return getDatabase(opCtx, dbName);
}

StatusWith<ChunkManager> CatalogCache::getCollectionRoutingInfoWithRefresh(
    OperationContext* opCtx, const NamespaceString& nss) {
    _collectionCache.advanceTimeInStore(
        nss, ComparableChunkVersion::makeComparableChunkVersionForForcedRefresh());
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
            ComparableDatabaseVersion::makeComparableDatabaseVersion(databaseVersion.value());
        LOGV2_FOR_CATALOG_REFRESH(4899101,
                                  2,
                                  "Registering new database version",
                                  "db"_attr = dbName,
                                  "version"_attr = version);
        _databaseCache.advanceTimeInStore(dbName, version);
    } else {
        _databaseCache.invalidateKey(dbName);
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
    const boost::optional<ShardVersion>& wantedVersion,
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

void CatalogCache::invalidateEntriesThatReferenceShard(const ShardId& shardId) {
    LOGV2_DEBUG(4997600,
                1,
                "Invalidating databases and collections referencing a specific shard",
                "shardId"_attr = shardId);

    _databaseCache.invalidateLatestCachedValueIf_IgnoreInProgress(
        [&](const std::string&, const DatabaseType& dbt) { return dbt.getPrimary() == shardId; });

    // Invalidate collections which contain data on this shard.
    _collectionCache.invalidateLatestCachedValueIf_IgnoreInProgress(
        [&](const NamespaceString&, const OptionalRoutingTableHistory& ort) {
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
    _databaseCache.invalidateKey(dbName);
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
    _databaseCache.invalidateKey(dbName);
}

void CatalogCache::invalidateCollectionEntry_LINEARIZABLE(const NamespaceString& nss) {
    _collectionCache.invalidateKey(nss);
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

    // This object will define the new time of the database info obtained by this refresh
    auto newDbVersion = ComparableDatabaseVersion::makeComparableDatabaseVersion(boost::none);

    Timer t{};
    try {
        auto newDb = _catalogCacheLoader.getDatabase(dbName).get();
        uassertStatusOKWithContext(
            Grid::get(opCtx)->shardRegistry()->getShard(opCtx, newDb.getPrimary()),
            str::stream() << "The primary shard for database " << dbName << " does not exist");

        newDbVersion.setDatabaseVersion(newDb.getVersion());

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
            return CatalogCache::DatabaseCache::LookupResult(boost::none, std::move(newDbVersion));
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

    // This object will define the new time of the routing info obtained by this refresh
    auto newComparableVersion =
        ComparableChunkVersion::makeComparableChunkVersion(ChunkVersion::UNSHARDED());

    Timer t{};
    try {
        auto lookupVersion =
            isIncremental ? existingHistory->optRt->getVersion() : ChunkVersion::UNSHARDED();

        LOGV2_FOR_CATALOG_REFRESH(4619900,
                                  1,
                                  "Refreshing cached collection",
                                  "namespace"_attr = nss,
                                  "lookupSinceVersion"_attr = lookupVersion,
                                  "timeInStore"_attr = previousVersion);

        auto collectionAndChunks = _catalogCacheLoader.getChunksSince(nss, lookupVersion).get();

        std::shared_ptr<RoutingTableHistory> newRoutingHistory = createUpdatedRoutingTableHistory(
            opCtx, nss, isIncremental, existingHistory, collectionAndChunks);
        invariant(newRoutingHistory);

        newRoutingHistory->setAllShardsRefreshed();

        // Check that the shards all match with what is on the config server
        std::set<ShardId> shardIds;
        newRoutingHistory->getAllShardIds(&shardIds);
        for (const auto& shardId : shardIds) {
            uassertStatusOKWithContext(Grid::get(opCtx)->shardRegistry()->getShard(opCtx, shardId),
                                       str::stream() << "Collection " << nss
                                                     << " references shard which does not exist");
        }

        const ChunkVersion newVersion = newRoutingHistory->getVersion();
        newComparableVersion.setChunkVersion(newVersion);

        LOGV2_FOR_CATALOG_REFRESH(4619901,
                                  isIncremental || newComparableVersion != previousVersion ? 0 : 1,
                                  "Refreshed cached collection",
                                  "namespace"_attr = nss,
                                  "lookupSinceVersion"_attr = lookupVersion,
                                  "newVersion"_attr = newComparableVersion,
                                  "timeInStore"_attr = previousVersion,
                                  "duration"_attr = Milliseconds(t.millis()));
        _updateRefreshesStats(isIncremental, false);

        return LookupResult(OptionalRoutingTableHistory(std::move(newRoutingHistory)),
                            std::move(newComparableVersion));
    } catch (const DBException& ex) {
        _stats.countFailedRefreshes.addAndFetch(1);
        _updateRefreshesStats(isIncremental, false);

        if (ex.code() == ErrorCodes::NamespaceNotFound) {
            LOGV2_FOR_CATALOG_REFRESH(4619902,
                                      0,
                                      "Collection has found to be unsharded after refresh",
                                      "namespace"_attr = nss,
                                      "duration"_attr = Milliseconds(t.millis()));

            return LookupResult(OptionalRoutingTableHistory(), std::move(newComparableVersion));
        } else if (ex.code() == ErrorCodes::InvalidOptions) {
            LOGV2_WARNING(5738000,
                          "This error could be due to the fact that the config server is running "
                          "an older version");
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
