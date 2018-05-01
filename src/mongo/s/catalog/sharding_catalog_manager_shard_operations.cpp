/**
 *    Copyright (C) 2017 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kSharding

#include "mongo/platform/basic.h"

#include "mongo/s/catalog/sharding_catalog_manager.h"

#include <iomanip>
#include <pcrecpp.h>
#include <set>

#include "mongo/base/status_with.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/client/connection_string.h"
#include "mongo/client/read_preference.h"
#include "mongo/client/remote_command_targeter.h"
#include "mongo/client/replica_set_monitor.h"
#include "mongo/db/audit.h"
#include "mongo/db/catalog/catalog_raii.h"
#include "mongo/db/client.h"
#include "mongo/db/commands/feature_compatibility_version.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/repl_client_info.h"
#include "mongo/db/repl/repl_set_config.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/s/type_shard_identity.h"
#include "mongo/db/sessions_collection.h"
#include "mongo/db/wire_version.h"
#include "mongo/executor/task_executor.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/s/catalog/config_server_version.h"
#include "mongo/s/catalog/sharding_catalog_client.h"
#include "mongo/s/catalog/type_database.h"
#include "mongo/s/catalog/type_shard.h"
#include "mongo/s/client/shard.h"
#include "mongo/s/client/shard_connection.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/cluster_identity_loader.h"
#include "mongo/s/grid.h"
#include "mongo/s/shard_util.h"
#include "mongo/s/write_ops/batched_command_request.h"
#include "mongo/s/write_ops/batched_command_response.h"
#include "mongo/util/fail_point_service.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"
#include "mongo/util/scopeguard.h"

namespace mongo {
namespace {

using std::vector;

using CallbackHandle = executor::TaskExecutor::CallbackHandle;
using CallbackArgs = executor::TaskExecutor::CallbackArgs;
using RemoteCommandCallbackArgs = executor::TaskExecutor::RemoteCommandCallbackArgs;
using RemoteCommandCallbackFn = executor::TaskExecutor::RemoteCommandCallbackFn;

const Seconds kDefaultFindHostMaxWaitTime(20);

const ReadPreferenceSetting kConfigReadSelector(ReadPreference::Nearest, TagSet{});
const WriteConcernOptions kNoWaitWriteConcern(1, WriteConcernOptions::SyncMode::UNSET, Seconds(0));

/**
 * Generates a unique name to be given to a newly added shard.
 */
StatusWith<std::string> generateNewShardName(OperationContext* opCtx) {
    BSONObjBuilder shardNameRegex;
    shardNameRegex.appendRegex(ShardType::name(), "^shard");

    auto findStatus = Grid::get(opCtx)->shardRegistry()->getConfigShard()->exhaustiveFindOnConfig(
        opCtx,
        kConfigReadSelector,
        repl::ReadConcernLevel::kMajorityReadConcern,
        NamespaceString(ShardType::ConfigNS),
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
        host, dbName.toString(), cmdObj, rpc::makeEmptyMetadata(), nullptr, Seconds(30));

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
        LOG(0) << "Operation timed out with status " << redact(response.status);
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
        commandStatus = {ErrorCodes::OperationFailed,
                         str::stream() << "failed to run command " << cmdObj
                                       << " when attempting to add shard "
                                       << targeter->connectionString().toString()
                                       << causedBy(commandStatus)};
    }

    Status writeConcernStatus = getWriteConcernStatusFromCommandResult(result);
    if (!Shard::shouldErrorBePropagated(writeConcernStatus.code())) {
        writeConcernStatus = {ErrorCodes::OperationFailed,
                              str::stream() << "failed to satisfy writeConcern for command "
                                            << cmdObj
                                            << " when attempting to add shard "
                                            << targeter->connectionString().toString()
                                            << causedBy(writeConcernStatus)};
    }

    return Shard::CommandResponse(std::move(host),
                                  std::move(result),
                                  response.metadata.getOwned(),
                                  std::move(commandStatus),
                                  std::move(writeConcernStatus));
}

