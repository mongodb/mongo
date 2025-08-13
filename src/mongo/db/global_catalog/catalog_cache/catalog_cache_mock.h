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

#pragma once

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/db/global_catalog/catalog_cache/catalog_cache.h"
#include "mongo/db/global_catalog/catalog_cache/config_server_catalog_cache_loader_mock.h"
#include "mongo/db/global_catalog/chunk_manager.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"

#include <memory>

#include <boost/move/utility_core.hpp>

namespace mongo {

/**
 * Mocks the metadata refresh results with settable return values. The purpose of this class is to
 * facilitate testing of classes that use a CatalogCacheLoader.
 */
class CatalogCacheMock final : public CatalogCache {
    CatalogCacheMock(const CatalogCacheMock&) = delete;
    CatalogCacheMock& operator=(const CatalogCacheMock&) = delete;

public:
    CatalogCacheMock(ServiceContext* context, std::shared_ptr<CatalogCacheLoader> loader);
    ~CatalogCacheMock() override = default;

    StatusWith<CachedDatabaseInfo> getDatabase(OperationContext* opCtx,
                                               const DatabaseName& dbName) override;

    StatusWith<CollectionRoutingInfo> getCollectionRoutingInfo(OperationContext* opCtx,
                                                               const NamespaceString& nss,
                                                               bool allowLocks) override;

    StatusWith<ChunkManager> getCollectionPlacementInfoWithRefresh(
        OperationContext* opCtx, const NamespaceString& nss) override;

    void setDatabaseReturnValue(const DatabaseName& dbName, CachedDatabaseInfo databaseInfo);

    void setCollectionReturnValue(const NamespaceString& nss, CollectionRoutingInfo chunkManager);

    void advanceCollectionTimeInStore(const NamespaceString& nss,
                                      const ChunkVersion& newVersionInStore) override;

    static std::unique_ptr<CatalogCacheMock> make();

    static const Status kChunkManagerInternalErrorStatus;

    static CollectionRoutingInfo makeCollectionRoutingInfoUntracked(const NamespaceString& nss,
                                                                    const ShardId& dbPrimaryShard,
                                                                    DatabaseVersion dbVersion);
    struct ExtraCollectionOptions {
        boost::optional<TimeseriesOptions> timeseriesOptions;
    };

    struct Chunk {
        ChunkRange range;
        ShardId shard;
    };

    static CollectionRoutingInfo makeCollectionRoutingInfoUnsplittable(
        const NamespaceString& nss,
        const ShardId& dbPrimaryShard,
        DatabaseVersion dbVersion,
        const ShardId& dataShard,
        ExtraCollectionOptions extraOptions = {});

    static CollectionRoutingInfo makeCollectionRoutingInfoSharded(
        const NamespaceString& nss,
        const ShardId& dbPrimaryShard,
        DatabaseVersion dbVersion,
        KeyPattern shardKeyPattern,
        std::vector<Chunk> chunks,
        ExtraCollectionOptions extraOptions = {});

    static CachedDatabaseInfo makeDatabaseInfo(const DatabaseName& dbName,
                                               const ShardId& dbPrimaryShard,
                                               const DatabaseVersion& dbVersion);

    stdx::unordered_map<NamespaceString, ChunkVersion> lastNotifiedTimeInStore;

private:
    stdx::unordered_map<DatabaseName, CachedDatabaseInfo> _dbCache;
    stdx::unordered_map<NamespaceString, CollectionRoutingInfo> _collectionCache;

    static CollectionRoutingInfo _makeCollectionRoutingInfoTracked(
        const NamespaceString& nss,
        const ShardId& dbPrimaryShard,
        DatabaseVersion dbVersion,
        KeyPattern shardKeyPattern,
        std::vector<Chunk> chunks,
        bool unsplittable,
        ExtraCollectionOptions extraOptions = {});
};

}  // namespace mongo
