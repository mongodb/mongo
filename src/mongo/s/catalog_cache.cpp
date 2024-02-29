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

#include <boost/none.hpp>
#include <boost/optional.hpp>
#include <cstddef>
#include <cstdint>
#include <fmt/format.h>
#include <list>
#include <memory>
#include <set>
#include <vector>

#include <absl/container/node_hash_map.h>
#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/cluster_role.h"
#include "mongo/db/curop.h"
#include "mongo/db/feature_flag.h"
#include "mongo/db/keypattern.h"
#include "mongo/db/logical_time.h"
#include "mongo/db/query/collation/collator_factory_interface.h"
#include "mongo/db/query/collation/collator_interface.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/db/repl/read_concern_level.h"
#include "mongo/db/server_options.h"
#include "mongo/db/transaction_resources.h"
#include "mongo/db/vector_clock.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_attr.h"
#include "mongo/logv2/log_component.h"
#include "mongo/logv2/redaction.h"
#include "mongo/s/catalog/sharding_catalog_client.h"
#include "mongo/s/catalog/type_collection.h"
#include "mongo/s/catalog/type_database_gen.h"
#include "mongo/s/catalog/type_index_catalog_gen.h"
#include "mongo/s/chunk_version.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/grid.h"
#include "mongo/s/index_version.h"
#include "mongo/s/mongod_and_mongos_server_parameters_gen.h"
#include "mongo/s/resharding/type_collection_fields_gen.h"
#include "mongo/s/shard_cannot_refresh_due_to_locks_held_exception.h"
#include "mongo/s/shard_version_factory.h"
#include "mongo/s/sharding_feature_flags_gen.h"
#include "mongo/s/type_collection_common_types_gen.h"
#include "mongo/util/decorable.h"
#include "mongo/util/duration.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/future.h"
#include "mongo/util/invalidating_lru_cache.h"
#include "mongo/util/str.h"
#include "mongo/util/timer.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding


namespace mongo {
namespace {

MONGO_FAIL_POINT_DEFINE(blockCollectionCacheLookup);
MONGO_FAIL_POINT_DEFINE(blockDatabaseCacheLookup);

// How many times to try refreshing the routing info or the index info of a collection if the
// information loaded from the config server is found to be inconsistent.
const int kMaxInconsistentCollectionRefreshAttempts = 3;

const int kDatabaseCacheSize = 10000;
const int kCollectionCacheSize = 10000;
const int kIndexCacheSize = 10000;

const OperationContext::Decoration<bool> operationShouldBlockBehindCatalogCacheRefresh =
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
                            oldReshardingFields->toBSON().toString(),
                            newReshardingFields->toBSON().toString()),
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

}  // namespace

ShardVersion CollectionRoutingInfo::getCollectionVersion() const {
    return ShardVersionFactory::make(
        cm, sii ? boost::make_optional(sii->getCollectionIndexes()) : boost::none);
}