StatusWith<boost::optional<ShardType>> ShardingCatalogManager::_checkIfShardExists(
    OperationContext* opCtx,
    const ConnectionString& proposedShardConnectionString,
    const std::string* proposedShardName,
    long long proposedShardMaxSize) {
    // Check whether any host in the connection is already part of the cluster.
    const auto existingShards = Grid::get(opCtx)->catalogClient()->getAllShards(
        opCtx, repl::ReadConcernLevel::kLocalReadConcern);
    if (!existingShards.isOK()) {
        return Status(existingShards.getStatus().code(),
                      str::stream() << "Failed to load existing shards during addShard"
                                    << causedBy(existingShards.getStatus().reason()));
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
            if (proposedShardConnectionString.type() == ConnectionString::SET &&
                proposedShardConnectionString.getSetName() != existingShardConnStr.getSetName()) {
                return false;
            }
            if (proposedShardMaxSize != existingShard.getMaxSizeMB()) {
                return false;
            }
            return true;
        };

        if (existingShardConnStr.type() == ConnectionString::SET &&
            proposedShardConnectionString.type() == ConnectionString::SET &&
            existingShardConnStr.getSetName() == proposedShardConnectionString.getSetName()) {
            // An existing shard has the same replica set name as the shard being added.
            // If the options aren't the same, then this is an error,
            // but if the options match then the addShard operation should be immediately
            // considered a success and terminated.
            if (shardsAreEquivalent()) {
                return {existingShard};
            } else {
                return {ErrorCodes::IllegalOperation,
                        str::stream() << "A shard already exists containing the replica set '"
                                      << existingShardConnStr.getSetName()
                                      << "'"};
            }
        }

        for (const auto& existingHost : existingShardConnStr.getServers()) {
            // Look if any of the hosts in the existing shard are present within the shard trying
            // to be added.
            for (const auto& addingHost : proposedShardConnectionString.getServers()) {
                if (existingHost == addingHost) {
                    // At least one of the hosts in the shard being added already exists in an
                    // existing shard.  If the options aren't the same, then this is an error,
                    // but if the options match then the addShard operation should be immediately
                    // considered a success and terminated.
                    if (shardsAreEquivalent()) {
                        return {existingShard};
                    } else {
                        return {ErrorCodes::IllegalOperation,
                                str::stream() << "'" << addingHost.toString() << "' "
                                              << "is already a member of the existing shard '"
                                              << existingShard.getHost()
                                              << "' ("
                                              << existingShard.getName()
                                              << ")."};
                    }
                }
            }
        }

        if (proposedShardName && *proposedShardName == existingShard.getName()) {
            // If we get here then we're trying to add a shard with the same name as an existing
            // shard, but there was no overlap in the hosts between the existing shard and the
            // proposed connection string for the new shard.
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
    const ConnectionString& connectionString) {
    auto swCommandResponse = _runCommandForAddShard(
        opCtx, targeter.get(), NamespaceString::kAdminDb, BSON("isMaster" << 1));
    if (!swCommandResponse.isOK()) {
        return swCommandResponse.getStatus();
    }

    // Check for a command response error
    auto resIsMasterStatus = std::move(swCommandResponse.getValue().commandStatus);
    if (!resIsMasterStatus.isOK()) {
        return {resIsMasterStatus.code(),
                str::stream() << "Error running isMaster against "
                              << targeter->connectionString().toString()
                              << ": "
                              << causedBy(resIsMasterStatus)};
    }

    auto resIsMaster = std::move(swCommandResponse.getValue().response);

    // Fail if the node being added is a mongos.
    const std::string msg = resIsMaster.getStringField("msg");
    if (msg == "isdbgrid") {
        return {ErrorCodes::IllegalOperation, "cannot add a mongos as a shard"};
    }

    // Fail if the node being added's binary version is lower than the cluster's
    // featureCompatibilityVersion.
    long long maxWireVersion;
    Status status = bsonExtractIntegerField(resIsMaster, "maxWireVersion", &maxWireVersion);
    if (!status.isOK()) {
        return Status(status.code(),
                      str::stream() << "isMaster returned invalid 'maxWireVersion' "
                                    << "field when attempting to add "
                                    << connectionString.toString()
                                    << " as a shard: "
                                    << status.reason());
    }

    switch (serverGlobalParams.featureCompatibility.getVersion()) {
        case ServerGlobalParams::FeatureCompatibility::Version::kFullyUpgradedTo36:
        case ServerGlobalParams::FeatureCompatibility::Version::kUpgradingTo36:
        case ServerGlobalParams::FeatureCompatibility::Version::kDowngradingTo34:
            if (maxWireVersion < WireVersion::LATEST_WIRE_VERSION) {
                return {ErrorCodes::IncompatibleServerVersion,
                        str::stream() << "Cannot add " << connectionString.toString()
                                      << " as a shard because its binary version is below the "
                                         "cluster's feature compatibility version."};
            }
            break;
        default:
            if (maxWireVersion < WireVersion::LATEST_WIRE_VERSION - 1) {
                return {ErrorCodes::IncompatibleServerVersion,
                        str::stream() << "Cannot add " << connectionString.toString()
                                      << " as a shard because its binary version is below the "
                                         "cluster's feature compatibility version."};
            }
            break;
    }

    // Check whether there is a master. If there isn't, the replica set may not have been
    // initiated. If the connection is a standalone, it will return true for isMaster.
    bool isMaster;
    status = bsonExtractBooleanField(resIsMaster, "ismaster", &isMaster);
    if (!status.isOK()) {
        return Status(status.code(),
                      str::stream() << "isMaster returned invalid 'ismaster' "
                                    << "field when attempting to add "
                                    << connectionString.toString()
                                    << " as a shard: "
                                    << status.reason());
    }
    if (!isMaster) {
        return {ErrorCodes::NotMaster,
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
                              << "is the replica set still initializing? "
                              << resIsMaster};
    }

    // Make sure the set name specified in the connection string matches the one where its hosts
    // belong into
    if (!providedSetName.empty() && (providedSetName != foundSetName)) {
        return {ErrorCodes::OperationFailed,
                str::stream() << "the provided connection string (" << connectionString.toString()
                              << ") does not match the actual set name "
                              << foundSetName};
    }

    // Is it a config server?
    if (resIsMaster.hasField("configsvr")) {
        return {ErrorCodes::OperationFailed,
                str::stream() << "Cannot add " << connectionString.toString()
                              << " as a shard since it is a config server"};
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
                                      << host
                                      << " does not belong to replica set "
                                      << foundSetName
                                      << "; found "
                                      << resIsMaster.toString()};
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
    if (actualShardName == NamespaceString::kConfigDb) {
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
    builder.append("drop", SessionsCollection::kSessionsNamespaceString.coll());
    {
        BSONObjBuilder wcBuilder(builder.subobjStart("writeConcern"));
        wcBuilder.append("w", "majority");
    }

    auto swCommandResponse = _runCommandForAddShard(
        opCtx, targeter.get(), SessionsCollection::kSessionsNamespaceString.db(), builder.done());
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
                               NamespaceString::kAdminDb,
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

        if (!(dbName == NamespaceString::kAdminDb || dbName == NamespaceString::kLocalDb ||
              dbName == NamespaceString::kConfigDb)) {
            dbNames.push_back(dbName);
        }
    }

    return dbNames;
}

StatusWith<std::string> ShardingCatalogManager::addShard(
    OperationContext* opCtx,
    const std::string* shardProposedName,
    const ConnectionString& shardConnectionString,
    const long long maxSize) {
    if (shardConnectionString.type() == ConnectionString::INVALID) {
        return {ErrorCodes::BadValue, "Invalid connection string"};
    }

    if (shardProposedName && shardProposedName->empty()) {
        return {ErrorCodes::BadValue, "shard name cannot be empty"};
    }

    // Only one addShard operation can be in progress at a time.
    Lock::ExclusiveLock lk(opCtx->lockState(), _kShardMembershipLock);

    // Check if this shard has already been added (can happen in the case of a retry after a network
    // error, for example) and thus this addShard request should be considered a no-op.
    auto existingShard =
        _checkIfShardExists(opCtx, shardConnectionString, shardProposedName, maxSize);
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

    // Force a reload of the ShardRegistry to ensure that, in case this addShard is to re-add a
    // replica set that has recently been removed, we have detached the ReplicaSetMonitor for the
    // set with that setName from the ReplicaSetMonitorManager and will create a new
    // ReplicaSetMonitor when targeting the set below.
    // Note: This is necessary because as of 3.4, removeShard is performed by mongos (unlike
    // addShard), so the ShardRegistry is not synchronously reloaded on the config server when a
    // shard is removed.
    if (!Grid::get(opCtx)->shardRegistry()->reload(opCtx)) {
        // If the first reload joined an existing one, call reload again to ensure the reload is
        // fresh.
        Grid::get(opCtx)->shardRegistry()->reload(opCtx);
    }

    // TODO: Don't create a detached Shard object, create a detached RemoteCommandTargeter instead.
    const std::shared_ptr<Shard> shard{
        Grid::get(opCtx)->shardRegistry()->createConnection(shardConnectionString)};
    invariant(shard);
    auto targeter = shard->getTargeter();

    auto stopMonitoringGuard = MakeGuard([&] {
        if (shardConnectionString.type() == ConnectionString::SET) {
            // This is a workaround for the case were we could have some bad shard being
            // requested to be added and we put that bad connection string on the global replica set
            // monitor registry. It needs to be cleaned up so that when a correct replica set is
            // added, it will be recreated.
            ReplicaSetMonitor::remove(shardConnectionString.getSetName());
        }
    });

    // Validate the specified connection string may serve as shard at all
    auto shardStatus =
        _validateHostAsShard(opCtx, targeter, shardProposedName, shardConnectionString);
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
        auto dbt = Grid::get(opCtx)->catalogClient()->getDatabase(
            opCtx, dbName, repl::ReadConcernLevel::kLocalReadConcern);
        if (dbt.isOK()) {
            const auto& dbDoc = dbt.getValue().value;
            return Status(ErrorCodes::OperationFailed,
                          str::stream() << "can't add shard "
                                        << "'"
                                        << shardConnectionString.toString()
                                        << "'"
                                        << " because a local database '"
                                        << dbName
                                        << "' exists in another "
                                        << dbDoc.getPrimary());
        } else if (dbt != ErrorCodes::NamespaceNotFound) {
            return dbt.getStatus();
        }
    }

    // Check that the shard candidate does not have a local config.system.sessions collection
    auto res = _dropSessionsCollection(opCtx, targeter);

    if (!res.isOK()) {
        return Status(
            res.code(),
            str::stream()
                << "can't add shard with a local copy of config.system.sessions due to "
                << res.reason()
                << ", please drop this collection from the shard manually and try again.");
    }

    // If a name for a shard wasn't provided, generate one
    if (shardType.getName().empty()) {
        auto result = generateNewShardName(opCtx);
        if (!result.isOK()) {
            return result.getStatus();
        }
        shardType.setName(result.getValue());
    }

    if (maxSize > 0) {
        shardType.setMaxSizeMB(maxSize);
    }

    // Insert a shardIdentity document onto the shard. This also triggers sharding initialization on
    // the shard.
    LOG(2) << "going to insert shardIdentity document into shard: " << shardType;
    auto commandRequest = createShardIdentityUpsertForAddShard(opCtx, shardType.getName());
    auto swCommandResponse =
        _runCommandForAddShard(opCtx, targeter.get(), NamespaceString::kAdminDb, commandRequest);
    if (!swCommandResponse.isOK()) {
        return swCommandResponse.getStatus();
    }
    auto commandResponse = std::move(swCommandResponse.getValue());
    BatchedCommandResponse batchResponse;
    auto batchResponseStatus =
        Shard::CommandResponse::processBatchWriteResponse(commandResponse, &batchResponse);
    if (!batchResponseStatus.isOK()) {
        return batchResponseStatus;
    }

    // Since addShard runs under the fcvLock, it is guaranteed the fcv state won't change, but it's
    // possible an earlier setFCV failed partway, so we handle all possible fcv states. Note, if
    // the state is upgrading (downgrading), a user cannot switch to downgrading (upgrading) without
    // first finishing the upgrade (downgrade).
    //
    // Note, we don't explicitly send writeConcern majority to the added shard, because a 3.4 mongod
    // will reject it (setFCV did not support writeConcern until 3.6), and a 3.6 mongod will still
    // default to majority writeConcern.
    // TODO SERVER-32045: propagate the user's writeConcern.
    BSONObj setFCVCmd;
    switch (serverGlobalParams.featureCompatibility.getVersion()) {
        case ServerGlobalParams::FeatureCompatibility::Version::kFullyUpgradedTo36:
        case ServerGlobalParams::FeatureCompatibility::Version::kUpgradingTo36:
            setFCVCmd = BSON(FeatureCompatibilityVersion::kCommandName
                             << FeatureCompatibilityVersionCommandParser::kVersion36);
            break;
        default:
            setFCVCmd = BSON(FeatureCompatibilityVersion::kCommandName
                             << FeatureCompatibilityVersionCommandParser::kVersion34);
            break;
    }
    auto versionResponse =
        _runCommandForAddShard(opCtx, targeter.get(), NamespaceString::kAdminDb, setFCVCmd);
    if (!versionResponse.isOK()) {
        return versionResponse.getStatus();
    }

    if (!versionResponse.getValue().commandStatus.isOK()) {
        return versionResponse.getValue().commandStatus;
    }

    log() << "going to insert new entry for shard into config.shards: " << shardType.toString();

    Status result = Grid::get(opCtx)->catalogClient()->insertConfigDocument(
        opCtx,
        ShardType::ConfigNS,
        shardType.toBSON(),
        ShardingCatalogClient::kMajorityWriteConcern);
    if (!result.isOK()) {
        log() << "error adding shard: " << shardType.toBSON() << " err: " << result.reason();
        return result;
    }

    // Add all databases which were discovered on the new shard
    for (const auto& dbName : dbNamesStatus.getValue()) {
        DatabaseType dbt;
        dbt.setName(dbName);
        dbt.setPrimary(shardType.getName());
        dbt.setSharded(false);

        Status status = Grid::get(opCtx)->catalogClient()->updateDatabase(opCtx, dbName, dbt);
        if (!status.isOK()) {
            log() << "adding shard " << shardConnectionString.toString()
                  << " even though could not add database " << dbName;
        }
    }

    // Record in changelog
    BSONObjBuilder shardDetails;
    shardDetails.append("name", shardType.getName());
    shardDetails.append("host", shardConnectionString.toString());

    Grid::get(opCtx)
        ->catalogClient()
        ->logChange(
            opCtx, "addShard", "", shardDetails.obj(), ShardingCatalogClient::kMajorityWriteConcern)
        .transitional_ignore();

    // Ensure the added shard is visible to this process.
    auto shardRegistry = Grid::get(opCtx)->shardRegistry();
    if (!shardRegistry->getShard(opCtx, shardType.getName()).isOK()) {
        return {ErrorCodes::OperationFailed,
                "Could not find shard metadata for shard after adding it. This most likely "
                "indicates that the shard was removed immediately after it was added."};
    }
    stopMonitoringGuard.Dismiss();

    return shardType.getName();
}

