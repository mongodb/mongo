/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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


#include "mongo/db/s/sharding_util.h"

#include <fmt/format.h>

#include "mongo/db/catalog/index_builds_manager.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/commands.h"
#include "mongo/db/concurrency/exception_util.h"
#include "mongo/db/index_builds_coordinator.h"
#include "mongo/logv2/log.h"
#include "mongo/s/catalog/type_index_catalog_gen.h"
#include "mongo/s/request_types/flush_routing_table_cache_updates_gen.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

namespace mongo {

namespace sharding_util {

using namespace fmt::literals;

void tellShardsToRefreshCollection(OperationContext* opCtx,
                                   const std::vector<ShardId>& shardIds,
                                   const NamespaceString& nss,
                                   const std::shared_ptr<executor::TaskExecutor>& executor) {
    auto cmd = FlushRoutingTableCacheUpdatesWithWriteConcern(nss);
    cmd.setSyncFromConfig(true);
    cmd.setDbName(nss.db());
    auto cmdObj = CommandHelpers::appendMajorityWriteConcern(cmd.toBSON({}));
    sendCommandToShards(opCtx, NamespaceString::kAdminDb, cmdObj, shardIds, executor);
}

std::vector<AsyncRequestsSender::Response> sendCommandToShards(
    OperationContext* opCtx,
    StringData dbName,
    const BSONObj& command,
    const std::vector<ShardId>& shardIds,
    const std::shared_ptr<executor::TaskExecutor>& executor,
    const bool throwOnError) {
    std::vector<AsyncRequestsSender::Request> requests;
    for (const auto& shardId : shardIds) {
        requests.emplace_back(shardId, command);
    }

    std::vector<AsyncRequestsSender::Response> responses;
    if (!requests.empty()) {
        // The _flushRoutingTableCacheUpdatesWithWriteConcern command will fail with a
        // QueryPlanKilled error response if the config.cache.chunks collection is dropped
        // concurrently. The config.cache.chunks collection is dropped by the shard when it detects
        // the sharded collection's epoch having changed. We use kIdempotentOrCursorInvalidated so
        // the ARS automatically retries in that situation.
        AsyncRequestsSender ars(opCtx,
                                executor,
                                dbName,
                                requests,
                                ReadPreferenceSetting(ReadPreference::PrimaryOnly),
                                Shard::RetryPolicy::kIdempotentOrCursorInvalidated,
                                nullptr /* resourceYielder */);

        while (!ars.done()) {
            // Retrieve the responses and throw at the first failure.
            auto response = ars.next();

            if (throwOnError) {
                const auto errorContext =
                    "Failed command {} for database '{}' on shard '{}'"_format(
                        command.toString(), dbName, StringData{response.shardId});

                auto shardResponse =
                    uassertStatusOKWithContext(std::move(response.swResponse), errorContext);

                auto status = getStatusFromCommandResult(shardResponse.data);
                uassertStatusOKWithContext(status, errorContext);

                auto wcStatus = getWriteConcernStatusFromCommandResult(shardResponse.data);
                uassertStatusOKWithContext(wcStatus, errorContext);
            }

            responses.push_back(std::move(response));
        }
    }
    return responses;
}

// TODO SERVER-67593: Investigate if DBDirectClient can be used instead.
Status createIndexOnCollection(OperationContext* opCtx,
                               const NamespaceString& ns,
                               const BSONObj& keys,
                               bool unique) {
    try {
        // TODO SERVER-50983: Create abstraction for creating collection when using
        // AutoGetCollection
        AutoGetCollection autoColl(opCtx, ns, MODE_X);
        const Collection* collection = autoColl.getCollection().get();
        if (!collection) {
            CollectionOptions options;
            options.uuid = UUID::gen();
            writeConflictRetry(opCtx, "createIndexOnCollection", ns.ns(), [&] {
                WriteUnitOfWork wunit(opCtx);
                auto db = autoColl.ensureDbExists(opCtx);
                collection = db->createCollection(opCtx, ns, options);
                invariant(collection,
                          str::stream() << "Failed to create collection " << ns.ns()
                                        << " for indexes: " << keys);
                wunit.commit();
            });
        }
        auto indexCatalog = collection->getIndexCatalog();
        IndexSpec index;
        index.addKeys(keys);
        index.unique(unique);
        index.version(int(IndexDescriptor::kLatestIndexVersion));
        auto removeIndexBuildsToo = false;
        auto indexSpecs = indexCatalog->removeExistingIndexes(
            opCtx,
            CollectionPtr(collection, CollectionPtr::NoYieldTag{}),
            uassertStatusOK(
                collection->addCollationDefaultsToIndexSpecsForCreate(opCtx, {index.toBSON()})),
            removeIndexBuildsToo);

        if (indexSpecs.empty()) {
            return Status::OK();
        }

        auto fromMigrate = false;
        if (!collection->isEmpty(opCtx)) {
            // We typically create indexes on config/admin collections for sharding while setting up
            // a sharded cluster, so we do not expect to see data in the collection.
            // Therefore, it is ok to log this index build.
            const auto& indexSpec = indexSpecs[0];
            LOGV2(5173300,
                  "Creating index on sharding collection with existing data",
                  logAttrs(ns),
                  "uuid"_attr = collection->uuid(),
                  "index"_attr = indexSpec);
            auto indexConstraints = IndexBuildsManager::IndexConstraints::kEnforce;
            IndexBuildsCoordinator::get(opCtx)->createIndex(
                opCtx, collection->uuid(), indexSpec, indexConstraints, fromMigrate);
        } else {
            writeConflictRetry(opCtx, "createIndexOnConfigCollection", ns.ns(), [&] {
                WriteUnitOfWork wunit(opCtx);
                CollectionWriter collWriter(opCtx, collection->uuid());
                IndexBuildsCoordinator::get(opCtx)->createIndexesOnEmptyCollection(
                    opCtx, collWriter, indexSpecs, fromMigrate);
                wunit.commit();
            });
        }
    } catch (const DBException& e) {
        return e.toStatus();
    }

    return Status::OK();
}

Status createGlobalIndexesIndexes(OperationContext* opCtx) {
    bool unique = true;
    if (serverGlobalParams.clusterRole == ClusterRole::ConfigServer) {
        auto result =
            createIndexOnCollection(opCtx,
                                    NamespaceString::kConfigsvrIndexCatalogNamespace,
                                    BSON(IndexCatalogType::kCollectionUUIDFieldName
                                         << 1 << IndexCatalogType::kLastModFieldName << 1),
                                    !unique);
        if (!result.isOK()) {
            return result.withContext(str::stream()
                                      << "couldn't create collectionUUID_1_lastmod_1 index on "
                                      << NamespaceString::kConfigsvrIndexCatalogNamespace);
        }
    } else {
        auto result =
            createIndexOnCollection(opCtx,
                                    NamespaceString::kShardsIndexCatalogNamespace,
                                    BSON(IndexCatalogType::kCollectionUUIDFieldName
                                         << 1 << IndexCatalogType::kLastModFieldName << 1),
                                    !unique);
        if (!result.isOK()) {
            return result.withContext(str::stream()
                                      << "couldn't create collectionUUID_1_lastmod_1 index on "
                                      << NamespaceString::kShardsIndexCatalogNamespace);
        }
    }
    return Status::OK();
}

}  // namespace sharding_util
}  // namespace mongo
