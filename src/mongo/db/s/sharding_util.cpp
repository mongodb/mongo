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
#include <string>
#include <utility>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

#include "mongo/base/status_with.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/client/index_spec.h"
#include "mongo/client/read_preference.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/collection_options.h"
#include "mongo/db/catalog/database.h"
#include "mongo/db/catalog/index_builds_manager.h"
#include "mongo/db/catalog/index_catalog.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/commands.h"
#include "mongo/db/concurrency/exception_util.h"
#include "mongo/db/concurrency/lock_manager_defs.h"
#include "mongo/db/database_name.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/index_builds_coordinator.h"
#include "mongo/db/resource_yielder.h"
#include "mongo/db/s/shard_authoritative_catalog_gen.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/executor/remote_command_response.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_attr.h"
#include "mongo/logv2/log_component.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/s/catalog/type_index_catalog_gen.h"
#include "mongo/s/client/shard.h"
#include "mongo/s/cluster_commands_helpers.h"
#include "mongo/s/request_types/flush_routing_table_cache_updates_gen.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/exit.h"
#include "mongo/util/str.h"
#include "mongo/util/uuid.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

namespace mongo {

namespace sharding_util {

using namespace fmt::literals;

const auto kLogRetryAttemptThreshold = 20;

void tellShardsToRefreshCollection(OperationContext* opCtx,
                                   const std::vector<ShardId>& shardIds,
                                   const NamespaceString& nss,
                                   const std::shared_ptr<executor::TaskExecutor>& executor) {
    auto cmd = FlushRoutingTableCacheUpdatesWithWriteConcern(nss);
    cmd.setSyncFromConfig(true);
    cmd.setDbName(nss.dbName());
    auto cmdObj = CommandHelpers::appendMajorityWriteConcern(cmd.toBSON({}));
    sendCommandToShards(opCtx, DatabaseName::kAdmin, cmdObj, shardIds, executor);
}

void triggerFireAndForgetShardRefreshes(OperationContext* opCtx,
                                        const std::vector<ShardId>& shardIds,
                                        const NamespaceString& nss) {
    auto cmd = FlushRoutingTableCacheUpdates(nss);
    cmd.setSyncFromConfig(true);
    cmd.setDbName(nss.dbName());

    for (const auto& shardId : shardIds) {
        auto recipientShard =
            uassertStatusOK(Grid::get(opCtx)->shardRegistry()->getShard(opCtx, shardId));

        recipientShard->runFireAndForgetCommand(opCtx,
                                                ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                                                DatabaseName::kAdmin,
                                                cmd.toBSON({}));
    }
}

std::vector<AsyncRequestsSender::Response> processShardResponses(
    OperationContext* opCtx,
    const DatabaseName& dbName,
    const BSONObj& command,
    const std::vector<AsyncRequestsSender::Request>& requests,
    const std::shared_ptr<executor::TaskExecutor>& executor,
    bool throwOnError) {

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
                                nullptr /* resourceYielder */,
                                {} /* designatedHostsMap */);

        while (!ars.done()) {
            // Retrieve the responses and throw at the first failure.
            auto response = ars.next();

            if (throwOnError) {
                auto errorContext = "Failed command {} for database '{}' on shard '{}'"_format(
                    command.toString(), dbName.toStringForErrorMsg(), StringData{response.shardId});

                uassertStatusOKWithContext(response.swResponse.getStatus(), errorContext);
                const auto& respBody = response.swResponse.getValue().data;

                auto status = getStatusFromCommandResult(respBody);
                uassertStatusOKWithContext(status, errorContext);

                auto wcStatus = getWriteConcernStatusFromCommandResult(respBody);
                uassertStatusOKWithContext(wcStatus, errorContext);
            }

            responses.push_back(std::move(response));
        }
    }
    return responses;
}

std::vector<AsyncRequestsSender::Response> sendCommandToShards(
    OperationContext* opCtx,
    const DatabaseName& dbName,
    const BSONObj& command,
    const std::vector<ShardId>& shardIds,
    const std::shared_ptr<executor::TaskExecutor>& executor,
    const bool throwOnError) {
    std::vector<AsyncRequestsSender::Request> requests;
    for (const auto& shardId : shardIds) {
        requests.emplace_back(shardId, command);
    }

    return processShardResponses(opCtx, dbName, command, requests, executor, throwOnError);
}

