// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/global_catalog/type_database_gen.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/router_role/routing_cache/config_server_catalog_cache_loader.h"
#include "mongo/db/versioning_protocol/chunk_version.h"
#include "mongo/util/concurrency/thread_pool.h"
#include "mongo/util/future.h"
#include "mongo/util/modules.h"

namespace mongo {

class [[MONGO_MOD_NEEDS_REPLACEMENT]] ConfigServerCatalogCacheLoaderImpl
    : public ConfigServerCatalogCacheLoader {
public:
    ConfigServerCatalogCacheLoaderImpl();
    ~ConfigServerCatalogCacheLoaderImpl() override = default;

    void shutDown() override;

    SemiFuture<CollectionAndChangedChunks> getChunksSince(const NamespaceString& nss,
                                                          ChunkVersion version) override;
    SemiFuture<DatabaseType> getDatabase(const DatabaseName& dbName) override;

private:
    // Thread pool to be used to perform metadata load
    std::shared_ptr<ThreadPool> _executor;
};

}  // namespace mongo
