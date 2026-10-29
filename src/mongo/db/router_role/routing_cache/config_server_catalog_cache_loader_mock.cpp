// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/router_role/routing_cache/config_server_catalog_cache_loader_mock.h"

#include "mongo/base/error_codes.h"
#include "mongo/db/global_catalog/type_chunk.h"
#include "mongo/db/global_catalog/type_collection.h"
#include "mongo/db/global_catalog/type_collection_common_types_gen.h"
#include "mongo/db/keypattern.h"
#include "mongo/db/operation_context.h"
#include "mongo/util/assert_util.h"

#include <absl/container/node_hash_map.h>
#include <absl/meta/type_traits.h>
#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {

using CollectionAndChangedChunks = CatalogCacheLoader::CollectionAndChangedChunks;

namespace {

CollectionAndChangedChunks getCollectionRefresh(
    const StatusWith<CollectionType>& swCollectionReturnValue,
    StatusWith<std::vector<ChunkType>> swChunksReturnValue,
    const boost::optional<TypeCollectionReshardingFields>& reshardingFields) {
    uassertStatusOK(swCollectionReturnValue);
    uassertStatusOK(swChunksReturnValue);

    // We swap the chunks out of _swChunksReturnValue to ensure if this task is
    // scheduled multiple times that we don't inform the ChunkManager about a chunk it
    // has already updated.
    std::vector<ChunkType> chunks;
    swChunksReturnValue.getValue().swap(chunks);

    return CollectionAndChangedChunks{swCollectionReturnValue.getValue().getEpoch(),
                                      swCollectionReturnValue.getValue().getTimestamp(),
                                      swCollectionReturnValue.getValue().getUuid(),
                                      swCollectionReturnValue.getValue().getUnsplittable(),
                                      swCollectionReturnValue.getValue().getKeyPattern().toBSON(),
                                      swCollectionReturnValue.getValue().getDefaultCollation(),
                                      swCollectionReturnValue.getValue().getUnique(),
                                      swCollectionReturnValue.getValue().getTimeseriesFields(),
                                      reshardingFields,
                                      swCollectionReturnValue.getValue().getAllowMigrations(),
                                      std::move(chunks)};
}

}  // namespace

const Status ConfigServerCatalogCacheLoaderMock::kCollectionInternalErrorStatus = {
    ErrorCodes::InternalError,
    "Mocked catalog cache loader received unexpected collection request"};
const Status ConfigServerCatalogCacheLoaderMock::kChunksInternalErrorStatus = {
    ErrorCodes::InternalError, "Mocked catalog cache loader received unexpected chunks request"};
const Status ConfigServerCatalogCacheLoaderMock::kDatabaseInternalErrorStatus = {
    ErrorCodes::InternalError, "Mocked catalog cache loader received unexpected database request"};

void ConfigServerCatalogCacheLoaderMock::shutDown() {}

SemiFuture<CollectionAndChangedChunks> ConfigServerCatalogCacheLoaderMock::getChunksSince(
    const NamespaceString& nss, ChunkVersion version) {

    return makeReadyFutureWith([&nss, this] {
               auto it = _refreshValues.find(nss);

               if (it != _refreshValues.end())
                   return getCollectionRefresh(it->second.swCollectionReturnValue,
                                               std::move(it->second.swChunksReturnValue),
                                               it->second.reshardingFields);

               return getCollectionRefresh(
                   _swCollectionReturnValue, std::move(_swChunksReturnValue), _reshardingFields);
           })
        .semi();
}

SemiFuture<DatabaseType> ConfigServerCatalogCacheLoaderMock::getDatabase(
    const DatabaseName& dbName) {
    return makeReadyFutureWith([this] { return _swDatabaseReturnValue; }).semi();
}

void ConfigServerCatalogCacheLoaderMock::setCollectionRefreshReturnValue(
    StatusWith<CollectionType> statusWithCollectionType) {
    _swCollectionReturnValue = std::move(statusWithCollectionType);
}

void ConfigServerCatalogCacheLoaderMock::clearCollectionReturnValue() {
    _swCollectionReturnValue = kCollectionInternalErrorStatus;
}

void ConfigServerCatalogCacheLoaderMock::setChunkRefreshReturnValue(
    StatusWith<std::vector<ChunkType>> statusWithChunks) {
    _swChunksReturnValue = std::move(statusWithChunks);
}

void ConfigServerCatalogCacheLoaderMock::clearChunksReturnValue() {
    _swChunksReturnValue = kChunksInternalErrorStatus;
}

void ConfigServerCatalogCacheLoaderMock::setDatabaseRefreshReturnValue(
    StatusWith<DatabaseType> swDatabase) {
    _swDatabaseReturnValue = std::move(swDatabase);
}

void ConfigServerCatalogCacheLoaderMock::clearDatabaseReturnValue() {
    _swDatabaseReturnValue = kDatabaseInternalErrorStatus;
}

}  // namespace mongo
