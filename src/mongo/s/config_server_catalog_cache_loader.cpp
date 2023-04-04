/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include "mongo/s/config_server_catalog_cache_loader.h"

#include "mongo/db/catalog_shard_feature_flag_gen.h"
#include "mongo/db/client.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/vector_clock.h"
#include "mongo/logv2/log.h"
#include "mongo/s/grid.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

namespace mongo {
namespace {

using CollectionAndChangedChunks = CatalogCacheLoader::CollectionAndChangedChunks;

/**
 * Blocking method, which returns the chunks which changed since the specified version.
 */
CollectionAndChangedChunks getChangedChunks(OperationContext* opCtx,
                                            const NamespaceString& nss,
                                            ChunkVersion sinceVersion) {
    const auto readConcern = [&]() -> repl::ReadConcernArgs {
        // (Ignore FCV check): This is in mongos so we expect to ignore FCV.
        if (serverGlobalParams.clusterRole.has(ClusterRole::ConfigServer) &&
            !gFeatureFlagCatalogShard.isEnabledAndIgnoreFCVUnsafe()) {
            // When the feature flag is on, the config server may read from a secondary which may
            // need to wait for replication, so we should use afterClusterTime.
            return {repl::ReadConcernLevel::kSnapshotReadConcern};
        } else {
            const auto vcTime = VectorClock::get(opCtx)->getTime();
            return {vcTime.configTime(), repl::ReadConcernLevel::kSnapshotReadConcern};
        }
    }();

    auto collAndChunks = Grid::get(opCtx)->catalogClient()->getCollectionAndChunks(
        opCtx, nss, sinceVersion, readConcern);

    const auto& coll = collAndChunks.first;
    return CollectionAndChangedChunks{coll.getEpoch(),
                                      coll.getTimestamp(),
                                      coll.getUuid(),
                                      coll.getKeyPattern().toBSON(),
                                      coll.getDefaultCollation(),
                                      coll.getUnique(),
                                      coll.getTimeseriesFields(),
                                      coll.getReshardingFields(),
                                      coll.getAllowMigrations(),
                                      std::move(collAndChunks.second)};
}

}  // namespace

ConfigServerCatalogCacheLoader::ConfigServerCatalogCacheLoader()
    : _executor(std::make_shared<ThreadPool>([] {
          ThreadPool::Options options;
          options.poolName = "ConfigServerCatalogCacheLoader";
          options.minThreads = 0;
          options.maxThreads = 6;
          return options;
      }())) {
    _executor->startup();
}

void ConfigServerCatalogCacheLoader::initializeReplicaSetRole(bool isPrimary) {
    MONGO_UNREACHABLE;
}

void ConfigServerCatalogCacheLoader::onStepDown() {
    MONGO_UNREACHABLE;
}

void ConfigServerCatalogCacheLoader::onStepUp() {
    MONGO_UNREACHABLE;
}

void ConfigServerCatalogCacheLoader::shutDown() {
    _executor->shutdown();
    _executor->join();
}

void ConfigServerCatalogCacheLoader::notifyOfCollectionPlacementVersionUpdate(
    const NamespaceString& nss) {
    MONGO_UNREACHABLE;
}

void ConfigServerCatalogCacheLoader::waitForCollectionFlush(OperationContext* opCtx,
                                                            const NamespaceString& nss) {
    MONGO_UNREACHABLE;
}

void ConfigServerCatalogCacheLoader::waitForDatabaseFlush(OperationContext* opCtx,
                                                          StringData dbName) {
    MONGO_UNREACHABLE;
}

SemiFuture<CollectionAndChangedChunks> ConfigServerCatalogCacheLoader::getChunksSince(
    const NamespaceString& nss, ChunkVersion version) {

    return ExecutorFuture<void>(_executor)
        .then([=]() {
            ThreadClient tc("ConfigServerCatalogCacheLoader::getChunksSince",
                            getGlobalServiceContext());
            auto opCtx = tc->makeOperationContext();

            return getChangedChunks(opCtx.get(), nss, version);
        })
        .semi();
}

SemiFuture<DatabaseType> ConfigServerCatalogCacheLoader::getDatabase(StringData dbName) {
    return ExecutorFuture<void>(_executor)
        .then([name = dbName.toString()] {
            ThreadClient tc("ConfigServerCatalogCacheLoader::getDatabase",
                            getGlobalServiceContext());
            auto opCtx = tc->makeOperationContext();
            return Grid::get(opCtx.get())
                ->catalogClient()
                ->getDatabase(opCtx.get(), name, repl::ReadConcernLevel::kMajorityReadConcern);
        })
        .semi();
}

}  // namespace mongo