StatusWith<ShardDrainingStatus> ShardingCatalogManager::removeShard(OperationContext* opCtx,
                                                                    const ShardId& shardId) {
    // Check preconditions for removing the shard
    std::string name = shardId.toString();
    auto countStatus = _runCountCommandOnConfig(
        opCtx,
        NamespaceString(ShardType::ConfigNS),
        BSON(ShardType::name() << NE << name << ShardType::draining(true)));
    if (!countStatus.isOK()) {
        return countStatus.getStatus();
    }
    if (countStatus.getValue() > 0) {
        return Status(ErrorCodes::ConflictingOperationInProgress,
                      "Can't have more than one draining shard at a time");
    }

    countStatus = _runCountCommandOnConfig(
        opCtx, NamespaceString(ShardType::ConfigNS), BSON(ShardType::name() << NE << name));
    if (!countStatus.isOK()) {
        return countStatus.getStatus();
    }
    if (countStatus.getValue() == 0) {
        return Status(ErrorCodes::IllegalOperation, "Can't remove last shard");
    }

    // Figure out if shard is already draining
    countStatus =
        _runCountCommandOnConfig(opCtx,
                                 NamespaceString(ShardType::ConfigNS),
                                 BSON(ShardType::name() << name << ShardType::draining(true)));
    if (!countStatus.isOK()) {
        return countStatus.getStatus();
    }

    auto* const shardRegistry = Grid::get(opCtx)->shardRegistry();

    if (countStatus.getValue() == 0) {
        log() << "going to start draining shard: " << name;

        auto updateStatus = Grid::get(opCtx)->catalogClient()->updateConfigDocument(
            opCtx,
            ShardType::ConfigNS,
            BSON(ShardType::name() << name),
            BSON("$set" << BSON(ShardType::draining(true))),
            false,
            ShardingCatalogClient::kLocalWriteConcern);
        if (!updateStatus.isOK()) {
            log() << "error starting removeShard: " << name
                  << causedBy(redact(updateStatus.getStatus()));
            return updateStatus.getStatus();
        }

        shardRegistry->reload(opCtx);

        // Record start in changelog
        Grid::get(opCtx)
            ->catalogClient()
            ->logChange(opCtx,
                        "removeShard.start",
                        "",
                        BSON("shard" << name),
                        ShardingCatalogClient::kLocalWriteConcern)
            .transitional_ignore();

        return ShardDrainingStatus::STARTED;
    }

    // Draining has already started, now figure out how many chunks and databases are still on the
    // shard.
    countStatus = _runCountCommandOnConfig(
        opCtx, NamespaceString(ChunkType::ConfigNS), BSON(ChunkType::shard(name)));
    if (!countStatus.isOK()) {
        return countStatus.getStatus();
    }
    const long long chunkCount = countStatus.getValue();

    countStatus = _runCountCommandOnConfig(
        opCtx, NamespaceString(DatabaseType::ConfigNS), BSON(DatabaseType::primary(name)));
    if (!countStatus.isOK()) {
        return countStatus.getStatus();
    }
    const long long databaseCount = countStatus.getValue();

    if (chunkCount > 0 || databaseCount > 0) {
        // Still more draining to do
        LOG(0) << "chunkCount: " << chunkCount;
        LOG(0) << "databaseCount: " << databaseCount;
        return ShardDrainingStatus::ONGOING;
    }

    // Draining is done, now finish removing the shard.
    log() << "going to remove shard: " << name;
    audit::logRemoveShard(opCtx->getClient(), name);

    Status status = Grid::get(opCtx)->catalogClient()->removeConfigDocuments(
        opCtx,
        ShardType::ConfigNS,
        BSON(ShardType::name() << name),
        ShardingCatalogClient::kLocalWriteConcern);
    if (!status.isOK()) {
        log() << "Error concluding removeShard operation on: " << name
              << "; err: " << status.reason();
        return status;
    }

    shardConnectionPool.removeHost(name);
    ReplicaSetMonitor::remove(name);

    shardRegistry->reload(opCtx);

    // Record finish in changelog
    Grid::get(opCtx)
        ->catalogClient()
        ->logChange(opCtx,
                    "removeShard",
                    "",
                    BSON("shard" << name),
                    ShardingCatalogClient::kLocalWriteConcern)
        .transitional_ignore();

    return ShardDrainingStatus::COMPLETED;
}

