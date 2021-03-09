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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

#include "mongo/platform/basic.h"

#include "mongo/s/config_server_catalog_cache_loader.h"

#include <memory>

#include "mongo/db/client.h"
#include "mongo/db/operation_context.h"
#include "mongo/logv2/log.h"
#include "mongo/s/catalog/sharding_catalog_client.h"
#include "mongo/s/grid.h"
#include "mongo/util/fail_point.h"

namespace mongo {

using CollectionAndChangedChunks = CatalogCacheLoader::CollectionAndChangedChunks;

namespace {

MONGO_FAIL_POINT_DEFINE(hangBeforeReadingChunks);

/**
 * Structure repsenting the generated query and sort order for a chunk diffing operation.
 */
struct QueryAndSort {
    const BSONObj query;
    const BSONObj sort;
};

/**
 * Returns the query needed to find incremental changes to a collection from the config server.
 *
 * The query has to find all the chunks $gte the current max version. Currently, any splits and
 * merges will increment the current max version.
 *
 * The sort needs to be by ascending version in order to pick up the chunks which changed most
 * recent and also in order to handle cursor yields between chunks being migrated/split/merged. This
 * ensures that changes to chunk version (which will always be higher) will always come *after* our
 * current position in the chunk cursor.
 */
QueryAndSort createConfigDiffQueryNs(const NamespaceString& nss, ChunkVersion collectionVersion) {
    return {BSON(ChunkType::ns() << nss.ns() << ChunkType::lastmod() << GTE
                                 << Timestamp(collectionVersion.toLong())),
            BSON(ChunkType::lastmod() << 1)};
}

QueryAndSort createConfigDiffQueryUuid(const UUID& uuid, ChunkVersion collectionVersion) {
    return {BSON(ChunkType::collectionUUID()
                 << uuid << ChunkType::lastmod() << GTE << Timestamp(collectionVersion.toLong())),
            BSON(ChunkType::lastmod() << 1)};
}

/**
 * Blocking method, which returns the chunks which changed since the specified version.
 */
CollectionAndChangedChunks getChangedChunks(OperationContext* opCtx,
                                            const NamespaceString& nss,
                                            ChunkVersion sinceVersion) {
    const auto catalogClient = Grid::get(opCtx)->catalogClient();

    // Decide whether to do a full or partial load based on the state of the collection
    const auto coll = catalogClient->getCollection(opCtx, nss);
    uassert(ErrorCodes::NamespaceNotFound,
            str::stream() << "Collection " << nss.ns() << " is dropped.",
            !coll.getDropped());

    // If the collection's epoch has changed, do a full refresh
    const ChunkVersion startingCollectionVersion = (sinceVersion.epoch() == coll.getEpoch())
        ? sinceVersion
        : ChunkVersion(0, 0, coll.getEpoch(), coll.getTimestamp());

    // Diff tracker should *always* find at least one chunk if collection exists
    const auto diffQuery = [&]() {
        if (coll.getTimestamp()) {
            return createConfigDiffQueryUuid(coll.getUuid(), startingCollectionVersion);
        } else {
            return createConfigDiffQueryNs(nss, startingCollectionVersion);
        }
    }();

    if (MONGO_unlikely(hangBeforeReadingChunks.shouldFail())) {
        LOGV2(5310504, "Hit hangBeforeReadingChunks failpoint");
        hangBeforeReadingChunks.pauseWhileSet(opCtx);
    }

    // TODO SERVER-53283: Remove once 5.0 has branched out.
    // Use a hint to make sure the query will use an index. This ensures that the query on
    // config.chunks will only execute if config.chunks is guaranteed to still have the same
    // metadata format as we inferred from the config.collections entry we read.
    // This is because when the config.chunks are patched up as part of the FCV upgrade (or
    // downgrade), first the ns_1_lastmod_1 index (or uuid_1_lastmod_1) is dropped, then the 'ns'
    // (or 'uuid') fields are unset from config.chunks. If the query is forced to use the expected
    // index, we can guarantee that the config.chunks we will read will have the expected format. If
    // it doesn't, it means that it's being patched-up. Then the query will fail and the refresh
    // will be retried, this time expecting the new metadata format.
    const auto hint = coll.getTimestamp()
        ? BSON(ChunkType::collectionUUID() << 1 << ChunkType::lastmod() << 1)
        : BSON(ChunkType::ns() << 1 << ChunkType::lastmod() << 1);

    // Query the chunks which have changed
    repl::OpTime opTime;
    const std::vector<ChunkType> changedChunks = uassertStatusOK(
        Grid::get(opCtx)->catalogClient()->getChunks(opCtx,
                                                     diffQuery.query,
                                                     diffQuery.sort,
                                                     boost::none,
                                                     &opTime,
                                                     repl::ReadConcernLevel::kMajorityReadConcern,
                                                     hint));

    uassert(ErrorCodes::ConflictingOperationInProgress,
            "No chunks were found for the collection",
            !changedChunks.empty());

    return CollectionAndChangedChunks{coll.getEpoch(),
                                      coll.getTimestamp(),
                                      coll.getUuid(),
                                      coll.getKeyPattern().toBSON(),
                                      coll.getDefaultCollation(),
                                      coll.getUnique(),
                                      coll.getReshardingFields(),
                                      coll.getAllowMigrations(),
                                      std::move(changedChunks)};
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

void ConfigServerCatalogCacheLoader::notifyOfCollectionVersionUpdate(const NamespaceString& nss) {
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