ShardVersion CollectionRoutingInfo::getShardVersion(const ShardId& shardId) const {
    return ShardVersionFactory::make(
        cm, shardId, sii ? boost::make_optional(sii->getCollectionIndexes()) : boost::none);
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

CatalogCache::CatalogCache(ServiceContext* const service, CatalogCacheLoader& cacheLoader)
    : _cacheLoader(cacheLoader),
      _executor([] {
          ThreadPool::Options options;
          options.poolName = "CatalogCache";
          options.minThreads = 0;
          options.maxThreads = 6;
          return options;
      }()),
      _databaseCache(service, _executor, _cacheLoader),
      _collectionCache(service, _executor, _cacheLoader),
      _indexCache(service, _executor) {
    _executor.startup();
}

CatalogCache::~CatalogCache() {
    // The executor is used by all the caches that correspond to the router role, so it must be
    // joined before these caches are destroyed, per the contract of ReadThroughCache.
    shutDownAndJoin();
}

void CatalogCache::shutDownAndJoin() {
    _executor.shutdown();
    _executor.join();
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
        const auto swDbInfo = _getDatabase(opCtx, nss.dbName(), allowLocks);
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
                                          logAttrs(nss),
                                          "exception"_attr = redact(ex));
                acquireTries++;
                if (acquireTries == kMaxInconsistentCollectionRefreshAttempts) {
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

StatusWith<CollectionRoutingInfo> CatalogCache::getCollectionRoutingInfoAt(
    OperationContext* opCtx, const NamespaceString& nss, Timestamp atClusterTime) {
    try {
        auto cm = uassertStatusOK(_getCollectionPlacementInfoAt(opCtx, nss, atClusterTime));
        if (!cm.isSharded()) {
            // If the collection is unsharded, it cannot have global indexes so there is no need to
            // fetch the index information.
            return CollectionRoutingInfo{std::move(cm), boost::none};
        }
        auto sii = _getCollectionIndexInfoAt(opCtx, nss);
        return _retryUntilConsistentRoutingInfo(opCtx, nss, std::move(cm), std::move(sii));
    } catch (const DBException& ex) {
        return ex.toStatus();
    }
}

StatusWith<CollectionRoutingInfo> CatalogCache::getCollectionRoutingInfo(OperationContext* opCtx,
                                                                         const NamespaceString& nss,
                                                                         bool allowLocks) {
    try {
        auto cm =
            uassertStatusOK(_getCollectionPlacementInfoAt(opCtx, nss, boost::none, allowLocks));
        if (!cm.isSharded()) {
            // If the collection is unsharded, it cannot have global indexes so there is no need to
            // fetch the index information.
            return CollectionRoutingInfo{std::move(cm), boost::none};
        }
        auto sii = _getCollectionIndexInfoAt(opCtx, nss, allowLocks);
        return _retryUntilConsistentRoutingInfo(opCtx, nss, std::move(cm), std::move(sii));
    } catch (const DBException& ex) {
        return ex.toStatus();
    }
}

StatusWith<CollectionRoutingInfo> CatalogCache::_getCollectionRoutingInfoWithoutOptimization(
    OperationContext* opCtx, const NamespaceString& nss) {
    try {
        auto cm = uassertStatusOK(_getCollectionPlacementInfoAt(opCtx, nss, boost::none));
        auto sii = _getCollectionIndexInfoAt(opCtx, nss);
        return _retryUntilConsistentRoutingInfo(opCtx, nss, std::move(cm), std::move(sii));
    } catch (const DBException& ex) {
        return ex.toStatus();
    }
}

boost::optional<ShardingIndexesCatalogCache> CatalogCache::_getCollectionIndexInfoAt(
    OperationContext* opCtx, const NamespaceString& nss, bool allowLocks) {

    // (Ignore FCV check): It is okay to ignore FCV in mongos. This is a temporary solution to solve
    // performance issue when fetching index information in mongos.
    if (!feature_flags::gGlobalIndexesShardingCatalog.isEnabledAndIgnoreFCVUnsafe()) {
        return boost::none;
    }

    if (!allowLocks) {
        invariant(!shard_role_details::getLocker(opCtx)->isLocked(),
                  "Do not hold a lock while refreshing the catalog cache. Doing so would "
                  "potentially hold "
                  "the lock during a network call, and can lead to a deadlock as described in "
                  "SERVER-37398.");
    }

    const auto swDbInfo = _getDatabase(opCtx, nss.dbName(), allowLocks);
    if (!swDbInfo.isOK()) {
        if (swDbInfo == ErrorCodes::ShardCannotRefreshDueToLocksHeld) {
            // Since collection refreshes always imply database refreshes, it is ok to transform
            // this error into a collection error rather than a database error.
            auto dbRefreshInfo =
                swDbInfo.getStatus().extraInfo<ShardCannotRefreshDueToLocksHeldInfo>();
            LOGV2_DEBUG(7850501,
                        2,
                        "Adding collection name to ShardCannotRefreshDueToLocksHeld error",
                        "dbName"_attr = dbRefreshInfo->getNss().dbName(),
                        "nss"_attr = nss);
            uasserted(Status{ShardCannotRefreshDueToLocksHeldInfo(nss),
                             "Routing info refresh did not complete"});
        } else if (swDbInfo == ErrorCodes::NamespaceNotFound) {
            LOGV2_FOR_CATALOG_REFRESH(
                6686300,
                2,
                "Invalidating cached index entry because its database has been dropped",
                logAttrs(nss));
            invalidateIndexEntry_LINEARIZABLE(nss);
        }
        uasserted(swDbInfo.getStatus().code(),
                  str::stream() << "Error getting database info for index refresh: "
                                << swDbInfo.getStatus().reason());
    }

    Timer curOpTimer{};
    ScopeGuard finishTiming([&] {
        CurOp::get(opCtx)->debug().catalogCacheIndexLookupMillis +=
            Milliseconds(curOpTimer.millis());
    });

    const auto dbInfo = std::move(swDbInfo.getValue());

    auto indexEntryFuture = _indexCache.acquireAsync(nss, CacheCausalConsistency::kLatestKnown);

    if (allowLocks) {
        // When allowLocks is true we may be holding a lock, so we don't
        // want to block the current thread: if the future is ready let's
        // use it, otherwise return an error
        uassert(ShardCannotRefreshDueToLocksHeldInfo(nss),
                "Index info refresh did not complete",
                indexEntryFuture.isReady());
        return indexEntryFuture.get(opCtx)->optSii;
    }

    // From this point we can guarantee that allowLocks is false
    size_t acquireTries = 0;

    while (true) {
        try {
            auto indexEntry = indexEntryFuture.get(opCtx);

            return indexEntry->optSii;
        } catch (const DBException& ex) {
            bool isCatalogCacheRetriableError = ex.isA<ErrorCategory::SnapshotError>() ||
                ex.code() == ErrorCodes::ConflictingOperationInProgress ||
                ex.code() == ErrorCodes::QueryPlanKilled;
            if (!isCatalogCacheRetriableError) {
                throw;
            }

            LOGV2_FOR_CATALOG_REFRESH(
                6686301, 0, "Index refresh failed", logAttrs(nss), "exception"_attr = redact(ex));

            acquireTries++;
            if (acquireTries == kMaxInconsistentCollectionRefreshAttempts) {
                throw;
            }
        }

        indexEntryFuture = _indexCache.acquireAsync(nss, CacheCausalConsistency::kLatestKnown);
    }
}

StatusWith<CachedDatabaseInfo> CatalogCache::getDatabaseWithRefresh(OperationContext* opCtx,
                                                                    const DatabaseName& dbName) {
    _databaseCache.advanceTimeInStore(
        dbName, ComparableDatabaseVersion::makeComparableDatabaseVersionForForcedRefresh());
    return getDatabase(opCtx, dbName);
}

void CatalogCache::_triggerPlacementVersionRefresh(OperationContext* opCtx,
                                                   const NamespaceString& nss) {
    _collectionCache.advanceTimeInStore(
        nss, ComparableChunkVersion::makeComparableChunkVersionForForcedRefresh());
    setOperationShouldBlockBehindCatalogCacheRefresh(opCtx, true);
}

void CatalogCache::_triggerIndexVersionRefresh(OperationContext* opCtx,
                                               const NamespaceString& nss) {
    _indexCache.advanceTimeInStore(
        nss, ComparableIndexVersion::makeComparableIndexVersionForForcedRefresh());
    setOperationShouldBlockBehindCatalogCacheRefresh(opCtx, true);
}

StatusWith<CollectionRoutingInfo> CatalogCache::_retryUntilConsistentRoutingInfo(
    OperationContext* opCtx,
    const NamespaceString& nss,
    ChunkManager&& cm,
    boost::optional<ShardingIndexesCatalogCache>&& sii) {
    try {
        // A non-empty ShardingIndexesCatalogCache implies that the collection is sharded since
        // global indexes cannot be created on unsharded collections.
        while (sii && (!cm.isSharded() || !cm.uuidMatches(sii->getCollectionIndexes().uuid()))) {
            auto nextSii =
                uassertStatusOK(getCollectionRoutingInfoWithIndexRefresh(opCtx, nss)).sii;
            if (sii.is_initialized() && nextSii.is_initialized() &&
                sii->getCollectionIndexes() == nextSii->getCollectionIndexes()) {
                cm = uassertStatusOK(getCollectionRoutingInfoWithPlacementRefresh(opCtx, nss)).cm;
            }
            sii = std::move(nextSii);
        }
    } catch (const DBException& ex) {
        return ex.toStatus();
    }
    return CollectionRoutingInfo{std::move(cm), std::move(sii)};
}

StatusWith<CollectionRoutingInfo> CatalogCache::getCollectionRoutingInfoWithRefresh(
    OperationContext* opCtx, const NamespaceString& nss) {
    try {
        _triggerPlacementVersionRefresh(opCtx, nss);
        _triggerIndexVersionRefresh(opCtx, nss);
        return _getCollectionRoutingInfoWithoutOptimization(opCtx, nss);
    } catch (const DBException& ex) {
        return ex.toStatus();
    }
}

StatusWith<CollectionRoutingInfo> CatalogCache::getCollectionRoutingInfoWithPlacementRefresh(
    OperationContext* opCtx, const NamespaceString& nss) {
    try {
        _triggerPlacementVersionRefresh(opCtx, nss);
        return getCollectionRoutingInfo(opCtx, nss, false);
    } catch (const DBException& ex) {
        return ex.toStatus();
    }
}

StatusWith<CollectionRoutingInfo> CatalogCache::getCollectionRoutingInfoWithIndexRefresh(
    OperationContext* opCtx, const NamespaceString& nss) {
    try {
        _triggerIndexVersionRefresh(opCtx, nss);
        return _getCollectionRoutingInfoWithoutOptimization(opCtx, nss);
    } catch (const DBException& ex) {
        return ex.toStatus();
    }
}

CollectionRoutingInfo CatalogCache::getShardedCollectionRoutingInfo(OperationContext* opCtx,
                                                                    const NamespaceString& nss) {
    auto cri = uassertStatusOK(getCollectionRoutingInfo(opCtx, nss));
    uassert(ErrorCodes::NamespaceNotSharded,
            str::stream() << "Expected collection " << nss.toStringForErrorMsg()
                          << " to be sharded",
            cri.cm.isSharded());
    return cri;
}

StatusWith<CollectionRoutingInfo> CatalogCache::getShardedCollectionRoutingInfoWithRefresh(
    OperationContext* opCtx, const NamespaceString& nss) {
    try {
        auto cri = uassertStatusOK(getCollectionRoutingInfoWithRefresh(opCtx, nss));
        uassert(ErrorCodes::NamespaceNotSharded,
                str::stream() << "Expected collection " << nss.toStringForErrorMsg()
                              << " to be sharded",
                cri.cm.isSharded());
        return cri;
    } catch (const DBException& ex) {
        return ex.toStatus();
    }
}

StatusWith<CollectionRoutingInfo> CatalogCache::getShardedCollectionRoutingInfoWithPlacementRefresh(
    OperationContext* opCtx, const NamespaceString& nss) {
    try {
        auto cri = uassertStatusOK(getCollectionRoutingInfoWithPlacementRefresh(opCtx, nss));
        uassert(ErrorCodes::NamespaceNotSharded,
                str::stream() << "Expected collection " << nss.toStringForErrorMsg()
                              << " to be sharded",
                cri.cm.isSharded());
        return cri;
    } catch (const DBException& ex) {
        return ex.toStatus();
    }
}

CollectionRoutingInfo CatalogCache::getTrackedCollectionRoutingInfo(OperationContext* opCtx,
                                                                    const NamespaceString& nss) {
    auto cri = uassertStatusOK(getCollectionRoutingInfo(opCtx, nss));
    uassert(ErrorCodes::NamespaceNotFound,
            str::stream() << "Expected collection " << nss.toStringForErrorMsg()
                          << " to be tracked",
            cri.cm.hasRoutingTable());
    return cri;
}

StatusWith<CollectionRoutingInfo> CatalogCache::getTrackedCollectionRoutingInfoWithRefresh(
    OperationContext* opCtx, const NamespaceString& nss) {
    try {
        auto cri = uassertStatusOK(getCollectionRoutingInfoWithRefresh(opCtx, nss));
        uassert(ErrorCodes::NamespaceNotFound,
                str::stream() << "Expected collection " << nss.toStringForErrorMsg()
                              << " to be tracked",
                cri.cm.hasRoutingTable());
        return cri;
    } catch (const DBException& ex) {
        return ex.toStatus();
    }
}

StatusWith<CollectionRoutingInfo> CatalogCache::getTrackedCollectionRoutingInfoWithPlacementRefresh(
    OperationContext* opCtx, const NamespaceString& nss) {
    try {
        auto cri = uassertStatusOK(getCollectionRoutingInfoWithPlacementRefresh(opCtx, nss));
        uassert(ErrorCodes::NamespaceNotFound,
                str::stream() << "Expected collection " << nss.toStringForErrorMsg()
                              << " to be tracked",
                cri.cm.hasRoutingTable());
        return cri;
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
        ? ComparableChunkVersion::makeComparableChunkVersion(wantedVersion->placementVersion())
        : ComparableChunkVersion::makeComparableChunkVersionForForcedRefresh();

    const bool routingInfoTimeAdvanced = _collectionCache.advanceTimeInStore(nss, newChunkVersion);

    const auto newIndexVersion = wantedVersion
        ? ComparableIndexVersion::makeComparableIndexVersion(wantedVersion->indexVersion())
        : ComparableIndexVersion::makeComparableIndexVersionForForcedRefresh();

    _indexCache.advanceTimeInStore(nss, newIndexVersion);

    if (routingInfoTimeAdvanced && collectionEntry && collectionEntry->optRt) {
        // Shards marked stale will be reset on the next refresh.
        // We can mark the shard stale only if the time advanced, otherwise no refresh would happen
        // and the shard will remain marked stale.
        // Even if a concurrent refresh is happening this is still the old collectionEntry,
        // so it is safe to call setShardStale.
        collectionEntry->optRt->setShardStale(shardId);
    }
}

void CatalogCache::advanceCollectionTimeInStore(const NamespaceString& nss,
                                                const ChunkVersion& newVersionInStore) {
    const auto newChunkVersion =
        ComparableChunkVersion::makeComparableChunkVersion(newVersionInStore);
    _collectionCache.advanceTimeInStore(nss, newChunkVersion);
}


void CatalogCache::invalidateEntriesThatReferenceShard(const ShardId& shardId) {
    LOGV2_DEBUG(4997600,
                1,
                "Invalidating databases and collections referencing a specific shard",
                "shardId"_attr = shardId);

    _databaseCache.invalidateLatestCachedValueIf_IgnoreInProgress(
        [&](const DatabaseName&, const DatabaseType& dbt) { return dbt.getPrimary() == shardId; });

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
                        "Invalidating cached collection",
                        logAttrs(rt.nss()),
                        "shardId"_attr = shardId);
            return shardIds.find(shardId) != shardIds.end();
        });

    LOGV2(22648,
          "Finished invalidating databases and collections that reference specific shard",
          "shardId"_attr = shardId);
}

