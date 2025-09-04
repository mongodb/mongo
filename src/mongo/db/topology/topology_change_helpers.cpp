/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/db/topology/topology_change_helpers.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobj_comparator.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/json.h"
#include "mongo/bson/timestamp.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/client/connection_string.h"
#include "mongo/client/fetcher.h"
#include "mongo/client/read_preference.h"
#include "mongo/client/remote_command_targeter.h"
#include "mongo/client/replica_set_monitor.h"
#include "mongo/db/audit.h"
#include "mongo/db/cluster_parameters/cluster_server_parameter_common.h"
#include "mongo/db/cluster_parameters/set_cluster_parameter_invocation.h"
#include "mongo/db/cluster_parameters/sharding_cluster_parameters_gen.h"
#include "mongo/db/commands.h"
#include "mongo/db/database_name.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/generic_argument_util.h"
#include "mongo/db/global_catalog/ddl/ddl_lock_manager.h"
#include "mongo/db/global_catalog/ddl/sharded_ddl_commands_gen.h"
#include "mongo/db/global_catalog/ddl/sharding_catalog_manager.h"
#include "mongo/db/global_catalog/ddl/sharding_ddl_util.h"
#include "mongo/db/global_catalog/ddl/sharding_util.h"
#include "mongo/db/global_catalog/ddl/shardsvr_join_ddl_coordinators_request_gen.h"
#include "mongo/db/global_catalog/ddl/shardsvr_join_migrations_request_gen.h"
#include "mongo/db/global_catalog/sharding_catalog_client.h"
#include "mongo/db/global_catalog/type_chunk.h"
#include "mongo/db/global_catalog/type_remove_shard_event_gen.h"
#include "mongo/db/global_catalog/type_shard.h"
#include "mongo/db/keys_collection_util.h"
#include "mongo/db/local_catalog/catalog_raii.h"
#include "mongo/db/local_catalog/ddl/list_collections_gen.h"
#include "mongo/db/local_catalog/ddl/list_databases_for_all_tenants_gen.h"
#include "mongo/db/local_catalog/drop_database.h"
#include "mongo/db/logical_time.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/persistent_task_store.h"
#include "mongo/db/query/find_command.h"
#include "mongo/db/query/write_ops/write_ops_gen.h"
#include "mongo/db/query/write_ops/write_ops_parsers.h"
#include "mongo/db/read_write_concern_defaults.h"
#include "mongo/db/repl/hello/hello_gen.h"
#include "mongo/db/repl/optime_with.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/db/repl/read_concern_level.h"
#include "mongo/db/repl/repl_client_info.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/wait_for_majority_service.h"
#include "mongo/db/s/range_deletion_task_gen.h"
#include "mongo/db/server_parameter.h"
#include "mongo/db/service_context.h"
#include "mongo/db/session/logical_session_id.h"
#include "mongo/db/sharding_environment/client/shard.h"
#include "mongo/db/sharding_environment/cluster_identity_loader.h"
#include "mongo/db/sharding_environment/grid.h"
#include "mongo/db/sharding_environment/shard_id.h"
#include "mongo/db/sharding_environment/sharding_config_server_parameters_gen.h"
#include "mongo/db/tenant_id.h"
#include "mongo/db/topology/add_shard_gen.h"
#include "mongo/db/topology/remove_shard_draining_progress_gen.h"
#include "mongo/db/topology/topology_change_helpers.h"
#include "mongo/db/transaction/transaction_api.h"
#include "mongo/db/user_write_block/set_user_write_block_mode_gen.h"
#include "mongo/db/user_write_block/user_writes_critical_section_document_gen.h"
#include "mongo/db/user_write_block/user_writes_recoverable_critical_section_service.h"
#include "mongo/db/vector_clock/vector_clock_mutable.h"
#include "mongo/db/write_concern_options.h"
#include "mongo/executor/remote_command_request.h"
#include "mongo/executor/remote_command_response.h"
#include "mongo/executor/task_executor.h"
#include "mongo/logv2/log.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/rpc/metadata.h"
#include "mongo/s/write_ops/batched_command_request.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/database_name_util.h"
#include "mongo/util/duration.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/str.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

namespace mongo {

namespace {
MONGO_FAIL_POINT_DEFINE(hangAddShardBeforeUpdatingClusterCardinalityParameter);
MONGO_FAIL_POINT_DEFINE(skipBlockingDDLCoordinatorsDuringAddAndRemoveShard);
MONGO_FAIL_POINT_DEFINE(hangAfterDroppingDatabaseInTransitionToDedicatedConfigServer);

const Seconds kRemoteCommandTimeout{60};

const WriteConcernOptions kMajorityWriteConcern{WriteConcernOptions::kMajority,
                                                WriteConcernOptions::SyncMode::UNSET,
                                                WriteConcernOptions::kNoTimeout};

const ReadPreferenceSetting kConfigReadSelector(ReadPreference::Nearest, TagSet{});
constexpr StringData kAddOrRemoveShardInProgressRecoveryDocumentId =
    "addOrRemoveShardInProgressRecovery"_sd;

AggregateCommandRequest makeUnshardedCollectionsOnSpecificShardAggregation(OperationContext* opCtx,
                                                                           const ShardId& shardId,
                                                                           bool isCount = false) {
    static const BSONObj listStage = fromjson(R"({
       $listClusterCatalog: { "shards": true }
     })");
    const BSONObj shardsCondition = BSON("shards" << shardId);
    const BSONObj matchStage = fromjson(str::stream() << R"({
       $match: {
           $and: [
               { sharded: false },
               { db: {$ne: 'config'} },
               { db: {$ne: 'admin'} },
               )" << shardsCondition.jsonString() << R"(,
               { type: {$nin: ["timeseries","view"]} },
               { ns: {$not: {$regex: "^enxcol_\..*(\.esc|\.ecc|\.ecoc|\.ecoc\.compact)$"} }},
               { $or: [
                    {ns: {$not: { $regex: "\.system\." }}},
                    {ns: {$regex: "\.system\.buckets\."}}
               ]}
           ]
        }
    })");
    static const BSONObj projectStage = fromjson(R"({
       $project: {
           _id: 0,
           ns: {
               $cond: [
                   "$options.timeseries",
                   {
                       $replaceAll: {
                           input: "$ns",
                           find: ".system.buckets",
                           replacement: ""
                       }
                   },
                   "$ns"
               ]
           }
       }
    })");
    const BSONObj countStage = BSON("$count" << "totalCount");

    auto dbName = NamespaceString::makeCollectionlessAggregateNSS(DatabaseName::kAdmin);

    std::vector<mongo::BSONObj> pipeline;
    pipeline.reserve(4);
    pipeline.push_back(listStage);
    pipeline.push_back(matchStage);
    if (isCount) {
        pipeline.push_back(countStage);
    } else {
        pipeline.push_back(projectStage);
    }

    AggregateCommandRequest aggRequest{dbName, pipeline};
    aggRequest.setReadConcern(repl::ReadConcernArgs::kLocal);
    aggRequest.setWriteConcern({});
    return aggRequest;
}

