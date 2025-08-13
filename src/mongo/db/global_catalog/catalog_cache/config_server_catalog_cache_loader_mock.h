/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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
#include "mongo/base/string_data.h"
#include "mongo/db/global_catalog/catalog_cache/config_server_catalog_cache_loader.h"
#include "mongo/db/global_catalog/type_chunk.h"
#include "mongo/db/global_catalog/type_collection.h"
#include "mongo/db/global_catalog/type_database_gen.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/versioning_protocol/chunk_version.h"
#include "mongo/s/resharding/type_collection_fields_gen.h"
#include "mongo/stdx/unordered_map.h"
#include "mongo/util/concurrency/thread_pool.h"
#include "mongo/util/future.h"

#include <utility>
#include <vector>

#include <absl/container/node_hash_map.h>
#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {

/**
 * Mocks the metadata refresh results with settable return values. The purpose of this class is to
 * facilitate testing of classes that use a ConfigServerCatalogCacheLoader.
 */
class ConfigServerCatalogCacheLoaderMock final : public ConfigServerCatalogCacheLoader {
    ConfigServerCatalogCacheLoaderMock(const ConfigServerCatalogCacheLoaderMock&) = delete;
    ConfigServerCatalogCacheLoaderMock& operator=(const ConfigServerCatalogCacheLoaderMock&) =
        delete;

public:
    ConfigServerCatalogCacheLoaderMock() = default;
    ~ConfigServerCatalogCacheLoaderMock() override = default;

    void shutDown() override;

    SemiFuture<CollectionAndChangedChunks> getChunksSince(const NamespaceString& nss,
                                                          ChunkVersion version) override;

    SemiFuture<DatabaseType> getDatabase(const DatabaseName& dbName) override;

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
