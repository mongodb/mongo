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

#include "mongo/db/global_catalog/catalog_cache/config_server_catalog_cache_loader_impl.h"

#include "mongo/db/client.h"
#include "mongo/db/feature_flag.h"
#include "mongo/db/global_catalog/sharding_catalog_client.h"
#include "mongo/db/global_catalog/type_chunk.h"
#include "mongo/db/global_catalog/type_collection.h"
#include "mongo/db/global_catalog/type_collection_common_types_gen.h"
#include "mongo/db/keypattern.h"
#include "mongo/db/logical_time.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/db/repl/read_concern_level.h"
#include "mongo/db/server_options.h"
#include "mongo/db/service_context.h"
#include "mongo/db/sharding_environment/grid.h"
#include "mongo/db/topology/cluster_role.h"
#include "mongo/db/vector_clock/vector_clock.h"
#include "mongo/s/resharding/type_collection_fields_gen.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/future_impl.h"

#include <mutex>
#include <string>
#include <tuple>
#include <utility>

#include <boost/move/utility_core.hpp>
#include <boost/smart_ptr.hpp>

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
        const auto vcTime = VectorClock::get(opCtx)->getTime();
        return {vcTime.configTime(), repl::ReadConcernLevel::kSnapshotReadConcern};
    }();

    auto collAndChunks = Grid::get(opCtx)->catalogClient()->getCollectionAndChunks(
        opCtx, nss, sinceVersion, readConcern);

    const auto& coll = collAndChunks.first;
    return CollectionAndChangedChunks{coll.getEpoch(),
                                      coll.getTimestamp(),
                                      coll.getUuid(),
                                      coll.getUnsplittable(),
                                      coll.getKeyPattern().toBSON(),
                                      coll.getDefaultCollation(),
                                      coll.getUnique(),
                                      coll.getTimeseriesFields(),
                                      coll.getReshardingFields(),
                                      coll.getAllowMigrations(),
                                      std::move(collAndChunks.second)};
}

}  // namespace

ConfigServerCatalogCacheLoaderImpl::ConfigServerCatalogCacheLoaderImpl()
    : _executor(std::make_shared<ThreadPool>([] {
          ThreadPool::Options options;
          options.poolName = "ConfigServerCatalogCacheLoader";
          options.minThreads = 0;
          options.maxThreads = 6;
          return options;
      }())) {
    _executor->startup();
}

void ConfigServerCatalogCacheLoaderImpl::shutDown() {
    _executor->shutdown();
    _executor->join();
}

SemiFuture<CollectionAndChangedChunks> ConfigServerCatalogCacheLoaderImpl::getChunksSince(
    const NamespaceString& nss, ChunkVersion version) {
    // There's no need to refresh if a collection is always unsharded. Further, attempting to
    // refresh config.collections or config.chunks would trigger recursive refreshes since a config
    // shard can use the shard svr process interface.
    if (nss.isNamespaceAlwaysUntracked()) {
        return Status(ErrorCodes::NamespaceNotFound,
                      str::stream() << "Collection " << nss.toStringForErrorMsg() << " not found");
    }

    return ExecutorFuture<void>(_executor)
        .then([=]() {
            // TODO(SERVER-74658): Please revisit if this thread could be made killable.
            ThreadClient tc("ConfigServerCatalogCacheLoader::getChunksSince",
                            getGlobalServiceContext()->getService(),
                            ClientOperationKillableByStepdown{false});
            auto opCtx = tc->makeOperationContext();
            return getChangedChunks(opCtx.get(), nss, version);
        })
        .semi();
}

SemiFuture<DatabaseType> ConfigServerCatalogCacheLoaderImpl::getDatabase(
    const DatabaseName& dbName) {
    return ExecutorFuture<void>(_executor)
        .then([dbName] {
            // TODO(SERVER-74658): Please revisit if this thread could be made killable.
            ThreadClient tc("ConfigServerCatalogCacheLoader::getDatabase",
                            getGlobalServiceContext()->getService(),
                            ClientOperationKillableByStepdown{false});

            auto opCtx = tc->makeOperationContext();
            return Grid::get(opCtx.get())
                ->catalogClient()
                ->getDatabase(opCtx.get(), dbName, repl::ReadConcernLevel::kMajorityReadConcern);
        })
        .semi();
}

}  // namespace mongo
