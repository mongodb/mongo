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

#include "mongo/db/s/topology_change_helpers.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobj_comparator.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/timestamp.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/client/connection_string.h"
#include "mongo/client/fetcher.h"
#include "mongo/client/read_preference.h"
#include "mongo/client/remote_command_targeter.h"
#include "mongo/client/replica_set_monitor.h"
#include "mongo/db/audit.h"
#include "mongo/db/catalog/drop_database.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/set_user_write_block_mode_gen.h"
#include "mongo/db/database_name.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/generic_argument_util.h"
#include "mongo/db/logical_time.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/persistent_task_store.h"
#include "mongo/db/query/find_command.h"
#include "mongo/db/query/write_ops/write_ops_gen.h"
#include "mongo/db/query/write_ops/write_ops_parsers.h"
#include "mongo/db/read_write_concern_defaults.h"
#include "mongo/db/repl/hello_gen.h"
#include "mongo/db/repl/optime_with.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/db/repl/read_concern_level.h"
#include "mongo/db/repl/repl_client_info.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/s/range_deletion_task_gen.h"
#include "mongo/db/s/remove_shard_draining_progress_gen.h"
#include "mongo/db/s/topology_change_helpers.h"
#include "mongo/db/server_parameter.h"
#include "mongo/db/service_context.h"
#include "mongo/db/session/logical_session_id.h"
#include "mongo/db/shard_id.h"
#include "mongo/db/tenant_id.h"
#include "mongo/db/write_concern_options.h"
#include "mongo/executor/remote_command_request.h"
#include "mongo/executor/remote_command_response.h"
#include "mongo/executor/task_executor.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_attr.h"
#include "mongo/logv2/log_component.h"
#include "mongo/logv2/redaction.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/rpc/metadata.h"
#include "mongo/s/catalog/sharding_catalog_client.h"
#include "mongo/s/catalog/type_chunk.h"
#include "mongo/s/catalog/type_shard.h"
#include "mongo/s/client/shard.h"
#include "mongo/s/cluster_identity_loader.h"
#include "mongo/s/request_types/add_shard_gen.h"
#include "mongo/s/request_types/sharded_ddl_commands_gen.h"
#include "mongo/s/request_types/shardsvr_join_migrations_request_gen.h"
#include "mongo/s/write_ops/batched_command_request.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/database_name_util.h"
#include "mongo/util/duration.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/str.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

