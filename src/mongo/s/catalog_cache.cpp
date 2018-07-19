/**
 *    Copyright (C) 2015 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kSharding

#include "mongo/platform/basic.h"

#include "mongo/s/catalog_cache.h"

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/query/collation/collator_factory_interface.h"
#include "mongo/db/repl/optime_with.h"
#include "mongo/platform/unordered_set.h"
#include "mongo/s/catalog/sharding_catalog_client.h"
#include "mongo/s/catalog/type_collection.h"
#include "mongo/s/catalog/type_database.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/grid.h"
#include "mongo/util/concurrency/with_lock.h"
#include "mongo/util/log.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/timer.h"

namespace mongo {
namespace {

// How many times to try refreshing the routing info if the set of chunks loaded from the config
// server is found to be inconsistent.
const int kMaxInconsistentRoutingInfoRefreshAttempts = 3;

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
 * dropped or recreated concurrently, the caller must retry the reload up to some configurable
 * number of attempts.
 */
std::shared_ptr<ChunkManager> refreshCollectionRoutingInfo(
    OperationContext* opCtx,
    const NamespaceString& nss,
    std::shared_ptr<ChunkManager> existingRoutingInfo,
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
        return ChunkManager::makeNew(nss,
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
    try {
        return {CachedDatabaseInfo(_getDatabase(opCtx, dbName))};
    } catch (const DBException& ex) {
        return ex.toStatus();
    }
}

StatusWith<CachedCollectionRoutingInfo> CatalogCache::getCollectionRoutingInfo(
    OperationContext* opCtx, const NamespaceString& nss) {
    while (true) {
        std::shared_ptr<DatabaseInfoEntry> dbEntry;
        try {
            dbEntry = _getDatabase(opCtx, nss.db());
        } catch (const DBException& ex) {
            return ex.toStatus();
        }

        stdx::unique_lock<stdx::mutex> ul(_mutex);

        auto& collections = dbEntry->collections;

        auto it = collections.find(nss.ns());
        if (it == collections.end()) {
            auto shardStatus =
                Grid::get(opCtx)->shardRegistry()->getShard(opCtx, dbEntry->primaryShardId);
            if (!shardStatus.isOK()) {
                return {ErrorCodes::Error(40371),
                        str::stream() << "The primary shard for collection " << nss.ns()
                                      << " could not be loaded due to error "
                                      << shardStatus.getStatus().toString()};
            }

            return {CachedCollectionRoutingInfo(
                dbEntry->primaryShardId, nss, std::move(shardStatus.getValue()))};
        }

        auto& collEntry = it->second;

        if (collEntry.needsRefresh) {
            auto refreshNotification = collEntry.refreshCompletionNotification;
            if (!refreshNotification) {
                refreshNotification = (collEntry.refreshCompletionNotification =
                                           std::make_shared<Notification<Status>>());
                _scheduleCollectionRefresh(ul, dbEntry, std::move(collEntry.routingInfo), nss, 1);
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
                return refreshStatus;
            }

            // Once the refresh is complete, loop around to get the latest value
            continue;
        }

        return {CachedCollectionRoutingInfo(dbEntry->primaryShardId, collEntry.routingInfo)};
    }
}

StatusWith<CachedCollectionRoutingInfo> CatalogCache::getCollectionRoutingInfoWithRefresh(
    OperationContext* opCtx, const NamespaceString& nss) {
    invalidateShardedCollection(nss);
    return getCollectionRoutingInfo(opCtx, nss);
}

StatusWith<CachedCollectionRoutingInfo> CatalogCache::getShardedCollectionRoutingInfoWithRefresh(
    OperationContext* opCtx, const NamespaceString& nss) {
    invalidateShardedCollection(nss);

    auto routingInfoStatus = getCollectionRoutingInfo(opCtx, nss);
    if (routingInfoStatus.isOK() && !routingInfoStatus.getValue().cm()) {
        return {ErrorCodes::NamespaceNotSharded,
                str::stream() << "Collection " << nss.ns() << " is not sharded."};
    }

    return routingInfoStatus;
}

void CatalogCache::onStaleConfigError(CachedCollectionRoutingInfo&& ccriToInvalidate) {
    _stats.countStaleConfigErrors.addAndFetch(1);

    // Ensure the move constructor of CachedCollectionRoutingInfo is invoked in order to clear the
    // input argument so it can't be used anymore
    auto ccri(ccriToInvalidate);

    if (!ccri._cm) {
        // Here we received a stale config error for a collection which we previously thought was
        // unsharded.
        invalidateShardedCollection(ccri._nss);
        return;
    }

    // Here we received a stale config error for a collection which we previously though was sharded
    stdx::lock_guard<stdx::mutex> lg(_mutex);

    auto it = _databases.find(NamespaceString(ccri._cm->getns()).db());
    if (it == _databases.end()) {
        // If the database does not exist, the collection must have been dropped so there is
        // nothing to invalidate. The getCollectionRoutingInfo will handle the reload of the
        // entire database and its collections.
        return;
    }

    auto& collections = it->second->collections;

    auto itColl = collections.find(ccri._cm->getns());
    if (itColl == collections.end()) {
        // If the collection does not exist, this means it must have been dropped since the last
        // time we retrieved a cache entry for it. Doing nothing in this case will cause the
        // next call to getCollectionRoutingInfo to return an unsharded collection.
        return;
    } else if (itColl->second.needsRefresh) {
        // Refresh has been scheduled for the collection already
        return;
    } else if (itColl->second.routingInfo->getVersion() == ccri._cm->getVersion()) {
        // If the versions match, the last version of the routing information that we used is no
        // longer valid, so trigger a refresh.
        itColl->second.needsRefresh = true;
    }
}

void CatalogCache::invalidateShardedCollection(const NamespaceString& nss) {
    stdx::lock_guard<stdx::mutex> lg(_mutex);

    auto it = _databases.find(nss.db());
    if (it == _databases.end()) {
        return;
    }

    it->second->collections[nss.ns()].needsRefresh = true;
}

void CatalogCache::purgeDatabase(StringData dbName) {
    stdx::lock_guard<stdx::mutex> lg(_mutex);
    _databases.erase(dbName);
}

void CatalogCache::purgeAllDatabases() {
    stdx::lock_guard<stdx::mutex> lg(_mutex);
    _databases.clear();
}

void CatalogCache::report(BSONObjBuilder* builder) const {
    BSONObjBuilder cacheStatsBuilder(builder->subobjStart("catalogCache"));

    size_t numDatabaseEntries;
    size_t numCollectionEntries{0};
    {
        stdx::lock_guard<stdx::mutex> ul(_mutex);
        numDatabaseEntries = _databases.size();
        for (const auto& entry : _databases) {
            numCollectionEntries += entry.second->collections.size();
        }
    }

    cacheStatsBuilder.append("numDatabaseEntries", static_cast<long long>(numDatabaseEntries));
    cacheStatsBuilder.append("numCollectionEntries", static_cast<long long>(numCollectionEntries));

    _stats.report(&cacheStatsBuilder);
}

std::shared_ptr<CatalogCache::DatabaseInfoEntry> CatalogCache::_getDatabase(OperationContext* opCtx,
                                                                            StringData dbName) {
    stdx::lock_guard<stdx::mutex> lg(_mutex);

    auto it = _databases.find(dbName);
    if (it != _databases.end()) {
        return it->second;
    }

    const auto catalogClient = Grid::get(opCtx)->catalogClient();

    const auto dbNameCopy = dbName.toString();

    // Load the database entry
    const auto opTimeWithDb = uassertStatusOK(catalogClient->getDatabase(
        opCtx, dbNameCopy, repl::ReadConcernLevel::kMajorityReadConcern));
    const auto& dbDesc = opTimeWithDb.value;

    // Load the sharded collections entries
    std::vector<CollectionType> collections;
    repl::OpTime collLoadConfigOptime;
    uassertStatusOK(
        catalogClient->getCollections(opCtx, &dbNameCopy, &collections, &collLoadConfigOptime));

    StringMap<CollectionRoutingInfoEntry> collectionEntries;
    for (const auto& coll : collections) {
        if (coll.getDropped()) {
            continue;
        }

        collectionEntries[coll.getNs().ns()].needsRefresh = true;
    }

    return _databases[dbName] = std::shared_ptr<DatabaseInfoEntry>(new DatabaseInfoEntry{
               dbDesc.getPrimary(), dbDesc.getSharded(), std::move(collectionEntries)});
}

void CatalogCache::_scheduleCollectionRefresh(WithLock lk,
                                              std::shared_ptr<DatabaseInfoEntry> dbEntry,
                                              std::shared_ptr<ChunkManager> existingRoutingInfo,
                                              NamespaceString const& nss,
                                              int refreshAttempt) {
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
    const auto onRefreshCompleted = [ this, t = Timer(), nss, isIncremental, existingRoutingInfo ](
        const Status& status, ChunkManager* routingInfoAfterRefresh) {
        if (isIncremental) {
            _stats.numActiveIncrementalRefreshes.subtractAndFetch(1);
        } else {
            _stats.numActiveFullRefreshes.subtractAndFetch(1);
        }

        if (!status.isOK()) {
            _stats.countFailedRefreshes.addAndFetch(1);

            LOG_CATALOG_REFRESH(0) << "Refresh for collection " << nss << " took " << t.millis()
                                   << " ms and failed" << causedBy(redact(status));
        } else if (routingInfoAfterRefresh) {
            const int logLevel = (!existingRoutingInfo || (existingRoutingInfo &&
                                                           routingInfoAfterRefresh->getVersion() !=
                                                               existingRoutingInfo->getVersion()))
                ? 0
                : 1;
            LOG_CATALOG_REFRESH(logLevel)
                << "Refresh for collection " << nss.toString()
                << (existingRoutingInfo
                        ? (" from version " + existingRoutingInfo->getVersion().toString())
                        : "")
                << " to version " << routingInfoAfterRefresh->getVersion().toString() << " took "
                << t.millis() << " ms";
        } else {
            LOG_CATALOG_REFRESH(0) << "Refresh for collection " << nss << " took " << t.millis()
                                   << " ms and found the collection is not sharded";
        }
    };

    // Invoked if getChunksSince resulted in error
    const auto onRefreshFailed = [ this, dbEntry, nss, refreshAttempt, onRefreshCompleted ](
        WithLock lk, const Status& status) noexcept {
        onRefreshCompleted(status, nullptr);

        auto& collections = dbEntry->collections;
        auto it = collections.find(nss.ns());
        invariant(it != collections.end());
        auto& collEntry = it->second;

        // It is possible that the metadata is being changed concurrently, so retry the
        // refresh again
        if (status == ErrorCodes::ConflictingOperationInProgress &&
            refreshAttempt < kMaxInconsistentRoutingInfoRefreshAttempts) {
            _scheduleCollectionRefresh(lk, dbEntry, nullptr, nss, refreshAttempt + 1);
        } else {
            // Leave needsRefresh to true so that any subsequent get attempts will kick off
            // another round of refresh
            collEntry.refreshCompletionNotification->set(status);
            collEntry.refreshCompletionNotification = nullptr;
        }
    };

    const auto refreshCallback =
        [ this, dbEntry, nss, existingRoutingInfo, onRefreshFailed, onRefreshCompleted ](
            OperationContext * opCtx,
            StatusWith<CatalogCacheLoader::CollectionAndChangedChunks> swCollAndChunks) noexcept {
        std::shared_ptr<ChunkManager> newRoutingInfo;
        try {
            newRoutingInfo = refreshCollectionRoutingInfo(
                opCtx, nss, std::move(existingRoutingInfo), std::move(swCollAndChunks));

            onRefreshCompleted(Status::OK(), newRoutingInfo.get());
        } catch (const DBException& ex) {
            stdx::lock_guard<stdx::mutex> lg(_mutex);
            onRefreshFailed(lg, ex.toStatus());
            return;
        }

        stdx::lock_guard<stdx::mutex> lg(_mutex);

        auto& collections = dbEntry->collections;
        auto it = collections.find(nss.ns());
        invariant(it != collections.end());
        auto& collEntry = it->second;

        collEntry.needsRefresh = false;
        collEntry.refreshCompletionNotification->set(Status::OK());
        collEntry.refreshCompletionNotification = nullptr;

        if (!newRoutingInfo) {
            collections.erase(it);
        } else {
            collEntry.routingInfo = std::move(newRoutingInfo);
        }
    };

    const ChunkVersion startingCollectionVersion =
        (existingRoutingInfo ? existingRoutingInfo->getVersion() : ChunkVersion::UNSHARDED());

    LOG_CATALOG_REFRESH(1) << "Refreshing chunks for collection " << nss << " based on version "
                           << startingCollectionVersion;

    try {
        _cacheLoader.getChunksSince(nss, startingCollectionVersion, refreshCallback);
    } catch (const DBException& ex) {
        const auto status = ex.toStatus();

        // ConflictingOperationInProgress errors trigger retry of the catalog cache reload logic. If
        // we failed to schedule the asynchronous reload, there is no point in doing another
        // attempt.
        invariant(status != ErrorCodes::ConflictingOperationInProgress);

        stdx::lock_guard<stdx::mutex> lg(_mutex);
        onRefreshFailed(lg, status);
    }
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

CachedDatabaseInfo::CachedDatabaseInfo(std::shared_ptr<CatalogCache::DatabaseInfoEntry> db)
    : _db(std::move(db)) {}

const ShardId& CachedDatabaseInfo::primaryId() const {
    return _db->primaryShardId;
}

bool CachedDatabaseInfo::shardingEnabled() const {
    return _db->shardingEnabled;
}

CachedCollectionRoutingInfo::CachedCollectionRoutingInfo(ShardId primaryId,
                                                         std::shared_ptr<ChunkManager> cm)
    : _primaryId(std::move(primaryId)), _cm(std::move(cm)) {}

CachedCollectionRoutingInfo::CachedCollectionRoutingInfo(ShardId primaryId,
                                                         NamespaceString nss,
                                                         std::shared_ptr<Shard> primary)
    : _primaryId(std::move(primaryId)), _nss(std::move(nss)), _primary(std::move(primary)) {}

}  // namespace mongo