void ShardingCatalogManager::appendConnectionStats(executor::ConnectionPoolStats* stats) {
    _executorForAddShard->appendConnectionStats(stats);
}

BSONObj ShardingCatalogManager::createShardIdentityUpsertForAddShard(OperationContext* opCtx,
                                                                     const std::string& shardName) {
    BatchedCommandRequest request([&] {
        write_ops::Update updateOp(NamespaceString::kServerConfigurationNamespace);
        updateOp.setUpdates(
            {[&] {
                write_ops::UpdateOpEntry entry;
                entry.setQ(BSON("_id"
                                << "shardIdentity"
                                << ShardIdentityType::shardName(shardName)
                                << ShardIdentityType::clusterId(
                                       ClusterIdentityLoader::get(opCtx)->getClusterId())));
                entry.setU(BSON("$set" << BSON(ShardIdentityType::configsvrConnString(
                                    repl::ReplicationCoordinator::get(opCtx)
                                        ->getConfig()
                                        .getConnectionString()
                                        .toString()))));
                entry.setUpsert(true);
                return entry;
            }()});
        return updateOp;
    }());
    request.setWriteConcern(ShardingCatalogClient::kMajorityWriteConcern.toBSON());

    return request.toBSON();
}

// static
StatusWith<ShardId> ShardingCatalogManager::_selectShardForNewDatabase(
    OperationContext* opCtx, ShardRegistry* shardRegistry) {
    vector<ShardId> allShardIds;

    shardRegistry->getAllShardIds(&allShardIds);
    if (allShardIds.empty()) {
        shardRegistry->reload(opCtx);
        shardRegistry->getAllShardIds(&allShardIds);

        if (allShardIds.empty()) {
            return Status(ErrorCodes::ShardNotFound, "No shards found");
        }
    }

    ShardId candidateShardId = allShardIds[0];

    auto candidateSizeStatus = shardutil::retrieveTotalShardSize(opCtx, candidateShardId);
    if (!candidateSizeStatus.isOK()) {
        return candidateSizeStatus.getStatus();
    }

    for (size_t i = 1; i < allShardIds.size(); i++) {
        const ShardId shardId = allShardIds[i];

        const auto sizeStatus = shardutil::retrieveTotalShardSize(opCtx, shardId);
        if (!sizeStatus.isOK()) {
            return sizeStatus.getStatus();
        }

        if (sizeStatus.getValue() < candidateSizeStatus.getValue()) {
            candidateSizeStatus = sizeStatus;
            candidateShardId = shardId;
        }
    }

    return candidateShardId;
}

StatusWith<long long> ShardingCatalogManager::_runCountCommandOnConfig(OperationContext* opCtx,
                                                                       const NamespaceString& ns,
                                                                       BSONObj query) {
    BSONObjBuilder countBuilder;
    countBuilder.append("count", ns.coll());
    countBuilder.append("query", query);

    auto configShard = Grid::get(opCtx)->shardRegistry()->getConfigShard();
    auto resultStatus =
        configShard->runCommandWithFixedRetryAttempts(opCtx,
                                                      kConfigReadSelector,
                                                      ns.db().toString(),
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

}  // namespace mongo
