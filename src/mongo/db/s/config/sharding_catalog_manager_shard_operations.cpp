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

#include <boost/cstdint.hpp>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr.hpp>
#include <fmt/format.h>
// IWYU pragma: no_include "ext/alloc_traits.h"
#include <algorithm>
#include <cstdint>
#include <iomanip>
#include <istream>
#include <iterator>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bson_field.h"
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
#include "mongo/db/commands.h"
#include "mongo/db/commands/cluster_server_parameter_cmds_gen.h"
#include "mongo/db/commands/feature_compatibility_version.h"
#include "mongo/db/commands/notify_sharding_event_gen.h"
#include "mongo/db/commands/set_cluster_parameter_invocation.h"
#include "mongo/db/commands/set_feature_compatibility_version_gen.h"
#include "mongo/db/commands/set_user_write_block_mode_gen.h"
#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/database_name.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/feature_flag.h"
#include "mongo/db/keys_collection_document_gen.h"
#include "mongo/db/keys_collection_util.h"
#include "mongo/db/list_collections_gen.h"
#include "mongo/db/logical_time.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/ops/write_ops_gen.h"
#include "mongo/db/ops/write_ops_parsers.h"
#include "mongo/db/persistent_task_store.h"
#include "mongo/db/query/find_command.h"
#include "mongo/db/read_write_concern_defaults.h"
#include "mongo/db/repl/hello_gen.h"
#include "mongo/db/repl/optime_with.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/db/repl/read_concern_level.h"
#include "mongo/db/repl/repl_client_info.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/wait_for_majority_service.h"
#include "mongo/db/resource_yielder.h"
#include "mongo/db/s/add_shard_cmd_gen.h"
#include "mongo/db/s/add_shard_util.h"
#include "mongo/db/s/config/sharding_catalog_manager.h"
#include "mongo/db/s/range_deletion_task_gen.h"
#include "mongo/db/s/sharding_cluster_parameters_gen.h"
#include "mongo/db/s/sharding_config_server_parameters_gen.h"
#include "mongo/db/s/sharding_ddl_util.h"
#include "mongo/db/s/sharding_logging.h"
#include "mongo/db/s/user_writes_critical_section_document_gen.h"
#include "mongo/db/s/user_writes_recoverable_critical_section_service.h"
#include "mongo/db/server_options.h"
#include "mongo/db/server_parameter.h"
#include "mongo/db/service_context.h"
#include "mongo/db/session/logical_session_id.h"
#include "mongo/db/shard_id.h"
#include "mongo/db/tenant_id.h"
#include "mongo/db/transaction/transaction_api.h"
#include "mongo/db/vector_clock_mutable.h"
#include "mongo/db/write_concern_options.h"
#include "mongo/executor/connection_pool_stats.h"
#include "mongo/executor/inline_executor.h"
#include "mongo/executor/remote_command_request.h"
#include "mongo/executor/remote_command_response.h"
#include "mongo/executor/task_executor.h"
#include "mongo/executor/task_executor_pool.h"
#include "mongo/idl/cluster_server_parameter_common.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_attr.h"
#include "mongo/logv2/log_component.h"
#include "mongo/logv2/redaction.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/rpc/metadata.h"
#include "mongo/s/catalog/sharding_catalog_client.h"
#include "mongo/s/catalog/type_chunk.h"
#include "mongo/s/catalog/type_database_gen.h"
#include "mongo/s/catalog/type_namespace_placement_gen.h"
#include "mongo/s/catalog/type_shard.h"
#include "mongo/s/client/shard.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/database_version.h"
#include "mongo/s/grid.h"
#include "mongo/s/request_types/sharded_ddl_commands_gen.h"
#include "mongo/s/sharding_feature_flags_gen.h"
#include "mongo/s/sharding_state.h"
#include "mongo/s/write_ops/batched_command_response.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/clock_source.h"
#include "mongo/util/database_name_util.h"
#include "mongo/util/decorable.h"
#include "mongo/util/duration.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/future.h"
#include "mongo/util/future_impl.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/out_of_line_executor.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/str.h"
#include "mongo/util/time_support.h"
#include "mongo/util/uuid.h"
#include "mongo/util/version/releases.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding


namespace mongo {
namespace {

MONGO_FAIL_POINT_DEFINE(hangAddShardBeforeUpdatingClusterCardinalityParameter);
MONGO_FAIL_POINT_DEFINE(hangAfterDroppingDatabaseInTransitionToDedicatedConfigServer);
MONGO_FAIL_POINT_DEFINE(skipUpdatingClusterCardinalityParameterAfterAddShard);
MONGO_FAIL_POINT_DEFINE(skipUpdatingClusterCardinalityParameterAfterRemoveShard);

using CallbackHandle = executor::TaskExecutor::CallbackHandle;
using CallbackArgs = executor::TaskExecutor::CallbackArgs;
using RemoteCommandCallbackArgs = executor::TaskExecutor::RemoteCommandCallbackArgs;
using RemoteCommandCallbackFn = executor::TaskExecutor::RemoteCommandCallbackFn;

const ReadPreferenceSetting kConfigReadSelector(ReadPreference::Nearest, TagSet{});
const WriteConcernOptions kMajorityWriteConcern{WriteConcernOptions::kMajority,
                                                WriteConcernOptions::SyncMode::UNSET,
                                                WriteConcernOptions::kNoTimeout};

const Seconds kRemoteCommandTimeout{60};

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
    const DatabaseName& dbName,
    const BSONObj& cmdObj) {
    auto swHost = targeter->findHost(opCtx, ReadPreferenceSetting{ReadPreference::PrimaryOnly});
    if (!swHost.isOK()) {
        return swHost.getStatus();
    }
    auto host = std::move(swHost.getValue());

    executor::RemoteCommandRequest request(
        host, dbName, cmdObj, rpc::makeEmptyMetadata(), opCtx, kRemoteCommandTimeout);

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
        LOGV2(21941, "Operation timed out", "error"_attr = redact(response.status));
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
    bool isConfigShard) {
    auto swCommandResponse =
        _runCommandForAddShard(opCtx, targeter.get(), DatabaseName::kAdmin, BSON("hello" << 1));
    if (swCommandResponse.getStatus() == ErrorCodes::IncompatibleServerVersion) {
        return swCommandResponse.getStatus().withReason(
            str::stream() << "Cannot add " << connectionString.toString()
                          << " as a shard because its binary version is not compatible with "
                             "the cluster's featureCompatibilityVersion.");
    } else if (!swCommandResponse.isOK()) {
        return swCommandResponse.getStatus();
    }

    // Check for a command response error
    auto resHelloStatus = std::move(swCommandResponse.getValue().commandStatus);
    if (!resHelloStatus.isOK()) {
        return resHelloStatus.withContext(str::stream() << "Error running 'hello' against "
                                                        << targeter->connectionString().toString());
    }

    auto resHello = std::move(swCommandResponse.getValue().response);

    // Fail if the node being added is a mongos.
    const std::string msg = resHello.getStringField("msg").toString();
    if (msg == "isdbgrid") {
        return {ErrorCodes::IllegalOperation, "cannot add a mongos as a shard"};
    }

    // Extract the maxWireVersion so we can verify that the node being added has a binary version
    // greater than or equal to the cluster's featureCompatibilityVersion. We expect an incompatible
    // binary node to be unable to communicate, returning an IncompatibleServerVersion error,
    // because of our internal wire version protocol. So we can safely invariant here that the node
    // is compatible.
    long long maxWireVersion;
    Status status = bsonExtractIntegerField(resHello, "maxWireVersion", &maxWireVersion);
    if (!status.isOK()) {
        return status.withContext(str::stream() << "hello returned invalid 'maxWireVersion' "
                                                << "field when attempting to add "
                                                << connectionString.toString() << " as a shard");
    }

    // Check whether the host is a writable primary. If not, the replica set may not have been
    // initiated. If the connection is a standalone, it will return true for "isWritablePrimary".
    bool isWritablePrimary;
    status = bsonExtractBooleanField(resHello, "isWritablePrimary", &isWritablePrimary);
    if (!status.isOK()) {
        return status.withContext(str::stream() << "hello returned invalid 'isWritablePrimary' "
                                                << "field when attempting to add "
                                                << connectionString.toString() << " as a shard");
    }
    if (!isWritablePrimary) {
        return {ErrorCodes::NotWritablePrimary,
                str::stream()
                    << connectionString.toString()
                    << " does not have a master. If this is a replica set, ensure that it has a"
                    << " healthy primary and that the set has been properly initiated."};
    }

    const std::string providedSetName = connectionString.getSetName();
    const std::string foundSetName = resHello["setName"].str();

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
                              << "is the replica set still initializing? " << resHello};
    }

    // Make sure the set name specified in the connection string matches the one where its hosts
    // belong into
    if (!providedSetName.empty() && (providedSetName != foundSetName)) {
        return {ErrorCodes::OperationFailed,
                str::stream() << "the provided connection string (" << connectionString.toString()
                              << ") does not match the actual set name " << foundSetName};
    }

    // Is it a config server?
    if (resHello.hasField("configsvr") && !isConfigShard) {
        return {ErrorCodes::OperationFailed,
                str::stream() << "Cannot add " << connectionString.toString()
                              << " as a shard since it is a config server"};
    }

    if (resHello.hasField(HelloCommandReply::kIsImplicitDefaultMajorityWCFieldName) &&
        !resHello.getBoolField(HelloCommandReply::kIsImplicitDefaultMajorityWCFieldName) &&
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

    if (resHello.hasField(HelloCommandReply::kCwwcFieldName)) {
        auto cwwcOnShard =
            WriteConcernOptions::parse(resHello.getObjectField(HelloCommandReply::kCwwcFieldName))
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
                return {ErrorCodes::OperationFailed,
                        str::stream() << "in seed list " << connectionString.toString() << ", host "
                                      << host << " does not belong to replica set " << foundSetName
                                      << "; found " << resHello.toString()};
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
    if (!isConfigShard && actualShardName == DatabaseName::kConfig.db(omitTenant)) {
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
        opCtx, targeter.get(), NamespaceString::kLogicalSessionsNamespace.dbName(), builder.done());
    if (!swCommandResponse.isOK()) {
        return swCommandResponse.getStatus();
    }

    auto cmdStatus = std::move(swCommandResponse.getValue().commandStatus);
    if (!cmdStatus.isOK() && cmdStatus.code() != ErrorCodes::NamespaceNotFound) {
        return cmdStatus;
    }

    return Status::OK();
}

StatusWith<std::vector<DatabaseName>> ShardingCatalogManager::_getDBNamesListFromShard(
    OperationContext* opCtx, std::shared_ptr<RemoteCommandTargeter> targeter) {

    auto swCommandResponse =
        _runCommandForAddShard(opCtx,
                               targeter.get(),
                               DatabaseName::kAdmin,
                               BSON("listDatabases" << 1 << "nameOnly" << true));
    if (!swCommandResponse.isOK()) {
        return swCommandResponse.getStatus();
    }

    auto cmdStatus = std::move(swCommandResponse.getValue().commandStatus);
    if (!cmdStatus.isOK()) {
        return cmdStatus;
    }

    auto cmdResult = std::move(swCommandResponse.getValue().response);

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

StatusWith<std::vector<CollectionType>> ShardingCatalogManager::_getCollListFromShard(
    OperationContext* opCtx,
    const std::vector<DatabaseName>& dbNames,
    std::shared_ptr<RemoteCommandTargeter> targeter) {
    std::vector<CollectionType> nssList;

    for (auto& dbName : dbNames) {
        Status fetchStatus =
            Status(ErrorCodes::InternalError, "Internal error running cursor callback in command");
        auto host = uassertStatusOK(
            targeter->findHost(opCtx, ReadPreferenceSetting{ReadPreference::PrimaryOnly}));
        const Milliseconds maxTimeMS =
            std::min(opCtx->getRemainingMaxTimeMillis(), Milliseconds(kRemoteCommandTimeout));

        auto fetcherCallback = [&](const Fetcher::QueryResponseStatus& dataStatus,
                                   Fetcher::NextAction* nextAction,
                                   BSONObjBuilder* getMoreBob) {
            // Throw out any accumulated results on error.
            if (!dataStatus.isOK()) {
                fetchStatus = dataStatus.getStatus();
                return;
            }
            const auto& data = dataStatus.getValue();

            try {
                for (const BSONObj& doc : data.documents) {
                    auto collInfo = ListCollectionsReplyItem::parse(
                        IDLParserContext("ListCollectionReply"), doc);
                    // Skip views and special collections.
                    if (!collInfo.getInfo() || !collInfo.getInfo()->getUuid()) {
                        continue;
                    }

                    const auto nss = NamespaceStringUtil::deserialize(dbName, collInfo.getName());

                    if (nss.isNamespaceAlwaysUntracked()) {
                        continue;
                    }

                    uassert(ErrorCodes::InvalidNamespace,
                            str::stream()
                                << "Namespace too long. Namespace: " << nss.toStringForErrorMsg()
                                << " Max: " << NamespaceString::MaxNsShardedCollectionLen,
                            nss.size() <= NamespaceString::MaxNsShardedCollectionLen);
                    auto coll = CollectionType(nss,
                                               OID::gen(),
                                               Timestamp(Date_t::now()),
                                               Date_t::now(),
                                               collInfo.getInfo()->getUuid().get(),
                                               sharding_ddl_util::unsplittableCollectionShardKey());
                    coll.setUnsplittable(true);
                    if (!doc["options"].eoo() && !doc["options"]["timeseries"].eoo()) {
                        coll.setTimeseriesFields(TypeCollectionTimeseriesFields::parse(
                            IDLParserContext("AddShardContext"),
                            doc["options"]["timeseries"].Obj()));
                    }
                    nssList.push_back(coll);
                }
                *nextAction = Fetcher::NextAction::kNoAction;
            } catch (DBException& ex) {
                fetchStatus = ex.toStatus();
                return;
            }
            fetchStatus = Status::OK();

            if (!getMoreBob) {
                return;
            }
            getMoreBob->append("getMore", data.cursorId);
            getMoreBob->append("collection", data.nss.coll());
        };
        ListCollections listCollections;
        listCollections.setDbName(dbName);
        auto fetcher =
            std::make_unique<Fetcher>(_executorForAddShard.get(),
                                      host,
                                      dbName,
                                      listCollections.toBSON({}),
                                      fetcherCallback,
                                      BSONObj() /* metadata tracking, only used for shards */,
                                      maxTimeMS /* command network timeout */,
                                      maxTimeMS /* getMore network timeout */);

        auto scheduleStatus = fetcher->schedule();
        if (!scheduleStatus.isOK()) {
            return scheduleStatus;
        }

        auto joinStatus = fetcher->join(opCtx);
        if (!joinStatus.isOK()) {
            return joinStatus;
        }
        if (!fetchStatus.isOK()) {
            return fetchStatus;
        }
    }

    return nssList;
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

Status ShardingCatalogManager::updateClusterCardinalityParameter(OperationContext* opCtx,
                                                                 int numShards) {
    ConfigsvrSetClusterParameter configsvrSetClusterParameter(BSON(
        "shardedClusterCardinalityForDirectConns"
        << BSON(ShardedClusterCardinalityParam::kHasTwoOrMoreShardsFieldName << (numShards >= 2))));
    configsvrSetClusterParameter.setDbName(DatabaseName::kAdmin);

    const auto shardRegistry = Grid::get(opCtx)->shardRegistry();
    const auto cmdResponse = shardRegistry->getConfigShard()->runCommandWithFixedRetryAttempts(
        opCtx,
        ReadPreferenceSetting(ReadPreference::PrimaryOnly),
        DatabaseName::kAdmin,
        configsvrSetClusterParameter.toBSON({}),
        Shard::RetryPolicy::kIdempotent);

    return Shard::CommandResponse::getEffectiveStatus(cmdResponse);
}

Status ShardingCatalogManager::_updateClusterCardinalityParameterAfterAddShardIfNeeded(
    const Lock::ExclusiveLock&, OperationContext* opCtx) {
    if (MONGO_unlikely(skipUpdatingClusterCardinalityParameterAfterAddShard.shouldFail())) {
        return Status::OK();
    }

    auto numShards = Grid::get(opCtx)->shardRegistry()->getNumShards(opCtx);
    if (numShards == 2) {
        // Only need to update the parameter when adding the second shard.
        return updateClusterCardinalityParameter(opCtx, numShards);
    }
    return Status::OK();
}

Status ShardingCatalogManager::_updateClusterCardinalityParameterAfterRemoveShardIfNeeded(
    const Lock::ExclusiveLock&, OperationContext* opCtx) {
    if (MONGO_unlikely(skipUpdatingClusterCardinalityParameterAfterRemoveShard.shouldFail())) {
        return Status::OK();
    }

    auto numShards = Grid::get(opCtx)->shardRegistry()->getNumShards(opCtx);
    if (numShards == 1) {
        // Only need to update the parameter when removing the second shard.
        return updateClusterCardinalityParameter(opCtx, numShards);
    }
    return Status::OK();
}

StatusWith<std::string> ShardingCatalogManager::addShard(
    OperationContext* opCtx,
    const std::string* shardProposedName,
    const ConnectionString& shardConnectionString,
    bool isConfigShard) {
    if (!shardConnectionString) {
        return {ErrorCodes::BadValue, "Invalid connection string"};
    }

    if (shardProposedName && shardProposedName->empty()) {
        return {ErrorCodes::BadValue, "shard name cannot be empty"};
    }

    const auto shardRegistry = Grid::get(opCtx)->shardRegistry();

    // Take the cluster cardinality parameter lock and the shard membership lock in exclusive mode
    // so that no add/remove shard operation and its set cluster cardinality parameter operation can
    // interleave with the ones below. Release the shard membership lock before initiating the
    // _configsvrSetClusterParameter command after finishing the add shard operation since setting a
    // cluster parameter requires taking this lock.
    Lock::ExclusiveLock clusterCardinalityParameterLock(opCtx, _kClusterCardinalityParameterLock);
    Lock::ExclusiveLock shardMembershipLock(opCtx, _kShardMembershipLock);

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

        // Release the shard membership lock since the set cluster parameter operation below
        // require taking this lock.
        shardMembershipLock.unlock();
        auto updateStatus = _updateClusterCardinalityParameterAfterAddShardIfNeeded(
            clusterCardinalityParameterLock, opCtx);
        if (!updateStatus.isOK()) {
            return updateStatus;
        }

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
        opCtx, targeter, shardProposedName, shardConnectionString, isConfigShard);
    if (!shardStatus.isOK()) {
        return shardStatus.getStatus();
    }
    ShardType& shardType = shardStatus.getValue();

    // TODO SERVER-80532: the sharding catalog might lose some databases.
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
                          str::stream()
                              << "can't add shard "
                              << "'" << shardConnectionString.toString() << "'"
                              << " because a local database '" << dbName.toStringForErrorMsg()
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

    if (!isConfigShard) {
        // If the shard is also the config server itself, there is no need to pull the keys since
        // the keys already exists in the local admin.system.keys collection.
        auto pullKeysStatus = _pullClusterTimeKeys(opCtx, targeter);
        if (!pullKeysStatus.isOK()) {
            return pullKeysStatus;
        }
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
            _runCommandForAddShard(opCtx, targeter.get(), DatabaseName::kAdmin, cmd);
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

    if (!isConfigShard) {
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

        const auto fcvSnapshot = (*fcvRegion).acquireFCVSnapshot();

        std::vector<CollectionType> collList;
        if (feature_flags::gTrackUnshardedCollectionsOnShardingCatalog.isEnabled(fcvSnapshot)) {
            // TODO SERVER-80532: the sharding catalog might lose some collections.
            auto listStatus = _getCollListFromShard(opCtx, dbNamesStatus.getValue(), targeter);
            if (!listStatus.isOK()) {
                return listStatus.getStatus();
            }

            collList = std::move(listStatus.getValue());
        }

        // (Generic FCV reference): These FCV checks should exist across LTS binary versions.
        uassert(5563603,
                "Cannot add shard while in upgrading/downgrading FCV state",
                !fcvSnapshot.isUpgradingOrDowngrading());

        const auto currentFCV = fcvSnapshot.getVersion();
        invariant(currentFCV == multiversion::GenericFCV::kLatest ||
                  currentFCV == multiversion::GenericFCV::kLastContinuous ||
                  currentFCV == multiversion::GenericFCV::kLastLTS);

        if (!isConfigShard) {
            SetFeatureCompatibilityVersion setFcvCmd(currentFCV);
            setFcvCmd.setDbName(DatabaseName::kAdmin);
            setFcvCmd.setFromConfigServer(true);

            auto versionResponse = _runCommandForAddShard(
                opCtx,
                targeter.get(),
                DatabaseName::kAdmin,
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
              "Going to insert new entry for shard into config.shards",
              "shardType"_attr = shardType.toString());

        _addShardInTransaction(
            opCtx, shardType, std::move(dbNamesStatus.getValue()), std::move(collList));

        // Record in changelog
        BSONObjBuilder shardDetails;
        shardDetails.append("name", shardType.getName());
        shardDetails.append("host", shardConnectionString.toString());

        ShardingLogging::get(opCtx)->logChange(opCtx,
                                               "addShard",
                                               NamespaceString::kEmpty,
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

        hangAddShardBeforeUpdatingClusterCardinalityParameter.pauseWhileSet(opCtx);
        // Release the shard membership lock since the set cluster parameter operation below
        // require taking this lock.
        shardMembershipLock.unlock();
        auto updateStatus = _updateClusterCardinalityParameterAfterAddShardIfNeeded(
            clusterCardinalityParameterLock, opCtx);
        if (!updateStatus.isOK()) {
            return updateStatus;
        }

        return shardType.getName();
    }
}

void ShardingCatalogManager::addConfigShard(OperationContext* opCtx) {
    // Set the operation context read concern level to local for reads into the config
    // database.
    repl::ReadConcernArgs::get(opCtx) =
        repl::ReadConcernArgs(repl::ReadConcernLevel::kLocalReadConcern);

    auto configConnString = repl::ReplicationCoordinator::get(opCtx)->getConfigConnectionString();

    auto shardingState = ShardingState::get(opCtx);
    uassert(7368500, "sharding state not enabled", shardingState->enabled());

    std::string shardName = shardingState->shardId().toString();
    uassertStatusOK(addShard(opCtx, &shardName, configConnString, true));
}

RemoveShardProgress ShardingCatalogManager::removeShard(OperationContext* opCtx,
                                                        const ShardId& shardId) {
    const auto name = shardId.toString();
    audit::logRemoveShard(opCtx->getClient(), name);

    Lock::ExclusiveLock shardMembershipLock(opCtx, _kShardMembershipLock);

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
        LOGV2(21945, "Going to start draining shard", "shardId"_attr = name);

        // Record start in changelog
        uassertStatusOK(
            ShardingLogging::get(opCtx)->logChangeChecked(opCtx,
                                                          "removeShard.start",
                                                          NamespaceString::kEmpty,
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

    shardMembershipLock.unlock();

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
              "removeShard: draining",
              "chunkCount"_attr = chunkCount,
              "databaseCount"_attr = databaseCount,
              "jumboCount"_attr = jumboCount);

        return {RemoveShardProgress::ONGOING,
                boost::optional<RemoveShardProgress::DrainingShardUsage>(
                    {chunkCount, databaseCount, jumboCount}),
                boost::none};
    }

    if (shardId == ShardId::kConfigServerId) {
        // The config server may be added as a shard again, so we locally drop its drained
        // sharded collections to enable that without user intervention. But we have to wait for
        // the range deleter to quiesce to give queries and stale routers time to discover the
        // migration, to match the usual probabilistic guarantees for migrations.
        auto pendingRangeDeletions = [opCtx]() {
            PersistentTaskStore<RangeDeletionTask> store(NamespaceString::kRangeDeletionNamespace);
            return static_cast<long long>(store.count(opCtx, BSONObj()));
        }();
        if (pendingRangeDeletions > 0) {
            LOGV2(7564600,
                  "removeShard: waiting for range deletions",
                  "pendingRangeDeletions"_attr = pendingRangeDeletions);

            return {
                RemoveShardProgress::PENDING_RANGE_DELETIONS, boost::none, pendingRangeDeletions};
        }

        // Drop all tracked databases locally now that all user data has been drained so the config
        // server can transition back to catalog shard mode without requiring users to manually drop
        // them.
        LOGV2(7509600, "Locally dropping drained databases", "shardId"_attr = name);

        auto trackedDBs =
            _localCatalogClient->getAllDBs(opCtx, repl::ReadConcernLevel::kLocalReadConcern);
        for (auto&& db : trackedDBs) {
            tassert(7783700,
                    "Cannot drop admin or config database from the config server",
                    !db.getDbName().isConfigDB() && !db.getDbName().isAdminDB());

            DBDirectClient client(opCtx);
            BSONObj result;
            if (!client.dropDatabase(
                    db.getDbName(), ShardingCatalogClient::kLocalWriteConcern, &result)) {
                uassertStatusOK(getStatusFromCommandResult(result));
            }

            hangAfterDroppingDatabaseInTransitionToDedicatedConfigServer.pauseWhileSet(opCtx);
        }

        // Also drop the sessions collection, which we assume is the only sharded collection in the
        // config database.
        DBDirectClient client(opCtx);
        BSONObj result;
        if (!client.dropCollection(NamespaceString::kLogicalSessionsNamespace,
                                   ShardingCatalogClient::kLocalWriteConcern,
                                   &result)) {
            uassertStatusOK(getStatusFromCommandResult(result));
        }
    }

    // Draining is done, now finish removing the shard.
    LOGV2(21949, "Going to remove shard", "shardId"_attr = name);

    // Take the cluster cardinality parameter lock and the shard membership lock in exclusive mode
    // so that no add/remove shard operation and its set cluster cardinality parameter operation can
    // interleave with the ones below. Release the shard membership lock before initiating the
    // _configsvrSetClusterParameter command after finishing the remove shard operation since
    // setting a cluster parameter requires taking this lock.
    Lock::ExclusiveLock clusterCardinalityParameterLock(opCtx, _kClusterCardinalityParameterLock);
    // Synchronize the control shard selection, the shard's document removal, and the topology time
    // update to exclude potential race conditions in case of concurrent add/remove shard
    // operations.
    shardMembershipLock.lock();

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

    shardMembershipLock.unlock();

    // The shard which was just removed must be reflected in the shard registry, before the replica
    // set monitor is removed, otherwise the shard would be referencing a dropped RSM.
    Grid::get(opCtx)->shardRegistry()->reload(opCtx);

    if (shardId != ShardId::kConfigServerId) {
        // Don't remove the config shard's RSM because it is used to target the config server.
        ReplicaSetMonitor::remove(name);
    }

    // Record finish in changelog
    ShardingLogging::get(opCtx)->logChange(opCtx,
                                           "removeShard",
                                           NamespaceString::kEmpty,
                                           BSON("shard" << name),
                                           ShardingCatalogClient::kLocalWriteConcern,
                                           _localConfigShard,
                                           _localCatalogClient.get());

    uassertStatusOK(_updateClusterCardinalityParameterAfterRemoveShardIfNeeded(
        clusterCardinalityParameterLock, opCtx));

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
        for (const auto& dbName : databases) {
            if (!dbName.isLocalDB()) {
                dbs.append(
                    DatabaseNameUtil::serialize(dbName, SerializationContext::stateDefault()));
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
        case RemoveShardProgress::PENDING_RANGE_DELETIONS: {
            result.append("msg", "waiting for pending range deletions");
            result.append("state", "pendingRangeDeletions");
            result.append("pendingRangeDeletions", *shardDrainingStatus.pendingRangeDeletions);
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

    auto resultStatus = _localConfigShard->runCommandWithFixedRetryAttempts(
        opCtx,
        kConfigReadSelector,
        nss.dbName(),
        countBuilder.done(),
        Milliseconds(defaultConfigCommandTimeoutMS.load()),
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
                                   NamespaceString::kUserWritesCriticalSectionsNamespace.dbName(),
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
                _runCommandForAddShard(opCtx, targeter, DatabaseName::kAdmin, cmd);
            uassertStatusOK(Shard::CommandResponse::getEffectiveStatus(cmdResponse));
        }

        if (doc.getBlockUserWrites()) {
            invariant(doc.getBlockNewUserShardedDDL());
            const auto cmd = makeShardsvrSetUserWriteBlockModeCommand(
                ShardsvrSetUserWriteBlockModePhaseEnum::kComplete);

            const auto cmdResponse =
                _runCommandForAddShard(opCtx, targeter, DatabaseName::kAdmin, cmd);
            uassertStatusOK(Shard::CommandResponse::getEffectiveStatus(cmdResponse));
        }

        return true;
    });
}

std::unique_ptr<Fetcher> ShardingCatalogManager::_createFetcher(
    OperationContext* opCtx,
    std::shared_ptr<RemoteCommandTargeter> targeter,
    const NamespaceString& nss,
    const repl::ReadConcernLevel& readConcernLevel,
    FetcherDocsCallbackFn processDocsCallback,
    FetcherStatusCallbackFn processStatusCallback) {
    auto host = uassertStatusOK(
        targeter->findHost(opCtx, ReadPreferenceSetting{ReadPreference::PrimaryOnly}));

    FindCommandRequest findCommand(nss);
    const auto readConcern =
        repl::ReadConcernArgs(boost::optional<repl::ReadConcernLevel>(readConcernLevel));
    findCommand.setReadConcern(readConcern.toBSONInner());
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

    return std::make_unique<Fetcher>(_executorForAddShard.get(),
                                     host,
                                     nss.dbName(),
                                     findCommand.toBSON({}),
                                     fetcherCallback,
                                     BSONObj(), /* metadata tracking, only used for shards */
                                     maxTimeMS, /* command network timeout */
                                     maxTimeMS /* getMore network timeout */);
}

Status ShardingCatalogManager::_pullClusterTimeKeys(
    OperationContext* opCtx, std::shared_ptr<RemoteCommandTargeter> targeter) {
    Status fetchStatus =
        Status(ErrorCodes::InternalError, "Internal error running cursor callback in command");
    std::vector<ExternalKeysCollectionDocument> keyDocs;

    auto expireAt = opCtx->getServiceContext()->getFastClockSource()->now() +
        Seconds(gNewShardExistingClusterTimeKeysExpirationSecs.load());
    auto fetcher = _createFetcher(
        opCtx,
        targeter,
        NamespaceString::kKeysCollectionNamespace,
        repl::ReadConcernLevel::kLocalReadConcern,
        [&](const std::vector<BSONObj>& docs) -> bool {
            for (const BSONObj& doc : docs) {
                keyDocs.push_back(keys_collection_util::makeExternalClusterTimeKeyDoc(
                    doc.getOwned(), boost::none /* migrationId */, expireAt));
            }
            return true;
        },
        [&](const Status& status) { fetchStatus = status; });

    auto scheduleStatus = fetcher->schedule();
    if (!scheduleStatus.isOK()) {
        return scheduleStatus;
    }

    auto joinStatus = fetcher->join(opCtx);
    if (!joinStatus.isOK()) {
        return joinStatus;
    }

    if (keyDocs.empty()) {
        return fetchStatus;
    }

    auto opTime = keys_collection_util::storeExternalClusterTimeKeyDocs(opCtx, std::move(keyDocs));
    auto waitStatus = WaitForMajorityService::get(opCtx->getServiceContext())
                          .waitUntilMajorityForWrite(
                              opCtx->getServiceContext(), opTime, opCtx->getCancellationToken())
                          .getNoThrow();
    if (!waitStatus.isOK()) {
        return waitStatus;
    }

    return fetchStatus;
}

void ShardingCatalogManager::_setClusterParametersLocally(OperationContext* opCtx,
                                                          const std::vector<BSONObj>& parameters) {
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

    for (auto& parameter : parameters) {
        SetClusterParameter setClusterParameterRequest(
            BSON(parameter["_id"].String() << parameter.filterFieldsUndotted(
                     BSON("_id" << 1 << "clusterParameterTime" << 1), false)));
        setClusterParameterRequest.setDbName(DatabaseNameUtil::deserialize(
            tenantId, DatabaseName::kAdmin.db(omitTenant), SerializationContext::stateDefault()));
        std::unique_ptr<ServerParameterService> parameterService =
            std::make_unique<ClusterParameterService>();
        SetClusterParameterInvocation invocation{std::move(parameterService), dbService};
        invocation.invoke(opCtx,
                          setClusterParameterRequest,
                          parameter["clusterParameterTime"].timestamp(),
                          boost::none /* previousTime */,
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
    auto tenantIds =
        uassertStatusOK(getTenantsWithConfigDbsOnShard(opCtx, shard, _executorForAddShard.get()));

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
        auto fetcher = _createFetcher(
            opCtx,
            targeter,
            NamespaceString::makeClusterParametersNSS(tenantId),
            repl::ReadConcernLevel::kMajorityReadConcern,
            [&allParameters, i](const std::vector<BSONObj>& docs) -> bool {
                std::vector<BSONObj> parameters;
                parameters.reserve(docs.size());
                for (const BSONObj& doc : docs) {
                    parameters.push_back(doc.getOwned());
                }
                allParameters[i] = parameters;
                return true;
            },
            [&statuses, i](const Status& status) { statuses[i] = status; });
        uassertStatusOK(fetcher->schedule());
        fetchers.push_back(std::move(fetcher));
        i++;
    }

    i = 0;
    for (const auto& tenantId : tenantIds) {
        uassertStatusOK(fetchers[i]->join(opCtx));
        uassertStatusOK(statuses[i]);

        auth::ValidatedTenancyScopeGuard::runAsTenant(opCtx, tenantId, [&]() -> void {
            _setClusterParametersLocally(opCtx, allParameters[i]);
        });
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
        const auto& nss = NamespaceString::makeClusterParametersNSS(tenantId);
        write_ops::DeleteCommandRequest deleteOp(nss);
        write_ops::DeleteOpEntry query({}, true /*multi*/);
        deleteOp.setDeletes({query});

        const auto swCommandResponse =
            _runCommandForAddShard(opCtx,
                                   targeter.get(),
                                   nss.dbName(),
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

            const auto cmdResponse = _runCommandForAddShard(
                opCtx,
                targeter.get(),
                dbName,
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
    std::vector<DatabaseName>&& databasesInNewShard,
    std::vector<CollectionType>&& collectionsInNewShard) {

    const auto existingShardIds = Grid::get(opCtx)->shardRegistry()->getAllShardIds(opCtx);

    // 1. Send out the "prepareCommit" notification
    std::vector<DatabaseName> importedDbNames;
    std::transform(databasesInNewShard.begin(),
                   databasesInNewShard.end(),
                   std::back_inserter(importedDbNames),
                   [](const DatabaseName& dbName) { return dbName; });
    DatabasesAdded notification(
        std::move(importedDbNames), true /*addImported*/, CommitPhaseEnum::kPrepare);
    notification.setPrimaryShard(ShardId(newShard.getName()));
    uassertStatusOK(_notifyClusterOnNewDatabases(opCtx, notification, existingShardIds));

    const auto collCreationTime = [&]() {
        const auto currentTime = VectorClock::get(opCtx)->getTime();
        return currentTime.clusterTime().asTimestamp();
    }();
    for (auto& coll : collectionsInNewShard) {
        coll.setTimestamp(collCreationTime);
    }

    // 2. Set up and run the commit statements
    // TODO SERVER-66261 newShard may be passed by reference.
    // TODO SERVER-81582: generate batches of transactions to insert the database/placementHistory
    // and collection/placementHistory before adding the shard in config.shards.
    auto transactionChain = [opCtx,
                             newShard,
                             dbNames = std::move(databasesInNewShard),
                             nssList = std::move(collectionsInNewShard)](
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
                if (nssList.empty()) {
                    BatchedCommandResponse noOpResponse;
                    noOpResponse.setStatus(Status::OK());
                    noOpResponse.setN(0);

                    return SemiFuture<BatchedCommandResponse>(std::move(noOpResponse));
                }
                std::vector<BSONObj> collEntries;

                std::transform(nssList.begin(),
                               nssList.end(),
                               std::back_inserter(collEntries),
                               [&](const CollectionType& coll) { return coll.toBSON(); });
                write_ops::InsertCommandRequest insertCollectionEntries(
                    NamespaceString::kConfigsvrCollectionsNamespace, std::move(collEntries));
                return txnClient.runCRUDOp(insertCollectionEntries, {});
            })
            .thenRunOn(txnExec)
            .then([&](const BatchedCommandResponse& insertCollectionEntriesResponse) {
                uassertStatusOK(insertCollectionEntriesResponse.toStatus());
                if (nssList.empty()) {
                    BatchedCommandResponse noOpResponse;
                    noOpResponse.setStatus(Status::OK());
                    noOpResponse.setN(0);

                    return SemiFuture<BatchedCommandResponse>(std::move(noOpResponse));
                }
                std::vector<BSONObj> chunkEntries;
                const auto unsplittableShardKey =
                    ShardKeyPattern(sharding_ddl_util::unsplittableCollectionShardKey());
                const auto shardId = ShardId(newShard.getName());
                std::transform(
                    nssList.begin(),
                    nssList.end(),
                    std::back_inserter(chunkEntries),
                    [&](const CollectionType& coll) {
                        // Create a single chunk for this
                        ChunkType chunk(
                            coll.getUuid(),
                            {coll.getKeyPattern().globalMin(), coll.getKeyPattern().globalMax()},
                            {{coll.getEpoch(), coll.getTimestamp()}, {1, 0}},
                            shardId);
                        chunk.setOnCurrentShardSince(coll.getTimestamp());
                        chunk.setHistory({ChunkHistory(*chunk.getOnCurrentShardSince(), shardId)});
                        return chunk.toConfigBSON();
                    });

                write_ops::InsertCommandRequest insertChunkEntries(
                    NamespaceString::kConfigsvrChunksNamespace, std::move(chunkEntries));
                return txnClient.runCRUDOp(insertChunkEntries, {});
            })
            .thenRunOn(txnExec)
            .then([&](const BatchedCommandResponse& insertChunkEntriesResponse) {
                uassertStatusOK(insertChunkEntriesResponse.toStatus());
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
                               [&](const DatabaseName& dbName) {
                                   return NamespacePlacementType(NamespaceString(dbName),
                                                                 newShard.getTopologyTime(),
                                                                 {ShardId(newShard.getName())})
                                       .toBSON();
                               });
                std::transform(nssList.begin(),
                               nssList.end(),
                               std::back_inserter(placementEntries),
                               [&](const CollectionType& coll) {
                                   NamespacePlacementType placementInfo(
                                       NamespaceString(coll.getNss()),
                                       coll.getTimestamp(),
                                       {ShardId(newShard.getName())});
                                   placementInfo.setUuid(coll.getUuid());

                                   return placementInfo.toBSON();
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
        auto& executor = Grid::get(opCtx)->getExecutorPool()->getFixedExecutor();
        auto inlineExecutor = std::make_shared<executor::InlineExecutor>();

        txn_api::SyncTransactionWithRetries txn(opCtx, executor, nullptr, inlineExecutor);
        txn.run(opCtx, transactionChain);
    }

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

    auto& executor = Grid::get(opCtx)->getExecutorPool()->getFixedExecutor();
    auto inlineExecutor = std::make_shared<executor::InlineExecutor>();

    txn_api::SyncTransactionWithRetries txn(opCtx, executor, nullptr, inlineExecutor);

    txn.run(opCtx, removeShardFn);
}

}  // namespace mongo