void CatalogCache::purgeDatabase(const DatabaseName& dbName) {
    _databaseCache.invalidateKey(dbName);
    _collectionCache.invalidateKeyIf(
        [&](const NamespaceString& nss) { return nss.dbName() == dbName; });
    _indexCache.invalidateKeyIf([&](const NamespaceString& nss) { return nss.dbName() == dbName; });
}

void CatalogCache::purgeAllDatabases() {
    _databaseCache.invalidateAll();
    _collectionCache.invalidateAll();
    _indexCache.invalidateAll();
}

void CatalogCache::report(BSONObjBuilder* builder) const {
    BSONObjBuilder cacheStatsBuilder(builder->subobjStart("catalogCache"));

    const size_t numDatabaseEntries = _databaseCache.getCacheInfo().size();
    const size_t numCollectionEntries = _collectionCache.getCacheInfo().size();
    const size_t numIndexEntries = _indexCache.getCacheInfo().size();

    cacheStatsBuilder.append("numDatabaseEntries", static_cast<long long>(numDatabaseEntries));
    cacheStatsBuilder.append("numCollectionEntries", static_cast<long long>(numCollectionEntries));
    cacheStatsBuilder.append("numIndexEntries", static_cast<long long>(numIndexEntries));

    _stats.report(&cacheStatsBuilder);
    _collectionCache.reportStats(&cacheStatsBuilder);
}

