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


#include "mongo/db/global_catalog/ddl/sharding_util.h"

#include "mongo/base/status_with.h"
#include "mongo/client/index_spec.h"
#include "mongo/client/read_preference.h"
#include "mongo/db/commands.h"
#include "mongo/db/database_name.h"
#include "mongo/db/generic_argument_util.h"
#include "mongo/db/global_catalog/router_role_api/cluster_commands_helpers.h"
#include "mongo/db/index_builds/index_builds_coordinator.h"
#include "mongo/db/index_builds/index_builds_manager.h"
#include "mongo/db/local_catalog/catalog_raii.h"
#include "mongo/db/local_catalog/collection.h"
#include "mongo/db/local_catalog/collection_options.h"
#include "mongo/db/local_catalog/database.h"
#include "mongo/db/local_catalog/ddl/list_databases_gen.h"
#include "mongo/db/local_catalog/index_catalog.h"
#include "mongo/db/local_catalog/index_descriptor.h"
#include "mongo/db/local_catalog/lock_manager/exception_util.h"
#include "mongo/db/local_catalog/lock_manager/lock_manager_defs.h"
#include "mongo/db/local_catalog/shard_role_catalog/flush_routing_table_cache_updates_gen.h"
#include "mongo/db/sharding_environment/client/shard.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/executor/remote_command_response.h"
#include "mongo/logv2/log.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/exit.h"
#include "mongo/util/str.h"
#include "mongo/util/uuid.h"

#include <string>
#include <utility>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>
#include <fmt/format.h>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

namespace mongo {

namespace sharding_util {

const auto kLogRetryAttemptThreshold = 20;

void tellShardsToRefreshCollection(OperationContext* opCtx,
                                   const std::vector<ShardId>& shardIds,
                                   const NamespaceString& nss,
                                   const std::shared_ptr<executor::TaskExecutor>& executor) {
    auto cmd = FlushRoutingTableCacheUpdatesWithWriteConcern(nss);
    cmd.setSyncFromConfig(true);
    cmd.setDbName(nss.dbName());
    generic_argument_util::setMajorityWriteConcern(cmd);
    sendCommandToShards(opCtx, DatabaseName::kAdmin, cmd.toBSON(), shardIds, executor);
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
                                                cmd.toBSON());
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
                auto errorContext = fmt::format("Failed command {} for database '{}' on shard '{}'",
                                                command.toString(),
                                                dbName.toStringForErrorMsg(),
                                                StringData{response.shardId});

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
        auto acquisition = acquireCollection(
            opCtx,
            CollectionAcquisitionRequest::fromOpCtx(opCtx, ns, AcquisitionPrerequisites::kWrite),
            MODE_X);
        if (!acquisition.exists()) {
            CollectionOptions options;
            options.uuid = UUID::gen();
            writeConflictRetry(opCtx, "createIndexOnCollection", ns, [&] {
                WriteUnitOfWork wunit(opCtx);
                AutoGetDb autodb(opCtx, ns.dbName(), MODE_IX);
                ScopedLocalCatalogWriteFence fence(opCtx, &acquisition);
                auto db = autodb.ensureDbExists(opCtx);
                auto collection = db->createCollection(opCtx, ns, options);
                invariant(collection,
                          str::stream() << "Failed to create collection "
                                        << ns.toStringForErrorMsg() << " for indexes: " << keys);
                wunit.commit();
            });
        }
        auto indexCatalog = acquisition.getCollectionPtr()->getIndexCatalog();
        IndexSpec index;
        index.addKeys(keys);
        index.unique(unique);
        index.version(int(IndexConfig::kLatestIndexVersion));
        auto removeIndexBuildsToo = false;
        auto indexSpecs = indexCatalog->removeExistingIndexes(
            opCtx,
            acquisition.getCollectionPtr(),
            uassertStatusOK(
                acquisition.getCollectionPtr()->addCollationDefaultsToIndexSpecsForCreate(
                    opCtx, std::vector<BSONObj>{index.toBSON()})),
            removeIndexBuildsToo);

        if (indexSpecs.empty()) {
            return Status::OK();
        }