namespace mongo {

namespace {
const Seconds kRemoteCommandTimeout{60};
}

namespace add_shard_util {

ShardsvrAddShard createAddShardCmd(OperationContext* opCtx, const ShardId& shardName) {
    ShardsvrAddShard addShardCmd;
    addShardCmd.setDbName(DatabaseName::kAdmin);

    ShardIdentity shardIdentity;
    shardIdentity.setShardName(shardName.toString());
    shardIdentity.setClusterId(ClusterIdentityLoader::get(opCtx)->getClusterId());
    shardIdentity.setConfigsvrConnectionString(
        repl::ReplicationCoordinator::get(opCtx)->getConfigConnectionString());

    addShardCmd.setShardIdentity(shardIdentity);
    return addShardCmd;
}

BSONObj createShardIdentityUpsertForAddShard(const ShardsvrAddShard& addShardCmd,
                                             const WriteConcernOptions& wc) {
    // TODO SERVER-88742 Just use write_ops::UpdateCommandRequest
    BatchedCommandRequest request([&] {
        write_ops::UpdateCommandRequest updateOp(NamespaceString::kServerConfigurationNamespace);
        updateOp.setUpdates({[&] {
            write_ops::UpdateOpEntry entry;
            entry.setQ(BSON("_id" << kShardIdentityDocumentId));
            entry.setU(write_ops::UpdateModification::parseFromClassicUpdate(
                addShardCmd.getShardIdentity().toBSON()));
            entry.setUpsert(true);
            return entry;
        }()});

        return updateOp;
    }());

    // Add the WC to the serialized BatchedCommandRequest.
    BSONObjBuilder cmdObjBuilder;
    request.serialize(&cmdObjBuilder);
    cmdObjBuilder.append(WriteConcernOptions::kWriteConcernField, wc.toBSON());
    return cmdObjBuilder.obj();
}

}  // namespace add_shard_util

namespace topology_change_helpers {

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
    const auto existingShards = uassertStatusOKWithContext(
        localCatalogClient.getAllShards(opCtx, repl::ReadConcernLevel::kLocalReadConcern),
        "Failed to load existing shards during addShard");

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

void deleteUserWriteCriticalSections(
    OperationContext* opCtx,
    RemoteCommandTargeter& targeter,
    boost::optional<std::function<OperationSessionInfo(OperationContext*)>> osiGenerator,
    std::shared_ptr<executor::TaskExecutor> executor) {
    // Delete all the config.user_writes_critical_sections documents from the new shard.
    {
        // We need to fetch manually rather than using Shard::runExhaustiveCursorCommand because
        // ShardRemote uses the FixedTaskExecutor which checks that the host being targeted is a
        // fully added shard already
        auto fetcherStatus = Status::OK();
        std::vector<BSONObj> userWriteCritSectionDocs;
        auto fetcher = topology_change_helpers::createFetcher(
            opCtx,
            targeter,
            NamespaceString::kUserWritesCriticalSectionsNamespace,
            repl::ReadConcernLevel::kMajorityReadConcern,
            [&](const std::vector<BSONObj>& docs) -> bool {
                for (const BSONObj& doc : docs) {
                    userWriteCritSectionDocs.emplace_back(doc.getOwned());
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
            write_ops::DeleteCommandRequest deleteOp(
                NamespaceString::kUserWritesCriticalSectionsNamespace);
            deleteOp.setDeletes(deleteOps);
            generic_argument_util::setMajorityWriteConcern(deleteOp);
            if (osiGenerator) {
                auto const osi = (*osiGenerator)(opCtx);
                generic_argument_util::setOperationSessionInfo(deleteOp, osi);
            }

            const auto commandResponse = runCommandForAddShard(
                opCtx,
                targeter,
                NamespaceString::kUserWritesCriticalSectionsNamespace.dbName(),
                deleteOp.toBSON(),
                executor);
            uassertStatusOK(getStatusFromWriteCommandReply(commandResponse.response));
        };

        std::vector<mongo::write_ops::DeleteOpEntry> deleteOps;
        deleteOps.reserve(write_ops::kMaxWriteBatchSize);
        for (auto& element : userWriteCritSectionDocs) {
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
}

void setUserWriteBlockingState(
    OperationContext* opCtx,
    RemoteCommandTargeter& targeter,
    UserWriteBlockingLevel level,
    bool block,
    boost::optional<std::function<OperationSessionInfo(OperationContext*)>> osiGenerator,
    std::shared_ptr<executor::TaskExecutor> executor) {

    deleteUserWriteCriticalSections(opCtx, targeter, osiGenerator, executor);

    const auto makeShardsvrSetUserWriteBlockModeCommand =
        [block, &osiGenerator](OperationContext* opCtx,
                               ShardsvrSetUserWriteBlockModePhaseEnum phase) -> BSONObj {
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
            opCtx, ShardsvrSetUserWriteBlockModePhaseEnum::kPrepare);

        const auto cmdResponse =
            runCommandForAddShard(opCtx, targeter, DatabaseName::kAdmin, cmd, executor);
        uassertStatusOK(Shard::CommandResponse::getEffectiveStatus(cmdResponse));
    }

    if (level & UserWriteBlockingLevel::Writes) {
        const auto cmd = makeShardsvrSetUserWriteBlockModeCommand(
            opCtx, ShardsvrSetUserWriteBlockModePhaseEnum::kComplete);

        const auto cmdResponse =
            runCommandForAddShard(opCtx, targeter, DatabaseName::kAdmin, cmd, executor);
        uassertStatusOK(Shard::CommandResponse::getEffectiveStatus(cmdResponse));
    }
}

std::vector<DatabaseName> getDBNamesListFromShard(
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

BSONObj greetShard(OperationContext* opCtx,
                   RemoteCommandTargeter& targeter,
                   std::shared_ptr<executor::TaskExecutor> executor) {

    boost::optional<Shard::CommandResponse> commandResponse;
    try {
        commandResponse = runCommandForAddShard(
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

void validateHostAsShard(OperationContext* opCtx,
                         RemoteCommandTargeter& targeter,
                         const ConnectionString& connectionString,
                         bool isConfigShard,
                         std::shared_ptr<executor::TaskExecutor> executor) {
    auto resHello = greetShard(opCtx, targeter, executor);

    // Fail if the node being added is a mongos.
    const std::string msg = resHello.getStringField("msg").toString();
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

std::unique_ptr<Fetcher> createFetcher(OperationContext* opCtx,
                                       RemoteCommandTargeter& targeter,
                                       const NamespaceString& nss,
                                       const repl::ReadConcernLevel& readConcernLevel,
                                       FetcherDocsCallbackFn processDocsCallback,
                                       FetcherStatusCallbackFn processStatusCallback,
                                       std::shared_ptr<executor::TaskExecutor> executor) {
    auto host = uassertStatusOK(
        targeter.findHost(opCtx, ReadPreferenceSetting{ReadPreference::PrimaryOnly}));

    FindCommandRequest findCommand(nss);
    const auto readConcern = repl::ReadConcernArgs(readConcernLevel);
    findCommand.setReadConcern(readConcern);
    const Milliseconds maxTimeMS =
        std::min(opCtx->getRemainingMaxTimeMillis(), Milliseconds(kRemoteCommandTimeout));
    findCommand.setMaxTimeMS(durationCount<Milliseconds>(maxTimeMS));

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
                                     nss.dbName(),
                                     findCommand.toBSON(),
                                     fetcherCallback,
                                     BSONObj(), /* metadata tracking, only used for shards */
                                     maxTimeMS, /* command network timeout */
                                     maxTimeMS /* getMore network timeout */);
}

std::string getRemoveShardMessage(const ShardDrainingStateEnum& status) {
    switch (status) {
        case ShardDrainingStateEnum::kStarted:
            return "draining started successfully";
        case ShardDrainingStateEnum::kOngoing:
            return "draining ongoing";
        case ShardDrainingStateEnum::kPendingDataCleanup:
            return "waiting for data to be cleaned up";
        case ShardDrainingStateEnum::kCompleted:
            return "removeshard completed successfully";
        default:
            MONGO_UNREACHABLE;
    }
}

}  // namespace topology_change_helpers

}  // namespace mongo
