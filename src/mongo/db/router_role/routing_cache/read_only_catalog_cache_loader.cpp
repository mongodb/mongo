// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/router_role/routing_cache/read_only_catalog_cache_loader.h"

#include "mongo/util/assert_util.h"

namespace mongo {

using CollectionAndChangedChunks = CatalogCacheLoader::CollectionAndChangedChunks;

ReadOnlyCatalogCacheLoader::~ReadOnlyCatalogCacheLoader() {
    shutDown();
}

void ReadOnlyCatalogCacheLoader::shutDown() {
    _configServerLoader.shutDown();
}

SemiFuture<CollectionAndChangedChunks> ReadOnlyCatalogCacheLoader::getChunksSince(
    const NamespaceString& nss, ChunkVersion version) {
    return _configServerLoader.getChunksSince(nss, version);
}

SemiFuture<DatabaseType> ReadOnlyCatalogCacheLoader::getDatabase(const DatabaseName& dbName) {
    return _configServerLoader.getDatabase(dbName);
}

}  // namespace mongo
