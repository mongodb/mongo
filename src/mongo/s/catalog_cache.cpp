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
#include "mongo/db/query/collation/collator_factory_interface.h"
#include "mongo/db/repl/optime_with.h"
#include "mongo/s/catalog/sharding_catalog_client.h"
#include "mongo/s/catalog/type_collection.h"
#include "mongo/s/catalog/type_database.h"
#include "mongo/s/chunk_diff.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/grid.h"
#include "mongo/stdx/memory.h"
#include "mongo/util/log.h"
#include "mongo/util/timer.h"

namespace mongo {
namespace {

// How many times to try refreshing the routing info if the set of chunks loaded from the config
// server is found to be inconsistent.
const int kMaxInconsistentRoutingInfoRefreshAttempts = 3;

/**
 * This is an adapter so we can use config diffs - mongos and mongod do them slightly differently.
 *
 * The mongos adapter here tracks all shards, and stores ranges by (max, Chunk) in the map.
 */
class CMConfigDiffTracker : public ConfigDiffTracker<std::shared_ptr<Chunk>> {
public:
    CMConfigDiffTracker(const NamespaceString& nss,
                        RangeMap* currMap,
                        ChunkVersion* maxVersion,
                        MaxChunkVersionMap* maxShardVersions)
        : ConfigDiffTracker<std::shared_ptr<Chunk>>(
              nss.ns(), currMap, maxVersion, maxShardVersions) {}

    bool isTracked(const ChunkType& chunk) const final {
        // Mongos tracks all shards
        return true;
    }

    bool isMinKeyIndexed() const final {
        return false;
    }

    std::pair<BSONObj, std::shared_ptr<Chunk>> rangeFor(OperationContext* opCtx,
                                                        const ChunkType& chunk) const final {
        return std::make_pair(chunk.getMax(), std::make_shared<Chunk>(chunk));
    }

