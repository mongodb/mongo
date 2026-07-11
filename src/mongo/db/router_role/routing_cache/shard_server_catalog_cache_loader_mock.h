// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/db/global_catalog/type_chunk.h"
#include "mongo/db/global_catalog/type_collection.h"
#include "mongo/db/global_catalog/type_database_gen.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/router_role/routing_cache/shard_server_catalog_cache_loader.h"
#include "mongo/db/versioning_protocol/chunk_version.h"
#include "mongo/s/resharding/type_collection_fields_gen.h"
#include "mongo/stdx/unordered_map.h"
#include "mongo/util/future.h"
#include "mongo/util/modules.h"

#include <utility>
#include <vector>

#include <absl/container/node_hash_map.h>
#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {

/**
 * Mocks the metadata refresh results with settable return values. The purpose of this class is to
 * facilitate testing of classes that use a ShardServerCatalogCacheLoader.
 */
class [[MONGO_MOD_NEEDS_REPLACEMENT]] ShardServerCatalogCacheLoaderMock final
    : public ShardServerCatalogCacheLoader {
    ShardServerCatalogCacheLoaderMock(const ShardServerCatalogCacheLoaderMock&) = delete;
    ShardServerCatalogCacheLoaderMock& operator=(const ShardServerCatalogCacheLoaderMock&) = delete;

public:
    ShardServerCatalogCacheLoaderMock() = default;
    ~ShardServerCatalogCacheLoaderMock() override = default;

    void shutDown() override;

    SemiFuture<CollectionAndChangedChunks> getChunksSince(const NamespaceString& nss,
                                                          ChunkVersion version) override;

    SemiFuture<DatabaseType> getDatabase(const DatabaseName& dbName) override;

    void initializeReplicaSetRole(bool isPrimary) override {}
    void onStepDown() override {}
    void onStepUp() override {}
    void onReplicationRollback() override {}
    void notifyOfCollectionRefreshEndMarkerSeen(const NamespaceString& nss,
                                                const Timestamp& commitTime) override {}
    void waitForCollectionFlush(OperationContext* opCtx, const NamespaceString& nss) override {}
    void waitForDatabaseFlush(OperationContext* opCtx, const DatabaseName& dbName) override {}
    void interruptAfterAuthoritativeShardsTransition() override {}
    void waitForAllFlushes(OperationContext* opCtx) override {}

    /**
     * Sets the mocked collection entry result that getChunksSince will use to construct its return
     * value.
     */

    void setCollectionRefreshReturnValue(StatusWith<CollectionType> statusWithCollectionType);
    void clearCollectionReturnValue();

    /**
     * Sets the mocked chunk results that getChunksSince will use to construct its return value.
     */
    void setChunkRefreshReturnValue(StatusWith<std::vector<ChunkType>> statusWithChunks);
    void clearChunksReturnValue();

    /**
     * Sets the mocked database entry result that getDatabase will use to construct its return
     * value.
     */
    void setDatabaseRefreshReturnValue(StatusWith<DatabaseType> swDatabase);
    void clearDatabaseReturnValue();

    void setReshardingFields(boost::optional<TypeCollectionReshardingFields> reshardingFields) {
        _reshardingFields = std::move(reshardingFields);
    }

    void setCollectionRefreshValues(
        const NamespaceString& nss,
        StatusWith<CollectionType> statusWithCollection,
        StatusWith<std::vector<ChunkType>> statusWithChunks,
        boost::optional<TypeCollectionReshardingFields> reshardingFields) {
        _refreshValues[nss] = {statusWithCollection, statusWithChunks, reshardingFields};
    }

    static const Status kCollectionInternalErrorStatus;
    static const Status kChunksInternalErrorStatus;
    static const Status kDatabaseInternalErrorStatus;

private:
    StatusWith<CollectionType> _swCollectionReturnValue{kCollectionInternalErrorStatus};

    StatusWith<std::vector<ChunkType>> _swChunksReturnValue{kChunksInternalErrorStatus};

    boost::optional<TypeCollectionReshardingFields> _reshardingFields;

    struct RefreshInfo {
        StatusWith<CollectionType> swCollectionReturnValue{kCollectionInternalErrorStatus};
        StatusWith<std::vector<ChunkType>> swChunksReturnValue{kChunksInternalErrorStatus};
        boost::optional<TypeCollectionReshardingFields> reshardingFields;
    };

    stdx::unordered_map<NamespaceString, RefreshInfo> _refreshValues;
    StatusWith<DatabaseType> _swDatabaseReturnValue{kDatabaseInternalErrorStatus};
};

}  // namespace mongo
