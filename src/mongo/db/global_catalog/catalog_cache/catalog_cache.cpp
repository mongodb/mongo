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

#include "mongo/db/global_catalog/catalog_cache/catalog_cache.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/curop.h"
#include "mongo/db/feature_flag.h"
#include "mongo/db/global_catalog/catalog_cache/shard_cannot_refresh_due_to_locks_held_exception.h"
#include "mongo/db/global_catalog/sharding_catalog_client.h"
#include "mongo/db/global_catalog/type_collection.h"
#include "mongo/db/global_catalog/type_collection_common_types_gen.h"
#include "mongo/db/global_catalog/type_database_gen.h"
#include "mongo/db/keypattern.h"
#include "mongo/db/local_catalog/shard_role_api/transaction_resources.h"
#include "mongo/db/logical_time.h"
#include "mongo/db/query/collation/collator_factory_interface.h"
#include "mongo/db/query/collation/collator_interface.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/db/repl/read_concern_level.h"
#include "mongo/db/server_options.h"
#include "mongo/db/sharding_environment/grid.h"
#include "mongo/db/sharding_environment/mongod_and_mongos_server_parameters_gen.h"
#include "mongo/db/sharding_environment/sharding_feature_flags_gen.h"
#include "mongo/db/topology/cluster_role.h"
#include "mongo/db/topology/shard_registry.h"
#include "mongo/db/vector_clock/vector_clock.h"
#include "mongo/db/versioning_protocol/chunk_version.h"
#include "mongo/db/versioning_protocol/shard_version_factory.h"
#include "mongo/logv2/log.h"
#include "mongo/s/resharding/type_collection_fields_gen.h"
#include "mongo/util/duration.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/future.h"
#include "mongo/util/invalidating_lru_cache.h"
#include "mongo/util/str.h"
#include "mongo/util/timer.h"

#include <cstddef>
#include <cstdint>
#include <list>
#include <memory>
#include <set>
#include <vector>

#include <absl/container/node_hash_map.h>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>
#include <fmt/format.h>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding


namespace mongo {
using CollectionAndChangedChunks = CatalogCacheLoader::CollectionAndChangedChunks;
namespace {

MONGO_FAIL_POINT_DEFINE(blockCollectionCacheLookup);
MONGO_FAIL_POINT_DEFINE(blockDatabaseCacheLookup);

// How many times to try refreshing the routing info of a collection if the
// information loaded from the config server is found to be inconsistent.
const int kMaxInconsistentCollectionRefreshAttempts = 3;

std::shared_ptr<RoutingTableHistory> createUpdatedRoutingTableHistory(
    OperationContext* opCtx,
    const NamespaceString& nss,
    bool isIncremental,
    const RoutingTableHistoryValueHandle& existingHistory,
    const CollectionAndChangedChunks& collectionAndChunks) {
    // If a refresh doesn't find new information -> re-use the existing RoutingTableHistory
    if (isIncremental && collectionAndChunks.changedChunks.size() == 1 &&
        collectionAndChunks.changedChunks[0].getVersion() == existingHistory->optRt->getVersion()) {

        tassert(7032310,
                fmt::format("allowMigrations field of collection '{}' changed without changing the "
                            "collection placement version {}. Old value: {}, new value: {}",
                            nss.toStringForErrorMsg(),
                            existingHistory->optRt->getVersion().toString(),
                            existingHistory->optRt->allowMigrations(),
                            collectionAndChunks.allowMigrations),
                collectionAndChunks.allowMigrations == existingHistory->optRt->allowMigrations());

        const auto& oldReshardingFields = existingHistory->optRt->getReshardingFields();
        const auto& newReshardingFields = collectionAndChunks.reshardingFields;
        tassert(7032311,
                fmt::format("reshardingFields field of collection '{}' changed without changing "
                            "the collection placement version {}. Old value: {}, new value: {}",
                            nss.toStringForErrorMsg(),
                            existingHistory->optRt->getVersion().toString(),
                            oldReshardingFields ? oldReshardingFields->toBSON().toString() : "{}",
                            newReshardingFields ? newReshardingFields->toBSON().toString() : "{}"),
                [&] {
                    if (oldReshardingFields && newReshardingFields)
                        return oldReshardingFields->toBSON().woCompare(
                                   newReshardingFields->toBSON()) == 0;
                    else
                        return !oldReshardingFields && !newReshardingFields;
                }());

        return existingHistory->optRt;
    }

    auto newRoutingHistory = [&] {
        // If we have routing info already and it's for the same collection, we're updating.
        // Otherwise, we are making a whole new routing table.
        if (isIncremental &&
            existingHistory->optRt->getVersion().isSameCollection(
                {collectionAndChunks.epoch, collectionAndChunks.timestamp})) {
            return existingHistory->optRt->makeUpdated(collectionAndChunks.timeseriesFields,
                                                       collectionAndChunks.reshardingFields,
                                                       collectionAndChunks.allowMigrations,
                                                       collectionAndChunks.unsplittable,
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
                                            collectionAndChunks.unsplittable,
                                            std::move(defaultCollator),
                                            collectionAndChunks.shardKeyIsUnique,
                                            collectionAndChunks.epoch,
                                            collectionAndChunks.timestamp,
                                            collectionAndChunks.timeseriesFields,
                                            std::move(collectionAndChunks.reshardingFields),
                                            collectionAndChunks.allowMigrations,
                                            collectionAndChunks.changedChunks);
    }();

    return std::make_shared<RoutingTableHistory>(std::move(newRoutingHistory));
}

const OperationContext::Decoration<bool> routerShouldRelaxCollectionUUIDConsistencyCheck =
    OperationContext::declareDecoration<bool>();

}  // namespace

bool CollectionRoutingInfo::hasRoutingTable() const {
    return _cm.hasRoutingTable();
}

const ShardId& CollectionRoutingInfo::getDbPrimaryShardId() const {
    return _dbInfo->getPrimary();
}

const DatabaseVersion& CollectionRoutingInfo::getDbVersion() const {
    return _dbInfo->getVersion();
}

ShardVersion CollectionRoutingInfo::getCollectionVersion() const {
    ShardVersion sv = ShardVersionFactory::make(_cm);
    if (MONGO_unlikely(shouldIgnoreUuidMismatch)) {
        sv.setIgnoreShardingCatalogUuidMismatch();
    }
    return sv;
}

ShardVersion CollectionRoutingInfo::getShardVersion(const ShardId& shardId) const {
    auto sv = ShardVersionFactory::make(_cm, shardId);
    if (MONGO_unlikely(shouldIgnoreUuidMismatch)) {
        sv.setIgnoreShardingCatalogUuidMismatch();
    }
    return sv;
}

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

CatalogCache::CatalogCache(ServiceContext* const service,
                           std::shared_ptr<CatalogCacheLoader> databaseCacheLoader,
                           std::shared_ptr<CatalogCacheLoader> collectionCacheLoader,
                           bool cascadeDatabaseCacheLoaderShutdown,
                           bool cascadeCollectionCacheLoaderShutdown,
                           StringData kind)
    : _kind(kind),
      _executor([this] {
          ThreadPool::Options options;
          options.poolName = "CatalogCache" + (_kind.empty() ? "" : "::" + _kind);
          options.minThreads = 0;
          options.maxThreads = 6;
          return options;
      }()),
      _cascadeDatabaseCacheLoaderShutdown(cascadeDatabaseCacheLoaderShutdown),
      _cascadeCollectionCacheLoaderShutdown(cascadeCollectionCacheLoaderShutdown),
      _databaseCache(service, _executor, databaseCacheLoader),
      _collectionCache(service, _executor, collectionCacheLoader) {
    _executor.startup();
}

CatalogCache::CatalogCache(ServiceContext* const service,
                           std::shared_ptr<CatalogCacheLoader> cacheLoader,
                           StringData kind)
    : _kind(kind),
      _executor([this] {
          ThreadPool::Options options;
          options.poolName = "CatalogCache" + (_kind.empty() ? "" : "::" + _kind);
          options.minThreads = 0;
          options.maxThreads = 6;
          return options;
      }()),
      // As both caches points to the same cache loader, it is enough to shutDown once.
      _cascadeDatabaseCacheLoaderShutdown(true),
      _cascadeCollectionCacheLoaderShutdown(false),
      _databaseCache(service, _executor, cacheLoader),
      _collectionCache(service, _executor, cacheLoader) {
    _executor.startup();
}

CatalogCache::~CatalogCache() {
    // The executor is used by all the caches that correspond to the router role, so it must be
    // joined before these caches are destroyed, per the contract of ReadThroughCache.
    shutDownAndJoin();
}

void CatalogCache::shutDownAndJoin() {
    // The CatalogCache must be shuted down before shutting down the CatalogCacheLoader as the
    // CatalogCache may try to schedule work on CatalogCacheLoader and fail.
    _executor.shutdown();
    _executor.join();

    if (_cascadeDatabaseCacheLoaderShutdown) {
        _databaseCache.shutDown();
    }

    if (_cascadeCollectionCacheLoaderShutdown) {
        _collectionCache.shutDown();
    }
}

StatusWith<CachedDatabaseInfo> CatalogCache::getDatabase(OperationContext* opCtx,
                                                         const DatabaseName& dbName) {
    return _getDatabase(opCtx, dbName);
}

StatusWith<CachedDatabaseInfo> CatalogCache::_getDatabase(OperationContext* opCtx,
                                                          const DatabaseName& dbName,
                                                          bool allowLocks) {
    tassert(7032313,
            "Do not hold a lock while refreshing the catalog cache. Doing so would potentially "
            "hold the lock during a network call, and can lead to a deadlock as described in "
            "SERVER-37398.",
            allowLocks || !shard_role_details::getLocker(opCtx) ||
                !shard_role_details::getLocker(opCtx)->isLocked());

    if (dbName.isAdminDB() || dbName.isConfigDB()) {
        // The 'admin' and 'config' databases are known to always be in the config server, there is
        // no need to make a request to the DatabaseCache. This prevents a potential deadlock on
        // config shard nodes issuing aggregations on 'config' to themselves during a refresh, and
        // finding the CatalogCache thread pool is full when the aggregation itself needs to refresh
        // the cache (i.e. $unionWith).
        return CachedDatabaseInfo{
            DatabaseType(dbName, ShardId::kConfigServerId, DatabaseVersion::makeFixed())};
    }

    Timer t{};
    ScopeGuard finishTiming([&] {
        CurOp::get(opCtx)->debug().catalogCacheDatabaseLookupMillis += Milliseconds(t.millis());
    });

    try {
        auto dbEntryFuture =
            _databaseCache.acquireAsync(dbName, CacheCausalConsistency::kLatestKnown);

        if (allowLocks) {
            // When allowLocks is true we may be holding a lock, so we don't want to block the
            // current thread: if the future is ready let's use it, otherwise return an error.
            if (dbEntryFuture.isReady()) {
                return dbEntryFuture.get(opCtx);
            } else {
                // This error only contains the database name and must be handled by any callers of
                // _getDatabase with the potential for allowLocks to be true. The caller should
                // convert this to ErrorCodes::ShardCannotRefreshDueToLocksHeld with the full
                // namespace.
                return Status{ShardCannotRefreshDueToLocksHeldInfo(NamespaceString(dbName)),
                              "Database info refresh did not complete"};
            }
        }

        // From this point we can guarantee that allowLocks is false.
        auto dbEntry = dbEntryFuture.get(opCtx);
        uassert(ErrorCodes::NamespaceNotFound,
                str::stream() << "database " << dbName.toStringForErrorMsg() << " not found",
                dbEntry);

        return dbEntry;
    } catch (const DBException& ex) {
        return ex.toStatus();
    }
}

StatusWith<CachedDatabaseInfo> CatalogCache::_getDatabaseForCollectionRoutingInfo(
    OperationContext* opCtx, const NamespaceString& nss, bool allowLocks) {
    tassert(10271000,
            "Do not hold a lock while refreshing the catalog cache. Doing so would potentially "
            "hold the lock during a network call, and can lead to a deadlock as described in "
            "SERVER-37398.",
            allowLocks || !shard_role_details::getLocker(opCtx)->isLocked());

    auto swDbInfo = _getDatabase(opCtx, nss.dbName(), allowLocks);
    if (!swDbInfo.isOK()) {
        if (swDbInfo == ErrorCodes::ShardCannotRefreshDueToLocksHeld) {
            // Since collection refreshes always imply database refreshes, it is ok to transform
            // this error into a collection error rather than a database error.
            auto dbRefreshInfo =
                swDbInfo.getStatus().extraInfo<ShardCannotRefreshDueToLocksHeldInfo>();
            LOGV2_DEBUG(7850500,
                        2,
                        "Adding collection name to ShardCannotRefreshDueToLocksHeld error",
                        "dbName"_attr = dbRefreshInfo->getNss().dbName(),
                        "nss"_attr = nss);
            return Status{ShardCannotRefreshDueToLocksHeldInfo(nss),
                          "Routing info refresh did not complete"};
        } else if (swDbInfo == ErrorCodes::NamespaceNotFound) {
            LOGV2_FOR_CATALOG_REFRESH(
                4947103,
                2,
                "Invalidating cached collection entry because its database has been dropped",
                logAttrs(nss));
            invalidateCollectionEntry_LINEARIZABLE(nss);
        }
        return swDbInfo.getStatus();
    }
    return swDbInfo;
}

StatusWith<ChunkManager> CatalogCache::_getCollectionPlacementInfoAt(
    OperationContext* opCtx,
    const NamespaceString& nss,
    boost::optional<Timestamp> atClusterTime,
    bool allowLocks) {
    tassert(7032314,
            "Do not hold a lock while refreshing the catalog cache. Doing so would potentially "
            "hold the lock during a network call, and can lead to a deadlock as described in "
            "SERVER-37398.",
            allowLocks || !shard_role_details::getLocker(opCtx)->isLocked());

    try {
        Timer curOpTimer{};
        ScopeGuard finishTiming([&] {
            CurOp::get(opCtx)->debug().catalogCacheCollectionLookupMillis +=
                Milliseconds(curOpTimer.millis());
        });

        if (nss.isNamespaceAlwaysUntracked()) {
            // If the collection is known to always be untracked, there is no need to request it to
            // the CollectionCache.
            return ChunkManager(OptionalRoutingTableHistory(), atClusterTime);
        }

        auto collEntryFuture =
            _collectionCache.acquireAsync(nss, CacheCausalConsistency::kLatestKnown);

        if (allowLocks) {
            // When allowLocks is true we may be holding a lock, so we don't
            // want to block the current thread: if the future is ready let's
            // use it, otherwise return an error

            if (collEntryFuture.isReady()) {
                return ChunkManager(collEntryFuture.get(opCtx), atClusterTime);
            } else {
                return Status{ShardCannotRefreshDueToLocksHeldInfo(nss),
                              "Routing info refresh did not complete"};
            }
        }

        // From this point we can guarantee that allowLocks is false
        size_t acquireTries = 0;
        Timer t;

        while (true) {
            try {
                auto collEntry = collEntryFuture.get(opCtx);
                _stats.totalRefreshWaitTimeMicros.addAndFetch(t.micros());

                return ChunkManager(std::move(collEntry), atClusterTime);
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
                                          logAttrs(nss),
                                          "exception"_attr = redact(ex));
                acquireTries++;
                if (acquireTries == kMaxInconsistentCollectionRefreshAttempts) {
                    return ex.toStatus();
                }
            }

            collEntryFuture =
                _collectionCache.acquireAsync(nss, CacheCausalConsistency::kLatestKnown);
            t.reset();
        }
    } catch (const DBException& ex) {
        return ex.toStatus();
    }
}

StatusWith<CollectionRoutingInfo> CatalogCache::_getCollectionRoutingInfoAt(
    OperationContext* opCtx,
    const NamespaceString& nss,
    boost::optional<Timestamp> optAtClusterTime,
    bool allowLocks) {
    auto swDbInfo = _getDatabaseForCollectionRoutingInfo(opCtx, nss, allowLocks);
    if (!swDbInfo.isOK()) {
        return swDbInfo.getStatus();
    }

    auto swChunkManager = _getCollectionPlacementInfoAt(opCtx, nss, optAtClusterTime, allowLocks);
    if (!swChunkManager.isOK()) {
        return swChunkManager.getStatus();
    }

    auto cri =
        CollectionRoutingInfo{std::move(swChunkManager.getValue()), std::move(swDbInfo.getValue())};
    if (MONGO_unlikely(routerShouldRelaxCollectionUUIDConsistencyCheck(opCtx))) {
        cri.shouldIgnoreUuidMismatch = true;
    }
    return cri;
}

StatusWith<CollectionRoutingInfo> CatalogCache::getCollectionRoutingInfoAt(
    OperationContext* opCtx, const NamespaceString& nss, Timestamp atClusterTime, bool allowLocks) {
    return _getCollectionRoutingInfoAt(opCtx, nss, atClusterTime, allowLocks);
}

StatusWith<CollectionRoutingInfo> CatalogCache::getCollectionRoutingInfo(OperationContext* opCtx,
                                                                         const NamespaceString& nss,
                                                                         bool allowLocks) {
    return _getCollectionRoutingInfoAt(opCtx, nss, boost::none /* atClusterTime */, allowLocks);
}

void CatalogCache::_triggerPlacementVersionRefresh(const NamespaceString& nss) {
    _collectionCache.advanceTimeInStore(
        nss, ComparableChunkVersion::makeComparableChunkVersionForForcedRefresh());
}

StatusWith<ChunkManager> CatalogCache::getCollectionPlacementInfoWithRefresh(
    OperationContext* opCtx, const NamespaceString& nss) {
    try {
        _triggerPlacementVersionRefresh(nss);
        return _getCollectionPlacementInfoAt(opCtx, nss, boost::none /* atClusterTime */);
    } catch (const DBException& ex) {
        return ex.toStatus();
    }
}

void CatalogCache::onStaleDatabaseVersion(const DatabaseName& dbName,
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

void CatalogCache::onStaleCollectionVersion(const NamespaceString& nss,
                                            const boost::optional<ShardVersion>& wantedVersion) {
    _stats.countStaleConfigErrors.addAndFetch(1);

    const auto newChunkVersion = wantedVersion
        ? ComparableChunkVersion::makeComparableChunkVersion(wantedVersion->placementVersion())
        : ComparableChunkVersion::makeComparableChunkVersionForForcedRefresh();
    _collectionCache.advanceTimeInStore(nss, newChunkVersion);
}

void CatalogCache::advanceCollectionTimeInStore(const NamespaceString& nss,
                                                const ChunkVersion& newVersionInStore) {
    const auto newChunkVersion =
        ComparableChunkVersion::makeComparableChunkVersion(newVersionInStore);
    _collectionCache.advanceTimeInStore(nss, newChunkVersion);
}

void CatalogCache::advanceTimeInStoreForEntriesThatReferenceShard(const ShardId& shardId) {
    LOGV2_DEBUG(
        9927700,
        1,
        "Advancing the cached version for databases and collections referencing a specific shard",
        "shardId"_attr = shardId);

    const auto dbEntries = _databaseCache.peekLatestCachedIf(
        [&](const DatabaseName&, const DatabaseType& dbt) { return dbt.getPrimary() == shardId; });
    for (const auto& dbEntry : dbEntries) {
        LOGV2_DEBUG(9927701,
                    1,
                    "Advancing the cached version for a database",
                    "db"_attr = dbEntry->getDbName());

        _databaseCache.advanceTimeInStore(
            dbEntry->getDbName(),
            ComparableDatabaseVersion::makeComparableDatabaseVersionForForcedRefresh());
    }

    const auto collEntries = _collectionCache.peekLatestCachedIf(
        [&](const NamespaceString&, const OptionalRoutingTableHistory& ort) {
            if (!ort.optRt) {
                return false;
            }
            const auto& rt = *ort.optRt;

            std::set<ShardId> shardIds;
            rt.getAllShardIds(&shardIds);

            return shardIds.find(shardId) != shardIds.end();
        });
    for (const auto& collEntry : collEntries) {
        invariant(collEntry->optRt);
        const auto& rt = *collEntry->optRt;

        LOGV2_DEBUG(9927702,
                    1,
                    "Advancing the cached version for a collection",
                    "namespace"_attr = rt.nss());

        _collectionCache.advanceTimeInStore(
            rt.nss(), ComparableChunkVersion::makeComparableChunkVersionForForcedRefresh());
    }

    LOGV2(9927703,
          "Advanced the cached version for databases and collections referencing a specific shard",
          "shardId"_attr = shardId);
}

void CatalogCache::purgeDatabase(const DatabaseName& dbName) {
    _databaseCache.invalidateKey(dbName);
    _collectionCache.invalidateKeyIf(
        [&](const NamespaceString& nss) { return nss.dbName() == dbName; });
}

void CatalogCache::purgeAllDatabases() {
    _databaseCache.invalidateAll();
    _collectionCache.invalidateAll();
}

void CatalogCache::report(BSONObjBuilder* builder) const {
    BSONObjBuilder cacheStatsBuilder(
        builder->subobjStart("catalogCache" + (_kind.empty() ? "" : "::" + _kind)));

    const size_t numDatabaseEntries = _databaseCache.getCacheInfo().size();
    const size_t numCollectionEntries = _collectionCache.getCacheInfo().size();

    cacheStatsBuilder.append("numDatabaseEntries", static_cast<long long>(numDatabaseEntries));
    cacheStatsBuilder.append("numCollectionEntries", static_cast<long long>(numCollectionEntries));

    _stats.report(&cacheStatsBuilder);
    _collectionCache.reportStats(&cacheStatsBuilder);
}

void CatalogCache::invalidateDatabaseEntry_LINEARIZABLE(const DatabaseName& dbName) {
    _databaseCache.invalidateKey(dbName);
}

void CatalogCache::invalidateCollectionEntry_LINEARIZABLE(const NamespaceString& nss) {
    _collectionCache.invalidateKey(nss);
}

boost::optional<ChunkVersion> CatalogCache::peekCollectionCacheVersion(const NamespaceString& nss) {
    auto valueHandle = _collectionCache.peekLatestCached(nss);
    if (valueHandle && valueHandle->optRt) {
        return valueHandle->optRt->getVersion();
    } else {
        return boost::none;
    }
}

void CatalogCache::Stats::report(BSONObjBuilder* builder) const {
    builder->append("countStaleConfigErrors", countStaleConfigErrors.load());

    builder->append("totalRefreshWaitTimeMicros", totalRefreshWaitTimeMicros.load());
}

CatalogCache::DatabaseCache::DatabaseCache(ServiceContext* service,
                                           ThreadPoolInterface& threadPool,
                                           std::shared_ptr<CatalogCacheLoader> catalogCacheLoader)
    : ReadThroughCache(
          _mutex,
          service->getService(),
          threadPool,
          [this](OperationContext* opCtx,
                 const DatabaseName& dbName,
                 const ValueHandle& db,
                 const ComparableDatabaseVersion& previousDbVersion) {
              return _lookupDatabase(opCtx, dbName, db, previousDbVersion);
          },
          gCatalogCacheDatabaseMaxEntries),
      _catalogCacheLoader(catalogCacheLoader) {}

CatalogCache::DatabaseCache::LookupResult CatalogCache::DatabaseCache::_lookupDatabase(
    OperationContext* opCtx,
    const DatabaseName& dbName,
    const DatabaseTypeValueHandle& previousDbType,
    const ComparableDatabaseVersion& previousDbVersion) {
    if (MONGO_unlikely(blockDatabaseCacheLookup.shouldFail())) {
        LOGV2(8023400, "Hanging before refreshing cached database entry");
        blockDatabaseCacheLookup.pauseWhileSet();
    }
    // TODO (SERVER-34164): Track and increment stats for database refreshes

    LOGV2_FOR_CATALOG_REFRESH(24102, 2, "Refreshing cached database entry", "db"_attr = dbName);

    // This object will define the new time of the database info obtained by this refresh
    auto newDbVersion = ComparableDatabaseVersion::makeComparableDatabaseVersion(boost::none);

    Timer t{};
    try {
        auto newDb = _catalogCacheLoader->getDatabase(dbName).get();
        uassertStatusOKWithContext(
            Grid::get(opCtx)->shardRegistry()->getShard(opCtx, newDb.getPrimary()),
            str::stream() << "The primary shard for database " << dbName.toStringForErrorMsg()
                          << " does not exist");

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

CatalogCache::CollectionCache::CollectionCache(
    ServiceContext* service,
    ThreadPoolInterface& threadPool,
    std::shared_ptr<CatalogCacheLoader> catalogCacheLoader)
    : ReadThroughCache(
          _mutex,
          service->getService(),
          threadPool,
          [this](OperationContext* opCtx,
                 const NamespaceString& nss,
                 const ValueHandle& collectionHistory,
                 const ComparableChunkVersion& previousChunkVersion) {
              return _lookupCollection(opCtx, nss, collectionHistory, previousChunkVersion);
          },
          gCatalogCacheCollectionMaxEntries),
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
    const ComparableChunkVersion& timeInStore) {
    const bool isIncremental(existingHistory && existingHistory->optRt);
    _updateRefreshesStats(isIncremental, true);

    blockCollectionCacheLookup.executeIf(
        [&](const BSONObj& data) {
            LOGV2(9131800, "Hanging before refreshing cached collection entry", "nss"_attr = nss);
            blockCollectionCacheLookup.pauseWhileSet();
        },
        [&](const BSONObj& data) {
            if (data.isEmpty())
                return true;  // If there is no data, always hang.

            const auto fpNss = NamespaceStringUtil::parseFailPointData(data, "nss");
            return fpNss == nss || (fpNss.isDbOnly() && fpNss.isEqualDb(nss));
        });

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
                                  logAttrs(nss),
                                  "lookupSinceVersion"_attr = lookupVersion,
                                  "timeInStore"_attr = timeInStore);

        auto collectionAndChunks = _catalogCacheLoader->getChunksSince(nss, lookupVersion).get();

        std::shared_ptr<RoutingTableHistory> newRoutingHistory = createUpdatedRoutingTableHistory(
            opCtx, nss, isIncremental, existingHistory, collectionAndChunks);
        invariant(newRoutingHistory);

        // Check that the shards all match with what is on the config server
        std::set<ShardId> shardIds;
        newRoutingHistory->getAllShardIds(&shardIds);
        for (const auto& shardId : shardIds) {
            uassertStatusOKWithContext(Grid::get(opCtx)->shardRegistry()->getShard(opCtx, shardId),
                                       str::stream() << "Collection " << nss.toStringForErrorMsg()
                                                     << " references shard which does not exist");
        }

        const ChunkVersion newVersion = newRoutingHistory->getVersion();
        newComparableVersion.setChunkVersion(newVersion);

        // The log below is logged at debug(0) (equivalent to info level) only if the new placement
        // version is different than the one we already had (if any).
        LOGV2_FOR_CATALOG_REFRESH(
            4619901,
            (!isIncremental || newVersion != existingHistory->optRt->getVersion()) ? 0 : 1,
            "Refreshed cached collection",
            logAttrs(nss),
            "lookupSinceVersion"_attr = lookupVersion,
            "newVersion"_attr = newComparableVersion,
            "timeInStore"_attr = timeInStore,
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
                                      logAttrs(nss),
                                      "duration"_attr = Milliseconds(t.millis()));

            return LookupResult(OptionalRoutingTableHistory(), std::move(newComparableVersion));
        }

        LOGV2_FOR_CATALOG_REFRESH(4619903,
                                  0,
                                  "Error refreshing cached collection",
                                  logAttrs(nss),
                                  "duration"_attr = Milliseconds(t.millis()),
                                  "error"_attr = redact(ex));

        throw;
    }
}

RouterRelaxCollectionUUIDConsistencyCheckBlock::RouterRelaxCollectionUUIDConsistencyCheckBlock(
    OperationContext* opCtx)
    : _opCtx(opCtx) {
    routerShouldRelaxCollectionUUIDConsistencyCheck(opCtx) = true;
}

RouterRelaxCollectionUUIDConsistencyCheckBlock::~RouterRelaxCollectionUUIDConsistencyCheckBlock() {
    routerShouldRelaxCollectionUUIDConsistencyCheck(_opCtx) = false;
}

}  // namespace mongo
