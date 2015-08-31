/**
 *    Copyright (C) 2015 MongoDB Inc.
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

#include "mongo/s/client/shard_registry.h"

#include "mongo/client/connection_string.h"
#include "mongo/client/query_fetcher.h"
#include "mongo/client/remote_command_targeter.h"
#include "mongo/client/remote_command_targeter_factory.h"
#include "mongo/client/replica_set_monitor.h"
#include "mongo/executor/task_executor.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/rpc/metadata/repl_set_metadata.h"
#include "mongo/s/catalog/catalog_manager.h"
#include "mongo/s/catalog/type_shard.h"
#include "mongo/s/client/shard.h"
#include "mongo/s/client/shard_connection.h"
#include "mongo/s/grid.h"
#include "mongo/stdx/memory.h"
#include "mongo/stdx/mutex.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {

using std::shared_ptr;
using std::string;
using std::unique_ptr;
using std::vector;

using executor::TaskExecutor;
using RemoteCommandCallbackArgs = TaskExecutor::RemoteCommandCallbackArgs;

namespace {
const Seconds kConfigCommandTimeout{30};
const int kNotMasterNumRetries = 3;
const Milliseconds kNotMasterRetryInterval{500};
}  // unnamed namespace

ShardRegistry::ShardRegistry(std::unique_ptr<RemoteCommandTargeterFactory> targeterFactory,
                             std::unique_ptr<executor::TaskExecutor> executor,
                             executor::NetworkInterface* network,
                             std::unique_ptr<executor::TaskExecutor> addShardExecutor,
                             ConnectionString configServerCS)
    : _targeterFactory(std::move(targeterFactory)),
      _executor(std::move(executor)),
      _network(network),
      _executorForAddShard(std::move(addShardExecutor)) {
    updateConfigServerConnectionString(configServerCS);
}

ShardRegistry::~ShardRegistry() = default;

void ShardRegistry::updateConfigServerConnectionString(ConnectionString configServerCS) {
    log() << "Updating config server connection string to: " << configServerCS.toString();
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    _configServerCS = std::move(configServerCS);

    _addConfigShard_inlock();
}

void ShardRegistry::startup() {
    _executorForAddShard->startup();
    _executor->startup();
}

void ShardRegistry::shutdown() {
    _executor->shutdown();
    _executorForAddShard->shutdown();
    _executor->join();
    _executorForAddShard->join();
}

void ShardRegistry::reload(OperationContext* txn) {
    vector<ShardType> shards;
    Status status = grid.catalogManager(txn)->getAllShards(txn, &shards);
    uassert(13632,
            str::stream() << "could not get updated shard list from config server due to "
                          << status.toString(),
            status.isOK());

    int numShards = shards.size();

    LOG(1) << "found " << numShards << " shards listed on config server(s)";

    stdx::lock_guard<stdx::mutex> lk(_mutex);

    _lookup.clear();
    _rsLookup.clear();

    _addConfigShard_inlock();

    for (const ShardType& shardType : shards) {
        uassertStatusOK(shardType.validate());

        // Skip the config host even if there is one left over from legacy installations. The
        // config host is installed manually from the catalog manager data.
        if (shardType.getName() == "config") {
            continue;
        }

        _addShard_inlock(shardType);
    }
}

shared_ptr<Shard> ShardRegistry::getShard(OperationContext* txn, const ShardId& shardId) {
    shared_ptr<Shard> shard = _findUsingLookUp(shardId);
    if (shard) {
        return shard;
    }

    // If we can't find the shard, we might just need to reload the cache
    reload(txn);

    return _findUsingLookUp(shardId);
}

shared_ptr<Shard> ShardRegistry::getShardNoReload(const ShardId& shardId) {
    return _findUsingLookUp(shardId);
}

shared_ptr<Shard> ShardRegistry::getConfigShard() {
    shared_ptr<Shard> shard = _findUsingLookUp("config");
    invariant(shard);
    return shard;
}

unique_ptr<Shard> ShardRegistry::createConnection(const ConnectionString& connStr) const {
    return stdx::make_unique<Shard>("<unnamed>", connStr, _targeterFactory->create(connStr));
}

shared_ptr<Shard> ShardRegistry::lookupRSName(const string& name) const {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    ShardMap::const_iterator i = _rsLookup.find(name);

    return (i == _rsLookup.end()) ? nullptr : i->second;
}

void ShardRegistry::remove(const ShardId& id) {
    stdx::lock_guard<stdx::mutex> lk(_mutex);

    for (ShardMap::iterator i = _lookup.begin(); i != _lookup.end();) {
        shared_ptr<Shard> s = i->second;
        if (s->getId() == id) {
            _lookup.erase(i++);
        } else {
            ++i;
        }
    }

    for (ShardMap::iterator i = _rsLookup.begin(); i != _rsLookup.end();) {
        shared_ptr<Shard> s = i->second;
        if (s->getId() == id) {
            _rsLookup.erase(i++);
        } else {
            ++i;
        }
    }

    shardConnectionPool.removeHost(id);
    ReplicaSetMonitor::remove(id);
}

void ShardRegistry::getAllShardIds(vector<ShardId>* all) const {
    std::set<string> seen;

    {
        stdx::lock_guard<stdx::mutex> lk(_mutex);
        for (ShardMap::const_iterator i = _lookup.begin(); i != _lookup.end(); ++i) {
            const shared_ptr<Shard>& s = i->second;
            if (s->getId() == "config") {
                continue;
            }

            if (seen.count(s->getId())) {
                continue;
            }
            seen.insert(s->getId());
        }
    }

    all->assign(seen.begin(), seen.end());
}

void ShardRegistry::toBSON(BSONObjBuilder* result) {
    BSONObjBuilder b(_lookup.size() + 50);

    stdx::lock_guard<stdx::mutex> lk(_mutex);

    for (ShardMap::const_iterator i = _lookup.begin(); i != _lookup.end(); ++i) {
        b.append(i->first, i->second->getConnString().toString());
    }

    result->append("map", b.obj());
}

void ShardRegistry::_addConfigShard_inlock() {
    ShardType configServerShard;
    configServerShard.setName("config");
    configServerShard.setHost(_configServerCS.toString());
    _addShard_inlock(configServerShard);
}

void ShardRegistry::_addShard_inlock(const ShardType& shardType) {
    // This validation should ideally go inside the ShardType::validate call. However, doing
    // it there would prevent us from loading previously faulty shard hosts, which might have
    // been stored (i.e., the entire getAllShards call would fail).
    auto shardHostStatus = ConnectionString::parse(shardType.getHost());
    if (!shardHostStatus.isOK()) {
        warning() << "Unable to parse shard host " << shardHostStatus.getStatus().toString();
    }

    const ConnectionString& shardHost(shardHostStatus.getValue());

    shared_ptr<Shard> shard;

    if (shardHost.type() == ConnectionString::SYNC) {
        // Sync cluster connections (legacy config server) do not go through the normal targeting
        // mechanism and must only be reachable through CatalogManagerLegacy or legacy-style queries
        // and inserts. Do not create targeter for these connections. This code should go away after
        // 3.2 is released.
        shard = std::make_shared<Shard>(shardType.getName(), shardHost, nullptr);
    } else {
        // Non-SYNC shards use targeter factory.
        shard = std::make_shared<Shard>(
            shardType.getName(), shardHost, std::move(_targeterFactory->create(shardHost)));
    }

    _lookup[shardType.getName()] = shard;

    if (shardHost.type() == ConnectionString::SET) {
        _rsLookup[shardHost.getSetName()] = shard;
    }

    // TODO: The only reason to have the shard host names in the lookup table is for the
    // setShardVersion call, which resolves the shard id from the shard address. This is
    // error-prone and will go away eventually when we switch all communications to go through
    // the remote command runner.
    _lookup[shardType.getHost()] = shard;

    for (const HostAndPort& hostAndPort : shardHost.getServers()) {
        _lookup[hostAndPort.toString()] = shard;
    }
}

shared_ptr<Shard> ShardRegistry::_findUsingLookUp(const ShardId& shardId) {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    ShardMap::iterator it = _lookup.find(shardId);
    if (it != _lookup.end()) {
        return it->second;
    }

    return nullptr;
}

StatusWith<ShardRegistry::QueryResponse> ShardRegistry::exhaustiveFindOnConfigNode(
    const HostAndPort& host,
    const NamespaceString& nss,
    const BSONObj& query,
    const BSONObj& sort,
    boost::optional<long long> limit,
    boost::optional<repl::ReadConcernArgs> readConcern,
    const BSONObj& metadata) {
    // If for some reason the callback never gets invoked, we will return this status
    Status status = Status(ErrorCodes::InternalError, "Internal error running find command");
    QueryResponse response;

    auto fetcherCallback = [&status, &response](const Fetcher::QueryResponseStatus& dataStatus,
                                                Fetcher::NextAction* nextAction) {

        // Throw out any accumulated results on error
        if (!dataStatus.isOK()) {
            status = dataStatus.getStatus();
            response.docs.clear();
            return;
        }

        auto& data = dataStatus.getValue();
        if (data.otherFields.metadata.hasField(rpc::kReplSetMetadataFieldName)) {
            auto replParseStatus =
                rpc::ReplSetMetadata::readFromMetadata(data.otherFields.metadata);

            if (!replParseStatus.isOK()) {
                status = replParseStatus.getStatus();
                response.docs.clear();
                return;
            }

            response.opTime = replParseStatus.getValue().getLastOpCommitted();
        }

        for (const BSONObj& doc : data.documents) {
            response.docs.push_back(doc.getOwned());
        }

        status = Status::OK();
    };

    auto lpq = LiteParsedQuery::makeAsFindCmd(nss,
                                              query,
                                              BSONObj(),  // projection
                                              sort,
                                              BSONObj(),    // hint
                                              boost::none,  // skip
                                              limit);

    BSONObjBuilder findCmdBuilder;
    lpq->asFindCommand(&findCmdBuilder);

    if (readConcern) {
        BSONObjBuilder builder;
        readConcern->appendInfo(&findCmdBuilder);
    }

    QueryFetcher fetcher(
        _executor.get(), host, nss, findCmdBuilder.done(), fetcherCallback, metadata);

    Status scheduleStatus = fetcher.schedule();
    if (!scheduleStatus.isOK()) {
        return scheduleStatus;
    }

    fetcher.wait();

    if (!status.isOK()) {
        return status;
    }

    return response;
}

StatusWith<BSONObj> ShardRegistry::runCommand(OperationContext* txn,
                                              const HostAndPort& host,
                                              const std::string& dbName,
                                              const BSONObj& cmdObj) {
    auto status =
        _runCommandWithMetadata(_executor.get(), host, dbName, cmdObj, rpc::makeEmptyMetadata());
    if (status.getStatus() == ErrorCodes::ShardNotFound) {  // One retry if the shard isn't found.
        reload(txn);
        status = _runCommandWithMetadata(
            _executor.get(), host, dbName, cmdObj, rpc::makeEmptyMetadata());
    }
    if (!status.isOK()) {
        return status.getStatus();
    }

    return status.getValue().response;
}

StatusWith<BSONObj> ShardRegistry::runCommandForAddShard(OperationContext* txn,
                                                         const HostAndPort& host,
                                                         const std::string& dbName,
                                                         const BSONObj& cmdObj) {
    auto status = _runCommandWithMetadata(
        _executorForAddShard.get(), host, dbName, cmdObj, rpc::makeEmptyMetadata());
    if (!status.isOK()) {
        return status.getStatus();
    }

    return status.getValue().response;
}

StatusWith<ShardRegistry::CommandResponse> ShardRegistry::runCommandOnConfig(
    const HostAndPort& host,
    const std::string& dbName,
    const BSONObj& cmdObj,
    const BSONObj& metadata) {
    return _runCommandWithMetadata(_executor.get(), host, dbName, cmdObj, metadata);
}

StatusWith<BSONObj> ShardRegistry::runCommandOnConfigWithNotMasterRetries(const std::string& dbname,
                                                                          const BSONObj& cmdObj) {
    auto status = runCommandOnConfigWithNotMasterRetries(dbname, cmdObj, rpc::makeEmptyMetadata());

    if (!status.isOK()) {
        return status.getStatus();
    }

    return status.getValue().response;
}

StatusWith<ShardRegistry::CommandResponse> ShardRegistry::runCommandOnConfigWithNotMasterRetries(
    const std::string& dbname, const BSONObj& cmdObj, const BSONObj& metadata) {
    auto configShard = getConfigShard();
    return _runCommandWithNotMasterRetries(
        _executor.get(), configShard->getTargeter(), dbname, cmdObj, metadata);
}

StatusWith<BSONObj> ShardRegistry::runCommandWithNotMasterRetries(OperationContext* txn,
                                                                  const ShardId& shardId,
                                                                  const std::string& dbname,
                                                                  const BSONObj& cmdObj) {
    auto shard = getShard(txn, shardId);
    auto response = _runCommandWithNotMasterRetries(
        _executor.get(), shard->getTargeter(), dbname, cmdObj, rpc::makeEmptyMetadata());
    if (response.getStatus() == ErrorCodes::ShardNotFound) {  // One retry if the shard isn't found.
        reload(txn);
        response = _runCommandWithNotMasterRetries(
            _executor.get(), shard->getTargeter(), dbname, cmdObj, rpc::makeEmptyMetadata());
    }
    if (!response.isOK()) {
        return response.getStatus();
    }
    return response.getValue().response;
}

StatusWith<ShardRegistry::CommandResponse> ShardRegistry::_runCommandWithNotMasterRetries(
    TaskExecutor* executor,
    RemoteCommandTargeter* targeter,
    const std::string& dbname,
    const BSONObj& cmdObj,
    const BSONObj& metadata) {
    const ReadPreferenceSetting readPref(ReadPreference::PrimaryOnly, TagSet{});

    for (int i = 0; i < kNotMasterNumRetries; ++i) {
        auto target = targeter->findHost(readPref);
        if (!target.isOK()) {
            if (ErrorCodes::NotMaster == target.getStatus()) {
                if (i == kNotMasterNumRetries - 1) {
                    // If we're out of retries don't bother sleeping, just return.
                    return target.getStatus();
                }
                sleepmillis(durationCount<Milliseconds>(kNotMasterRetryInterval));
                continue;
            }
            return target.getStatus();
        }

        auto response =
            _runCommandWithMetadata(executor, target.getValue(), dbname, cmdObj, metadata);
        if (!response.isOK()) {
            return response.getStatus();
        }

        Status commandStatus = getStatusFromCommandResult(response.getValue().response);
        if (ErrorCodes::NotMaster == commandStatus ||
            ErrorCodes::NotMasterNoSlaveOkCode == commandStatus) {
            targeter->markHostNotMaster(target.getValue());
            if (i == kNotMasterNumRetries - 1) {
                // If we're out of retries don't bother sleeping, just return.
                return commandStatus;
            }
            sleepmillis(durationCount<Milliseconds>(kNotMasterRetryInterval));
            continue;
        }

        return response.getValue();
    }

    MONGO_UNREACHABLE;
}

StatusWith<ShardRegistry::CommandResponse> ShardRegistry::_runCommandWithMetadata(
    TaskExecutor* executor,
    const HostAndPort& host,
    const std::string& dbName,
    const BSONObj& cmdObj,
    const BSONObj& metadata) {
    StatusWith<executor::RemoteCommandResponse> responseStatus =
        Status(ErrorCodes::InternalError, "Internal error running command");

    executor::RemoteCommandRequest request(host, dbName, cmdObj, metadata, kConfigCommandTimeout);
    auto callStatus =
        executor->scheduleRemoteCommand(request,
                                        [&responseStatus](const RemoteCommandCallbackArgs& args) {
                                            responseStatus = args.response;
                                        });
    if (!callStatus.isOK()) {
        return callStatus.getStatus();
    }

    // Block until the command is carried out
    executor->wait(callStatus.getValue());

    if (!responseStatus.isOK()) {
        return responseStatus.getStatus();
    }

    auto response = responseStatus.getValue();

    CommandResponse cmdResponse;
    cmdResponse.response = response.data;

    if (response.metadata.hasField(rpc::kReplSetMetadataFieldName)) {
        auto replParseStatus = rpc::ReplSetMetadata::readFromMetadata(response.metadata);

        if (!replParseStatus.isOK()) {
            return replParseStatus.getStatus();
        }

        // TODO: SERVER-19734 use config server snapshot time.
        cmdResponse.opTime = replParseStatus.getValue().getLastOpCommitted();
    }

    return cmdResponse;
}

}  // namespace mongo