void CatalogCache::invalidateDatabaseEntry_LINEARIZABLE(const DatabaseName& dbName) {
    _databaseCache.invalidateKey(dbName);
}

void CatalogCache::invalidateCollectionEntry_LINEARIZABLE(const NamespaceString& nss) {
    _collectionCache.invalidateKey(nss);
}

void CatalogCache::invalidateIndexEntry_LINEARIZABLE(const NamespaceString& nss) {
    // (Ignore FCV check): It is okay to ignore FCV in mongos.
    if (!feature_flags::gGlobalIndexesShardingCatalog.isEnabledAndIgnoreFCVUnsafe()) {
        _indexCache.invalidateKey(nss);
    }
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
                                           CatalogCacheLoader& catalogCacheLoader)
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
          kDatabaseCacheSize),
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
        auto newDb = _catalogCacheLoader.getDatabase(dbName).get();
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

CatalogCache::CollectionCache::CollectionCache(ServiceContext* service,
                                               ThreadPoolInterface& threadPool,
                                               CatalogCacheLoader& catalogCacheLoader)
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
    const ComparableChunkVersion& timeInStore) {
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
                                  logAttrs(nss),
                                  "lookupSinceVersion"_attr = lookupVersion,
                                  "timeInStore"_attr = timeInStore);

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

