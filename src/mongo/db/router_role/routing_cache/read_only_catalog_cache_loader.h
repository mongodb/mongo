// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/global_catalog/type_database_gen.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/router_role/routing_cache/config_server_catalog_cache_loader_impl.h"
#include "mongo/db/router_role/routing_cache/shard_server_catalog_cache_loader.h"
#include "mongo/db/versioning_protocol/chunk_version.h"
#include "mongo/util/future.h"
#include "mongo/util/modules.h"

namespace mongo {

/**
 * Contains a ConfigServerCatalogCacheLoader for remote metadata loading. Inactive functions simply
 * return, rather than invariant, so this class can be plugged into the shard server for read-only
 * mode, where persistence should not be attempted.
 */
class [[MONGO_MOD_PARENT_PRIVATE]] ReadOnlyCatalogCacheLoader final
    : public ShardServerCatalogCacheLoader {
public:
    ReadOnlyCatalogCacheLoader() = default;
    ~ReadOnlyCatalogCacheLoader() override;

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
    void waitForCollectionFlush(OperationContext* opCtx, const NamespaceString& nss) override {
        MONGO_UNIMPLEMENTED_TASSERT(10083541);
    }
    void waitForDatabaseFlush(OperationContext* opCtx, const DatabaseName& dbName) override {
        MONGO_UNIMPLEMENTED_TASSERT(10083542);
    }
    void interruptAfterAuthoritativeShardsTransition() override {
        MONGO_UNIMPLEMENTED_TASSERT(13044001);
    }
    void waitForAllFlushes(OperationContext* opCtx) override {
        MONGO_UNIMPLEMENTED_TASSERT(12797900);
    }

private:
    ConfigServerCatalogCacheLoaderImpl _configServerLoader;
};

}  // namespace mongo