std::vector<AsyncRequestsSender::Response> sendCommandToShardsWithVersion(
    OperationContext* opCtx,
    const DatabaseName& dbName,
    const BSONObj& command,
    const std::vector<ShardId>& shardIds,
    const std::shared_ptr<executor::TaskExecutor>& executor,
    const CollectionRoutingInfo& cri,
    const bool throwOnError) {
    std::vector<AsyncRequestsSender::Request> requests;
    for (const auto& shardId : shardIds) {
        requests.emplace_back(shardId, appendShardVersion(command, cri.getShardVersion(shardId)));
    }
    return processShardResponses(opCtx, dbName, command, requests, executor, throwOnError);
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
            writeConflictRetry(opCtx, "createIndexOnCollection", ns, [&] {
                WriteUnitOfWork wunit(opCtx);
                auto db = autoColl.ensureDbExists(opCtx);
                collection = db->createCollection(opCtx, ns, options);
                invariant(collection,
                          str::stream() << "Failed to create collection "
                                        << ns.toStringForErrorMsg() << " for indexes: " << keys);
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
            CollectionPtr(collection),
            uassertStatusOK(collection->addCollationDefaultsToIndexSpecsForCreate(
                opCtx, std::vector<BSONObj>{index.toBSON()})),
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
            writeConflictRetry(opCtx, "createIndexOnConfigCollection", ns, [&] {
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

Status createShardingIndexCatalogIndexes(OperationContext* opCtx,
                                         const NamespaceString& indexCatalogNamespace) {
    bool unique = true;
    auto result = createIndexOnCollection(opCtx,
                                          indexCatalogNamespace,
                                          BSON(IndexCatalogType::kCollectionUUIDFieldName
                                               << 1 << IndexCatalogType::kLastmodFieldName << 1),
                                          !unique);
    if (!result.isOK()) {
        return result.withContext(str::stream()
                                  << "couldn't create collectionUUID_1_lastmod_1 index on "
                                  << indexCatalogNamespace.toStringForErrorMsg());
    }
    result = createIndexOnCollection(opCtx,
                                     indexCatalogNamespace,
                                     BSON(IndexCatalogType::kCollectionUUIDFieldName
                                          << 1 << IndexCatalogType::kNameFieldName << 1),
                                     unique);
    if (!result.isOK()) {
        return result.withContext(str::stream()
                                  << "couldn't create collectionUUID_1_name_1 index on "
                                  << indexCatalogNamespace.toStringForErrorMsg());
    }
    return Status::OK();
}

Status createShardCollectionCatalogIndexes(OperationContext* opCtx) {
    bool unique = true;
    auto result =
        createIndexOnCollection(opCtx,
                                NamespaceString::kShardCollectionCatalogNamespace,
                                BSON(ShardAuthoritativeCollectionType::kUuidFieldName << 1),
                                !unique);
    if (!result.isOK()) {
        return result.withContext(str::stream() << "couldn't create uuid_1 index on "
                                                << NamespaceString::kShardCollectionCatalogNamespace
                                                       .toStringForErrorMsg());
    }

    return Status::OK();
}

void invokeCommandOnShardWithIdempotentRetryPolicy(OperationContext* opCtx,
                                                   const ShardId& recipientId,
                                                   const DatabaseName& dbName,
                                                   const BSONObj& cmd) {
    auto recipientShard =
        uassertStatusOK(Grid::get(opCtx)->shardRegistry()->getShard(opCtx, recipientId));

    LOGV2_DEBUG(22023, 1, "Sending request to recipient", "commandToSend"_attr = redact(cmd));

    auto response = recipientShard->runCommandWithFixedRetryAttempts(
        opCtx,
        ReadPreferenceSetting{ReadPreference::PrimaryOnly},
        dbName,
        cmd,
        Shard::RetryPolicy::kIdempotent);

    uassertStatusOK(response.getStatus());
    uassertStatusOK(getStatusFromWriteCommandReply(response.getValue().response));
}

void retryIdempotentWorkAsPrimaryUntilSuccessOrStepdown(
    OperationContext* opCtx,
    StringData taskDescription,
    std::function<void(OperationContext*)> doWork,
    boost::optional<Backoff> backoff) {
    const std::string newClientName = "{}-{}"_format(getThreadName(), taskDescription);
    const auto initialTerm = repl::ReplicationCoordinator::get(opCtx)->getTerm();

    for (int attempt = 1;; attempt++) {
        // Since we can't differenciate if a shutdown exception is coming from a remote node or
        // locally we need to directly inspect the the global shutdown state to correctly
        // interrupt this task in case this node is shutting down.
        if (globalInShutdownDeprecated()) {
            uasserted(ErrorCodes::ShutdownInProgress, "Shutdown in progress");
        }

        // If the node is no longer primary, stop retrying.
        uassert(ErrorCodes::InterruptedDueToReplStateChange,
                "Stepped down while {}"_format(taskDescription),
                repl::ReplicationCoordinator::get(opCtx)->getMemberState() ==
                    repl::MemberState::RS_PRIMARY);

        // If the term changed, that means that the step up recovery could have run or is
        // running so stop retrying in order to avoid duplicate work.
        uassert(ErrorCodes::InterruptedDueToReplStateChange,
                "Term changed while {}"_format(taskDescription),
                initialTerm == repl::ReplicationCoordinator::get(opCtx)->getTerm());

        try {
            auto newClient = opCtx->getServiceContext()
                                 ->getService(ClusterRole::ShardServer)
                                 ->makeClient(newClientName);
            auto newOpCtx = newClient->makeOperationContext();
            AlternativeClientRegion altClient(newClient);

            doWork(newOpCtx.get());
            break;
        } catch (DBException& ex) {
            if (backoff) {
                sleepFor(backoff->nextSleep());
            }

            if (attempt % kLogRetryAttemptThreshold == 1) {
                LOGV2_WARNING(23937,
                              "Retrying task after failed attempt",
                              "taskDescription"_attr = redact(taskDescription),
                              "attempt"_attr = attempt,
                              "error"_attr = redact(ex));
            }
        }
    }
}

}  // namespace sharding_util
}  // namespace mongo
