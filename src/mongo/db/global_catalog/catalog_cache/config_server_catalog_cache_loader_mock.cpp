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

#include "mongo/db/global_catalog/catalog_cache/config_server_catalog_cache_loader_mock.h"

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
