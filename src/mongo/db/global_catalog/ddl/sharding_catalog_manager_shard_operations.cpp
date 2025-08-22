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
#include "mongo/bson/json.h"
#include "mongo/bson/timestamp.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/client/connection_string.h"
#include "mongo/client/fetcher.h"
#include "mongo/client/read_preference.h"
#include "mongo/client/remote_command_targeter.h"
#include "mongo/client/replica_set_monitor.h"
#include "mongo/db/audit.h"
#include "mongo/db/cluster_parameters/cluster_server_parameter_cmds_gen.h"
#include "mongo/db/cluster_parameters/cluster_server_parameter_common.h"
#include "mongo/db/cluster_parameters/set_cluster_parameter_invocation.h"
#include "mongo/db/cluster_parameters/sharding_cluster_parameters_gen.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/feature_compatibility_version.h"
#include "mongo/db/commands/set_feature_compatibility_version_gen.h"
#include "mongo/db/database_name.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/feature_flag.h"
#include "mongo/db/generic_argument_util.h"
#include "mongo/db/global_catalog/ddl/ddl_lock_manager.h"
#include "mongo/db/global_catalog/ddl/notify_sharding_event_gen.h"
#include "mongo/db/global_catalog/ddl/sharded_ddl_commands_gen.h"
#include "mongo/db/global_catalog/ddl/sharding_catalog_manager.h"
#include "mongo/db/global_catalog/ddl/sharding_ddl_util.h"
#include "mongo/db/global_catalog/ddl/sharding_util.h"
#include "mongo/db/global_catalog/ddl/shardsvr_join_migrations_request_gen.h"
#include "mongo/db/global_catalog/sharding_catalog_client.h"
#include "mongo/db/global_catalog/type_chunk.h"
#include "mongo/db/global_catalog/type_database_gen.h"
#include "mongo/db/global_catalog/type_namespace_placement_gen.h"
#include "mongo/db/global_catalog/type_remove_shard_event_gen.h"
#include "mongo/db/global_catalog/type_shard.h"
#include "mongo/db/keys_collection_document_gen.h"
#include "mongo/db/keys_collection_util.h"
#include "mongo/db/local_catalog/catalog_raii.h"
#include "mongo/db/local_catalog/ddl/list_collections_gen.h"
#include "mongo/db/local_catalog/drop_database.h"
#include "mongo/db/local_catalog/lock_manager/d_concurrency.h"
#include "mongo/db/local_catalog/shard_role_api/resource_yielder.h"
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
#include "mongo/db/s/replica_set_endpoint_feature_flag.h"
#include "mongo/db/server_options.h"
#include "mongo/db/server_parameter.h"
#include "mongo/db/service_context.h"
#include "mongo/db/session/logical_session_id.h"
#include "mongo/db/sharding_environment/client/shard.h"
#include "mongo/db/sharding_environment/grid.h"
#include "mongo/db/sharding_environment/shard_id.h"
#include "mongo/db/sharding_environment/sharding_config_server_parameters_gen.h"
#include "mongo/db/sharding_environment/sharding_feature_flags_gen.h"
#include "mongo/db/sharding_environment/sharding_logging.h"
#include "mongo/db/tenant_id.h"
#include "mongo/db/topology/add_shard_gen.h"
#include "mongo/db/topology/remove_shard_draining_progress_gen.h"
#include "mongo/db/topology/shard_registry.h"
#include "mongo/db/topology/sharding_state.h"
#include "mongo/db/topology/topology_change_helpers.h"
#include "mongo/db/transaction/transaction_api.h"
#include "mongo/db/user_write_block/set_user_write_block_mode_gen.h"
#include "mongo/db/user_write_block/user_writes_critical_section_document_gen.h"
#include "mongo/db/user_write_block/user_writes_recoverable_critical_section_service.h"
#include "mongo/db/vector_clock/vector_clock_mutable.h"
#include "mongo/db/versioning_protocol/database_version.h"
#include "mongo/db/write_concern_options.h"
#include "mongo/executor/connection_pool_stats.h"
#include "mongo/executor/inline_executor.h"
#include "mongo/executor/remote_command_request.h"
#include "mongo/executor/remote_command_response.h"
#include "mongo/executor/task_executor.h"
#include "mongo/executor/task_executor_pool.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/rpc/metadata.h"
#include "mongo/s/write_ops/batched_command_response.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/clock_source.h"
#include "mongo/util/database_name_util.h"
#include "mongo/util/decorable.h"
#include "mongo/util/duration.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/future.h"
#include "mongo/util/future_impl.h"
#include "mongo/util/namespace_string_util.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/out_of_line_executor.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/str.h"
#include "mongo/util/time_support.h"
#include "mongo/util/uuid.h"
#include "mongo/util/version/releases.h"

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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding


namespace mongo {
namespace {

MONGO_FAIL_POINT_DEFINE(hangRemoveShardAfterSettingDrainingFlag);
MONGO_FAIL_POINT_DEFINE(hangRemoveShardAfterDrainingDDL);
MONGO_FAIL_POINT_DEFINE(hangRemoveShardBeforeUpdatingClusterCardinalityParameter);
MONGO_FAIL_POINT_DEFINE(skipUpdatingClusterCardinalityParameterAfterAddShard);
MONGO_FAIL_POINT_DEFINE(skipUpdatingClusterCardinalityParameterAfterRemoveShard);
MONGO_FAIL_POINT_DEFINE(changeBSONObjMaxUserSize);

using CallbackHandle = executor::TaskExecutor::CallbackHandle;
using CallbackArgs = executor::TaskExecutor::CallbackArgs;
using RemoteCommandCallbackArgs = executor::TaskExecutor::RemoteCommandCallbackArgs;
using RemoteCommandCallbackFn = executor::TaskExecutor::RemoteCommandCallbackFn;

const ReadPreferenceSetting kConfigReadSelector(ReadPreference::Nearest, TagSet{});
const WriteConcernOptions kMajorityWriteConcern{WriteConcernOptions::kMajority,
                                                WriteConcernOptions::SyncMode::UNSET,
                                                WriteConcernOptions::kNoTimeout};

const Seconds kRemoteCommandTimeout{60};

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

std::vector<NamespaceString> getCollectionsToMoveForShard(OperationContext* opCtx,
                                                          Shard* shard,
                                                          const ShardId& shardId) {

    auto listCollectionAggReq = makeUnshardedCollectionsOnSpecificShardAggregation(opCtx, shardId);

    std::vector<NamespaceString> collections;

    uassertStatusOK(
        shard->runAggregation(opCtx,
                              listCollectionAggReq,
                              [&collections](const std::vector<BSONObj>& batch,
                                             const boost::optional<BSONObj>& postBatchResumeToken) {
                                  for (const auto& doc : batch) {
                                      collections.push_back(NamespaceStringUtil::deserialize(
                                          boost::none,
                                          doc.getField("ns").String(),
                                          SerializationContext::stateDefault()));
                                  }
                                  return true;
                              }));

    return collections;
}

bool appendToArrayIfRoom(int offset,
                         BSONArrayBuilder& arrayBuilder,
                         const std::string& toAppend,
                         const int maxUserSize) {
    if (static_cast<int>(offset + arrayBuilder.len() + toAppend.length()) < maxUserSize) {
        arrayBuilder.append(toAppend);
        return true;
    }
    return false;
}

}  // namespace

StatusWith<Shard::CommandResponse> ShardingCatalogManager::_runCommandForAddShard(
    OperationContext* opCtx,
    RemoteCommandTargeter* targeter,
    const DatabaseName& dbName,
    const BSONObj& cmdObj) {
    try {
        return topology_change_helpers::runCommandForAddShard(
            opCtx, *targeter, dbName, cmdObj, _executorForAddShard);
    } catch (const DBException& ex) {
        return ex.toStatus();
    }
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
    try {
        return topology_change_helpers::getDBNamesListFromReplicaSet(
            opCtx, *targeter, _executorForAddShard);
    } catch (DBException& ex) {
        return ex.toStatus();
    }
}

void ShardingCatalogManager::installConfigShardIdentityDocument(OperationContext* opCtx,
                                                                bool deferShardingInitialization) {
    invariant(!ShardingState::get(opCtx)->enabled());
    auto identity = topology_change_helpers::createShardIdentity(opCtx, ShardId::kConfigServerId);
    if (deferShardingInitialization) {
        identity.setDeferShardingInitialization(true);
    }
    topology_change_helpers::installShardIdentity(opCtx, identity);
}

Status ShardingCatalogManager::_updateClusterCardinalityParameterAfterAddShardIfNeeded(
    const Lock::ExclusiveLock& clusterCardinalityParameterLock, OperationContext* opCtx) {
    if (MONGO_unlikely(skipUpdatingClusterCardinalityParameterAfterAddShard.shouldFail())) {
        return Status::OK();
    }

    auto numShards = Grid::get(opCtx)->shardRegistry()->getNumShards(opCtx);
    if (numShards == 2) {
        // Only need to update the parameter when adding the second shard.
        try {
            topology_change_helpers::updateClusterCardinalityParameter(
                clusterCardinalityParameterLock, opCtx);
            return Status::OK();
        } catch (const DBException& ex) {
            return ex.toStatus();
        }
    }
    return Status::OK();
}

Status ShardingCatalogManager::_updateClusterCardinalityParameterAfterRemoveShardIfNeeded(
    const Lock::ExclusiveLock& clusterCardinalityParameterLock, OperationContext* opCtx) {
    if (MONGO_unlikely(skipUpdatingClusterCardinalityParameterAfterRemoveShard.shouldFail())) {
        return Status::OK();
    }

    // If the replica set endpoint is not active, then it isn't safe to allow direct connections
    // again after a second shard has been added. Unsharded collections are allowed to be tracked
    // and moved as soon as a second shard is added to the cluster, and these collections will not
    // handle direct connections properly.
    if (!replica_set_endpoint::isFeatureFlagEnabled(VersionContext::getDecoration(opCtx))) {
        return Status::OK();
    }

    auto numShards = Grid::get(opCtx)->shardRegistry()->getNumShards(opCtx);
    if (numShards == 1) {
        // Only need to update the parameter when removing the second shard.
        try {
            topology_change_helpers::updateClusterCardinalityParameter(
                clusterCardinalityParameterLock, opCtx);
            return Status::OK();
        } catch (const DBException& ex) {
            return ex.toStatus();
        }
    }
    return Status::OK();
}

Status ShardingCatalogManager::updateClusterCardinalityParameterIfNeeded(OperationContext* opCtx) {
    auto clusterCardinalityParameterLock =
        acquireClusterCardinalityParameterLockForTopologyChange(opCtx);

    auto shardRegistry = Grid::get(opCtx)->shardRegistry();
    shardRegistry->reload(opCtx);

    auto numShards = shardRegistry->getNumShards(opCtx);
    if (numShards <= 2) {
        // Only need to update the parameter when adding or removing the second shard.
        try {
            topology_change_helpers::updateClusterCardinalityParameter(
                clusterCardinalityParameterLock, opCtx);
            return Status::OK();
        } catch (const DBException& ex) {
            return ex.toStatus();
        }
    }
    return Status::OK();
}

StatusWith<std::string> ShardingCatalogManager::addShard(
    OperationContext* opCtx,
    const FixedFCVRegion& fcvRegion,
    const std::string* shardProposedName,
    const ConnectionString& shardConnectionString,
    bool isConfigShard) {
    static Lock::ResourceMutex _kAddShardLock("addShardLock");

    auto addShardLock = Lock::ExclusiveLock(opCtx, _kAddShardLock);

    if (!shardConnectionString) {
        return {ErrorCodes::BadValue, "Invalid connection string"};
    }

    if (shardProposedName && shardProposedName->empty()) {
        return {ErrorCodes::BadValue, "shard name cannot be empty"};
    }

    const auto shardRegistry = Grid::get(opCtx)->shardRegistry();

    // Unset the addOrRemoveShardInProgress cluster parameter in case it was left set by a previous
    // failed addShard/removeShard operation.
    topology_change_helpers::resetDDLBlockingForTopologyChangeIfNeeded(opCtx);

    // Take the cluster cardinality parameter lock and the shard membership lock in exclusive mode
    // so that no add/remove shard operation and its set cluster cardinality parameter operation can
    // interleave with the ones below. Release the shard membership lock before initiating the
    // _configsvrSetClusterParameter command after finishing the add shard operation since setting a
    // cluster parameter requires taking this lock.
    auto clusterCardinalityParameterLock =
        acquireClusterCardinalityParameterLockForTopologyChange(opCtx);
    auto shardMembershipLock = acquireShardMembershipLockForTopologyChange(opCtx);

    // Check if this shard has already been added (can happen in the case of a retry after a network
    // error, for example) and thus this addShard request should be considered a no-op.
    const auto existingShard = std::invoke([&]() -> StatusWith<boost::optional<ShardType>> {
        try {
            return topology_change_helpers::getExistingShard(
                opCtx,
                shardConnectionString,
                shardProposedName ? boost::optional<StringData>(*shardProposedName) : boost::none,
                *_localCatalogClient);
        } catch (const DBException& ex) {
            return ex.toStatus();
        }
    });
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

    shardMembershipLock.unlock();
    clusterCardinalityParameterLock.unlock();

    const std::shared_ptr<Shard> shard{shardRegistry->createConnection(shardConnectionString)};
    auto targeter = shard->getTargeter();

    ScopeGuard stopMonitoringGuard(
        [&] { topology_change_helpers::removeReplicaSetMonitor(opCtx, shardConnectionString); });

    // Validate the specified connection string may serve as shard at all
    try {
        topology_change_helpers::validateHostAsShard(
            opCtx, *targeter, shardConnectionString, isConfigShard, _executorForAddShard);
    } catch (DBException& ex) {
        return ex.toStatus();
    }

    std::string shardName;
    try {
        shardName = topology_change_helpers::createShardName(
            opCtx,
            *targeter,
            isConfigShard,
            shardProposedName ? boost::optional<StringData>(*shardProposedName) : boost::none,
            _executorForAddShard);
    } catch (const DBException& ex) {
        return ex.toStatus();
    }

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

    // Check that the shard candidate does not have a local config.system.sessions collection. We do
    // not want to drop this once featureFlagSessionsCollectionCoordinatorOnConfigServer is enabled
    // but we do not have stability yet. We optimistically do not drop it here and then double check
    // later under the fixed FCV region.
    if (!isConfigShard) {
        auto res = _dropSessionsCollection(opCtx, targeter);
        if (!res.isOK()) {
            return res.withContext(
                "can't add shard with a local copy of config.system.sessions, please drop this "
                "collection from the shard manually and try again.");
        }

        // If the shard is also the config server itself, there is no need to pull the keys since
        // the keys already exists in the local admin.system.keys collection.
        try {
            topology_change_helpers::getClusterTimeKeysFromReplicaSet(
                opCtx, *targeter, _executorForAddShard);
        } catch (const DBException& ex) {
            return ex.toStatus();
        }

        try {
            const auto shardIdentity =
                topology_change_helpers::createShardIdentity(opCtx, shardName);
            topology_change_helpers::installShardIdentity(
                opCtx, shardIdentity, *targeter, boost::none, boost::none, _executorForAddShard);
        } catch (const DBException& ex) {
            return ex.toStatus();
        }

        // Set the user-writes blocking state on the new shard.
        topology_change_helpers::propagateClusterUserWriteBlockToReplicaSet(
            opCtx, *targeter, _executorForAddShard);

        // Determine the set of cluster parameters to be used.
        _standardizeClusterParameters(opCtx, *targeter);
    }

    const auto fcvSnapshot = fcvRegion->acquireFCVSnapshot();

    // (Generic FCV reference): These FCV checks should exist across LTS binary versions.
    const auto currentFCV = fcvSnapshot.getVersion();
    invariant(currentFCV == multiversion::GenericFCV::kLatest ||
              currentFCV == multiversion::GenericFCV::kLastContinuous ||
              currentFCV == multiversion::GenericFCV::kLastLTS);

    if (isConfigShard &&
        !feature_flags::gSessionsCollectionCoordinatorOnConfigServer.isEnabled(
            VersionContext::getDecoration(opCtx), fcvSnapshot)) {
        auto res = _dropSessionsCollection(opCtx, targeter);
        if (!res.isOK()) {
            return res.withContext(
                "can't add shard with a local copy of config.system.sessions, please drop this "
                "collection from the shard manually and try again.");
        }
    }

    if (!isConfigShard) {
        SetFeatureCompatibilityVersion setFcvCmd(currentFCV);
        setFcvCmd.setDbName(DatabaseName::kAdmin);
        setFcvCmd.setFromConfigServer(true);
        setFcvCmd.setWriteConcern(opCtx->getWriteConcern());

        auto versionResponse =
            _runCommandForAddShard(opCtx, targeter.get(), DatabaseName::kAdmin, setFcvCmd.toBSON());
        if (!versionResponse.isOK()) {
            return versionResponse.getStatus();
        }

        if (!versionResponse.getValue().commandStatus.isOK()) {
            return versionResponse.getValue().commandStatus;
        }
    }

    // Block new ShardingDDLCoordinators on the cluster and join ongoing ones.
    ScopeGuard unblockDDLCoordinatorsGuard([&] { scheduleAsyncUnblockDDLCoordinators(opCtx); });
    topology_change_helpers::blockDDLCoordinatorsAndDrain(opCtx);

    // Tick clusterTime to get a new topologyTime for this mutation of the topology.
    auto newTopologyTime = VectorClockMutable::get(opCtx)->tickClusterTime(1);

    ShardType shardType;
    shardType.setName(shardName);
    shardType.setHost(targeter->connectionString().toString());
    shardType.setState(ShardType::ShardState::kShardAware);
    shardType.setTopologyTime(newTopologyTime.asTimestamp());

    LOGV2(21942,
          "Going to insert new entry for shard into config.shards",
          "shardType"_attr = shardType.toString());

    clusterCardinalityParameterLock.lock();
    shardMembershipLock.lock();

    {
        // Execute the transaction with a local write concern to make sure `stopMonitorGuard` is
        // dimissed only when the transaction really fails.
        const auto originalWC = opCtx->getWriteConcern();
        ScopeGuard resetWCGuard([&] { opCtx->setWriteConcern(originalWC); });
        opCtx->setWriteConcern(ShardingCatalogClient::writeConcernLocalHavingUpstreamWaiter());

        auto& executor = Grid::get(opCtx)->getExecutorPool()->getFixedExecutor();
        topology_change_helpers::addShardInTransaction(
            opCtx, shardType, std::move(dbNamesStatus.getValue()), executor);
    }
    // Once the transaction has committed, we must immediately dismiss the guard to avoid
    // incorrectly removing the RSM after persisting the shard addition.
    stopMonitoringGuard.dismiss();

    // Wait for majority can only be done after dismissing the `stopMonitoringGuard`.
    if (opCtx->getWriteConcern().isMajority()) {
        const auto majorityWriteStatus =
            WaitForMajorityService::get(opCtx->getServiceContext())
                .waitUntilMajorityForWrite(
                    repl::ReplicationCoordinator::get(opCtx->getServiceContext())
                        ->getMyLastAppliedOpTime(),
                    opCtx->getCancellationToken())
                .getNoThrow();

        if (majorityWriteStatus == ErrorCodes::CallbackCanceled) {
            uassertStatusOK(opCtx->checkForInterruptNoAssert());
        }
        uassertStatusOK(majorityWriteStatus);
    }

    // Record in changelog
    BSONObjBuilder shardDetails;
    shardDetails.append("name", shardType.getName());
    shardDetails.append("host", shardConnectionString.toString());

    ShardingLogging::get(opCtx)->logChange(opCtx,
                                           "addShard",
                                           NamespaceString::kEmpty,
                                           shardDetails.obj(),
                                           defaultMajorityWriteConcernDoNotUse(),
                                           _localConfigShard,
                                           _localCatalogClient.get());

    // Ensure the added shard is visible to this process.
    shardRegistry->reload(opCtx);
    tassert(9870600,
            "Shard not found in ShardRegistry after committing addShard",
            shardRegistry->getShard(opCtx, shardType.getName()).isOK());

    topology_change_helpers::hangAddShardBeforeUpdatingClusterCardinalityParameterFailpoint(opCtx);

    // Release the shard membership lock since the set cluster parameter operation below requires
    // taking this lock.
    shardMembershipLock.unlock();

    // Unblock ShardingDDLCoordinators on the cluster.

    // TODO (SERVER-99433) remove this once the _kClusterCardinalityParameterLock is removed
    // alongside the RSEndpoint. Some paths of add/remove shard take the
    // _kClusterCardinalityParameterLock before the FixedFCVRegion and others take the
    // FixedFCVRegion before the _kClusterCardinalityParameterLock lock. However, all paths take the
    // kConfigsvrShardsNamespace ddl lock before either, so we do not actually have a lock ordering
    // problem. See SERVER-99708 for more information.
    DisableLockerRuntimeOrderingChecks disableChecks{opCtx};
    topology_change_helpers::unblockDDLCoordinators(opCtx);
    unblockDDLCoordinatorsGuard.dismiss();

    auto updateStatus = _updateClusterCardinalityParameterAfterAddShardIfNeeded(
        clusterCardinalityParameterLock, opCtx);
    if (!updateStatus.isOK()) {
        return updateStatus;
    }

    return shardType.getName();
}

std::pair<ConnectionString, std::string> ShardingCatalogManager::getConfigShardParameters(
    OperationContext* opCtx) {
    // Set the operation context read concern level to local for reads into the config
    // database.
    repl::ReadConcernArgs::get(opCtx) =
        repl::ReadConcernArgs(repl::ReadConcernLevel::kLocalReadConcern);

    auto configConnString = repl::ReplicationCoordinator::get(opCtx)->getConfigConnectionString();

    return std::make_pair(configConnString, ShardId::kConfigServerId.toString());
}

void ShardingCatalogManager::addConfigShard(OperationContext* opCtx,
                                            const FixedFCVRegion& fixedFcvRegion) {
    const auto [configConnString, shardName] = getConfigShardParameters(opCtx);
    uassertStatusOK(addShard(opCtx, fixedFcvRegion, &shardName, configConnString, true));
}

boost::optional<RemoveShardProgress> ShardingCatalogManager::checkPreconditionsAndStartDrain(
    OperationContext* opCtx, const ShardId& shardId) {
    // Unset the addOrRemoveShardInProgress cluster parameter in case it was left set by a previous
    // failed addShard/removeShard operation.
    topology_change_helpers::resetDDLBlockingForTopologyChangeIfNeeded(opCtx);

    const auto shardName = shardId.toString();
    audit::logRemoveShard(opCtx->getClient(), shardName);

    // Take the cluster cardinality parameter lock and the shard membership lock in exclusive mode
    // so that no add/remove shard operation and its set cluster cardinality parameter operation can
    // interleave with the ones below. Release the shard membership lock before initiating the
    // _configsvrSetClusterParameter command after finishing the remove shard operation since
    // setting a cluster parameter requires taking this lock.
    Lock::ExclusiveLock clusterCardinalityParameterLock =
        acquireClusterCardinalityParameterLockForTopologyChange(opCtx);
    Lock::ExclusiveLock shardMembershipLock =
        ShardingCatalogManager::acquireShardMembershipLockForTopologyChange(opCtx);

    auto optShard = topology_change_helpers::getShardIfExists(opCtx, _localConfigShard, shardId);
    if (!optShard) {
        // Release the shard membership lock since the set cluster parameter operation below
        // requires taking this lock.
        shardMembershipLock.unlock();

        auto updateStatus = _updateClusterCardinalityParameterAfterRemoveShardIfNeeded(
            clusterCardinalityParameterLock, opCtx);
        uassertStatusOK(updateStatus);
        return RemoveShardProgress(ShardDrainingStateEnum::kCompleted);
    }

    const auto shard = *optShard;

    // Find how many *other* shards exist, which are *not* currently draining
    const auto countOtherNotDrainingShards = topology_change_helpers::runCountCommandOnConfig(
        opCtx,
        _localConfigShard,
        NamespaceString::kConfigsvrShardsNamespace,
        BSON(ShardType::name() << NE << shardName << ShardType::draining.ne(true)));
    uassert(ErrorCodes::IllegalOperation,
            "Operation not allowed because it would drain the last shard",
            countOtherNotDrainingShards > 0);

    // Ensure there are no non-empty zones that only belong to this shard
    for (auto& zoneName : shard.getTags()) {
        auto isRequiredByZone = uassertStatusOK(
            _isShardRequiredByZoneStillInUse(opCtx, kConfigReadSelector, shardName, zoneName));
        uassert(ErrorCodes::ZoneStillInUse,
                str::stream() << "Operation not allowed because there is no other shard in zone "
                              << zoneName << " to drain to.",
                !isRequiredByZone);
    }

    // Figure out if shard is already draining
    const bool isShardCurrentlyDraining =
        ShardingCatalogManager::isShardCurrentlyDraining(opCtx, shardId);

    if (!isShardCurrentlyDraining) {
        LOGV2(21945, "Going to start draining shard", "shardId"_attr = shardName);

        // Record start in changelog
        uassertStatusOK(ShardingLogging::get(opCtx)->logChangeChecked(
            opCtx,
            "removeShard.start",
            NamespaceString::kEmpty,
            BSON("shard" << shardName),
            ShardingCatalogClient::writeConcernLocalHavingUpstreamWaiter(),
            _localConfigShard,
            _localCatalogClient.get()));

        uassertStatusOKWithContext(
            _localCatalogClient->updateConfigDocument(
                opCtx,
                NamespaceString::kConfigsvrShardsNamespace,
                BSON(ShardType::name() << shardName),
                BSON("$set" << BSON(ShardType::draining(true))),
                false,
                ShardingCatalogClient::writeConcernLocalHavingUpstreamWaiter()),
            "error starting shard draining");

        return RemoveShardProgress{ShardDrainingStateEnum::kStarted};
    }
    return boost::none;
}

void ShardingCatalogManager::stopDrain(OperationContext* opCtx, const ShardId& shardId) {
    // Unset the addOrRemoveShardInProgress cluster parameter in case it was left set by a previous
    // failed addShard/removeShard operation.
    topology_change_helpers::resetDDLBlockingForTopologyChangeIfNeeded(opCtx);

    const auto shardName = shardId.toString();

    // Take the cluster cardinality parameter lock and the shard membership lock in exclusive mode
    // so that no add/remove shard operation and its set cluster cardinality parameter operation can
    // interleave with the ones below. Release the shard membership lock before initiating the
    // _configsvrSetClusterParameter command after finishing the remove shard operation since
    // setting a cluster parameter requires taking this lock.
    Lock::ExclusiveLock clusterCardinalityParameterLock =
        acquireClusterCardinalityParameterLockForTopologyChange(opCtx);
    Lock::ExclusiveLock shardMembershipLock =
        ShardingCatalogManager::acquireShardMembershipLockForTopologyChange(opCtx);

    auto optShard = topology_change_helpers::getShardIfExists(opCtx, _localConfigShard, shardId);

    uassert(ErrorCodes::ShardNotFound,
            fmt::format("Couldn't find the shard {} in the cluster", shardName),
            optShard);

    // Check if shard is already draining
    const bool isShardCurrentlyDraining =
        ShardingCatalogManager::isShardCurrentlyDraining(opCtx, shardId);

    if (isShardCurrentlyDraining) {
        // Record stop in changelog
        uassertStatusOK(ShardingLogging::get(opCtx)->logChangeChecked(
            opCtx,
            "removeShard.stopShardDraining",
            NamespaceString::kEmpty,
            BSON("shard" << shardName),
            ShardingCatalogClient::writeConcernLocalHavingUpstreamWaiter(),
            _localConfigShard,
            _localCatalogClient.get()));

        uassertStatusOKWithContext(
            _localCatalogClient->updateConfigDocument(
                opCtx,
                NamespaceString::kConfigsvrShardsNamespace,
                BSON(ShardType::name() << shardName),
                BSON("$unset" << BSON(ShardType::draining(""))),
                false,
                ShardingCatalogClient::writeConcernLocalHavingUpstreamWaiter()),
            "error stopping shard draining");
    }
}

RemoveShardProgress ShardingCatalogManager::checkDrainingProgress(OperationContext* opCtx,
                                                                  const ShardId& shardId) {
    hangRemoveShardAfterSettingDrainingFlag.pauseWhileSet(opCtx);

    // Draining has already started, now figure out how many chunks and databases are still on the
    // shard.
    auto drainingProgress =
        topology_change_helpers::getDrainingProgress(opCtx, _localConfigShard, shardId.toString());
    // The counters: `shardedChunks`, `totalCollections`, and `databases` are used to present the
    // ongoing status to the user. Additionally, `totalChunks` on the shard is checked for safety,
    // as it is a critical point in the removeShard process, to ensure that a non-empty shard is not
    // removed. For example the number of unsharded collections might be inaccurate due to
    // $listClusterCatalog potentially returning an incorrect list of shards during concurrent DDL
    // operations.
    if (!drainingProgress.isFullyDrained()) {
        // Still more draining to do
        LOGV2(21946,
              "removeShard: draining",
              "chunkCount"_attr = drainingProgress.totalChunks,
              "shardedChunkCount"_attr = drainingProgress.removeShardCounts.getChunks(),
              "unshardedCollectionsCount"_attr =
                  drainingProgress.removeShardCounts.getCollectionsToMove(),
              "databaseCount"_attr = drainingProgress.removeShardCounts.getDbs(),
              "jumboCount"_attr = drainingProgress.removeShardCounts.getJumboChunks());
        RemoveShardProgress progress(ShardDrainingStateEnum::kOngoing);
        progress.setRemaining(drainingProgress.removeShardCounts);
        return progress;
    }
    RemoveShardProgress progress(ShardDrainingStateEnum::kDrainingComplete);
    return progress;
}

bool ShardingCatalogManager::isShardCurrentlyDraining(OperationContext* opCtx,
                                                      const ShardId& shardId) {
    const auto shardName = shardId.toString();
    return topology_change_helpers::runCountCommandOnConfig(
               opCtx,
               _localConfigShard,
               NamespaceString::kConfigsvrShardsNamespace,
               BSON(ShardType::name() << shardName << ShardType::draining(true))) > 0;
}

RemoveShardProgress ShardingCatalogManager::removeShard(OperationContext* opCtx,
                                                        const ShardId& shardId) {
    // Unset the addOrRemoveShardInProgress cluster parameter in case it was left set by a previous
    // failed addShard/removeShard operation.
    topology_change_helpers::resetDDLBlockingForTopologyChangeIfNeeded(opCtx);

    // Since we released the addRemoveShardLock between checking the preconditions and here, it is
    // possible that the shard has already been removed.
    auto optShard = topology_change_helpers::getShardIfExists(opCtx, _localConfigShard, shardId);
    if (!optShard.is_initialized()) {
        return RemoveShardProgress(ShardDrainingStateEnum::kCompleted);
    }
    uassert(ErrorCodes::ConflictingOperationInProgress,
            str::stream() << "Shard " << shardId << " is not currently draining",
            optShard->getDraining());

    const auto shard = *optShard;
    const auto replicaSetName =
        uassertStatusOK(ConnectionString::parse(shard.getHost())).getReplicaSetName();
    const auto shardName = shardId.toString();

    if (shardId == ShardId::kConfigServerId) {
        topology_change_helpers::joinMigrations(opCtx);
        // The config server may be added as a shard again, so we locally drop its drained
        // sharded collections to enable that without user intervention. But we have to wait for
        // the range deleter to quiesce to give queries and stale routers time to discover the
        // migration, to match the usual probabilistic guarantees for migrations.
        auto pendingRangeDeletions = topology_change_helpers::getRangeDeletionCount(opCtx);
        if (pendingRangeDeletions > 0) {
            LOGV2(7564600,
                  "removeShard: waiting for range deletions",
                  "pendingRangeDeletions"_attr = pendingRangeDeletions);
            RemoveShardProgress progress(ShardDrainingStateEnum::kPendingDataCleanup);
            progress.setPendingRangeDeletions(pendingRangeDeletions);
            return progress;
        }
    }

    // Prevent new ShardingDDLCoordinators operations from starting across the cluster.
    ScopeGuard unblockDDLCoordinatorsGuard([&] { scheduleAsyncUnblockDDLCoordinators(opCtx); });
    topology_change_helpers::blockDDLCoordinatorsAndDrain(opCtx);

    hangRemoveShardAfterDrainingDDL.pauseWhileSet(opCtx);

    // Now that DDL operations are not executing, recheck that this shard truly does not own any
    // chunks nor database.
    auto drainingProgress =
        topology_change_helpers::getDrainingProgress(opCtx, _localConfigShard, shardName);
    // The counters: `shardedChunks`, `totalCollections`, and `databases` are used to present the
    // ongoing status to the user. Additionally, `totalChunks` on the shard is checked for safety,
    // as it is a critical point in the removeShard process, to ensure that a non-empty shard is not
    // removed. For example the number of unsharded collections might be inaccurate due to
    // $listClusterCatalog potentially returning an incorrect list of shards during concurrent DDL
    // operations.
    if (!drainingProgress.isFullyDrained()) {
        // Still more draining to do
        LOGV2(5687909,
              "removeShard: more draining to do after having blocked DDLCoordinators",
              "chunkCount"_attr = drainingProgress.totalChunks,
              "shardedChunkCount"_attr = drainingProgress.removeShardCounts.getChunks(),
              "unshardedCollectionsCount"_attr =
                  drainingProgress.removeShardCounts.getCollectionsToMove(),
              "databaseCount"_attr = drainingProgress.removeShardCounts.getDbs(),
              "jumboCount"_attr = drainingProgress.removeShardCounts.getJumboChunks());
        RemoveShardProgress progress(ShardDrainingStateEnum::kOngoing);
        progress.setRemaining(drainingProgress.removeShardCounts);
        return progress;
    }

    // Declare locks that we will need below then unlock them for now.
    Lock::ExclusiveLock clusterCardinalityParameterLock =
        acquireClusterCardinalityParameterLockForTopologyChange(opCtx);
    Lock::ExclusiveLock shardMembershipLock =
        ShardingCatalogManager::acquireShardMembershipLockForTopologyChange(opCtx);
    shardMembershipLock.unlock();
    clusterCardinalityParameterLock.unlock();

    {
        // Keep the FCV stable across the commit of the shard removal. This allows us to only drop
        // the sessions collection when needed.
        //
        // NOTE: We don't use a Global IX lock here, because we don't want to hold the global lock
        // while blocking on the network).
        FixedFCVRegion fcvRegion(opCtx);

        if (shardId == ShardId::kConfigServerId) {
            auto trackedDBs =
                _localCatalogClient->getAllDBs(opCtx, repl::ReadConcernLevel::kLocalReadConcern);

            if (auto pendingCleanupState =
                    topology_change_helpers::dropLocalCollectionsAndDatabases(
                        opCtx, trackedDBs, shardName)) {
                return *pendingCleanupState;
            }

            // Also drop the sessions collection, which we assume is the only sharded collection in
            // the config database. Only do this if
            // featureFlagSessionsCollectionCoordinatorOnConfigServer is disabled. We don't have
            // synchronization with setFCV here, so it is still possible for rare interleavings to
            // drop the collection when they shouldn't, but the create coordinator will re-create it
            // on the next periodic refresh.
            if (!feature_flags::gSessionsCollectionCoordinatorOnConfigServer.isEnabled(
                    VersionContext::getDecoration(opCtx), fcvRegion->acquireFCVSnapshot())) {
                DBDirectClient client(opCtx);
                BSONObj result;
                if (!client.dropCollection(
                        NamespaceString::kLogicalSessionsNamespace,
                        ShardingCatalogClient::writeConcernLocalHavingUpstreamWaiter(),
                        &result)) {
                    uassertStatusOK(getStatusFromCommandResult(result));
                }
            }
        }

        // Draining is done, now finish removing the shard.
        LOGV2(21949, "Going to remove shard", "shardId"_attr = shardName);

        // Synchronize the control shard selection, the shard's document removal, and the topology
        // time update to exclude potential race conditions in case of concurrent add/remove shard
        // operations.
        clusterCardinalityParameterLock.lock();
        shardMembershipLock.lock();

        topology_change_helpers::commitRemoveShard(
            shardMembershipLock,
            opCtx,
            _localConfigShard,
            shardName,
            Grid::get(opCtx)->getExecutorPool()->getFixedExecutor());

        // Release the shard membership lock since the set cluster parameter operation below
        // require taking this lock.
        shardMembershipLock.unlock();

        // Release also the fixedFCVRegion since it is not needed after this and prevents us from
        // blocking FCV change for a long time if the following setClusterParameter takes a while.
    }

    // Unset the addOrRemoveShardInProgress cluster parameter. Note that
    // _removeShardInTransaction has already waited for the commit to be majority-acknowledged.

    // TODO (SERVER-99433) remove this once the _kClusterCardinalityParameterLock is removed
    // alongside the RSEndpoint.
    // Some paths of add/remove shard take the _kClusterCardinalityParameterLock before
    // the FixedFCVRegion and others take the FixedFCVRegion before the
    // _kClusterCardinalityParameterLock lock. However, all paths take the kConfigsvrShardsNamespace
    // ddl lock, so we do not actually have a lock ordering problem. See SERVER-99708 for more
    // information.
    DisableLockerRuntimeOrderingChecks disableChecks{opCtx};
    topology_change_helpers::unblockDDLCoordinators(opCtx);
    unblockDDLCoordinatorsGuard.dismiss();

    // The shard which was just removed must be reflected in the shard registry, before the
    // replica set monitor is removed, otherwise the shard would be referencing a dropped RSM.
    Grid::get(opCtx)->shardRegistry()->reload(opCtx);

    if (shardId != ShardId::kConfigServerId) {
        // Don't remove the config shard's RSM because it is used to target the config server.
        ReplicaSetMonitor::remove(replicaSetName);
    }

    // Record finish in changelog
    ShardingLogging::get(opCtx)->logChange(
        opCtx,
        "removeShard",
        NamespaceString::kEmpty,
        BSON("shard" << shardName),
        ShardingCatalogClient::writeConcernLocalHavingUpstreamWaiter(),
        _localConfigShard,
        _localCatalogClient.get());

    hangRemoveShardBeforeUpdatingClusterCardinalityParameter.pauseWhileSet(opCtx);

    uassertStatusOK(_updateClusterCardinalityParameterAfterRemoveShardIfNeeded(
        clusterCardinalityParameterLock, opCtx));

    return RemoveShardProgress(ShardDrainingStateEnum::kCompleted);
}

void ShardingCatalogManager::appendDBAndCollDrainingInfo(OperationContext* opCtx,
                                                         BSONObjBuilder& result,
                                                         ShardId shardId) {
    const auto databases =
        uassertStatusOK(_localCatalogClient->getDatabasesForShard(opCtx, shardId));

    const auto collections = getCollectionsToMoveForShard(opCtx, _localConfigShard.get(), shardId);

    // Get BSONObj containing:
    // 1) note about moving or dropping databases in a shard
    // 2) list of databases (excluding 'local' database) that need to be moved
    std::vector<DatabaseName> userDatabases;
    for (const auto& dbName : databases) {
        if (!dbName.isLocalDB()) {
            userDatabases.push_back(dbName);
        }
    }

    if (!userDatabases.empty() || !collections.empty()) {
        result.append("note",
                      "you need to call moveCollection for collectionsToMove and "
                      "afterwards movePrimary for the dbsToMove");
    }

    // Note the `dbsToMove` field could be excluded if we have no user database to move but we
    // enforce it for backcompatibilty.
    BSONArrayBuilder dbs(result.subarrayStart("dbsToMove"));
    bool canAppendToDoc = true;
    // The dbsToMove and collectionsToMove arrays will be truncated accordingly if they exceed
    // the 16MB BSON limitation. The offset for calculation is set to 10K to reserve place for
    // other attributes.
    int reservationOffsetBytes = 10 * 1024 + dbs.len();

    const auto maxUserSize = std::invoke([&] {
        if (auto failpoint = changeBSONObjMaxUserSize.scoped();
            MONGO_unlikely(failpoint.isActive())) {
            return failpoint.getData()["maxUserSize"].Int();
        } else {
            return BSONObjMaxUserSize;
        }
    });

    for (const auto& dbName : userDatabases) {
        canAppendToDoc = appendToArrayIfRoom(
            reservationOffsetBytes,
            dbs,
            DatabaseNameUtil::serialize(dbName, SerializationContext::stateDefault()),
            maxUserSize);
        if (!canAppendToDoc)
            break;
    }
    dbs.doneFast();
    reservationOffsetBytes += dbs.len();

    BSONArrayBuilder collectionsToMove(result.subarrayStart("collectionsToMove"));
    if (canAppendToDoc) {
        for (const auto& collectionName : collections) {
            canAppendToDoc =
                appendToArrayIfRoom(reservationOffsetBytes,
                                    collectionsToMove,
                                    NamespaceStringUtil::serialize(
                                        collectionName, SerializationContext::stateDefault()),
                                    maxUserSize);
            if (!canAppendToDoc)
                break;
        }
    }
    collectionsToMove.doneFast();
    if (!canAppendToDoc) {
        result.appendElements(BSON("truncated" << true));
    }
}

void ShardingCatalogManager::appendShardDrainingStatus(OperationContext* opCtx,
                                                       BSONObjBuilder& result,
                                                       RemoveShardProgress shardDrainingStatus,
                                                       ShardId shardId) {
    const auto& state = shardDrainingStatus.getState();
    if (state == ShardDrainingStateEnum::kStarted || state == ShardDrainingStateEnum::kOngoing) {
        appendDBAndCollDrainingInfo(opCtx, result, shardId);
    }
    if (state == ShardDrainingStateEnum::kStarted || state == ShardDrainingStateEnum::kCompleted) {
        result.append("shard", shardId);
    }
    result.append("msg", topology_change_helpers::getRemoveShardMessage(state));
    shardDrainingStatus.serialize(&result);
}

Lock::SharedLock ShardingCatalogManager::enterStableTopologyRegion(OperationContext* opCtx) {
    return Lock::SharedLock(opCtx, _kShardMembershipLock);
}

Lock::ExclusiveLock ShardingCatalogManager::acquireShardMembershipLockForTopologyChange(
    OperationContext* opCtx) {
    return Lock::ExclusiveLock(opCtx, _kShardMembershipLock);
}

Lock::ExclusiveLock ShardingCatalogManager::acquireClusterCardinalityParameterLockForTopologyChange(
    OperationContext* opCtx) {
    return Lock::ExclusiveLock(opCtx, _kClusterCardinalityParameterLock);
}

void ShardingCatalogManager::appendConnectionStats(executor::ConnectionPoolStats* stats) {
    _executorForAddShard->appendConnectionStats(stats);
}

void ShardingCatalogManager::_standardizeClusterParameters(OperationContext* opCtx,
                                                           RemoteCommandTargeter& targeter) {
    auto configSvrClusterParameterDocs =
        topology_change_helpers::getClusterParametersLocally(opCtx);

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
            auto parameters = topology_change_helpers::getClusterParametersFromReplicaSet(
                opCtx, targeter, _executorForAddShard);
            topology_change_helpers::setClusterParametersLocally(opCtx, parameters);
            return;
        }
    }
    topology_change_helpers::setClusterParametersOnReplicaSet(
        opCtx, targeter, configSvrClusterParameterDocs, boost::none, _executorForAddShard);
}

