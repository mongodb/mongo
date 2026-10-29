// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/db/global_catalog/chunk_manager.h"
#include "mongo/db/global_catalog/type_collection.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/router_role/routing_cache/catalog_cache.h"
#include "mongo/db/service_context.h"
#include "mongo/util/modules.h"

#include <boost/optional.hpp>

namespace mongo {

/**
 * Mocks the metadata refresh results with settable return values. The purpose of this class is to
 * facilitate testing of classes that use a CatalogCacheLoader.
 */
class [[MONGO_MOD_OPEN]] CatalogCacheMock : public CatalogCache {
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

    StatusWith<CurrentChunkManager> getCollectionPlacementInfoWithRefresh(
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
        ExtraCollectionOptions extraOptions = {},
        boost::optional<ReshardingFields> reshardingFields = boost::none);

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
        ExtraCollectionOptions extraOptions = {},
        boost::optional<ReshardingFields> reshardingFields = boost::none);
};

}  // namespace mongo