        auto fromMigrate = false;
        if (!acquisition.getCollectionPtr()->isEmpty(opCtx)) {
            // We typically create indexes on config/admin collections for sharding while setting up
            // a sharded cluster, so we do not expect to see data in the collection.
            // Therefore, it is ok to log this index build.
            const auto& indexSpec = indexSpecs[0];
            LOGV2(5173300,
                  "Creating index on sharding collection with existing data",
                  logAttrs(ns),
                  "uuid"_attr = acquisition.uuid(),
                  "index"_attr = indexSpec);
            auto indexConstraints = IndexBuildsManager::IndexConstraints::kEnforce;
            IndexBuildsCoordinator::get(opCtx)->createIndex(
                opCtx, acquisition.uuid(), indexSpec, indexConstraints, fromMigrate);
        } else {
            writeConflictRetry(opCtx, "createIndexOnConfigCollection", ns, [&] {
                WriteUnitOfWork wunit(opCtx);
                CollectionWriter collWriter(opCtx, &acquisition);
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

void invokeCommandOnShardWithIdempotentRetryPolicy(OperationContext* opCtx,
                                                   const ShardId& recipientId,
                                                   const DatabaseName& dbName,
                                                   const BSONObj& cmd) {
    auto recipientShard =
        uassertStatusOK(Grid::get(opCtx)->shardRegistry()->getShard(opCtx, recipientId));

    LOGV2_DEBUG(22023, 1, "Sending request to recipient", "commandToSend"_attr = redact(cmd));

    auto response = recipientShard->runCommand(opCtx,
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
    const std::string newClientName = fmt::format("{}-{}", getThreadName(), taskDescription);
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
                fmt::format("Stepped down while {}", taskDescription),
                repl::ReplicationCoordinator::get(opCtx)->getMemberState() ==
                    repl::MemberState::RS_PRIMARY);

        // If the term changed, that means that the step up recovery could have run or is
        // running so stop retrying in order to avoid duplicate work.
        uassert(ErrorCodes::InterruptedDueToReplStateChange,
                fmt::format("Term changed while {}", taskDescription),
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

ShardId selectLeastLoadedNonDrainingShard(OperationContext* opCtx) {
    const auto shardsAndOpTime = [&] {
        try {
            return Grid::get(opCtx)->catalogClient()->getAllShards(
                opCtx,
                repl::ReadConcernLevel::kSnapshotReadConcern,
                BSON(ShardType::draining.ne(true)) /* excludeDraining */);
        } catch (DBException& ex) {
            ex.addContext("Cannot retrieve updated shard list from config server");
            throw;
        }
    }();

    const auto& nonDrainingShards = shardsAndOpTime.value;
    uassert(ErrorCodes::ShardNotFound, "No non-draining shard found", !nonDrainingShards.empty());

    std::vector<ShardId> shardIds;
    std::transform(nonDrainingShards.begin(),
                   nonDrainingShards.end(),
                   std::back_inserter(shardIds),
                   [](const ShardType& shard) { return ShardId(shard.getName()); });

    if (shardIds.size() == 1) {
        return shardIds.front();
    }

    ListDatabasesCommand command;
    command.setDbName(DatabaseName::kAdmin);

    const auto executor = Grid::get(opCtx)->getExecutorPool()->getFixedExecutor();
    auto responsesFromShards = sharding_util::sendCommandToShards(opCtx,
                                                                  DatabaseName::kAdmin,
                                                                  command.toBSON(),
                                                                  shardIds,
                                                                  executor,
                                                                  false /* throwOnError */);

    auto candidateShardId = shardIds.front();
    auto candidateSize = std::numeric_limits<long long>::max();

    for (auto&& response : responsesFromShards) {
        const auto& shardId = response.shardId;

        auto errorContext =
            fmt::format("Failed to get the list of databases from shard '{}'", shardId.toString());
        const auto responseValue =
            uassertStatusOKWithContext(std::move(response.swResponse), errorContext);
        const ListDatabasesReply reply =
            ListDatabasesReply::parse(responseValue.data, IDLParserContext("ListDatabasesReply"));
        const auto currentSize = reply.getTotalSize();
        uassert(ErrorCodes::UnknownError,
                fmt::format("Received unrecognized reply for ListDatabasesCommand : {}",
                            responseValue.data.toString()),
                currentSize.has_value());

        if (currentSize.value() < candidateSize) {
            candidateSize = currentSize.value();
            candidateShardId = shardId;
        }
    }

    return candidateShardId;
}

}  // namespace sharding_util
}  // namespace mongo