void ShardingCatalogManager::scheduleAsyncUnblockDDLCoordinators(OperationContext* opCtx) {
    auto executor = Grid::get(opCtx)->getExecutorPool()->getFixedExecutor();
    const auto serviceContext = opCtx->getServiceContext();
    AsyncTry([this, serviceContext] {
        ThreadClient tc{"resetDDLBlockingForTopologyChange",
                        serviceContext->getService(ClusterRole::ShardServer)};
        auto uniqueOpCtx{tc->makeOperationContext()};
        auto opCtx{uniqueOpCtx.get()};
        opCtx->setAlwaysInterruptAtStepDownOrUp_UNSAFE();

        DDLLockManager::ScopedCollectionDDLLock ddlLock(opCtx,
                                                        NamespaceString::kConfigsvrShardsNamespace,
                                                        "scheduleAsyncUnblockDDLCoordinators",
                                                        LockMode::MODE_X);

        topology_change_helpers::resetDDLBlockingForTopologyChangeIfNeeded(opCtx);
    })
        .until([serviceContext](Status status) {
            // Retry until success or until this node is no longer the primary.
            const bool primary =
                repl::ReplicationCoordinator::get(serviceContext)->getMemberState().primary();
            return status.isOK() || !primary;
        })
        .withDelayBetweenIterations(Seconds(1))
        .on(executor, CancellationToken::uncancelable())
        .onError([](Status status) {
            LOGV2_WARNING(
                5687908,
                "Failed to reset addOrRemoveShardInProgress cluster parameter after failure",
                "error"_attr = status.toString());
        })
        .getAsync([](auto) {});
}

}  // namespace mongo
