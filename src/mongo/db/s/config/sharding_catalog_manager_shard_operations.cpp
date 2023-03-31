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

#include "mongo/platform/basic.h"

#include "mongo/db/s/config/sharding_catalog_manager.h"

#include <iomanip>
#include <set>

#include "mongo/base/status_with.h"
#include "mongo/bson/bsonobj_comparator.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/client/connection_string.h"
#include "mongo/client/fetcher.h"
#include "mongo/client/read_preference.h"
#include "mongo/client/remote_command_targeter.h"
#include "mongo/client/replica_set_monitor.h"
#include "mongo/db/api_parameters.h"
#include "mongo/db/audit.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/catalog_shard_feature_flag_gen.h"
#include "mongo/db/client.h"
#include "mongo/db/commands/cluster_server_parameter_cmds_gen.h"
#include "mongo/db/commands/feature_compatibility_version.h"
#include "mongo/db/commands/set_cluster_parameter_invocation.h"
#include "mongo/db/commands/set_feature_compatibility_version_gen.h"
#include "mongo/db/feature_compatibility_version_parser.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/ops/write_ops_gen.h"
#include "mongo/db/persistent_task_store.h"
#include "mongo/db/read_write_concern_defaults.h"
#include "mongo/db/repl/hello_gen.h"
#include "mongo/db/repl/repl_client_info.h"
#include "mongo/db/repl/repl_set_config.h"
#include "mongo/db/s/add_shard_cmd_gen.h"
#include "mongo/db/s/add_shard_util.h"
#include "mongo/db/s/sharding_ddl_util.h"
#include "mongo/db/s/sharding_logging.h"
#include "mongo/db/s/sharding_state.h"
#include "mongo/db/s/sharding_util.h"
#include "mongo/db/s/type_shard_identity.h"
#include "mongo/db/s/user_writes_critical_section_document_gen.h"
#include "mongo/db/s/user_writes_recoverable_critical_section_service.h"
#include "mongo/db/transaction/transaction_api.h"
#include "mongo/db/vector_clock_mutable.h"
#include "mongo/db/wire_version.h"
#include "mongo/executor/task_executor.h"
#include "mongo/idl/cluster_server_parameter_common.h"
#include "mongo/idl/cluster_server_parameter_gen.h"
#include "mongo/logv2/log.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/rpc/metadata/repl_set_metadata.h"
#include "mongo/rpc/metadata/tracking_metadata.h"
#include "mongo/s/catalog/sharding_catalog_client.h"
#include "mongo/s/catalog/type_database_gen.h"
#include "mongo/s/catalog/type_namespace_placement_gen.h"
#include "mongo/s/catalog/type_shard.h"
#include "mongo/s/client/shard.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/cluster_identity_loader.h"
#include "mongo/s/database_version.h"
#include "mongo/s/grid.h"
#include "mongo/s/request_types/sharded_ddl_commands_gen.h"
#include "mongo/s/write_ops/batched_command_request.h"
#include "mongo/s/write_ops/batched_command_response.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/str.h"
#include "mongo/util/version/releases.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding


namespace mongo {
namespace {

MONGO_FAIL_POINT_DEFINE(hangBeforeNotifyingaddShardCommitted);
MONGO_FAIL_POINT_DEFINE(hangAfterDroppingCollectionInTransitionToDedicatedConfigServer);

using CallbackHandle = executor::TaskExecutor::CallbackHandle;
using CallbackArgs = executor::TaskExecutor::CallbackArgs;
using RemoteCommandCallbackArgs = executor::TaskExecutor::RemoteCommandCallbackArgs;
using RemoteCommandCallbackFn = executor::TaskExecutor::RemoteCommandCallbackFn;

const ReadPreferenceSetting kConfigReadSelector(ReadPreference::Nearest, TagSet{});
const WriteConcernOptions kMajorityWriteConcern{WriteConcernOptions::kMajority,
                                                WriteConcernOptions::SyncMode::UNSET,
                                                WriteConcernOptions::kNoTimeout};

/**
 * Generates a unique name to be given to a newly added shard.
 */
StatusWith<std::string> generateNewShardName(OperationContext* opCtx, Shard* configShard) {
    BSONObjBuilder shardNameRegex;
    shardNameRegex.appendRegex(ShardType::name(), "^shard");

    auto findStatus =
        configShard->exhaustiveFindOnConfig(opCtx,
                                            kConfigReadSelector,
                                            repl::ReadConcernLevel::kLocalReadConcern,
                                            NamespaceString::kConfigsvrShardsNamespace,
                                            shardNameRegex.obj(),
                                            BSON(ShardType::name() << -1),
                                            1);
    if (!findStatus.isOK()) {
        return findStatus.getStatus();
    }

    const auto& docs = findStatus.getValue().docs;

    int count = 0;
    if (!docs.empty()) {
        const auto shardStatus = ShardType::fromBSON(docs.front());
        if (!shardStatus.isOK()) {
            return shardStatus.getStatus();
        }

        std::istringstream is(shardStatus.getValue().getName().substr(5));
        is >> count;
        count++;
    }

    // TODO: fix so that we can have more than 10000 automatically generated shard names
    if (count < 9999) {
        std::stringstream ss;
        ss << "shard" << std::setfill('0') << std::setw(4) << count;
        return ss.str();
    }

    return Status(ErrorCodes::OperationFailed, "unable to generate new shard name");
}

}  // namespace

StatusWith<Shard::CommandResponse> ShardingCatalogManager::_runCommandForAddShard(
    OperationContext* opCtx,
    RemoteCommandTargeter* targeter,
    StringData dbName,
    const BSONObj& cmdObj) {
    auto swHost = targeter->findHost(opCtx, ReadPreferenceSetting{ReadPreference::PrimaryOnly});
    if (!swHost.isOK()) {
        return swHost.getStatus();
    }
    auto host = std::move(swHost.getValue());

    executor::RemoteCommandRequest request(
        host, dbName.toString(), cmdObj, rpc::makeEmptyMetadata(), opCtx, Seconds(60));

    executor::RemoteCommandResponse response =
        Status(ErrorCodes::InternalError, "Internal error running command");

    auto swCallbackHandle = _executorForAddShard->scheduleRemoteCommand(
        request, [&response](const executor::TaskExecutor::RemoteCommandCallbackArgs& args) {
            response = args.response;
        });
    if (!swCallbackHandle.isOK()) {
        return swCallbackHandle.getStatus();
    }

    // Block until the command is carried out
    _executorForAddShard->wait(swCallbackHandle.getValue());

    if (response.status == ErrorCodes::ExceededTimeLimit) {
        LOGV2(21941,
              "Operation timed out with {error}",
              "Operation timed out",
              "error"_attr = redact(response.status));
    }

    if (!response.isOK()) {
        if (!Shard::shouldErrorBePropagated(response.status.code())) {
            return {ErrorCodes::OperationFailed,
                    str::stream() << "failed to run command " << cmdObj
                                  << " when attempting to add shard "
                                  << targeter->connectionString().toString()
                                  << causedBy(response.status)};
        }
        return response.status;
    }

    BSONObj result = response.data.getOwned();

    Status commandStatus = getStatusFromCommandResult(result);
    if (!Shard::shouldErrorBePropagated(commandStatus.code())) {
        commandStatus = {
            ErrorCodes::OperationFailed,
            str::stream() << "failed to run command " << cmdObj << " when attempting to add shard "
                          << targeter->connectionString().toString() << causedBy(commandStatus)};
    }

    Status writeConcernStatus = getWriteConcernStatusFromCommandResult(result);
    if (!Shard::shouldErrorBePropagated(writeConcernStatus.code())) {
        writeConcernStatus = {ErrorCodes::OperationFailed,
                              str::stream() << "failed to satisfy writeConcern for command "
                                            << cmdObj << " when attempting to add shard "
                                            << targeter->connectionString().toString()
                                            << causedBy(writeConcernStatus)};
    }

    return Shard::CommandResponse(std::move(host),
                                  std::move(result),
                                  std::move(commandStatus),
                                  std::move(writeConcernStatus));
}

StatusWith<boost::optional<ShardType>> ShardingCatalogManager::_checkIfShardExists(
    OperationContext* opCtx,
    const ConnectionString& proposedShardConnectionString,
    const std::string* proposedShardName) {
    // Check whether any host in the connection is already part of the cluster.
    const auto existingShards =
        _localCatalogClient->getAllShards(opCtx, repl::ReadConcernLevel::kLocalReadConcern);
    if (!existingShards.isOK()) {
        return existingShards.getStatus().withContext(
            "Failed to load existing shards during addShard");
    }

    // Now check if this shard already exists - if it already exists *with the same options* then
    // the addShard request can return success early without doing anything more.
    for (const auto& existingShard : existingShards.getValue().value) {
        auto swExistingShardConnStr = ConnectionString::parse(existingShard.getHost());
        if (!swExistingShardConnStr.isOK()) {
            return swExistingShardConnStr.getStatus();
        }
        auto existingShardConnStr = std::move(swExistingShardConnStr.getValue());

        // Function for determining if the options for the shard that is being added match the
        // options of an existing shard that conflicts with it.
        auto shardsAreEquivalent = [&]() {
            if (proposedShardName && *proposedShardName != existingShard.getName()) {
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
            [&](bool checkShardEquivalency) -> StatusWith<boost::optional<ShardType>> {
            for (const auto& existingHost : existingShardConnStr.getServers()) {
                for (const auto& addingHost : proposedShardConnectionString.getServers()) {
                    if (existingHost == addingHost) {
                        if (checkShardEquivalency) {
                            // At least one of the hosts in the shard being added already exists in
                            // an existing shard. If the options aren't the same, then this is an
                            // error, but if the options match then the addShard operation should be
                            // immediately considered a success and terminated.
                            if (shardsAreEquivalent()) {
                                return {existingShard};
                            } else {
                                return {ErrorCodes::IllegalOperation,
                                        str::stream()
                                            << "'" << addingHost.toString() << "' "
                                            << "is already a member of the existing shard '"
                                            << existingShard.getHost() << "' ("
                                            << existingShard.getName() << ")."};
                            }
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
            if (shardsAreEquivalent()) {
                auto hostsAreEquivalent = checkIfHostsAreEquivalent(false);
                if (hostsAreEquivalent.isOK() && hostsAreEquivalent.getValue() != boost::none) {
                    return {existingShard};
                } else {
                    return {ErrorCodes::IllegalOperation,
                            str::stream()
                                << "A shard named " << existingShardConnStr.getSetName()
                                << " containing the replica set '"
                                << existingShardConnStr.getSetName() << "' already exists"};
                }
            } else {
                return {ErrorCodes::IllegalOperation,
                        str::stream() << "A shard already exists containing the replica set '"
                                      << existingShardConnStr.getSetName() << "'"};
            }
        }

        // Look if any of the hosts in the existing shard are present within the shard trying
        // to be added.
        auto hostsAreEquivalent = checkIfHostsAreEquivalent(true);
        if (!hostsAreEquivalent.isOK() || hostsAreEquivalent.getValue() != boost::none) {
            return hostsAreEquivalent;
        }

        if (proposedShardName && *proposedShardName == existingShard.getName()) {
            // If we get here then we're trying to add a shard with the same name as an
            // existing shard, but there was no overlap in the hosts between the existing
            // shard and the proposed connection string for the new shard.
            return {ErrorCodes::IllegalOperation,
                    str::stream() << "A shard named " << *proposedShardName << " already exists"};
        }
    }

    return {boost::none};
}

StatusWith<ShardType> ShardingCatalogManager::_validateHostAsShard(
    OperationContext* opCtx,
    std::shared_ptr<RemoteCommandTargeter> targeter,
    const std::string* shardProposedName,
    const ConnectionString& connectionString,
    bool isCatalogShard) {
    auto swCommandResponse = _runCommandForAddShard(
        opCtx, targeter.get(), DatabaseName::kAdmin.db(), BSON("isMaster" << 1));
    if (swCommandResponse.getStatus() == ErrorCodes::IncompatibleServerVersion) {
        return swCommandResponse.getStatus().withReason(
            str::stream() << "Cannot add " << connectionString.toString()
                          << " as a shard because its binary version is not compatible with "
                             "the cluster's featureCompatibilityVersion.");
    } else if (!swCommandResponse.isOK()) {
        return swCommandResponse.getStatus();
    }

    // Check for a command response error
    auto resIsMasterStatus = std::move(swCommandResponse.getValue().commandStatus);
    if (!resIsMasterStatus.isOK()) {
        return resIsMasterStatus.withContext(str::stream()
                                             << "Error running isMaster against "
                                             << targeter->connectionString().toString());
    }

    auto resIsMaster = std::move(swCommandResponse.getValue().response);

    // Fail if the node being added is a mongos.
    const std::string msg = resIsMaster.getStringField("msg").toString();
    if (msg == "isdbgrid") {
        return {ErrorCodes::IllegalOperation, "cannot add a mongos as a shard"};
    }

    // Extract the maxWireVersion so we can verify that the node being added has a binary version
    // greater than or equal to the cluster's featureCompatibilityVersion. We expect an incompatible
    // binary node to be unable to communicate, returning an IncompatibleServerVersion error,
    // because of our internal wire version protocol. So we can safely invariant here that the node
    // is compatible.
    long long maxWireVersion;
    Status status = bsonExtractIntegerField(resIsMaster, "maxWireVersion", &maxWireVersion);
    if (!status.isOK()) {
        return status.withContext(str::stream() << "isMaster returned invalid 'maxWireVersion' "
                                                << "field when attempting to add "
                                                << connectionString.toString() << " as a shard");
    }

    // Check whether there is a master. If there isn't, the replica set may not have been
    // initiated. If the connection is a standalone, it will return true for isMaster.
    bool isMaster;
    status = bsonExtractBooleanField(resIsMaster, "ismaster", &isMaster);
    if (!status.isOK()) {
        return status.withContext(str::stream() << "isMaster returned invalid 'ismaster' "
                                                << "field when attempting to add "
                                                << connectionString.toString() << " as a shard");
    }
    if (!isMaster) {
        return {ErrorCodes::NotWritablePrimary,
                str::stream()
                    << connectionString.toString()
                    << " does not have a master. If this is a replica set, ensure that it has a"
                    << " healthy primary and that the set has been properly initiated."};
    }

    const std::string providedSetName = connectionString.getSetName();
    const std::string foundSetName = resIsMaster["setName"].str();

    // Make sure the specified replica set name (if any) matches the actual shard's replica set
    if (providedSetName.empty() && !foundSetName.empty()) {
        return {ErrorCodes::OperationFailed,
                str::stream() << "host is part of set " << foundSetName << "; "
                              << "use replica set url format "
                              << "<setname>/<server1>,<server2>, ..."};
    }

    if (!providedSetName.empty() && foundSetName.empty()) {
        return {ErrorCodes::OperationFailed,
                str::stream() << "host did not return a set name; "
                              << "is the replica set still initializing? " << resIsMaster};
    }

    // Make sure the set name specified in the connection string matches the one where its hosts
    // belong into
    if (!providedSetName.empty() && (providedSetName != foundSetName)) {
        return {ErrorCodes::OperationFailed,
                str::stream() << "the provided connection string (" << connectionString.toString()
                              << ") does not match the actual set name " << foundSetName};
    }

    // Is it a config server?
    if (resIsMaster.hasField("configsvr") && !isCatalogShard) {
        return {ErrorCodes::OperationFailed,
                str::stream() << "Cannot add " << connectionString.toString()
                              << " as a shard since it is a config server"};
    }

    if (resIsMaster.hasField(HelloCommandReply::kIsImplicitDefaultMajorityWCFieldName) &&
        !resIsMaster.getBoolField(HelloCommandReply::kIsImplicitDefaultMajorityWCFieldName) &&
        !ReadWriteConcernDefaults::get(opCtx).isCWWCSet(opCtx)) {
        return {
            ErrorCodes::OperationFailed,
            str::stream()
                << "Cannot add " << connectionString.toString()
                << " as a shard since the implicit default write concern on this shard is set to "
                   "{w : 1}, because number of arbiters in the shard's configuration caused the "
                   "number of writable voting members not to be strictly more than the voting "
                   "majority. Change the shard configuration or set the cluster-wide write concern "
                   "using the setDefaultRWConcern command and try again."};
    }

    if (resIsMaster.hasField(HelloCommandReply::kCwwcFieldName)) {
        auto cwwcOnShard = WriteConcernOptions::parse(
                               resIsMaster.getObjectField(HelloCommandReply::kCwwcFieldName))
                               .getValue()
                               .toBSON();

        auto cachedCWWC = ReadWriteConcernDefaults::get(opCtx).getCWWC(opCtx);
        if (!cachedCWWC) {
            return {ErrorCodes::OperationFailed,
                    str::stream() << "Cannot add " << connectionString.toString()
                                  << " as a shard since the cluster-wide write concern is set on "
                                     "the shard and not set on the cluster. Set the CWWC on the "
                                     "cluster to the same CWWC as the shard and try again."
                                  << " The CWWC on the shard is (" << cwwcOnShard << ")."};
        }

        auto cwwcOnConfig = cachedCWWC.value().toBSON();
        BSONObjComparator comparator(
            BSONObj(), BSONObjComparator::FieldNamesMode::kConsider, nullptr);
        if (comparator.compare(cwwcOnShard, cwwcOnConfig) != 0) {
            return {
                ErrorCodes::OperationFailed,
                str::stream()
                    << "Cannot add " << connectionString.toString()
                    << " as a shard since the cluster-wide write concern set on the shard doesn't "
                       "match the one set on the cluster. Make sure they match and try again."
                    << " The CWWC on the shard is (" << cwwcOnShard
                    << "), and the CWWC on the cluster is (" << cwwcOnConfig << ")."};
        }
    }

    // If the shard is part of a replica set, make sure all the hosts mentioned in the connection
    // string are part of the set. It is fine if not all members of the set are mentioned in the
    // connection string, though.
    if (!providedSetName.empty()) {
        std::set<std::string> hostSet;

        BSONObjIterator iter(resIsMaster["hosts"].Obj());
        while (iter.more()) {
            hostSet.insert(iter.next().String());  // host:port
        }

        if (resIsMaster["passives"].isABSONObj()) {
            BSONObjIterator piter(resIsMaster["passives"].Obj());
            while (piter.more()) {
                hostSet.insert(piter.next().String());  // host:port
            }
        }

        if (resIsMaster["arbiters"].isABSONObj()) {
            BSONObjIterator piter(resIsMaster["arbiters"].Obj());
            while (piter.more()) {
                hostSet.insert(piter.next().String());  // host:port
            }
        }

        for (const auto& hostEntry : connectionString.getServers()) {
            const auto& host = hostEntry.toString();  // host:port
            if (hostSet.find(host) == hostSet.end()) {
                return {ErrorCodes::OperationFailed,
                        str::stream() << "in seed list " << connectionString.toString() << ", host "
                                      << host << " does not belong to replica set " << foundSetName
                                      << "; found " << resIsMaster.toString()};
            }
        }
    }

    std::string actualShardName;

    if (shardProposedName) {
        actualShardName = *shardProposedName;
    } else if (!foundSetName.empty()) {
        // Default it to the name of the replica set
        actualShardName = foundSetName;
    }

    // Disallow adding shard replica set with name 'config'
    if (!isCatalogShard && actualShardName == DatabaseName::kConfig.db()) {
        return {ErrorCodes::BadValue, "use of shard replica set with name 'config' is not allowed"};
    }

    // Retrieve the most up to date connection string that we know from the replica set monitor (if
    // this is a replica set shard, otherwise it will be the same value as connectionString).
    ConnectionString actualShardConnStr = targeter->connectionString();

    ShardType shard;
    shard.setName(actualShardName);
    shard.setHost(actualShardConnStr.toString());
    shard.setState(ShardType::ShardState::kShardAware);

    return shard;
}

Status ShardingCatalogManager::_dropSessionsCollection(
    OperationContext* opCtx, std::shared_ptr<RemoteCommandTargeter> targeter) {

    BSONObjBuilder builder;
    builder.append("drop", NamespaceString::kLogicalSessionsNamespace.coll());
    {
        BSONObjBuilder wcBuilder(builder.subobjStart("writeConcern"));
        wcBuilder.append("w", "majority");
    }

    auto swCommandResponse = _runCommandForAddShard(
        opCtx, targeter.get(), NamespaceString::kLogicalSessionsNamespace.db(), builder.done());
    if (!swCommandResponse.isOK()) {
        return swCommandResponse.getStatus();
    }

    auto cmdStatus = std::move(swCommandResponse.getValue().commandStatus);
    if (!cmdStatus.isOK() && cmdStatus.code() != ErrorCodes::NamespaceNotFound) {
        return cmdStatus;
    }

    return Status::OK();
}

StatusWith<std::vector<std::string>> ShardingCatalogManager::_getDBNamesListFromShard(
    OperationContext* opCtx, std::shared_ptr<RemoteCommandTargeter> targeter) {

    auto swCommandResponse =
        _runCommandForAddShard(opCtx,
                               targeter.get(),
                               DatabaseName::kAdmin.db(),
                               BSON("listDatabases" << 1 << "nameOnly" << true));
    if (!swCommandResponse.isOK()) {
        return swCommandResponse.getStatus();
    }

    auto cmdStatus = std::move(swCommandResponse.getValue().commandStatus);
    if (!cmdStatus.isOK()) {
        return cmdStatus;
    }

    auto cmdResult = std::move(swCommandResponse.getValue().response);

    std::vector<std::string> dbNames;

    for (const auto& dbEntry : cmdResult["databases"].Obj()) {
        const auto& dbName = dbEntry["name"].String();

        if (!(dbName == DatabaseName::kAdmin.db() || dbName == DatabaseName::kLocal.db() ||
              dbName == DatabaseName::kConfig.db())) {
            dbNames.push_back(dbName);
        }
    }

    return dbNames;
}

void ShardingCatalogManager::installConfigShardIdentityDocument(OperationContext* opCtx) {
    invariant(!ShardingState::get(opCtx)->enabled());

    // Insert a shard identity document. Note we insert with local write concern, so the shard
    // identity may roll back, which will trigger an fassert to clear the in-memory sharding state.
    {
        auto addShardCmd = add_shard_util::createAddShardCmd(opCtx, ShardId::kConfigServerId);

        auto shardIdUpsertCmd = add_shard_util::createShardIdentityUpsertForAddShard(
            addShardCmd, ShardingCatalogClient::kLocalWriteConcern);
        DBDirectClient localClient(opCtx);
        BSONObj res;

        localClient.runCommand(DatabaseName::kAdmin, shardIdUpsertCmd, res);

        uassertStatusOK(getStatusFromWriteCommandReply(res));
    }
}

StatusWith<std::string> ShardingCatalogManager::addShard(
    OperationContext* opCtx,
    const std::string* shardProposedName,
    const ConnectionString& shardConnectionString,
    bool isCatalogShard) {
    if (!shardConnectionString) {
        return {ErrorCodes::BadValue, "Invalid connection string"};
    }

    if (shardProposedName && shardProposedName->empty()) {
        return {ErrorCodes::BadValue, "shard name cannot be empty"};
    }

    const auto shardRegistry = Grid::get(opCtx)->shardRegistry();

    // Only one addShard operation can be in progress at a time.
    Lock::ExclusiveLock lk(opCtx, _kShardMembershipLock);

    // Check if this shard has already been added (can happen in the case of a retry after a network
    // error, for example) and thus this addShard request should be considered a no-op.
    auto existingShard = _checkIfShardExists(opCtx, shardConnectionString, shardProposedName);
    if (!existingShard.isOK()) {
        return existingShard.getStatus();
    }
    if (existingShard.getValue()) {
        // These hosts already belong to an existing shard, so report success and terminate the
        // addShard request.  Make sure to set the last optime for the client to the system last
        // optime so that we'll still wait for replication so that this state is visible in the
        // committed snapshot.
        repl::ReplClientInfo::forClient(opCtx->getClient()).setLastOpToSystemLastOpTime(opCtx);
        return existingShard.getValue()->getName();
    }

    const std::shared_ptr<Shard> shard{shardRegistry->createConnection(shardConnectionString)};
    auto targeter = shard->getTargeter();

    ScopeGuard stopMonitoringGuard([&] {
        // Do not remove the RSM for the config server because it is still needed even if
        // adding the config server as a shard failed.
        if (shardConnectionString.type() == ConnectionString::ConnectionType::kReplicaSet &&
            shardConnectionString.getReplicaSetName() !=
                repl::ReplicationCoordinator::get(opCtx)
                    ->getConfigConnectionString()
                    .getReplicaSetName()) {
            // This is a workaround for the case were we could have some bad shard being
            // requested to be added and we put that bad connection string on the global replica set
            // monitor registry. It needs to be cleaned up so that when a correct replica set is
            // added, it will be recreated.
            ReplicaSetMonitor::remove(shardConnectionString.getSetName());
        }
    });

    // Validate the specified connection string may serve as shard at all
    auto shardStatus = _validateHostAsShard(
        opCtx, targeter, shardProposedName, shardConnectionString, isCatalogShard);
    if (!shardStatus.isOK()) {
        return shardStatus.getStatus();
    }
    ShardType& shardType = shardStatus.getValue();

    // Check that none of the existing shard candidate's dbs exist already
    auto dbNamesStatus = _getDBNamesListFromShard(opCtx, targeter);
    if (!dbNamesStatus.isOK()) {
        return dbNamesStatus.getStatus();
    }

    for (const auto& dbName : dbNamesStatus.getValue()) {
        try {
            auto dbt = _localCatalogClient->getDatabase(
                opCtx, dbName, repl::ReadConcernLevel::kLocalReadConcern);
            return Status(ErrorCodes::OperationFailed,
                          str::stream() << "can't add shard "
                                        << "'" << shardConnectionString.toString() << "'"
                                        << " because a local database '" << dbName
                                        << "' exists in another " << dbt.getPrimary());
        } catch (const ExceptionFor<ErrorCodes::NamespaceNotFound>&) {
        }
    }

    // Check that the shard candidate does not have a local config.system.sessions collection
    auto res = _dropSessionsCollection(opCtx, targeter);
    if (!res.isOK()) {
        return res.withContext(
            "can't add shard with a local copy of config.system.sessions, please drop this "
            "collection from the shard manually and try again.");
    }

    // If a name for a shard wasn't provided, generate one
    if (shardType.getName().empty()) {
        auto result = generateNewShardName(opCtx, _localConfigShard.get());
        if (!result.isOK()) {
            return result.getStatus();
        }
        shardType.setName(result.getValue());
    }

    // Helper function that runs a command on the to-be shard and returns the status
    auto runCmdOnNewShard = [this, &opCtx, &targeter](const BSONObj& cmd) -> Status {
        auto swCommandResponse =
            _runCommandForAddShard(opCtx, targeter.get(), DatabaseName::kAdmin.db(), cmd);
        if (!swCommandResponse.isOK()) {
            return swCommandResponse.getStatus();
        }
        // Grabs the underlying status from a StatusWith object by taking the first
        // non-OK status, if there is one. This is needed due to the semantics of
        // _runCommandForAddShard.
        auto commandResponse = std::move(swCommandResponse.getValue());
        BatchedCommandResponse batchResponse;
        return Shard::CommandResponse::processBatchWriteResponse(commandResponse, &batchResponse);
    };

    if (!isCatalogShard) {
        AddShard addShardCmd = add_shard_util::createAddShardCmd(opCtx, shardType.getName());

        // Use the _addShard command to add the shard, which in turn inserts a shardIdentity
        // document into the shard and triggers sharding state initialization.
        auto addShardStatus = runCmdOnNewShard(addShardCmd.toBSON({}));
        if (!addShardStatus.isOK()) {
            return addShardStatus;
        }

        // Set the user-writes blocking state on the new shard.
        _setUserWriteBlockingStateOnNewShard(opCtx, targeter.get());

        // Determine the set of cluster parameters to be used.
        _standardizeClusterParameters(opCtx, shard.get());
    }

    {
        // Keep the FCV stable across checking the FCV, sending setFCV to the new shard and writing
        // the entry for the new shard to config.shards. This ensures the FCV doesn't change after
        // we send setFCV to the new shard, but before we write its entry to config.shards.
        //
        // NOTE: We don't use a Global IX lock here, because we don't want to hold the global lock
        // while blocking on the network).
        FixedFCVRegion fcvRegion(opCtx);

        // Prevent the race where an FCV downgrade happens concurrently with the catalogShard
        // being added and the FCV downgrade finishes before the catalogShard is added.
        uassert(
            5563604,
            "Cannot add catalog shard because it is not supported in featureCompatibilityVersion: {}"_format(
                multiversion::toString(serverGlobalParams.featureCompatibility.getVersion())),
            gFeatureFlagCatalogShard.isEnabled(serverGlobalParams.featureCompatibility) ||
                !isCatalogShard);

        if (isCatalogShard) {
            // TODO SERVER-75391: Remove.
            //
            // At this point we know the config primary is in the latest FCV, but secondaries may
            // not yet have replicated the FCV update, so write a noop and wait for it to replicate
            // to all nodes in the config server to guarantee they have replicated up to the latest
            // FCV.
            //
            // This guarantees all secondaries use the shard server method to refresh their
            // metadata, which contains synchronization to prevent secondaries from serving reads
            // for owned chunks that have not yet replicated to them.
            _performLocalNoopWriteWithWAllWriteConcern(
                opCtx, "w:all write barrier in transitionToCatalogShard");
        }

        uassert(5563603,
                "Cannot add shard while in upgrading/downgrading FCV state",
                !fcvRegion->isUpgradingOrDowngrading());

        // (Generic FCV reference): These FCV checks should exist across LTS binary versions.
        invariant(fcvRegion == multiversion::GenericFCV::kLatest ||
                  fcvRegion == multiversion::GenericFCV::kLastContinuous ||
                  fcvRegion == multiversion::GenericFCV::kLastLTS);

        if (!isCatalogShard) {
            SetFeatureCompatibilityVersion setFcvCmd(fcvRegion->getVersion());
            setFcvCmd.setDbName(DatabaseName::kAdmin);
            setFcvCmd.setFromConfigServer(true);

            auto versionResponse = _runCommandForAddShard(
                opCtx,
                targeter.get(),
                DatabaseName::kAdmin.db(),
                setFcvCmd.toBSON(BSON(WriteConcernOptions::kWriteConcernField
                                      << opCtx->getWriteConcern().toBSON())));
            if (!versionResponse.isOK()) {
                return versionResponse.getStatus();
            }

            if (!versionResponse.getValue().commandStatus.isOK()) {
                return versionResponse.getValue().commandStatus;
            }
        }

        // Tick clusterTime to get a new topologyTime for this mutation of the topology.
        auto newTopologyTime = VectorClockMutable::get(opCtx)->tickClusterTime(1);

        shardType.setTopologyTime(newTopologyTime.asTimestamp());

        LOGV2(21942,
              "Going to insert new entry for shard into config.shards: {shardType}",
              "Going to insert new entry for shard into config.shards",
              "shardType"_attr = shardType.toString());

        _addShardInTransaction(opCtx, shardType, std::move(dbNamesStatus.getValue()));

        // Record in changelog
        BSONObjBuilder shardDetails;
        shardDetails.append("name", shardType.getName());
        shardDetails.append("host", shardConnectionString.toString());

        ShardingLogging::get(opCtx)->logChange(opCtx,
                                               "addShard",
                                               "",
                                               shardDetails.obj(),
                                               ShardingCatalogClient::kMajorityWriteConcern,
                                               _localConfigShard,
                                               _localCatalogClient.get());

        // Ensure the added shard is visible to this process.
        shardRegistry->reload(opCtx);
        if (!shardRegistry->getShard(opCtx, shardType.getName()).isOK()) {
            return {ErrorCodes::OperationFailed,
                    "Could not find shard metadata for shard after adding it. This most likely "
                    "indicates that the shard was removed immediately after it was added."};
        }

        stopMonitoringGuard.dismiss();

        return shardType.getName();
    }
}

RemoveShardProgress ShardingCatalogManager::removeShard(OperationContext* opCtx,
                                                        const ShardId& shardId) {
    const auto name = shardId.toString();
    audit::logRemoveShard(opCtx->getClient(), name);

    Lock::ExclusiveLock shardLock(opCtx, _kShardMembershipLock);

    auto findShardResponse = uassertStatusOK(
        _localConfigShard->exhaustiveFindOnConfig(opCtx,
                                                  kConfigReadSelector,
                                                  repl::ReadConcernLevel::kLocalReadConcern,
                                                  NamespaceString::kConfigsvrShardsNamespace,
                                                  BSON(ShardType::name() << name),
                                                  BSONObj(),
                                                  1));
    uassert(ErrorCodes::ShardNotFound,
            str::stream() << "Shard " << shardId << " does not exist",
            !findShardResponse.docs.empty());
    const auto shard = uassertStatusOK(ShardType::fromBSON(findShardResponse.docs[0]));

    // Find how many *other* shards exist, which are *not* currently draining
    const auto countOtherNotDrainingShards = uassertStatusOK(_runCountCommandOnConfig(
        opCtx,
        NamespaceString::kConfigsvrShardsNamespace,
        BSON(ShardType::name() << NE << name << ShardType::draining.ne(true))));
    uassert(ErrorCodes::IllegalOperation,
            "Operation not allowed because it would remove the last shard",
            countOtherNotDrainingShards > 0);

    // Ensure there are no non-empty zones that only belong to this shard
    for (auto& zoneName : shard.getTags()) {
        auto isRequiredByZone = uassertStatusOK(
            _isShardRequiredByZoneStillInUse(opCtx, kConfigReadSelector, name, zoneName));
        uassert(ErrorCodes::ZoneStillInUse,
                str::stream()
                    << "Operation not allowed because it would remove the only shard for zone "
                    << zoneName << " which has a chunk range is associated with it",
                !isRequiredByZone);
    }

    // Figure out if shard is already draining
    const bool isShardCurrentlyDraining =
        uassertStatusOK(_runCountCommandOnConfig(
            opCtx,
            NamespaceString::kConfigsvrShardsNamespace,
            BSON(ShardType::name() << name << ShardType::draining(true)))) > 0;

    if (!isShardCurrentlyDraining) {
        LOGV2(21945,
              "Going to start draining shard: {shardId}",
              "Going to start draining shard",
              "shardId"_attr = name);

        // Record start in changelog
        uassertStatusOK(
            ShardingLogging::get(opCtx)->logChangeChecked(opCtx,
                                                          "removeShard.start",
                                                          "",
                                                          BSON("shard" << name),
                                                          ShardingCatalogClient::kLocalWriteConcern,
                                                          _localConfigShard,
                                                          _localCatalogClient.get()));

        uassertStatusOKWithContext(_localCatalogClient->updateConfigDocument(
                                       opCtx,
                                       NamespaceString::kConfigsvrShardsNamespace,
                                       BSON(ShardType::name() << name),
                                       BSON("$set" << BSON(ShardType::draining(true))),
                                       false,
                                       ShardingCatalogClient::kLocalWriteConcern),
                                   "error starting removeShard");

        return {RemoveShardProgress::STARTED,
                boost::optional<RemoveShardProgress::DrainingShardUsage>(boost::none)};
    }

    shardLock.unlock();

    // Draining has already started, now figure out how many chunks and databases are still on the
    // shard.
    const auto chunkCount = uassertStatusOK(
        _runCountCommandOnConfig(opCtx, ChunkType::ConfigNS, BSON(ChunkType::shard(name))));

    const auto databaseCount =
        uassertStatusOK(_runCountCommandOnConfig(opCtx,
                                                 NamespaceString::kConfigDatabasesNamespace,
                                                 BSON(DatabaseType::kPrimaryFieldName << name)));

    const auto jumboCount = uassertStatusOK(_runCountCommandOnConfig(
        opCtx, ChunkType::ConfigNS, BSON(ChunkType::shard(name) << ChunkType::jumbo(true))));

    if (chunkCount > 0 || databaseCount > 0) {
        // Still more draining to do
        LOGV2(21946,
              "removeShard: draining chunkCount {chunkCount}; databaseCount {databaseCount}; "
              "jumboCount {jumboCount}",
              "removeShard: draining",
              "chunkCount"_attr = chunkCount,
              "databaseCount"_attr = databaseCount,
              "jumboCount"_attr = jumboCount);

        return {RemoveShardProgress::ONGOING,
                boost::optional<RemoveShardProgress::DrainingShardUsage>(
                    {chunkCount, databaseCount, jumboCount})};
    }

    // Draining is done, now finish removing the shard.
    LOGV2(
        21949, "Going to remove shard: {shardId}", "Going to remove shard", "shardId"_attr = name);

    if (shardId == ShardId::kConfigServerId) {
        // Drop the drained collections locally so the config server can transition back to catalog
        // shard mode in the future without requiring users to manually drop them.
        LOGV2(7509600, "Locally dropping drained collections", "shardId"_attr = name);

        auto shardedCollections = _localCatalogClient->getCollections(opCtx, {});
        for (auto&& collection : shardedCollections) {
            DBDirectClient client(opCtx);

            BSONObj result;
            if (!client.dropCollection(
                    collection.getNss(), ShardingCatalogClient::kLocalWriteConcern, &result)) {
                // Note attempting to drop a non-existent collection does not return an error, so
                // it's safe to assert the status is ok even if an earlier attempt was interrupted
                // by a failover.
                uassertStatusOK(getStatusFromCommandResult(result));
            }

            hangAfterDroppingCollectionInTransitionToDedicatedConfigServer.pauseWhileSet(opCtx);
        }
    }

    // Synchronize the control shard selection, the shard's document removal, and the topology time
    // update to exclude potential race conditions in case of concurrent add/remove shard
    // operations.
    shardLock.lock();

    // Find a controlShard to be updated.
    auto controlShardQueryStatus = _localConfigShard->exhaustiveFindOnConfig(
        opCtx,
        ReadPreferenceSetting{ReadPreference::PrimaryOnly},
        repl::ReadConcernLevel::kLocalReadConcern,
        NamespaceString::kConfigsvrShardsNamespace,
        BSON(ShardType::name.ne(name)),
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
    _removeShardInTransaction(opCtx, name, controlShardName, newTopologyTime.asTimestamp());

    shardLock.unlock();

    // The shard which was just removed must be reflected in the shard registry, before the replica
    // set monitor is removed, otherwise the shard would be referencing a dropped RSM.
    Grid::get(opCtx)->shardRegistry()->reload(opCtx);

    if (shardId != ShardId::kConfigServerId) {
        // Don't remove the catalog shard's RSM because it is used to target the config server.
        ReplicaSetMonitor::remove(name);
    }

    // Record finish in changelog
    ShardingLogging::get(opCtx)->logChange(opCtx,
                                           "removeShard",
                                           "",
                                           BSON("shard" << name),
                                           ShardingCatalogClient::kLocalWriteConcern,
                                           _localConfigShard,
                                           _localCatalogClient.get());

    return {RemoveShardProgress::COMPLETED,
            boost::optional<RemoveShardProgress::DrainingShardUsage>(boost::none)};
}

void ShardingCatalogManager::appendShardDrainingStatus(OperationContext* opCtx,
                                                       BSONObjBuilder& result,
                                                       RemoveShardProgress shardDrainingStatus,
                                                       ShardId shardId) {
    const auto databases =
        uassertStatusOK(_localCatalogClient->getDatabasesForShard(opCtx, shardId));

    // Get BSONObj containing:
    // 1) note about moving or dropping databases in a shard
    // 2) list of databases (excluding 'local' database) that need to be moved
    const auto dbInfo = [&] {
        BSONObjBuilder dbInfoBuilder;
        dbInfoBuilder.append("note", "you need to drop or movePrimary these databases");

        BSONArrayBuilder dbs(dbInfoBuilder.subarrayStart("dbsToMove"));
        for (const auto& db : databases) {
            if (db != DatabaseName::kLocal.db()) {
                dbs.append(db);
            }
        }
        dbs.doneFast();

        return dbInfoBuilder.obj();
    }();

    switch (shardDrainingStatus.status) {
        case RemoveShardProgress::STARTED:
            result.append("msg", "draining started successfully");
            result.append("state", "started");
            result.append("shard", shardId);
            result.appendElements(dbInfo);
            break;
        case RemoveShardProgress::ONGOING: {
            const auto& remainingCounts = shardDrainingStatus.remainingCounts;
            result.append("msg", "draining ongoing");
            result.append("state", "ongoing");
            result.append("remaining",
                          BSON("chunks" << remainingCounts->totalChunks << "dbs"
                                        << remainingCounts->databases << "jumboChunks"
                                        << remainingCounts->jumboChunks));
            result.appendElements(dbInfo);
            break;
        }
        case RemoveShardProgress::COMPLETED:
            result.append("msg", "removeshard completed successfully");
            result.append("state", "completed");
            result.append("shard", shardId);
            break;
    }
}

Lock::SharedLock ShardingCatalogManager::enterStableTopologyRegion(OperationContext* opCtx) {
    return Lock::SharedLock(opCtx, _kShardMembershipLock);
}

void ShardingCatalogManager::appendConnectionStats(executor::ConnectionPoolStats* stats) {
    _executorForAddShard->appendConnectionStats(stats);
}

StatusWith<long long> ShardingCatalogManager::_runCountCommandOnConfig(OperationContext* opCtx,
                                                                       const NamespaceString& nss,
                                                                       BSONObj query) {
    BSONObjBuilder countBuilder;
    countBuilder.append("count", nss.coll());
    countBuilder.append("query", query);

    auto resultStatus =
        _localConfigShard->runCommandWithFixedRetryAttempts(opCtx,
                                                            kConfigReadSelector,
                                                            nss.db().toString(),
                                                            countBuilder.done(),
                                                            Shard::kDefaultConfigCommandTimeout,
                                                            Shard::RetryPolicy::kIdempotent);
    if (!resultStatus.isOK()) {
        return resultStatus.getStatus();
    }
    if (!resultStatus.getValue().commandStatus.isOK()) {
        return resultStatus.getValue().commandStatus;
    }

    auto responseObj = std::move(resultStatus.getValue().response);

    long long result;
    auto status = bsonExtractIntegerField(responseObj, "n", &result);
    if (!status.isOK()) {
        return status;
    }

    return result;
}

void ShardingCatalogManager::_setUserWriteBlockingStateOnNewShard(OperationContext* opCtx,
                                                                  RemoteCommandTargeter* targeter) {
    // Delete all the config.user_writes_critical_sections documents from the new shard.
    {
        write_ops::DeleteCommandRequest deleteOp(
            NamespaceString::kUserWritesCriticalSectionsNamespace);
        write_ops::DeleteOpEntry query({}, true /*multi*/);
        deleteOp.setDeletes({query});

        const auto swCommandResponse =
            _runCommandForAddShard(opCtx,
                                   targeter,
                                   NamespaceString::kUserWritesCriticalSectionsNamespace.db(),
                                   CommandHelpers::appendMajorityWriteConcern(deleteOp.toBSON({})));
        uassertStatusOK(swCommandResponse.getStatus());
        uassertStatusOK(getStatusFromWriteCommandReply(swCommandResponse.getValue().response));
    }

    // Propagate the cluster's current user write blocking state onto the new shard.
    PersistentTaskStore<UserWriteBlockingCriticalSectionDocument> store(
        NamespaceString::kUserWritesCriticalSectionsNamespace);
    store.forEach(opCtx, BSONObj(), [&](const UserWriteBlockingCriticalSectionDocument& doc) {
        invariant(doc.getNss() ==
                  UserWritesRecoverableCriticalSectionService::kGlobalUserWritesNamespace);

        const auto makeShardsvrSetUserWriteBlockModeCommand =
            [](ShardsvrSetUserWriteBlockModePhaseEnum phase) -> BSONObj {
            ShardsvrSetUserWriteBlockMode shardsvrSetUserWriteBlockModeCmd;
            shardsvrSetUserWriteBlockModeCmd.setDbName(DatabaseName::kAdmin);
            SetUserWriteBlockModeRequest setUserWriteBlockModeRequest(true /* global */);
            shardsvrSetUserWriteBlockModeCmd.setSetUserWriteBlockModeRequest(
                std::move(setUserWriteBlockModeRequest));
            shardsvrSetUserWriteBlockModeCmd.setPhase(phase);

            return CommandHelpers::appendMajorityWriteConcern(
                shardsvrSetUserWriteBlockModeCmd.toBSON({}));
        };

        if (doc.getBlockNewUserShardedDDL()) {
            const auto cmd = makeShardsvrSetUserWriteBlockModeCommand(
                ShardsvrSetUserWriteBlockModePhaseEnum::kPrepare);

            const auto cmdResponse =
                _runCommandForAddShard(opCtx, targeter, DatabaseName::kAdmin.db(), cmd);
            uassertStatusOK(Shard::CommandResponse::getEffectiveStatus(cmdResponse));
        }

        if (doc.getBlockUserWrites()) {
            invariant(doc.getBlockNewUserShardedDDL());
            const auto cmd = makeShardsvrSetUserWriteBlockModeCommand(
                ShardsvrSetUserWriteBlockModePhaseEnum::kComplete);

            const auto cmdResponse =
                _runCommandForAddShard(opCtx, targeter, DatabaseName::kAdmin.db(), cmd);
            uassertStatusOK(Shard::CommandResponse::getEffectiveStatus(cmdResponse));
        }

        return true;
    });
}

void ShardingCatalogManager::_setClusterParametersLocally(OperationContext* opCtx,
                                                          const boost::optional<TenantId>& tenantId,
                                                          const std::vector<BSONObj>& parameters) {
    DBDirectClient client(opCtx);
    ClusterParameterDBClientService dbService(client);
    for (auto& parameter : parameters) {
        SetClusterParameter setClusterParameterRequest(
            BSON(parameter["_id"].String() << parameter.filterFieldsUndotted(
                     BSON("_id" << 1 << "clusterParameterTime" << 1), false)));
        setClusterParameterRequest.setDbName(DatabaseName(tenantId, DatabaseName::kAdmin.db()));
        std::unique_ptr<ServerParameterService> parameterService =
            std::make_unique<ClusterParameterService>();
        SetClusterParameterInvocation invocation{std::move(parameterService), dbService};
        invocation.invoke(opCtx,
                          setClusterParameterRequest,
                          parameter["clusterParameterTime"].timestamp(),
                          kMajorityWriteConcern);
    }
}

void ShardingCatalogManager::_pullClusterParametersFromNewShard(OperationContext* opCtx,
                                                                Shard* shard) {
    const auto& targeter = shard->getTargeter();
    LOGV2(6538600, "Pulling cluster parameters from new shard");

    // We can safely query the cluster parameters because the replica set must have been started
    // with --shardsvr in order to add it into the cluster, and in this mode no setClusterParameter
    // can be called on the replica set directly.
    auto host = uassertStatusOK(
        targeter->findHost(opCtx, ReadPreferenceSetting{ReadPreference::PrimaryOnly}));
    auto tenantIds =
        uassertStatusOK(getTenantsWithConfigDbsOnShard(opCtx, shard, _executorForAddShard.get()));
    const Milliseconds maxTimeMS =
        std::min(opCtx->getRemainingMaxTimeMillis(), Milliseconds(Seconds{30}));

    std::vector<std::unique_ptr<Fetcher>> fetchers;
    fetchers.reserve(tenantIds.size());
    // If for some reason the callback never gets invoked, we will return this status in
    // response.
    std::vector<Status> statuses(
        tenantIds.size(),
        Status(ErrorCodes::InternalError, "Internal error running cursor callback in command"));
    std::vector<std::vector<BSONObj>> allParameters(tenantIds.size());
    int i = 0;
    for (const auto& tenantId : tenantIds) {
        BSONObjBuilder findCmdBuilder;
        {
            FindCommandRequest findCommand(NamespaceString::makeClusterParametersNSS(tenantId));
            auto readConcern = repl::ReadConcernArgs(boost::optional<repl::ReadConcernLevel>(
                repl::ReadConcernLevel::kMajorityReadConcern));
            findCommand.setReadConcern(readConcern.toBSONInner());
            findCommand.setMaxTimeMS(durationCount<Milliseconds>(maxTimeMS));
            findCommand.serialize(BSONObj(), &findCmdBuilder);
        }

        auto fetcherCallback =
            [this, &statuses, &allParameters, i](const Fetcher::QueryResponseStatus& dataStatus,
                                                 Fetcher::NextAction* nextAction,
                                                 BSONObjBuilder* getMoreBob) {
                // Throw out any accumulated results on error
                if (!dataStatus.isOK()) {
                    statuses[i] = dataStatus.getStatus();
                    return;
                }
                const auto& data = dataStatus.getValue();

                std::vector<BSONObj> parameters;
                for (const BSONObj& doc : data.documents) {
                    parameters.push_back(doc.getOwned());
                }

                allParameters[i] = parameters;
                statuses[i] = Status::OK();

                if (!getMoreBob) {
                    return;
                }
                getMoreBob->append("getMore", data.cursorId);
                getMoreBob->append("collection", data.nss.coll());
            };

        auto fetcher = std::make_unique<Fetcher>(
            _executorForAddShard.get(),
            host,
            NamespaceString::makeClusterParametersNSS(tenantId).dbName().toStringWithTenantId(),
            findCmdBuilder.obj(),
            fetcherCallback,
            BSONObj(), /* metadata tracking, only used for shards */
            maxTimeMS, /* command network timeout */
            maxTimeMS /* getMore network timeout */);

        uassertStatusOK(fetcher->schedule());

        fetchers.push_back(std::move(fetcher));

        i++;
    }

    i = 0;
    for (const auto& tenantId : tenantIds) {
        uassertStatusOK(fetchers[i]->join(opCtx));
        uassertStatusOK(statuses[i]);

        _setClusterParametersLocally(opCtx, tenantId, allParameters[i]);

        i++;
    }
}

void ShardingCatalogManager::_removeAllClusterParametersFromShard(OperationContext* opCtx,
                                                                  Shard* shard) {
    const auto& targeter = shard->getTargeter();
    auto tenantsOnTarget =
        uassertStatusOK(getTenantsWithConfigDbsOnShard(opCtx, shard, _executorForAddShard.get()));

    // Remove possible leftovers config.clusterParameters documents from the new shard.
    for (const auto& tenantId : tenantsOnTarget) {
        write_ops::DeleteCommandRequest deleteOp(
            NamespaceString::makeClusterParametersNSS(tenantId));
        write_ops::DeleteOpEntry query({}, true /*multi*/);
        deleteOp.setDeletes({query});

        const auto swCommandResponse = _runCommandForAddShard(
            opCtx,
            targeter.get(),
            NamespaceString::makeClusterParametersNSS(tenantId).dbName().toStringWithTenantId(),
            CommandHelpers::appendMajorityWriteConcern(deleteOp.toBSON({})));
        uassertStatusOK(swCommandResponse.getStatus());
        uassertStatusOK(getStatusFromWriteCommandReply(swCommandResponse.getValue().response));
    }
}

void ShardingCatalogManager::_pushClusterParametersToNewShard(
    OperationContext* opCtx,
    Shard* shard,
    const TenantIdMap<std::vector<BSONObj>>& allClusterParameters) {
    // First, remove all existing parameters from the new shard.
    _removeAllClusterParametersFromShard(opCtx, shard);

    const auto& targeter = shard->getTargeter();
    LOGV2(6360600, "Pushing cluster parameters into new shard");

    for (const auto& [tenantId, clusterParameters] : allClusterParameters) {
        // Push cluster parameters into the newly added shard.
        for (auto& parameter : clusterParameters) {
            ShardsvrSetClusterParameter setClusterParamsCmd(
                BSON(parameter["_id"].String() << parameter.filterFieldsUndotted(
                         BSON("_id" << 1 << "clusterParameterTime" << 1), false)));
            setClusterParamsCmd.setDbName(DatabaseName(tenantId, DatabaseName::kAdmin.db()));
            setClusterParamsCmd.setClusterParameterTime(
                parameter["clusterParameterTime"].timestamp());

            const auto cmdResponse = _runCommandForAddShard(
                opCtx,
                targeter.get(),
                DatabaseName(tenantId, DatabaseName::kAdmin.db()).toStringWithTenantId(),
                CommandHelpers::appendMajorityWriteConcern(setClusterParamsCmd.toBSON({})));
            uassertStatusOK(Shard::CommandResponse::getEffectiveStatus(cmdResponse));
        }
    }
}

void ShardingCatalogManager::_standardizeClusterParameters(OperationContext* opCtx, Shard* shard) {
    auto tenantIds =
        uassertStatusOK(getTenantsWithConfigDbsOnShard(opCtx, _localConfigShard.get()));
    TenantIdMap<std::vector<BSONObj>> configSvrClusterParameterDocs;
    for (const auto& tenantId : tenantIds) {
        auto findResponse = uassertStatusOK(_localConfigShard->exhaustiveFindOnConfig(
            opCtx,
            ReadPreferenceSetting{ReadPreference::PrimaryOnly},
            repl::ReadConcernLevel::kLocalReadConcern,
            NamespaceString::makeClusterParametersNSS(tenantId),
            BSONObj(),
            BSONObj(),
            boost::none));

        configSvrClusterParameterDocs.emplace(tenantId, findResponse.docs);
    }

    auto shardsDocs = uassertStatusOK(_localConfigShard->exhaustiveFindOnConfig(
        opCtx,
        ReadPreferenceSetting(ReadPreference::PrimaryOnly),
        repl::ReadConcernLevel::kLocalReadConcern,
        NamespaceString::kConfigsvrShardsNamespace,
        BSONObj(),
        BSONObj(),
        boost::none));

    // If this is the first shard being added, and no cluster parameters have been set, then this
    // can be seen as a replica set to shard conversion -- absorb all of this shard's cluster
    // parameters. Otherwise, push our cluster parameters to the shard.
    if (shardsDocs.docs.empty()) {
        bool clusterParameterDocsEmpty = std::all_of(
            configSvrClusterParameterDocs.begin(),
            configSvrClusterParameterDocs.end(),
            [&](const std::pair<boost::optional<TenantId>, std::vector<BSONObj>>& tenantParams) {
                return tenantParams.second.empty();
            });
        if (clusterParameterDocsEmpty) {
            _pullClusterParametersFromNewShard(opCtx, shard);
            return;
        }
    }
    _pushClusterParametersToNewShard(opCtx, shard, configSvrClusterParameterDocs);
}

void ShardingCatalogManager::_addShardInTransaction(
    OperationContext* opCtx,
    const ShardType& newShard,
    std::vector<std::string>&& databasesInNewShard) {

    const auto existingShardIds = Grid::get(opCtx)->shardRegistry()->getAllShardIds(opCtx);

    // 1. Send out the "prepareCommit" notification
    std::vector<DatabaseName> importedDbNames;
    std::transform(
        databasesInNewShard.begin(),
        databasesInNewShard.end(),
        std::back_inserter(importedDbNames),
        [](const std::string& s) { return DatabaseNameUtil::deserialize(boost::none, s); });
    DatabasesAdded notification(std::move(importedDbNames), true /*addImported*/);
    notification.setPhase(CommitPhaseEnum::kPrepare);
    notification.setPrimaryShard(ShardId(newShard.getName()));
    uassertStatusOK(_notifyClusterOnNewDatabases(opCtx, notification, existingShardIds));

    // 2. Set up and run the commit statements
    // TODO SERVER-66261 newShard may be passed by reference.
    auto transactionChain = [newShard, dbNames = std::move(databasesInNewShard)](
                                const txn_api::TransactionClient& txnClient, ExecutorPtr txnExec) {
        write_ops::InsertCommandRequest insertShardEntry(NamespaceString::kConfigsvrShardsNamespace,
                                                         {newShard.toBSON()});
        return txnClient.runCRUDOp(insertShardEntry, {})
            .thenRunOn(txnExec)
            .then([&](const BatchedCommandResponse& insertShardEntryResponse) {
                uassertStatusOK(insertShardEntryResponse.toStatus());
                if (dbNames.empty()) {
                    BatchedCommandResponse noOpResponse;
                    noOpResponse.setStatus(Status::OK());
                    noOpResponse.setN(0);

                    return SemiFuture<BatchedCommandResponse>(std::move(noOpResponse));
                }

                std::vector<BSONObj> databaseEntries;
                std::transform(dbNames.begin(),
                               dbNames.end(),
                               std::back_inserter(databaseEntries),
                               [&](const std::string dbName) {
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
                if (dbNames.empty()) {
                    BatchedCommandResponse noOpResponse;
                    noOpResponse.setStatus(Status::OK());
                    noOpResponse.setN(0);

                    return SemiFuture<BatchedCommandResponse>(std::move(noOpResponse));
                }
                std::vector<BSONObj> placementEntries;
                std::transform(dbNames.begin(),
                               dbNames.end(),
                               std::back_inserter(placementEntries),
                               [&](const std::string dbName) {
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

    auto& executor = Grid::get(opCtx)->getExecutorPool()->getFixedExecutor();
    auto inlineExecutor = std::make_shared<executor::InlineExecutor>();
    auto sleepInlineExecutor = inlineExecutor->getSleepableExecutor(executor);

    txn_api::SyncTransactionWithRetries txn(opCtx, sleepInlineExecutor, nullptr, inlineExecutor);
    txn.run(opCtx, transactionChain);

    hangBeforeNotifyingaddShardCommitted.pauseWhileSet();

    // 3. Reuse the existing notification object to also broadcast the event of successful commit.
    notification.setPhase(CommitPhaseEnum::kSuccessful);
    notification.setPrimaryShard(boost::none);
    const auto notificationOutcome =
        _notifyClusterOnNewDatabases(opCtx, notification, existingShardIds);
    if (!notificationOutcome.isOK()) {
        LOGV2_WARNING(7175502,
                      "Unable to send out notification of successful import of databases "
                      "from added shard",
                      "err"_attr = notificationOutcome);
    }
}


void ShardingCatalogManager::_removeShardInTransaction(OperationContext* opCtx,
                                                       const std::string& removedShardName,
                                                       const std::string& controlShardName,
                                                       const Timestamp& newTopologyTime) {
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
            .then([removedShardName](auto updateResponse) {
                uassertStatusOK(updateResponse.toStatus());
                LOGV2_DEBUG(
                    6583701, 1, "Finished removing shard ", "shard"_attr = removedShardName);
            })
            .semi();
    };

    auto inlineExecutor = std::make_shared<executor::InlineExecutor>();
    auto sleepInlineExecutor = inlineExecutor->getSleepableExecutor(
        Grid::get(opCtx)->getExecutorPool()->getFixedExecutor());

    txn_api::SyncTransactionWithRetries txn(opCtx, sleepInlineExecutor, nullptr, inlineExecutor);

    txn.run(opCtx, removeShardFn);
}

}  // namespace mongo