    ShardId shardFor(OperationContext* opCtx, const ShardId& shardId) const final {
        const auto shard =
            uassertStatusOK(Grid::get(opCtx)->shardRegistry()->getShard(opCtx, shardId));
        return shard->getId();
    }
};

}  // namespace

CatalogCache::CatalogCache() = default;

CatalogCache::~CatalogCache() = default;

StatusWith<CachedDatabaseInfo> CatalogCache::getDatabase(OperationContext* opCtx,
                                                         StringData dbName) {
    stdx::lock_guard<stdx::mutex> lg(_mutex);

    try {
        return {CachedDatabaseInfo(_getDatabase_inlock(opCtx, dbName))};
    } catch (const DBException& ex) {
        return ex.toStatus();
    }
}

StatusWith<CachedCollectionRoutingInfo> CatalogCache::getCollectionRoutingInfo(
    OperationContext* opCtx, const NamespaceString& nss) {
    int numRefreshAttempts = 0;

    while (true) {
        stdx::unique_lock<stdx::mutex> ul(_mutex);

        std::shared_ptr<DatabaseInfoEntry> dbEntry;
        try {
            dbEntry = _getDatabase_inlock(opCtx, nss.db());
        } catch (const DBException& ex) {
            return ex.toStatus();
        }

        auto& collections = dbEntry->collections;

        auto it = collections.find(nss.ns());
        if (it == collections.end()) {
            auto shardStatus =
                Grid::get(opCtx)->shardRegistry()->getShard(opCtx, dbEntry->primaryShardId);
            if (!shardStatus.isOK()) {
                return {ErrorCodes::fromInt(40371),
                        str::stream() << "The primary shard for collection " << nss.ns()
                                      << " could not be loaded due to error "
                                      << shardStatus.getStatus().toString()};
            }

            return {CachedCollectionRoutingInfo(
                dbEntry->primaryShardId, nss, std::move(shardStatus.getValue()))};
        }

        auto& collEntry = it->second;

        if (collEntry.needsRefresh) {
            numRefreshAttempts++;

            try {
                auto newRoutingInfo =
                    refreshCollectionRoutingInfo(opCtx, nss, std::move(collEntry.routingInfo));
                if (newRoutingInfo == nullptr) {
                    collections.erase(it);

                    // Loop around so we can return an "unsharded" routing info
                    continue;
                }

                collEntry.routingInfo = std::move(newRoutingInfo);
                collEntry.needsRefresh = false;
            } catch (const DBException& ex) {
                // It is possible that the metadata is being changed concurrently, so retry the
                // refresh with a wait
                if (ex.getCode() == ErrorCodes::ConflictingOperationInProgress &&
                    numRefreshAttempts < kMaxInconsistentRoutingInfoRefreshAttempts) {
                    ul.unlock();

                    log() << "Metadata refresh for " << nss << " failed and will be retried"
                          << causedBy(redact(ex));

                    // Do the sleep outside of the mutex
                    sleepFor(Milliseconds(10) * numRefreshAttempts);
                    continue;
                }

                return ex.toStatus();
            }
        }

        return {CachedCollectionRoutingInfo(dbEntry->primaryShardId, collEntry.routingInfo)};
    }
}

StatusWith<CachedCollectionRoutingInfo> CatalogCache::getCollectionRoutingInfo(
    OperationContext* opCtx, StringData ns) {
    return getCollectionRoutingInfo(opCtx, NamespaceString(ns));
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

StatusWith<CachedCollectionRoutingInfo> CatalogCache::getShardedCollectionRoutingInfoWithRefresh(
    OperationContext* opCtx, StringData ns) {
    return getShardedCollectionRoutingInfoWithRefresh(opCtx, NamespaceString(ns));
}

void CatalogCache::onStaleConfigError(CachedCollectionRoutingInfo&& ccrt) {
    if (!ccrt._cm) {
        // Here we received a stale config error for a collection which we previously thought was
        // unsharded.
        invalidateShardedCollection(ccrt._nss);
        return;
    }

    // Here we received a stale config error for a collection which we previously though was sharded
    stdx::lock_guard<stdx::mutex> lg(_mutex);

    auto it = _databases.find(NamespaceString(ccrt._cm->getns()).db());
    if (it == _databases.end()) {
        // If the database does not exist, the collection must have been dropped so there is
        // nothing to invalidate. The getCollectionRoutingInfo will handle the reload of the
        // entire database and its collections.
        return;
    }

    auto& collections = it->second->collections;

    auto itColl = collections.find(ccrt._cm->getns());
    if (itColl == collections.end()) {
        // If the collection does not exist, this means it must have been dropped since the last
        // time we retrieved a cache entry for it. Doing nothing in this case will cause the
        // next call to getCollectionRoutingInfo to return an unsharded collection.
        return;
    } else if (itColl->second.needsRefresh) {
        // Refresh has been scheduled for the collection already
        return;
    } else if (itColl->second.routingInfo->getVersion() == ccrt._cm->getVersion()) {
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

void CatalogCache::invalidateShardedCollection(StringData ns) {
    invalidateShardedCollection(NamespaceString(ns));
}

void CatalogCache::purgeDatabase(StringData dbName) {
    stdx::lock_guard<stdx::mutex> lg(_mutex);

    auto it = _databases.find(dbName);
    if (it == _databases.end()) {
        return;
    }

    _databases.erase(it);
}

void CatalogCache::purgeAllDatabases() {
    stdx::lock_guard<stdx::mutex> lg(_mutex);
    _databases.clear();
}

std::shared_ptr<ChunkManager> CatalogCache::refreshCollectionRoutingInfo(
    OperationContext* opCtx,
    const NamespaceString& nss,
    std::shared_ptr<ChunkManager> existingRoutingInfo) {
    Timer t;

    const auto catalogClient = Grid::get(opCtx)->catalogClient(opCtx);

    // Decide whether to do a full or partial load based on the state of the collection
    auto collStatus = catalogClient->getCollection(opCtx, nss.ns());
    if (collStatus == ErrorCodes::NamespaceNotFound) {
        return nullptr;
    }

    const auto coll = uassertStatusOK(std::move(collStatus)).value;
    if (coll.getDropped()) {
        return nullptr;
    }

    ChunkVersion startingCollectionVersion;
    ChunkMap chunkMap =
        SimpleBSONObjComparator::kInstance.makeBSONObjIndexedMap<std::shared_ptr<Chunk>>();

    if (!existingRoutingInfo) {
        // If we don't have a basis chunk manager, do a full refresh
        startingCollectionVersion = ChunkVersion(0, 0, coll.getEpoch());
    } else if (existingRoutingInfo->getVersion().epoch() != coll.getEpoch()) {
        // If the collection's epoch has changed, do a full refresh
        startingCollectionVersion = ChunkVersion(0, 0, coll.getEpoch());
    } else {
        // Otherwise do a partial refresh
        startingCollectionVersion = existingRoutingInfo->getVersion();
        chunkMap = existingRoutingInfo->chunkMap();
    }

    log() << "Refreshing chunks for collection " << nss << " based on version "
          << startingCollectionVersion;

    // Diff tracker should *always* find at least one chunk if collection exists
    const auto diffQuery =
        CMConfigDiffTracker::createConfigDiffQuery(nss, startingCollectionVersion);

    // Query the chunks which have changed
    std::vector<ChunkType> newChunks;
    repl::OpTime opTime;
    uassertStatusOK(Grid::get(opCtx)->catalogClient(opCtx)->getChunks(
        opCtx,
        diffQuery.query,
        diffQuery.sort,
        boost::none,
        &newChunks,
        &opTime,
        repl::ReadConcernLevel::kMajorityReadConcern));

    ChunkVersion collectionVersion = startingCollectionVersion;

    ShardVersionMap unusedShardVersions;
    CMConfigDiffTracker differ(nss, &chunkMap, &collectionVersion, &unusedShardVersions);

    const int diffsApplied = differ.calculateConfigDiff(opCtx, newChunks);

    if (diffsApplied < 1) {
        log() << "Refresh for collection " << nss << " took " << t.millis()
              << " ms and failed because the collection's "
                 "sharding metadata either changed in between or "
                 "became corrupted";

        uasserted(ErrorCodes::ConflictingOperationInProgress,
                  "Collection sharding status changed during refresh or became corrupted");
    }

    // If at least one diff was applied, the metadata is correct, but it might not have changed so
    // in this case there is no need to recreate the chunk manager.
    //
    // NOTE: In addition to the above statement, it is also important that we return the same chunk
    // manager object, because the write commands' code relies on changes of the chunk manager's
    // sequence number to detect batch writes not making progress because of chunks moving across
    // shards too frequently.
    if (collectionVersion == startingCollectionVersion) {
        log() << "Refresh for collection " << nss << " took " << t.millis()
              << " ms and didn't find any metadata changes";

        return existingRoutingInfo;
    }

    std::unique_ptr<CollatorInterface> defaultCollator;
    if (!coll.getDefaultCollation().isEmpty()) {
        // The collation should have been validated upon collection creation
        defaultCollator = uassertStatusOK(CollatorFactoryInterface::get(opCtx->getServiceContext())
                                              ->makeFromBSON(coll.getDefaultCollation()));
    }

    log() << "Refresh for collection " << nss << " took " << t.millis() << " ms and found version "
          << collectionVersion;

    return stdx::make_unique<ChunkManager>(nss,
                                           coll.getKeyPattern(),
                                           std::move(defaultCollator),
                                           coll.getUnique(),
                                           std::move(chunkMap),
                                           collectionVersion);
}

std::shared_ptr<CatalogCache::DatabaseInfoEntry> CatalogCache::_getDatabase_inlock(
    OperationContext* opCtx, StringData dbName) {
    auto it = _databases.find(dbName);
    if (it != _databases.end()) {
        return it->second;
    }

    const auto catalogClient = Grid::get(opCtx)->catalogClient(opCtx);

    const auto dbNameCopy = dbName.toString();

    // Load the database entry
    const auto opTimeWithDb = uassertStatusOK(catalogClient->getDatabase(opCtx, dbNameCopy));
    const auto& dbDesc = opTimeWithDb.value;

    // Load the sharded collections entries
    std::vector<CollectionType> collections;
    repl::OpTime collLoadConfigOptime;
    uassertStatusOK(
        catalogClient->getCollections(opCtx, &dbNameCopy, &collections, &collLoadConfigOptime));

    StringMap<CollectionRoutingInfoEntry> collectionEntries;
    for (const auto& coll : collections) {
        collectionEntries[coll.getNs().ns()].needsRefresh = true;
    }

    return _databases[dbName] = std::shared_ptr<DatabaseInfoEntry>(new DatabaseInfoEntry{
               dbDesc.getPrimary(), dbDesc.getSharded(), std::move(collectionEntries)});
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