AggregateCommandRequest makeChunkCountAggregation(OperationContext* opCtx, const ShardId& shardId) {
    std::vector<BSONObj> pipeline;

    // Match documents in the chunks collection where shard is "shard01"
    pipeline.emplace_back(BSON("$match" << BSON("shard" << shardId)));
    // Perform a $group to count chunks by uuid
    pipeline.emplace_back(fromjson(R"(
            { $group: {
                '_id': '$uuid',
                'count': { $sum: 1 }
            }})"));
    // Fetch the collection global catalog
    pipeline.emplace_back(fromjson(R"(
            { $lookup: {
                from: {
                    db: "config",
                    coll: "collections"
                },
                localField: "_id",
                foreignField: "uuid",
                as: "collectionInfo"
            }})"));
    pipeline.emplace_back(fromjson(R"(
            { $unwind: "$collectionInfo"})"));
    pipeline.emplace_back(fromjson(R"(
            { $match: {
                "collectionInfo.unsplittable": {$ne: true}
            }})"));
    // Deliver a single document with the aggregation of all the chunks
    pipeline.emplace_back(fromjson(R"(
            { $group: {
                '_id': null,
                'totalChunks': {$sum: '$count'}
            }})"));

    AggregateCommandRequest aggRequest{NamespaceString::kConfigsvrChunksNamespace, pipeline};
    aggRequest.setReadConcern(repl::ReadConcernArgs(repl::ReadConcernLevel::kSnapshotReadConcern));
    return aggRequest;
}

long long getCollectionsToMoveForShardCount(OperationContext* opCtx,
                                            Shard* shard,
                                            const ShardId& shardId) {

    auto listCollectionAggReq =
        makeUnshardedCollectionsOnSpecificShardAggregation(opCtx, shardId, true);

    long long collectionsCounter = 0;

    uassertStatusOK(shard->runAggregation(
        opCtx,
        listCollectionAggReq,
        [&collectionsCounter](const std::vector<BSONObj>& batch,
                              const boost::optional<BSONObj>& postBatchResumeToken) {
            if (batch.size() > 0) {
                tassert(8988300, "totalCount field is missing", batch[0].hasField("totalCount"));
                collectionsCounter = batch[0].getField("totalCount").safeNumberLong();
            }
            return true;
        }));

    return collectionsCounter;
}

long long getChunkForShardCount(OperationContext* opCtx, Shard* shard, const ShardId& shardId) {

    auto chunkCounterAggReq = makeChunkCountAggregation(opCtx, shardId);

    long long chunkCounter = 0;

    uassertStatusOK(shard->runAggregation(
        opCtx,
        chunkCounterAggReq,
        [&chunkCounter](const std::vector<BSONObj>& batch,
                        const boost::optional<BSONObj>& postBatchResumeToken) {
            if (batch.size() > 0) {
                chunkCounter = batch[0].getField("totalChunks").safeNumberLong();
            }
            return true;
        }));

    return chunkCounter;
}

void joinOngoingShardingDDLCoordinatorsOnShards(OperationContext* opCtx) {
    const auto shardRegistry = Grid::get(opCtx)->shardRegistry();
    auto allShards = shardRegistry->getAllShardIds(opCtx);
    if (std::find(allShards.begin(), allShards.end(), ShardId::kConfigServerId) ==
        allShards.end()) {
        // The config server may be a shard, so only add if it isn't already in participants.
        allShards.emplace_back(shardRegistry->getConfigShard()->getId());
    }
    auto executor = Grid::get(opCtx)->getExecutorPool()->getFixedExecutor();

    ShardsvrJoinDDLCoordinators cmd;
    cmd.setDbName(DatabaseName::kAdmin);

    sharding_util::sendCommandToShards(
        opCtx, DatabaseName::kAdmin, cmd.toBSON(), allShards, executor);
}

void setAddOrRemoveShardInProgressClusterParam(OperationContext* opCtx, bool newState) {
    while (true) {
        try {
            ConfigsvrSetClusterParameter setClusterParameter(
                BSON("addOrRemoveShardInProgress" << BSON("inProgress" << newState)));
            setClusterParameter.setDbName(DatabaseName::kAdmin);
            setClusterParameter.set_compatibleWithTopologyChange(true);

            DBDirectClient client(opCtx);
            BSONObj res;
            client.runCommand(DatabaseName::kAdmin, setClusterParameter.toBSON(), res);
            uassertStatusOK(getStatusFromWriteCommandReply(res));
            break;
        } catch (const ExceptionFor<ErrorCodes::ConflictingOperationInProgress>&) {
            // Retry on ErrorCodes::ConflictingOperationInProgress errors, which can be caused by an
            // already running unrelated setClusterParameter.
            opCtx->sleepFor(Milliseconds(500));
            continue;
        }
    }
}

boost::optional<RemoveShardProgress> checkCollectionsAreEmpty(
    OperationContext* opCtx, const std::vector<NamespaceString>& collections) {
    for (const auto& nss : collections) {
        AutoGetCollection autoColl(opCtx, nss, MODE_IS);
        if (!autoColl) {
            // Can't find the collection, so it must not have data.
            continue;
        }

        if (!autoColl->isEmpty(opCtx)) {
            LOGV2(9022300, "removeShard: found non-empty local collection", logAttrs(nss));
            RemoveShardProgress progress(ShardDrainingStateEnum::kPendingDataCleanup);
            progress.setFirstNonEmptyCollection(nss);
            progress.setPendingRangeDeletions(
                0);  // Set this to 0 so that it is serialized in the response
            return {progress};
        }
    }

    return boost::none;
}

void waitUntilReadyToBlockNewDDLCoordinators(OperationContext* opCtx) {
    const auto wouldJoinCoordinatorsBlock = [](OperationContext* opCtx) -> bool {
        // Check that all shards will be able to join ongoing DDLs quickly.
        const auto shardRegistry = Grid::get(opCtx)->shardRegistry();
        auto allShards = shardRegistry->getAllShardIds(opCtx);
        if (std::find(allShards.begin(), allShards.end(), ShardId::kConfigServerId) ==
            allShards.end()) {
            // The config server may be a shard, so only add if it isn't already in participants.
            allShards.emplace_back(shardRegistry->getConfigShard()->getId());
        }
        auto executor = Grid::get(opCtx)->getExecutorPool()->getFixedExecutor();

        ShardsvrJoinDDLCoordinators cmd;
        cmd.setDbName(DatabaseName::kAdmin);

        // Attach a short MaxTimeMS. If _shardsvrJoinDDLOperations fails with MaxTimeMSExpired on
        // some shard, then it means that some long-running ShardingDDLCoordinators is executing.
        cmd.setMaxTimeMS(30000);

        try {
            const auto responses = sharding_util::sendCommandToShards(
                opCtx, DatabaseName::kAdmin, cmd.toBSON(), allShards, executor);
        } catch (const ExceptionFor<ErrorCodes::MaxTimeMSExpired>&) {
            // Return true if any of the shards failed with MaxTimeMSExpired.
            return true;
        }

        return false;
    };

    while (true) {
        if (wouldJoinCoordinatorsBlock(opCtx)) {
            LOGV2(5687901,
                  "Add/remove shard requires all DDL operations on the cluster to quiesce before it"
                  "can proceed safely. 30 seconds have passed without DDLs quiescing. Waiting for "
                  "DDL operations to quiesce before continuing.");
            continue;
        }
        return;
    }
}

void removeShardInTransaction(OperationContext* opCtx,
                              const std::string& removedShardName,
                              const std::string& controlShardName,
                              const Timestamp& newTopologyTime,
                              std::shared_ptr<executor::TaskExecutor> executor) {
    auto removeShardFn = [removedShardName, controlShardName, newTopologyTime](
                             const txn_api::TransactionClient& txnClient, ExecutorPtr txnExec) {
        write_ops::DeleteCommandRequest deleteOp(NamespaceString::kConfigsvrShardsNamespace);
        deleteOp.setDeletes({[&]() {
            write_ops::DeleteOpEntry entry;
            entry.setMulti(false);
            entry.setQ(BSON(ShardType::name() << removedShardName));
            return entry;
        }()});
        return txnClient.runCRUDOp(deleteOp, {})
            .thenRunOn(txnExec)
            .then([&txnClient, removedShardName, controlShardName, newTopologyTime](
                      auto deleteResponse) {
                uassertStatusOK(deleteResponse.toStatus());

                write_ops::UpdateCommandRequest updateOp(
                    NamespaceString::kConfigsvrShardsNamespace);
                updateOp.setUpdates({[&]() {
                    write_ops::UpdateOpEntry entry;
                    entry.setUpsert(false);
                    entry.setMulti(false);
                    entry.setQ(BSON(ShardType::name() << controlShardName));
                    entry.setU(write_ops::UpdateModification::parseFromClassicUpdate(
                        BSON("$set" << BSON(ShardType::topologyTime() << newTopologyTime))));
                    return entry;
                }()});

                return txnClient.runCRUDOp(updateOp, {});
            })
            .thenRunOn(txnExec)
            .then([&txnClient, newTopologyTime](auto updateResponse) {
                uassertStatusOK(updateResponse.toStatus());
                // Log the topology time associated to this commit in a dedicated document (and
                // delete information about a previous commit if present).
                write_ops::UpdateCommandRequest upsertOp(
                    NamespaceString::kConfigsvrShardRemovalLogNamespace);
                upsertOp.setUpdates({[&]() {
                    write_ops::UpdateOpEntry entry;
                    entry.setUpsert(true);
                    entry.setMulti(false);
                    entry.setQ(BSON("_id" << ShardingCatalogClient::kLatestShardRemovalLogId));
                    entry.setU(write_ops::UpdateModification::parseFromClassicUpdate(
                        BSON("$set" << BSON(RemoveShardEventType::kTimestampFieldName
                                            << newTopologyTime))));
                    return entry;
                }()});

                return txnClient.runCRUDOp(upsertOp, {});
            })
            .thenRunOn(txnExec)
            .then([removedShardName](auto upsertResponse) {
                uassertStatusOK(upsertResponse.toStatus());
                LOGV2_DEBUG(
                    6583701, 1, "Finished removing shard ", "shard"_attr = removedShardName);
            })
            .semi();
    };

    auto inlineExecutor = std::make_shared<executor::InlineExecutor>();

    txn_api::SyncTransactionWithRetries txn(opCtx, executor, nullptr, inlineExecutor);

    txn.run(opCtx, removeShardFn);
}

using FetcherDocsCallbackFn = std::function<bool(const std::vector<BSONObj>& batch)>;
using FetcherStatusCallbackFn = std::function<void(const Status& status)>;

std::unique_ptr<Fetcher> createFetcher(OperationContext* opCtx,
                                       RemoteCommandTargeter& targeter,
                                       const DatabaseName& dbName,
                                       const Milliseconds maxTimeMS,
                                       const BSONObj command,
                                       FetcherDocsCallbackFn processDocsCallback,
                                       FetcherStatusCallbackFn processStatusCallback,
                                       std::shared_ptr<executor::TaskExecutor> executor) {
    auto host = uassertStatusOK(
        targeter.findHost(opCtx, ReadPreferenceSetting{ReadPreference::PrimaryOnly}));

    auto fetcherCallback = [processDocsCallback,
                            processStatusCallback](const Fetcher::QueryResponseStatus& dataStatus,
                                                   Fetcher::NextAction* nextAction,
                                                   BSONObjBuilder* getMoreBob) {
        // Throw out any accumulated results on error.
        if (!dataStatus.isOK()) {
            processStatusCallback(dataStatus.getStatus());
            return;
        }
        const auto& data = dataStatus.getValue();

        try {
            if (!processDocsCallback(data.documents)) {
                *nextAction = Fetcher::NextAction::kNoAction;
            }
        } catch (DBException& ex) {
            processStatusCallback(ex.toStatus());
            return;
        }
        processStatusCallback(Status::OK());

        if (!getMoreBob) {
            return;
        }
        getMoreBob->append("getMore", data.cursorId);
        getMoreBob->append("collection", data.nss.coll());
    };

    return std::make_unique<Fetcher>(executor.get(),
                                     host,
                                     dbName,
                                     command,
                                     fetcherCallback,
                                     BSONObj(), /* metadata tracking, only used for shards */
                                     maxTimeMS, /* command network timeout */
                                     maxTimeMS /* getMore network timeout */);
}

void deleteAllDocumentsInCollection(
    OperationContext* opCtx,
    RemoteCommandTargeter& targeter,
    const NamespaceString& nss,
    boost::optional<std::function<OperationSessionInfo(OperationContext*)>> osiGenerator,
    std::shared_ptr<executor::TaskExecutor> executor) {
    // We need to fetch manually rather than using Shard::runExhaustiveCursorCommand because
    // ShardRemote uses the FixedTaskExecutor which checks that the host being targeted is a
    // fully added shard already
    auto fetcherStatus =
        Status(ErrorCodes::InternalError, "Internal error running cursor callback in command");
    std::vector<BSONObj> docsToDelete;
    auto fetcher = topology_change_helpers::createFindFetcher(
        opCtx,
        targeter,
        nss,
        BSONObj() /* filter */,
        repl::ReadConcernLevel::kMajorityReadConcern,
        [&](const std::vector<BSONObj>& docs) -> bool {
            for (const BSONObj& doc : docs) {
                docsToDelete.emplace_back(doc.getOwned());
            }
            return true;
        },
        [&](const Status& status) { fetcherStatus = status; },
        executor);
    uassertStatusOK(fetcher->schedule());
    uassertStatusOK(fetcher->join(opCtx));
    uassertStatusOK(fetcherStatus);

    const auto sendDelete = [&](std::vector<mongo::write_ops::DeleteOpEntry>&& deleteOps) {
        if (deleteOps.empty()) {
            return;
        }
        write_ops::DeleteCommandRequest deleteOp(nss);
        deleteOp.setDeletes(deleteOps);
        generic_argument_util::setMajorityWriteConcern(deleteOp);
        if (osiGenerator) {
            auto const osi = (*osiGenerator)(opCtx);
            generic_argument_util::setOperationSessionInfo(deleteOp, osi);
        }

        const auto commandResponse = topology_change_helpers::runCommandForAddShard(
            opCtx, targeter, nss.dbName(), deleteOp.toBSON(), executor);
        uassertStatusOK(getStatusFromWriteCommandReply(commandResponse.response));
    };

    std::vector<mongo::write_ops::DeleteOpEntry> deleteOps;
    deleteOps.reserve(write_ops::kMaxWriteBatchSize);
    for (auto& element : docsToDelete) {
        write_ops::DeleteOpEntry entry;
        entry.setQ(std::move(element));
        entry.setMulti(false);
        deleteOps.emplace_back(std::move(entry));

        if (deleteOps.size() == write_ops::kMaxWriteBatchSize) {
            sendDelete(std::move(deleteOps));
            deleteOps = std::vector<mongo::write_ops::DeleteOpEntry>();
            deleteOps.reserve(write_ops::kMaxWriteBatchSize);
        }
    }
    sendDelete(std::move(deleteOps));
}

BSONObj greetReplicaSet(OperationContext* opCtx,
                        RemoteCommandTargeter& targeter,
                        std::shared_ptr<executor::TaskExecutor> executor) {

    boost::optional<Shard::CommandResponse> commandResponse;
    try {
        commandResponse = topology_change_helpers::runCommandForAddShard(
            opCtx, targeter, DatabaseName::kAdmin, BSON("hello" << 1), executor);
    } catch (ExceptionFor<ErrorCodes::IncompatibleServerVersion>& ex) {
        uassertStatusOK(ex.toStatus().withReason(
            str::stream() << "Cannot add " << targeter.connectionString().toString()
                          << " as a shard because its binary version is not compatible with the "
                             "cluster's featureCompatibilityVersion."));
    }

    // Check for a command response error
    uassertStatusOKWithContext(commandResponse->commandStatus,
                               str::stream() << "Error running 'hello' against "
                                             << targeter.connectionString().toString());

    return std::move(commandResponse->response);
}

void removeAllClusterParametersFromReplicaSet(
    OperationContext* opCtx,
    RemoteCommandTargeter& targeter,
    boost::optional<std::function<OperationSessionInfo(OperationContext*)>> osiGenerator,
    std::shared_ptr<executor::TaskExecutor> executor) {
    auto tenantsOnTarget =
        uassertStatusOK(getTenantsWithConfigDbsOnShard(opCtx, targeter, executor));

    // Remove possible leftovers config.clusterParameters documents from the new shard.
    for (const auto& tenantId : tenantsOnTarget) {
        const auto& nss = NamespaceString::makeClusterParametersNSS(tenantId);
        deleteAllDocumentsInCollection(opCtx, targeter, nss, osiGenerator, executor);
    }
}

}  // namespace

namespace topology_change_helpers {

ShardIdentityType createShardIdentity(OperationContext* opCtx, const ShardId& shardName) {
    ShardIdentityType shardIdentity;
    shardIdentity.setShardName(shardName.toString());
    shardIdentity.setClusterId(ClusterIdentityLoader::get(opCtx)->getClusterId());
    shardIdentity.setConfigsvrConnectionString(
        repl::ReplicationCoordinator::get(opCtx)->getConfigConnectionString());

    return shardIdentity;
}

std::unique_ptr<Fetcher> createFindFetcher(OperationContext* opCtx,
                                           RemoteCommandTargeter& targeter,
                                           const NamespaceString& nss,
                                           const BSONObj& filter,
                                           const repl::ReadConcernLevel& readConcernLevel,
                                           FetcherDocsCallbackFn processDocsCallback,
                                           FetcherStatusCallbackFn processStatusCallback,
                                           std::shared_ptr<executor::TaskExecutor> executor) {
    FindCommandRequest findCommand(nss);
    const auto readConcern = repl::ReadConcernArgs(readConcernLevel);
    findCommand.setReadConcern(readConcern);
    const Milliseconds maxTimeMS =
        std::min(opCtx->getRemainingMaxTimeMillis(), Milliseconds(kRemoteCommandTimeout));
    findCommand.setMaxTimeMS(durationCount<Milliseconds>(maxTimeMS));
    findCommand.setFilter(filter);

    return createFetcher(opCtx,
                         targeter,
                         nss.dbName(),
                         maxTimeMS,
                         findCommand.toBSON(),
                         processDocsCallback,
                         processStatusCallback,
                         executor);
}

long long getRangeDeletionCount(OperationContext* opCtx) {
    PersistentTaskStore<RangeDeletionTask> store(NamespaceString::kRangeDeletionNamespace);
    return static_cast<long long>(store.count(opCtx, BSONObj()));
}

void joinMigrations(OperationContext* opCtx) {
    // Join migrations to make sure there's no ongoing MigrationDestinationManager. New ones
    // will observe the draining state and abort before performing any work that could re-create
    // local catalog collections/dbs.
    DBDirectClient client(opCtx);
    BSONObj resultInfo;
    ShardsvrJoinMigrations shardsvrJoinMigrations;
    shardsvrJoinMigrations.setDbName(DatabaseName::kAdmin);
    const auto result =
        client.runCommand(DatabaseName::kAdmin, shardsvrJoinMigrations.toBSON(), resultInfo);
    uassert(8955101, "Failed to await ongoing migrations before removing catalog shard", result);
}

boost::optional<ShardType> getExistingShard(OperationContext* opCtx,
                                            const ConnectionString& proposedShardConnectionString,
                                            const boost::optional<StringData>& proposedShardName,
                                            ShardingCatalogClient& localCatalogClient) {
    // Check whether any host in the connection is already part of the cluster.
    const auto existingShards = [&] {
        try {
            return localCatalogClient.getAllShards(opCtx,
                                                   repl::ReadConcernLevel::kLocalReadConcern);
        } catch (DBException& ex) {
            ex.addContext("Failed to load existing shards during addShard");
            throw;
        }
    }();

    // Now check if this shard already exists - if it already exists *with the same options* then
    // the addShard request can return success early without doing anything more.
    for (const auto& existingShard : existingShards.value) {
        auto existingShardConnStr =
            uassertStatusOK(ConnectionString::parse(existingShard.getHost()));

        // Function for determining if the options for the shard that is being added match the
        // options of an existing shard that conflicts with it.
        auto shardsAreEquivalent = [&]() {
            if (proposedShardName && proposedShardName.value() != existingShard.getName()) {
                return false;
            }
            if (proposedShardConnectionString.type() != existingShardConnStr.type()) {
                return false;
            }
            if (proposedShardConnectionString.type() ==
                    ConnectionString::ConnectionType::kReplicaSet &&
                proposedShardConnectionString.getSetName() != existingShardConnStr.getSetName()) {
                return false;
            }
            return true;
        };

        // Function for determining if there is an overlap amongst the hosts of the existing shard
        // and the propsed shard.
        auto checkIfHostsAreEquivalent =
            [&](bool checkShardEquivalency) -> boost::optional<ShardType> {
            for (const auto& existingHost : existingShardConnStr.getServers()) {
                for (const auto& addingHost : proposedShardConnectionString.getServers()) {
                    if (existingHost == addingHost) {
                        if (checkShardEquivalency) {
                            // At least one of the hosts in the shard being added already exists in
                            // an existing shard. If the options aren't the same, then this is an
                            // error, but if the options match then the addShard operation should be
                            // immediately considered a success and terminated.
                            uassert(ErrorCodes::IllegalOperation,
                                    str::stream() << "'" << addingHost.toString() << "' "
                                                  << "is already a member of the existing shard '"
                                                  << existingShard.getHost() << "' ("
                                                  << existingShard.getName() << ").",
                                    shardsAreEquivalent());
                            return {existingShard};
                        } else {
                            return {existingShard};
                        }
                    }
                }
            }
            return {boost::none};
        };

        if (existingShardConnStr.type() == ConnectionString::ConnectionType::kReplicaSet &&
            proposedShardConnectionString.type() == ConnectionString::ConnectionType::kReplicaSet &&
            existingShardConnStr.getSetName() == proposedShardConnectionString.getSetName()) {
            // An existing shard has the same replica set name as the shard being added.
            // If the options aren't the same, then this is an error.
            // If the shards are equivalent, there must be some overlap amongst their hosts, if not
            // then addShard results in an error.
            uassert(ErrorCodes::IllegalOperation,
                    str::stream() << "A shard already exists containing the replica set '"
                                  << existingShardConnStr.getSetName() << "'",
                    shardsAreEquivalent());

            auto hostsAreEquivalent = checkIfHostsAreEquivalent(false);

            uassert(ErrorCodes::IllegalOperation,
                    str::stream() << "A shard named " << existingShardConnStr.getSetName()
                                  << " containing the replica set '"
                                  << existingShardConnStr.getSetName() << "' already exists",
                    hostsAreEquivalent != boost::none);
        }

        // Look if any of the hosts in the existing shard are present within the shard trying
        // to be added.
        if (auto hostsAreEquivalent = checkIfHostsAreEquivalent(true);
            hostsAreEquivalent != boost::none) {
            return hostsAreEquivalent;
        }

        if (proposedShardName && proposedShardName.value() == existingShard.getName()) {
            // If we get here then we're trying to add a shard with the same name as an
            // existing shard, but there was no overlap in the hosts between the existing
            // shard and the proposed connection string for the new shard.
            uasserted(ErrorCodes::IllegalOperation,
                      str::stream()
                          << "A shard named " << proposedShardName.value() << " already exists");
        }
    }

    return {boost::none};
}

Shard::CommandResponse runCommandForAddShard(OperationContext* opCtx,
                                             RemoteCommandTargeter& targeter,
                                             const DatabaseName& dbName,
                                             const BSONObj& cmdObj,
                                             std::shared_ptr<executor::TaskExecutor> executor) {
    const auto host = uassertStatusOK(
        targeter.findHost(opCtx, ReadPreferenceSetting{ReadPreference::PrimaryOnly}));

    executor::RemoteCommandRequest request(
        host, dbName, cmdObj, rpc::makeEmptyMetadata(), opCtx, kRemoteCommandTimeout);

    executor::RemoteCommandResponse response(
        host, Status(ErrorCodes::InternalError, "Internal error running command"));

    auto callbackHandle = uassertStatusOK(executor->scheduleRemoteCommand(
        request, [&response](const executor::TaskExecutor::RemoteCommandCallbackArgs& args) {
            response = args.response;
        }));

    // Block until the command is carried out
    executor->wait(callbackHandle);

    if (response.status == ErrorCodes::ExceededTimeLimit) {
        LOGV2(21941, "Operation timed out", "error"_attr = redact(response.status));
    }

    if (!response.isOK()) {
        if (!Shard::shouldErrorBePropagated(response.status.code())) {
            uasserted(ErrorCodes::OperationFailed,
                      str::stream()
                          << "failed to run command " << cmdObj << " when attempting to add shard "
                          << targeter.connectionString().toString() << causedBy(response.status));
        }
        uassertStatusOK(response.status);
    }

    BSONObj result = response.data.getOwned();

    Status commandStatus = getStatusFromCommandResult(result);
    if (!Shard::shouldErrorBePropagated(commandStatus.code())) {
        commandStatus = {
            ErrorCodes::OperationFailed,
            str::stream() << "failed to run command " << cmdObj << " when attempting to add shard "
                          << targeter.connectionString().toString() << causedBy(commandStatus)};
    }

    Status writeConcernStatus = getWriteConcernStatusFromCommandResult(result);
    if (!Shard::shouldErrorBePropagated(writeConcernStatus.code())) {
        writeConcernStatus = {ErrorCodes::OperationFailed,
                              str::stream() << "failed to satisfy writeConcern for command "
                                            << cmdObj << " when attempting to add shard "
                                            << targeter.connectionString().toString()
                                            << causedBy(writeConcernStatus)};
    }

    return Shard::CommandResponse(std::move(host),
                                  std::move(result),
                                  std::move(commandStatus),
                                  std::move(writeConcernStatus));
}

void setUserWriteBlockingState(
    OperationContext* opCtx,
    RemoteCommandTargeter& targeter,
    uint8_t level,
    bool block,
    boost::optional<std::function<OperationSessionInfo(OperationContext*)>> osiGenerator,
    std::shared_ptr<executor::TaskExecutor> executor) {

    deleteAllDocumentsInCollection(opCtx,
                                   targeter,
                                   NamespaceString::kUserWritesCriticalSectionsNamespace,
                                   osiGenerator,
                                   executor);

    const auto makeShardsvrSetUserWriteBlockModeCommand =
        [opCtx, block, &osiGenerator](ShardsvrSetUserWriteBlockModePhaseEnum phase) -> BSONObj {
        ShardsvrSetUserWriteBlockMode shardsvrSetUserWriteBlockModeCmd;
        shardsvrSetUserWriteBlockModeCmd.setDbName(DatabaseName::kAdmin);
        SetUserWriteBlockModeRequest setUserWriteBlockModeRequest(block /* global */);
        shardsvrSetUserWriteBlockModeCmd.setSetUserWriteBlockModeRequest(
            std::move(setUserWriteBlockModeRequest));
        shardsvrSetUserWriteBlockModeCmd.setPhase(phase);
        generic_argument_util::setMajorityWriteConcern(shardsvrSetUserWriteBlockModeCmd);
        if (osiGenerator) {
            auto const osi = (*osiGenerator)(opCtx);
            generic_argument_util::setOperationSessionInfo(shardsvrSetUserWriteBlockModeCmd, osi);
        }

        return shardsvrSetUserWriteBlockModeCmd.toBSON();
    };

    if (level & UserWriteBlockingLevel::DDLOperations) {
        const auto cmd = makeShardsvrSetUserWriteBlockModeCommand(
            ShardsvrSetUserWriteBlockModePhaseEnum::kPrepare);

        const auto cmdResponse =
            runCommandForAddShard(opCtx, targeter, DatabaseName::kAdmin, cmd, executor);
        uassertStatusOK(Shard::CommandResponse::getEffectiveStatus(cmdResponse));
    }

    if (level & UserWriteBlockingLevel::Writes) {
        const auto cmd = makeShardsvrSetUserWriteBlockModeCommand(
            ShardsvrSetUserWriteBlockModePhaseEnum::kComplete);

        const auto cmdResponse =
            runCommandForAddShard(opCtx, targeter, DatabaseName::kAdmin, cmd, executor);
        uassertStatusOK(Shard::CommandResponse::getEffectiveStatus(cmdResponse));
    }
}

std::vector<DatabaseName> getDBNamesListFromReplicaSet(
    OperationContext* opCtx,
    RemoteCommandTargeter& targeter,
    std::shared_ptr<executor::TaskExecutor> executor) {
    auto commandResponse = runCommandForAddShard(opCtx,
                                                 targeter,
                                                 DatabaseName::kAdmin,
                                                 BSON("listDatabases" << 1 << "nameOnly" << true),
                                                 executor);
    uassertStatusOK(commandResponse.commandStatus);

    auto& cmdResult = commandResponse.response;

    std::vector<DatabaseName> dbNames;

    for (const auto& dbEntry : cmdResult["databases"].Obj()) {
        const auto& dbName = DatabaseNameUtil::deserialize(
            boost::none, dbEntry["name"].String(), SerializationContext::stateDefault());

        if (!(dbName.isAdminDB() || dbName.isLocalDB() || dbName.isConfigDB())) {
            dbNames.push_back(dbName);
        }
    }

    return dbNames;
}

void removeReplicaSetMonitor(OperationContext* opCtx, const ConnectionString& connectionString) {
    // Do not remove the RSM for the config server because it is still needed even if
    // adding the config server as a shard failed.
    if (connectionString.type() == ConnectionString::ConnectionType::kReplicaSet &&
        connectionString.getReplicaSetName() !=
            repl::ReplicationCoordinator::get(opCtx)
                ->getConfigConnectionString()
                .getReplicaSetName()) {
        // This is a workaround for the case were we could have some bad shard being
        // requested to be added and we put that bad connection string on the global replica set
        // monitor registry. It needs to be cleaned up so that when a correct replica set is
        // added, it will be recreated.
        ReplicaSetMonitor::remove(connectionString.getSetName());
    }
}

void validateHostAsShard(OperationContext* opCtx,
                         RemoteCommandTargeter& targeter,
                         const ConnectionString& connectionString,
                         bool isConfigShard,
                         std::shared_ptr<executor::TaskExecutor> executor) {
    auto resHello = greetReplicaSet(opCtx, targeter, executor);

    // Fail if the node being added is a mongos.
    const std::string msg = std::string{resHello.getStringField("msg")};
    uassert(ErrorCodes::IllegalOperation, "cannot add a mongos as a shard", msg != "isdbgrid");

    // Extract the maxWireVersion so we can verify that the node being added has a binary
    // version greater than or equal to the cluster's featureCompatibilityVersion. We expect an
    // incompatible binary node to be unable to communicate, returning an
    // IncompatibleServerVersion error, because of our internal wire version protocol. So we can
    // safely invariant here that the node is compatible.
    long long maxWireVersion;
    uassertStatusOKWithContext(
        bsonExtractIntegerField(resHello, "maxWireVersion", &maxWireVersion),
        str::stream() << "hello returned invalid 'maxWireVersion' field when attempting to add "
                      << connectionString.toString() << " as a shard");

    // Check whether the host is a writable primary. If not, the replica set may not have been
    // initiated. If the connection is a standalone, it will return true for "isWritablePrimary".
    bool isWritablePrimary;
    uassertStatusOKWithContext(
        bsonExtractBooleanField(resHello, "isWritablePrimary", &isWritablePrimary),
        str::stream() << "hello returned invalid 'isWritablePrimary' field when attempting to add "
                      << connectionString.toString() << " as a shard");

    uassert(
        ErrorCodes::NotWritablePrimary,
        str::stream() << connectionString.toString()
                      << " does not have a master. If this is a replica set, ensure that it has a"
                      << " healthy primary and that the set has been properly initiated.",
        isWritablePrimary);

    const std::string providedSetName = connectionString.getSetName();
    const std::string foundSetName = resHello["setName"].str();

    // Make sure the specified replica set name (if any) matches the actual shard's replica set
    if (providedSetName.empty() && !foundSetName.empty()) {
        uasserted(
            ErrorCodes::OperationFailed,
            str::stream() << "host is part of set " << foundSetName
                          << "; use replica set url format <setname>/<server1>,<server2>, ...");
    }

    if (!providedSetName.empty() && foundSetName.empty()) {
        uasserted(ErrorCodes::OperationFailed,
                  str::stream()
                      << "host did not return a set name; is the replica set still initializing? "
                      << resHello);
    }

    // Make sure the set name specified in the connection string matches the one where its hosts
    // belong into
    if (!providedSetName.empty() && (providedSetName != foundSetName)) {
        uasserted(ErrorCodes::OperationFailed,
                  str::stream() << "the provided connection string (" << connectionString.toString()
                                << ") does not match the actual set name " << foundSetName);
    }

    // Is it a config server?
    if (resHello.hasField("configsvr") && !isConfigShard) {
        uasserted(ErrorCodes::OperationFailed,
                  str::stream() << "Cannot add " << connectionString.toString()
                                << " as a shard since it is a config server");
    }

    if (resHello.hasField(HelloCommandReply::kIsImplicitDefaultMajorityWCFieldName) &&
        !resHello.getBoolField(HelloCommandReply::kIsImplicitDefaultMajorityWCFieldName) &&
        !ReadWriteConcernDefaults::get(opCtx).isCWWCSet(opCtx)) {
        uasserted(
            ErrorCodes::OperationFailed,
            str::stream()
                << "Cannot add " << connectionString.toString()
                << " as a shard since the implicit default write concern on this shard is set to "
                   "{w : 1}, because number of arbiters in the shard's configuration caused the "
                   "number of writable voting members not to be strictly more than the voting "
                   "majority. Change the shard configuration or set the cluster-wide write concern "
                   "using the setDefaultRWConcern command and try again.");
    }

    // If a config shard is being added, then we can skip comparing the CWWC on the shard and on the
    // config server. Doing this check can introduce a race condition where the CWWC on the config
    // server changes while it transitions to a config shard, causing an operation failure.
    if (!isConfigShard && resHello.hasField(HelloCommandReply::kCwwcFieldName)) {
        auto cwwcOnShard =
            WriteConcernOptions::parse(resHello.getObjectField(HelloCommandReply::kCwwcFieldName))
                .getValue()
                .toBSON();

        auto cachedCWWC = ReadWriteConcernDefaults::get(opCtx).getCWWC(opCtx);
        if (!cachedCWWC) {
            uasserted(ErrorCodes::OperationFailed,
                      str::stream()
                          << "Cannot add " << connectionString.toString()
                          << " as a shard since the cluster-wide write concern is set on the shard "
                             "and not set on the cluster. Set the CWWC on the cluster to the same "
                             "CWWC as the shard and try again. The CWWC on the shard is ("
                          << cwwcOnShard << ").");
        }

        auto cwwcOnConfig = cachedCWWC.value().toBSON();
        BSONObjComparator comparator(
            BSONObj(), BSONObjComparator::FieldNamesMode::kConsider, nullptr);
        if (comparator.compare(cwwcOnShard, cwwcOnConfig) != 0) {
            uasserted(ErrorCodes::OperationFailed,
                      str::stream() << "Cannot add " << connectionString.toString()
                                    << " as a shard since the cluster-wide write concern set on "
                                       "the shard doesn't match the one set on the cluster. Make "
                                       "sure they match and try again. The CWWC on the shard is ("
                                    << cwwcOnShard << "), and the CWWC on the cluster is ("
                                    << cwwcOnConfig << ").");
        }
    }

    // If the shard is part of a replica set, make sure all the hosts mentioned in the connection
    // string are part of the set. It is fine if not all members of the set are mentioned in the
    // connection string, though.
    if (!providedSetName.empty()) {
        std::set<std::string> hostSet;

        BSONObjIterator iter(resHello["hosts"].Obj());
        while (iter.more()) {
            hostSet.insert(iter.next().String());  // host:port
        }

        if (resHello["passives"].isABSONObj()) {
            BSONObjIterator piter(resHello["passives"].Obj());
            while (piter.more()) {
                hostSet.insert(piter.next().String());  // host:port
            }
        }

        if (resHello["arbiters"].isABSONObj()) {
            BSONObjIterator piter(resHello["arbiters"].Obj());
            while (piter.more()) {
                hostSet.insert(piter.next().String());  // host:port
            }
        }

        for (const auto& hostEntry : connectionString.getServers()) {
            const auto& host = hostEntry.toString();  // host:port
            if (hostSet.find(host) == hostSet.end()) {
                uasserted(ErrorCodes::OperationFailed,
                          str::stream() << "in seed list " << connectionString.toString()
                                        << ", host " << host << " does not belong to replica set "
                                        << foundSetName << "; found " << resHello.toString());
            }
        }
    }
}

std::string getRemoveShardMessage(const ShardDrainingStateEnum& status) {
    switch (status) {
        case ShardDrainingStateEnum::kStarted:
            return "draining started successfully";
        case ShardDrainingStateEnum::kOngoing:
            return "draining ongoing";
        case ShardDrainingStateEnum::kPendingDataCleanup:
            return "waiting for data to be cleaned up";
        case ShardDrainingStateEnum::kDrainingComplete:
            return "draining completed successfully";
        case ShardDrainingStateEnum::kCompleted:
            return "removeshard completed successfully";
        default:
            MONGO_UNREACHABLE_TASSERT(10083529);
    }
}

void getClusterTimeKeysFromReplicaSet(OperationContext* opCtx,
                                      RemoteCommandTargeter& targeter,
                                      std::shared_ptr<executor::TaskExecutor> executor) {
    auto fetchStatus =
        Status(ErrorCodes::InternalError, "Internal error running cursor callback in command");
    std::vector<ExternalKeysCollectionDocument> keyDocs;

    auto expireAt = opCtx->fastClockSource().now() +
        Seconds(gNewShardExistingClusterTimeKeysExpirationSecs.load());
    auto fetcher = createFindFetcher(
        opCtx,
        targeter,
        NamespaceString::kKeysCollectionNamespace,
        BSONObj() /* filter */,
        repl::ReadConcernLevel::kLocalReadConcern,
        [&](const std::vector<BSONObj>& docs) -> bool {
            for (const BSONObj& doc : docs) {
                keyDocs.push_back(
                    keys_collection_util::makeExternalClusterTimeKeyDoc(doc.getOwned(), expireAt));
            }
            return true;
        },
        [&](const Status& status) { fetchStatus = status; },
        executor);

    uassertStatusOK(fetcher->schedule());
    uassertStatusOK(fetcher->join(opCtx));
    uassertStatusOK(fetchStatus);

    auto opTime = keys_collection_util::storeExternalClusterTimeKeyDocs(opCtx, std::move(keyDocs));
    auto waitStatus = WaitForMajorityService::get(opCtx->getServiceContext())
                          .waitUntilMajorityForWrite(opTime, opCtx->getCancellationToken())
                          .getNoThrow();
    uassertStatusOK(waitStatus);
}

std::string createShardName(OperationContext* opCtx,
                            RemoteCommandTargeter& targeter,
                            bool isConfigShard,
                            const boost::optional<StringData>& proposedShardName,
                            std::shared_ptr<executor::TaskExecutor> executor) {
    std::string selectedName;

    if (proposedShardName) {
        selectedName = std::string{*proposedShardName};
    } else {
        auto greet = greetReplicaSet(opCtx, targeter, executor);
        selectedName = greet["setName"].str();
    }

    if (!isConfigShard && selectedName == DatabaseName::kConfig.db(omitTenant)) {
        uasserted(ErrorCodes::BadValue,
                  "use of shard replica set with name 'config' is not allowed");
    }

    return selectedName;
}

void installShardIdentity(
    OperationContext* opCtx,
    const ShardIdentityType& identity,
    RemoteCommandTargeter& targeter,
    boost::optional<APIParameters> apiParameters,
    boost::optional<std::function<OperationSessionInfo(OperationContext*)>> osiGenerator,
    std::shared_ptr<executor::TaskExecutor> executor) {
    ShardsvrAddShard addShardCmd;
    addShardCmd.setDbName(DatabaseName::kAdmin);
    addShardCmd.setShardIdentity(identity);

    if (apiParameters) {
        apiParameters->setInfo(addShardCmd);
    }

    if (osiGenerator) {
        auto const osi = (*osiGenerator)(opCtx);
        generic_argument_util::setOperationSessionInfo(addShardCmd, osi);
    }

    auto response = runCommandForAddShard(
        opCtx, targeter, DatabaseName::kAdmin, addShardCmd.toBSON(), executor);
    uassertStatusOK(response.commandStatus);
}

bool installShardIdentity(OperationContext* opCtx, const ShardIdentityType& identity) {
    BSONObj newIdentity = identity.toShardIdentityDocument();

    write_ops::InsertCommandRequest insertOp(NamespaceString::kServerConfigurationNamespace);
    insertOp.setDocuments({newIdentity});
    BSONObjBuilder cmdObjBuilder;
    insertOp.serialize(&cmdObjBuilder);
    cmdObjBuilder.append(WriteConcernOptions::kWriteConcernField,
                         ShardingCatalogClient::writeConcernLocalHavingUpstreamWaiter().toBSON());

    DBDirectClient localClient(opCtx);

    BSONObj res;
    localClient.runCommand(DatabaseName::kAdmin, cmdObjBuilder.obj(), res);
    const auto status = getStatusFromWriteCommandReply(res);
    if (status.code() != ErrorCodes::DuplicateKey) {
        uassertStatusOK(status);
        return true;
    }

    // If we have a duplicate key, that means, we already have a shardIdentity document. If
    // so, we only allow it to be the very same as the one in the command, otherwise a
    // cluster could steal an other cluster's shard without the former knowing it.
    const auto existingIdentity = localClient.findOne(
        NamespaceString::kServerConfigurationNamespace, BSON("_id" << identity.IdName));

    invariant(!existingIdentity.isEmpty());

    uassert(ErrorCodes::IllegalOperation,
            "Shard already has an identity that differs",
            newIdentity.woCompare(existingIdentity,
                                  {},
                                  BSONObj::ComparisonRules::kConsiderFieldName |
                                      BSONObj::ComparisonRules::kIgnoreFieldOrder) == 0);
    return false;
}

void updateShardIdentity(OperationContext* opCtx, const ShardIdentityType& identity) {
    BSONObj newIdentity = identity.toShardIdentityDocument();

    write_ops::UpdateOpEntry updateEntry;
    updateEntry.setMulti(false);
    updateEntry.setUpsert(false);
    updateEntry.setQ(BSON("_id" << identity.IdName));
    updateEntry.setU(mongo::write_ops::UpdateModification(
        newIdentity, write_ops::UpdateModification::ReplacementTag{}));
    write_ops::UpdateCommandRequest updateOp(NamespaceString::kServerConfigurationNamespace,
                                             {std::move(updateEntry)});

    BSONObjBuilder cmdObjBuilder;
    updateOp.serialize(&cmdObjBuilder);
    cmdObjBuilder.append(WriteConcernOptions::kWriteConcernField,
                         ShardingCatalogClient::writeConcernLocalHavingUpstreamWaiter().toBSON());

    DBDirectClient localClient(opCtx);

    BSONObj res;
    localClient.runCommand(DatabaseName::kAdmin, cmdObjBuilder.obj(), res);
    uassertStatusOK(getStatusFromWriteCommandReply(res));
}

void setClusterParametersOnReplicaSet(
    OperationContext* opCtx,
    RemoteCommandTargeter& targeter,
    const TenantIdMap<std::vector<BSONObj>>& allClusterParameters,
    boost::optional<std::function<OperationSessionInfo(OperationContext*)>> osiGenerator,
    std::shared_ptr<executor::TaskExecutor> executor) {
    // First, remove all existing parameters from the new shard.
    removeAllClusterParametersFromReplicaSet(opCtx, targeter, osiGenerator, executor);

    LOGV2(6360600, "Pushing cluster parameters into new shard");

    for (const auto& [tenantId, clusterParameters] : allClusterParameters) {
        const auto& dbName = DatabaseNameUtil::deserialize(
            tenantId, DatabaseName::kAdmin.db(omitTenant), SerializationContext::stateDefault());
        // Push cluster parameters into the newly added shard.
        for (auto& parameter : clusterParameters) {
            ShardsvrSetClusterParameter setClusterParamsCmd(
                BSON(parameter["_id"].String() << parameter.filterFieldsUndotted(
                         BSON("_id" << 1 << "clusterParameterTime" << 1), false)));
            setClusterParamsCmd.setDbName(dbName);
            setClusterParamsCmd.setClusterParameterTime(
                parameter["clusterParameterTime"].timestamp());
            generic_argument_util::setMajorityWriteConcern(setClusterParamsCmd);
            if (osiGenerator) {
                auto const osi = (*osiGenerator)(opCtx);
                generic_argument_util::setOperationSessionInfo(setClusterParamsCmd, osi);
            }

            const auto cmdResponse = runCommandForAddShard(
                opCtx, targeter, dbName, setClusterParamsCmd.toBSON(), executor);
            uassertStatusOK(cmdResponse.commandStatus);
        }
    }
}

void setClusterParametersLocally(OperationContext* opCtx,
                                 const TenantIdMap<std::vector<BSONObj>>& parameters) {
    for (const auto& tenantIdWithParameters : parameters) {
        auto& tenantId = tenantIdWithParameters.first;
        auto& parameterSet = tenantIdWithParameters.second;
        auth::ValidatedTenancyScopeGuard::runAsTenant(opCtx, tenantId, [&]() -> void {
            DBDirectClient client(opCtx);
            ClusterParameterDBClientService dbService(client);
            const auto tenantId = [&]() -> boost::optional<TenantId> {
                const auto vts = auth::ValidatedTenancyScope::get(opCtx);
                invariant(!vts || vts->hasTenantId());

                if (vts && vts->hasTenantId()) {
                    return vts->tenantId();
                }
                return boost::none;
            }();

            for (const auto& parameter : parameterSet) {
                SetClusterParameter setClusterParameterRequest(
                    BSON(parameter["_id"].String() << parameter.filterFieldsUndotted(
                             BSON("_id" << 1 << "clusterParameterTime" << 1), false)));
                setClusterParameterRequest.setDbName(
                    DatabaseNameUtil::deserialize(tenantId,
                                                  DatabaseName::kAdmin.db(omitTenant),
                                                  SerializationContext::stateDefault()));
                std::unique_ptr<ServerParameterService> parameterService =
                    std::make_unique<ClusterParameterService>();
                SetClusterParameterInvocation invocation{std::move(parameterService), dbService};
                invocation.invoke(opCtx,
                                  setClusterParameterRequest,
                                  parameter["clusterParameterTime"].timestamp(),
                                  boost::none /* previousTime */,
                                  kMajorityWriteConcern);
            }
        });
    }
}

TenantIdMap<std::vector<BSONObj>> getClusterParametersFromReplicaSet(
    OperationContext* opCtx,
    RemoteCommandTargeter& targeter,
    std::shared_ptr<executor::TaskExecutor> executor) {
    LOGV2(6538600, "Pulling cluster parameters from new shard");

    // We can safely query the cluster parameters because the replica set must have been started
    // with --shardsvr in order to add it into the cluster, and in this mode no setClusterParameter
    // can be called on the replica set directly.
    auto tenantIds = uassertStatusOK(getTenantsWithConfigDbsOnShard(opCtx, targeter, executor));

    std::map<boost::optional<TenantId>, std::pair<std::unique_ptr<Fetcher>, Status>> fetchers;
    // If for some reason the callback never gets invoked, we will return this status in
    // response.
    TenantIdMap<std::vector<BSONObj>> allParameters;

    for (const auto& tenantId : tenantIds) {
        auto fetcher = createFindFetcher(
            opCtx,
            targeter,
            NamespaceString::makeClusterParametersNSS(tenantId),
            BSONObj() /* filter */,
            repl::ReadConcernLevel::kMajorityReadConcern,
            [&allParameters, tenantId](const std::vector<BSONObj>& docs) -> bool {
                std::vector<BSONObj> parameters;
                parameters.reserve(docs.size());
                for (const BSONObj& doc : docs) {
                    parameters.push_back(doc.getOwned());
                }
                allParameters.emplace(tenantId, std::move(parameters));
                return true;
            },
            [&fetchers, tenantId](const Status& status) { fetchers.at(tenantId).second = status; },
            executor);
        const auto fetcherPtr = fetcher.get();

        fetchers.emplace(
            tenantId,
            std::make_pair(std::move(fetcher),
                           Status(ErrorCodes::InternalError,
                                  "Internal error running cursor callback in command")));

        uassertStatusOK(fetcherPtr->schedule());
    }

    for (auto& [tenantId, fetcherWithStatus] : fetchers) {
        auto& [fetcher, status] = fetcherWithStatus;
        uassertStatusOK(fetcher->join(opCtx));
        uassertStatusOK(status);
    }

    return allParameters;
}

TenantIdMap<std::vector<BSONObj>> getClusterParametersLocally(OperationContext* opCtx) {
    auto localConfigShard = ShardingCatalogManager::get(opCtx)->localConfigShard();
    auto tenantIds =
        uassertStatusOK(getTenantsWithConfigDbsOnShard(opCtx, *localConfigShard.get()));
    TenantIdMap<std::vector<BSONObj>> configSvrClusterParameterDocs;
    for (const auto& tenantId : tenantIds) {
        auto findResponse = uassertStatusOK(localConfigShard->exhaustiveFindOnConfig(
            opCtx,
            ReadPreferenceSetting{ReadPreference::PrimaryOnly},
            repl::ReadConcernLevel::kLocalReadConcern,
            NamespaceString::makeClusterParametersNSS(tenantId),
            BSONObj(),
            BSONObj(),
            boost::none));

        configSvrClusterParameterDocs.emplace(tenantId, findResponse.docs);
    }

    return configSvrClusterParameterDocs;
}

long long runCountCommandOnConfig(OperationContext* opCtx,
                                  const std::shared_ptr<Shard> localConfigShard,
                                  const NamespaceString& nss,
                                  BSONObj query) {
    BSONObjBuilder countBuilder;
    countBuilder.append("count", nss.coll());
    countBuilder.append("query", query);

    auto resultStatus =
        localConfigShard->runCommand(opCtx,
                                     kConfigReadSelector,
                                     nss.dbName(),
                                     countBuilder.done(),
                                     Shard::getConfiguredTimeoutForOperationOnNamespace(nss),
                                     Shard::RetryPolicy::kIdempotent);

    uassertStatusOK(Shard::CommandResponse::getEffectiveStatus(resultStatus));

    auto responseObj = std::move(resultStatus.getValue().response);

    long long result;
    uassertStatusOK(bsonExtractIntegerField(responseObj, "n", &result));

    return result;
}

DrainingShardUsage getDrainingProgress(OperationContext* opCtx,
                                       const std::shared_ptr<Shard> localConfigShard,
                                       const std::string& shardName) {
    const auto totalChunkCount = runCountCommandOnConfig(opCtx,
                                                         localConfigShard,
                                                         NamespaceString::kConfigsvrChunksNamespace,
                                                         BSON(ChunkType::shard(shardName)));

    const auto shardedChunkCount = getChunkForShardCount(opCtx, localConfigShard.get(), shardName);

    const auto unshardedCollectionsCount =
        getCollectionsToMoveForShardCount(opCtx, localConfigShard.get(), shardName);

    const auto databaseCount =
        runCountCommandOnConfig(opCtx,
                                localConfigShard,
                                NamespaceString::kConfigDatabasesNamespace,
                                BSON(DatabaseType::kPrimaryFieldName << shardName));

    const auto jumboCount =
        runCountCommandOnConfig(opCtx,
                                localConfigShard,
                                NamespaceString::kConfigsvrChunksNamespace,
                                BSON(ChunkType::shard(shardName) << ChunkType::jumbo(true)));

    return {
        RemainingCounts(shardedChunkCount, unshardedCollectionsCount, databaseCount, jumboCount),
        totalChunkCount};
}

// Sets the addOrRemoveShardInProgress cluster parameter to prevent new ShardingDDLCoordinators from
// starting, and then drains the ongoing ones. Must be called under the kConfigsvrShardsNamespace
// ddl lock.
void blockDDLCoordinatorsAndDrain(OperationContext* opCtx, bool persistRecoveryDocument) {
    if (MONGO_unlikely(skipBlockingDDLCoordinatorsDuringAddAndRemoveShard.shouldFail())) {
        return;
    }

    // Before we block new ShardingDDLCoordinator creations, first do a best-effort check that
    // there's no currently running one. If there is any, we wait until there is none. This is to
    // reduce impact to concurrent DDL operations.
    waitUntilReadyToBlockNewDDLCoordinators(opCtx);

    if (persistRecoveryDocument) {
        // Persist a recovery document before we set the addOrRemoveShardInProgress cluster
        // parameter. This way, in case of crash, the new primary node will unset the parameter.
        DBDirectClient client(opCtx);
        write_ops::checkWriteErrors(client.insert(write_ops::InsertCommandRequest(
            NamespaceString::kServerConfigurationNamespace,
            {BSON("_id" << kAddOrRemoveShardInProgressRecoveryDocumentId)})));
    }

    // Prevent new DDL coordinators from starting across the cluster.
    LOGV2(5687902,
          "Requesting all shards to block any new DDL cluster-wide in order to perform topology "
          "changes");
    setAddOrRemoveShardInProgressClusterParam(opCtx, true);


    // Wait for any ongoing DDL coordinator to finish.
    LOGV2(5687903, "Draining ongoing ShardingDDLCoordinators for topology change");
    joinOngoingShardingDDLCoordinatorsOnShards(opCtx);
    LOGV2(5687904, "Drained ongoing ShardingDDLCoordinators for topology change");
}

// Unsets the addOrRemoveShardInProgress cluster parameter. Must be called under the
// kConfigsvrShardsNamespace ddl lock.
void unblockDDLCoordinators(OperationContext* opCtx, bool removeRecoveryDocument) {
    if (MONGO_unlikely(skipBlockingDDLCoordinatorsDuringAddAndRemoveShard.shouldFail())) {
        return;
    }

    // Allow new DDL coordinators to start across the cluster.
    setAddOrRemoveShardInProgressClusterParam(opCtx, false);
    LOGV2(5687905, "Unblocked new ShardingDDLCoordinators after topology change");

    if (removeRecoveryDocument) {
        // Delete the recovery document.
        DBDirectClient client(opCtx);
        write_ops::checkWriteErrors(client.remove(write_ops::DeleteCommandRequest(
            NamespaceString::kServerConfigurationNamespace,
            {{BSON("_id" << kAddOrRemoveShardInProgressRecoveryDocumentId), false /* multi */}})));
    }
}

boost::optional<RemoveShardProgress> dropLocalCollectionsAndDatabases(
    OperationContext* opCtx,
    const std::vector<DatabaseType>& trackedDBs,
    const std::string& shardName) {
    // Drop all tracked databases locally now that all user data has been drained so the
    // config server can transition back to catalog shard mode without requiring users to
    // manually drop them.

    // First, verify all collections we would drop are empty. In normal operation, a
    // collection may still have data because of a sharded drop (which non-atomically
    // updates metadata before dropping user data). If this state persists, manual
    // intervention will be required to complete the transition, so we don't accidentally
    // delete real data.
    LOGV2(9022301, "Checking all local collections are empty", "shardId"_attr = shardName);

    for (auto&& db : trackedDBs) {
        tassert(7783700,
                "Cannot drop admin or config database from the config server",
                !db.getDbName().isConfigDB() && !db.getDbName().isAdminDB());

        auto collections = [&] {
            Lock::DBLock dbLock(opCtx, db.getDbName(), MODE_S);
            auto catalog = CollectionCatalog::get(opCtx);
            return catalog->getAllCollectionNamesFromDb(opCtx, db.getDbName());
        }();
        if (auto pendingDataCleanupState = checkCollectionsAreEmpty(opCtx, collections)) {
            return *pendingDataCleanupState;
        }
    }

    // Now actually drop the databases; each request must either succeed or resolve into a
    // no-op.
    LOGV2(7509600, "Locally dropping drained databases", "shardId"_attr = shardName);

    for (auto&& db : trackedDBs) {
        const auto dropStatus = dropDatabase(opCtx, db.getDbName(), true /*markFromMigrate*/);
        if (dropStatus != ErrorCodes::NamespaceNotFound) {
            uassertStatusOK(dropStatus);
        }
        hangAfterDroppingDatabaseInTransitionToDedicatedConfigServer.pauseWhileSet(opCtx);
    }

    // Check if the sessions collection is empty. We defer dropping this collection to the caller
    // since it should only be dropped if featureFlagSessionsCollectionCoordinatorOnConfigServer is
    // disabled so the drop must be done in a fixed FCV region.
    if (auto pendingDataCleanupState =
            checkCollectionsAreEmpty(opCtx, {NamespaceString::kLogicalSessionsNamespace})) {
        return *pendingDataCleanupState;
    }

    return boost::none;
}

void commitRemoveShard(const Lock::ExclusiveLock&,
                       OperationContext* opCtx,
                       const std::shared_ptr<Shard> localConfigShard,
                       const std::string& shardName,
                       std::shared_ptr<executor::TaskExecutor> executor) {
    // Find a controlShard to be updated.
    auto controlShardQueryStatus =
        localConfigShard->exhaustiveFindOnConfig(opCtx,
                                                 ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                                                 repl::ReadConcernLevel::kLocalReadConcern,
                                                 NamespaceString::kConfigsvrShardsNamespace,
                                                 BSON(ShardType::name.ne(shardName)),
                                                 {},
                                                 1);
    auto controlShardResponse = uassertStatusOK(controlShardQueryStatus);
    // Since it's not possible to remove the last shard, there should always be a control shard.
    uassert(4740601,
            "unable to find a controlShard to update during removeShard",
            !controlShardResponse.docs.empty());
    const auto controlShardStatus = ShardType::fromBSON(controlShardResponse.docs.front());
    uassertStatusOKWithContext(controlShardStatus, "unable to parse control shard");
    std::string controlShardName = controlShardStatus.getValue().getName();

    // Tick clusterTime to get a new topologyTime for this mutation of the topology.
    auto newTopologyTime = VectorClockMutable::get(opCtx)->tickClusterTime(1);

    // Remove the shard's document and update topologyTime within a transaction.
    removeShardInTransaction(
        opCtx, shardName, controlShardName, newTopologyTime.asTimestamp(), executor);
}

void addShardInTransaction(OperationContext* opCtx,
                           const ShardType& newShard,
                           std::vector<DatabaseName>&& databasesInNewShard,
                           std::shared_ptr<executor::TaskExecutor> executor) {

    // Set up and run the commit statements
    // TODO SERVER-81582: generate batches of transactions to insert the database/placementHistory
    // before adding the shard in config.shards.
    auto transactionChain = [opCtx, &newShard, &databasesInNewShard](
                                const txn_api::TransactionClient& txnClient, ExecutorPtr txnExec) {
        write_ops::InsertCommandRequest insertShardEntry(NamespaceString::kConfigsvrShardsNamespace,
                                                         {newShard.toBSON()});
        return txnClient.runCRUDOp(insertShardEntry, {})
            .thenRunOn(txnExec)
            .then([&](const BatchedCommandResponse& insertShardEntryResponse) {
                uassertStatusOK(insertShardEntryResponse.toStatus());
                if (databasesInNewShard.empty()) {
                    BatchedCommandResponse noOpResponse;
                    noOpResponse.setStatus(Status::OK());
                    noOpResponse.setN(0);

                    return SemiFuture<BatchedCommandResponse>(std::move(noOpResponse));
                }

                std::vector<BSONObj> databaseEntries;
                std::transform(databasesInNewShard.begin(),
                               databasesInNewShard.end(),
                               std::back_inserter(databaseEntries),
                               [&](const DatabaseName& dbName) {
                                   return DatabaseType(dbName,
                                                       newShard.getName(),
                                                       DatabaseVersion(UUID::gen(),
                                                                       newShard.getTopologyTime()))
                                       .toBSON();
                               });
                write_ops::InsertCommandRequest insertDatabaseEntries(
                    NamespaceString::kConfigDatabasesNamespace, std::move(databaseEntries));
                return txnClient.runCRUDOp(insertDatabaseEntries, {});
            })
            .thenRunOn(txnExec)
            .then([&](const BatchedCommandResponse& insertDatabaseEntriesResponse) {
                uassertStatusOK(insertDatabaseEntriesResponse.toStatus());
                if (databasesInNewShard.empty()) {
                    BatchedCommandResponse noOpResponse;
                    noOpResponse.setStatus(Status::OK());
                    noOpResponse.setN(0);

                    return SemiFuture<BatchedCommandResponse>(std::move(noOpResponse));
                }
                std::vector<BSONObj> placementEntries;
                std::transform(databasesInNewShard.begin(),
                               databasesInNewShard.end(),
                               std::back_inserter(placementEntries),
                               [&](const DatabaseName& dbName) {
                                   return NamespacePlacementType(NamespaceString(dbName),
                                                                 newShard.getTopologyTime(),
                                                                 {ShardId(newShard.getName())})
                                       .toBSON();
                               });
                write_ops::InsertCommandRequest insertPlacementEntries(
                    NamespaceString::kConfigsvrPlacementHistoryNamespace,
                    std::move(placementEntries));
                return txnClient.runCRUDOp(insertPlacementEntries, {});
            })
            .thenRunOn(txnExec)
            .then([](auto insertPlacementEntriesResponse) {
                uassertStatusOK(insertPlacementEntriesResponse.toStatus());
            })
            .semi();
    };

    {
        auto inlineExecutor = std::make_shared<executor::InlineExecutor>();

        txn_api::SyncTransactionWithRetries txn(opCtx, executor, nullptr, inlineExecutor);
        txn.run(opCtx, transactionChain);
    }
}

void updateClusterCardinalityParameter(const Lock::ExclusiveLock&, OperationContext* opCtx) {
    const auto shardRegistry = Grid::get(opCtx)->shardRegistry();

    const bool hasTwoOrMoreShard = shardRegistry->getNumShards(opCtx) >= 2;
    ConfigsvrSetClusterParameter configsvrSetClusterParameter(BSON(
        "shardedClusterCardinalityForDirectConns" << BSON(
            ShardedClusterCardinalityParam::kHasTwoOrMoreShardsFieldName << hasTwoOrMoreShard)));
    configsvrSetClusterParameter.setDbName(DatabaseName::kAdmin);
    configsvrSetClusterParameter.set_compatibleWithTopologyChange(true);

    while (true) {
        const auto cmdResponse = shardRegistry->getConfigShard()->runCommand(
            opCtx,
            ReadPreferenceSetting(ReadPreference::PrimaryOnly),
            DatabaseName::kAdmin,
            configsvrSetClusterParameter.toBSON(),
            Shard::RetryPolicy::kIdempotent);

        auto status = Shard::CommandResponse::getEffectiveStatus(cmdResponse);

        if (status != ErrorCodes::ConflictingOperationInProgress) {
            uassertStatusOK(status);
            return;
        }

        // Retry on ErrorCodes::ConflictingOperationInProgress errors, which can be caused by
        // another ConfigsvrCoordinator runnning concurrently.
        LOGV2_DEBUG(9314400,
                    2,
                    "Failed to update the cluster parameter. Retrying again after 500ms.",
                    "error"_attr = status);

        opCtx->sleepFor(Milliseconds(500));
    }
}

void hangAddShardBeforeUpdatingClusterCardinalityParameterFailpoint(OperationContext* opCtx) {
    hangAddShardBeforeUpdatingClusterCardinalityParameter.pauseWhileSet(opCtx);
}

boost::optional<ShardType> getShardIfExists(OperationContext* opCtx,
                                            std::shared_ptr<Shard> localConfigShard,
                                            const ShardId& shardId) {
    auto findShardResponse = uassertStatusOK(
        localConfigShard->exhaustiveFindOnConfig(opCtx,
                                                 kConfigReadSelector,
                                                 repl::ReadConcernLevel::kLocalReadConcern,
                                                 NamespaceString::kConfigsvrShardsNamespace,
                                                 BSON(ShardType::name() << shardId.toString()),
                                                 BSONObj(),
                                                 1));

    if (!findShardResponse.docs.empty()) {
        return uassertStatusOK(ShardType::fromBSON(findShardResponse.docs[0]));
    }
    return boost::none;
}

void propagateClusterUserWriteBlockToReplicaSet(OperationContext* opCtx,
                                                RemoteCommandTargeter& targeter,
                                                std::shared_ptr<executor::TaskExecutor> executor) {
    uint8_t level = topology_change_helpers::UserWriteBlockingLevel::None;

    PersistentTaskStore<UserWriteBlockingCriticalSectionDocument> store(
        NamespaceString::kUserWritesCriticalSectionsNamespace);
    store.forEach(opCtx, BSONObj(), [&](const UserWriteBlockingCriticalSectionDocument& doc) {
        invariant(doc.getNss() ==
                  UserWritesRecoverableCriticalSectionService::kGlobalUserWritesNamespace);

        if (doc.getBlockNewUserShardedDDL()) {
            level |= topology_change_helpers::UserWriteBlockingLevel::DDLOperations;
        }

        if (doc.getBlockUserWrites()) {
            invariant(doc.getBlockNewUserShardedDDL());
            level |= topology_change_helpers::UserWriteBlockingLevel::Writes;
        }

        return true;
    });

    topology_change_helpers::setUserWriteBlockingState(
        opCtx,
        targeter,
        topology_change_helpers::UserWriteBlockingLevel(level),
        true, /* block writes */
        boost::none,
        executor);
}

void resetDDLBlockingForTopologyChangeIfNeeded(OperationContext* opCtx) {
    // Check if we need to run recovery at all.
    {
        DBDirectClient client(opCtx);
        const auto recoveryDoc =
            client.findOne(NamespaceString::kServerConfigurationNamespace,
                           BSON("_id" << kAddOrRemoveShardInProgressRecoveryDocumentId));
        if (recoveryDoc.isEmpty()) {
            // No need to do anything.
            return;
        }
    }

    // Unset the addOrRemoveShardInProgress cluster parameter.
    LOGV2(5687906, "Resetting addOrRemoveShardInProgress cluster parameter after failure");
    topology_change_helpers::unblockDDLCoordinators(opCtx);
    LOGV2(5687907, "Resetted addOrRemoveShardInProgress cluster parameter after failure");
}

}  // namespace topology_change_helpers

}  // namespace mongo
