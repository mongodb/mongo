/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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

#include "mongo/db/global_catalog/catalog_cache/catalog_cache_mock.h"

#include "mongo/base/error_codes.h"
#include "mongo/db/service_context.h"
#include "mongo/db/sharding_environment/sharding_test_fixture_common.h"

#include <utility>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <fmt/format.h>

namespace mongo {

CatalogCacheMock::CatalogCacheMock(ServiceContext* serviceContext,
                                   std::shared_ptr<CatalogCacheLoader> loader)
    : CatalogCache(serviceContext, loader) {}

StatusWith<CachedDatabaseInfo> CatalogCacheMock::getDatabase(OperationContext* opCtx,
                                                             const DatabaseName& dbName) {
    const auto it = _dbCache.find(dbName);
    if (it != _dbCache.end()) {
        return it->second;
    } else {
        return Status(ErrorCodes::InternalError,
                      fmt::format("CatalogCacheMock: No mocked value for database '{}'",
                                  dbName.toStringForErrorMsg()));
    }
}

StatusWith<CollectionRoutingInfo> CatalogCacheMock::getCollectionRoutingInfo(
    OperationContext* opCtx, const NamespaceString& nss, bool allowLocks) {
    const auto it = _collectionCache.find(nss);
    if (it != _collectionCache.end()) {
        return it->second;
    } else {
        return Status(ErrorCodes::InternalError,
                      fmt::format("CatalogCacheMock: No mocked value for collection '{}'",
                                  nss.toStringForErrorMsg()));
    }
}
StatusWith<ChunkManager> CatalogCacheMock::getCollectionPlacementInfoWithRefresh(
    OperationContext* opCtx, const NamespaceString& nss) {
    const auto it = _collectionCache.find(nss);
    if (it != _collectionCache.end()) {
        return it->second.getChunkManager();
    } else {
        return Status(
            ErrorCodes::InternalError,
            fmt::format("CatalogCacheMock: No mocked ChunkManager value for collection '{}'",
                        nss.toStringForErrorMsg()));
    }
}

void CatalogCacheMock::setDatabaseReturnValue(const DatabaseName& dbName,
                                              CachedDatabaseInfo databaseInfo) {
    _dbCache[dbName] = databaseInfo;
}

void CatalogCacheMock::setCollectionReturnValue(const NamespaceString& nss,
                                                CollectionRoutingInfo collectionRoutingInfo) {
    _collectionCache.erase(nss);
    _collectionCache.emplace(nss, collectionRoutingInfo);
}

void CatalogCacheMock::advanceCollectionTimeInStore(const NamespaceString& nss,
                                                    const ChunkVersion& newVersionInStore) {
    lastNotifiedTimeInStore[nss] = newVersionInStore;
}

std::unique_ptr<CatalogCacheMock> CatalogCacheMock::make() {
    auto catalogCacheLoader = std::make_shared<ConfigServerCatalogCacheLoaderMock>();
    auto serviceContext = ServiceContext::make();
    return std::make_unique<CatalogCacheMock>(serviceContext.get(), catalogCacheLoader);
}

CollectionRoutingInfo CatalogCacheMock::makeCollectionRoutingInfoUntracked(
    const NamespaceString& nss, const ShardId& dbPrimaryShard, DatabaseVersion dbVersion) {
    ChunkManager cm(OptionalRoutingTableHistory(), boost::none);
    return CollectionRoutingInfo(
        std::move(cm),
        DatabaseTypeValueHandle(DatabaseType{nss.dbName(), dbPrimaryShard, dbVersion}));
}

CollectionRoutingInfo CatalogCacheMock::makeCollectionRoutingInfoUnsplittable(
    const NamespaceString& nss,
    const ShardId& dbPrimaryShard,
    DatabaseVersion dbVersion,
    const ShardId& dataShard,
    ExtraCollectionOptions extraOptions) {
    // Unsplittable collections always have this shard key pattern.
    const KeyPattern shardKeyPattern(BSON("_id" << 1));

    // Unsplittable collections always have one single chunk.
    const Chunk chunk{ChunkRange(BSON("_id" << MINKEY), BSON("_id" << MAXKEY)), dataShard};

    return _makeCollectionRoutingInfoTracked(nss,
                                             dbPrimaryShard,
                                             dbVersion,
                                             shardKeyPattern,
                                             {chunk},
                                             true /*unsplittable*/,
                                             extraOptions);
}


CollectionRoutingInfo CatalogCacheMock::makeCollectionRoutingInfoSharded(
    const NamespaceString& nss,
    const ShardId& dbPrimaryShard,
    DatabaseVersion dbVersion,
    KeyPattern shardKeyPattern,
    std::vector<Chunk> chunks,
    ExtraCollectionOptions extraOptions) {
    return _makeCollectionRoutingInfoTracked(nss,
                                             dbPrimaryShard,
                                             dbVersion,
                                             shardKeyPattern,
                                             chunks,
                                             false /*unsplittable*/,
                                             extraOptions);
}

CollectionRoutingInfo CatalogCacheMock::_makeCollectionRoutingInfoTracked(
    const NamespaceString& nss,
    const ShardId& dbPrimaryShard,
    DatabaseVersion dbVersion,
    KeyPattern shardKeyPattern,
    std::vector<Chunk> chunks,
    bool unsplittable,
    ExtraCollectionOptions extraOptions) {
    const auto collectionUUID = UUID::gen();
    const auto collectionEpoch = OID::gen();
    const Timestamp collectionTimestamp(1, 0);

    boost::optional<TypeCollectionTimeseriesFields> optTimeseriesFields;
    if (extraOptions.timeseriesOptions) {
        optTimeseriesFields.emplace();
        optTimeseriesFields->setTimeseriesOptions(*extraOptions.timeseriesOptions);
    }

    std::vector<ChunkType> chunkTypes = [&]() {
        std::vector<ChunkType> chunkTypes;

        ChunkVersion chunkVersion({collectionEpoch, collectionTimestamp}, {1, 0});
        for (const auto& chunk : chunks) {
            chunkTypes.emplace_back(collectionUUID, chunk.range, chunkVersion, chunk.shard);
            chunkVersion.incMajor();
        }

        return chunkTypes;
    }();

    auto rth = RoutingTableHistory::makeNew(nss,
                                            collectionUUID,
                                            shardKeyPattern,
                                            unsplittable,
                                            nullptr /*defaultCollator*/,
                                            false /*unique*/,
                                            collectionEpoch,
                                            collectionTimestamp,
                                            optTimeseriesFields,
                                            boost::none /*reshardingFields*/,
                                            true /*allowMigrations*/,
                                            chunkTypes);

    ChunkManager cm(ShardingTestFixtureCommon::makeStandaloneRoutingTableHistory(std::move(rth)),
                    boost::none /*clusterTime*/);
    return CollectionRoutingInfo(
        std::move(cm),
        DatabaseTypeValueHandle(DatabaseType{nss.dbName(), dbPrimaryShard, dbVersion}));
}

CachedDatabaseInfo CatalogCacheMock::makeDatabaseInfo(const DatabaseName& dbName,
                                                      const ShardId& dbPrimaryShard,
                                                      const DatabaseVersion& dbVersion) {
    DatabaseType dbInfo(dbName, dbPrimaryShard, dbVersion);
    return CachedDatabaseInfo(std::move(dbInfo), ComparableDatabaseVersion());
}

}  // namespace mongo