CatalogCache::IndexCache::IndexCache(ServiceContext* service, ThreadPoolInterface& threadPool)
    : ReadThroughCache(
          _mutex,
          service->getService(),
          threadPool,
          [this](OperationContext* opCtx,
                 const NamespaceString& nss,
                 const ValueHandle& indexes,
                 const ComparableIndexVersion& previousIndexVersion) {
              return _lookupIndexes(opCtx, nss, indexes, previousIndexVersion);
          },
          kIndexCacheSize) {}

CatalogCache::IndexCache::LookupResult CatalogCache::IndexCache::_lookupIndexes(
    OperationContext* opCtx,
    const NamespaceString& nss,
    const ValueHandle& indexes,
    const ComparableIndexVersion& previousVersion) {

    // This object will define the new time of the index info obtained by this refresh
    ComparableIndexVersion newComparableVersion =
        ComparableIndexVersion::makeComparableIndexVersion(boost::none);

    try {
        LOGV2_FOR_CATALOG_REFRESH(6686302,
                                  1,
                                  "Refreshing cached indexes",
                                  logAttrs(nss),
                                  "timeInStore"_attr = previousVersion);

        const auto readConcern = [&]() -> repl::ReadConcernArgs {
            const auto vcTime = VectorClock::get(opCtx)->getTime();
            return {vcTime.configTime(), repl::ReadConcernLevel::kSnapshotReadConcern};
        }();
        auto collAndIndexes =
            Grid::get(opCtx)->catalogClient()->getCollectionAndShardingIndexCatalogEntries(
                opCtx, nss, readConcern);
        const auto& coll = collAndIndexes.first;
        const auto& indexVersion = coll.getIndexVersion();
        newComparableVersion.setCollectionIndexes(
            indexVersion ? boost::make_optional(indexVersion->indexVersion()) : boost::none);

        LOGV2_FOR_CATALOG_REFRESH(6686303,
                                  newComparableVersion != previousVersion ? 0 : 1,
                                  "Refreshed cached indexes",
                                  logAttrs(nss),
                                  "newVersion"_attr = newComparableVersion,
                                  "timeInStore"_attr = previousVersion);

        if (!indexVersion) {
            return LookupResult(OptionalShardingIndexCatalogInfo(),
                                std::move(newComparableVersion));
        }

        IndexCatalogTypeMap newIndexesMap;
        for (const auto& index : collAndIndexes.second) {
            newIndexesMap[index.getName()] = index;
        }

        return LookupResult(OptionalShardingIndexCatalogInfo(ShardingIndexesCatalogCache(
                                *indexVersion, std::move(newIndexesMap))),
                            std::move(newComparableVersion));
    } catch (const DBException& ex) {
        if (ex.code() == ErrorCodes::NamespaceNotFound) {
            LOGV2_FOR_CATALOG_REFRESH(
                7038200, 0, "Collection has found to be unsharded after refresh", logAttrs(nss));
            return LookupResult(OptionalShardingIndexCatalogInfo(),
                                std::move(newComparableVersion));
        }
        LOGV2_FOR_CATALOG_REFRESH(6686304,
                                  0,
                                  "Error refreshing cached indexes",
                                  logAttrs(nss),
                                  "error"_attr = redact(ex));
        throw;
    }
}
}  // namespace mongo
