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
#include "mongo/s/catalog/catalog_manager.h"
#include "mongo/s/catalog/type_shard.h"
#include "mongo/s/client/shard.h"
#include "mongo/s/client/shard_connection.h"
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
}  // unnamed namespace

ShardRegistry::ShardRegistry(std::unique_ptr<RemoteCommandTargeterFactory> targeterFactory,
                             std::unique_ptr<executor::TaskExecutor> executor,
                             executor::NetworkInterface* network)
    : _targeterFactory(std::move(targeterFactory)),
      _executor(std::move(executor)),
      _network(network),
      _catalogManager(nullptr) {}

ShardRegistry::~ShardRegistry() = default;

void ShardRegistry::init(CatalogManager* catalogManager) {
    invariant(!_catalogManager);
    _catalogManager = catalogManager;

    // add config shard registry entry so know it's always there
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    _addConfigShard_inlock();
}

void ShardRegistry::startup() {
    _executor->startup();
}

void ShardRegistry::shutdown() {
    _executor->shutdown();
    _executor->join();
}

void ShardRegistry::reload() {
    vector<ShardType> shards;
    Status status = _catalogManager->getAllShards(&shards);
    massert(13632, "couldn't get updated shard list from config server", status.isOK());

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

shared_ptr<Shard> ShardRegistry::getShard(const ShardId& shardId) {
    shared_ptr<Shard> shard = _findUsingLookUp(shardId);
    if (shard) {
        return shard;
    }

    // If we can't find the shard, we might just need to reload the cache
    reload();

    return _findUsingLookUp(shardId);
}

unique_ptr<Shard> ShardRegistry::createConnection(const ConnectionString& connStr) const {
    return stdx::make_unique<Shard>(
        "<unnamed>", connStr, std::move(_targeterFactory->create(connStr)));
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
    configServerShard.setHost(_catalogManager->connectionString().toString());
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

    // Sync cluster connections (legacy config server) do not go through the normal targeting
    // mechanism and must only be reachable through CatalogManagerLegacy or legacy-style
    // queries and inserts. Do not create targeter for these connections. This code should go
    // away after 3.2 is released.
    if (shardHost.type() == ConnectionString::SYNC) {
        _lookup[shardType.getName()] =
            std::make_shared<Shard>(shardType.getName(), shardHost, nullptr);
        return;
    }

    // Non-SYNC shards
    shared_ptr<Shard> shard = std::make_shared<Shard>(
        shardType.getName(), shardHost, std::move(_targeterFactory->create(shardHost)));

    _lookup[shardType.getName()] = shard;

    // TODO: The only reason to have the shard host names in the lookup table is for the
    // setShardVersion call, which resolves the shard id from the shard address. This is
    // error-prone and will go away eventually when we switch all communications to go through
    // the remote command runner.
    _lookup[shardType.getHost()] = shard;

    for (const HostAndPort& hostAndPort : shardHost.getServers()) {
        _lookup[hostAndPort.toString()] = shard;

        // Maintain a mapping from host to shard it belongs to for the case where we need to
        // update the shard connection string on reconfigurations.
        if (shardHost.type() == ConnectionString::SET) {
            _rsLookup[hostAndPort.toString()] = shard;
        }
    }

    if (shardHost.type() == ConnectionString::SET) {
        _rsLookup[shardHost.getSetName()] = shard;
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

StatusWith<std::vector<BSONObj>> ShardRegistry::exhaustiveFind(const HostAndPort& host,
                                                               const NamespaceString& nss,
                                                               const BSONObj& query,
                                                               const BSONObj& sort,
                                                               boost::optional<long long> limit) {
    // If for some reason the callback never gets invoked, we will return this status
    Status status = Status(ErrorCodes::InternalError, "Internal error running find command");
    vector<BSONObj> results;

    auto fetcherCallback = [&status, &results](const Fetcher::QueryResponseStatus& dataStatus,
                                               Fetcher::NextAction* nextAction) {

        // Throw out any accumulated results on error
        if (!dataStatus.isOK()) {
            status = dataStatus.getStatus();
            results.clear();
            return;
        }

        auto& data = dataStatus.getValue();
        for (const BSONObj& doc : data.documents) {
            results.push_back(std::move(doc.getOwned()));
        }

        status = Status::OK();
    };

    unique_ptr<LiteParsedQuery> findCmd(
        fassertStatusOK(28688, LiteParsedQuery::makeAsFindCmd(nss, query, sort, limit)));

    QueryFetcher fetcher(_executor.get(), host, nss, findCmd->asFindCommand(), fetcherCallback);

    Status scheduleStatus = fetcher.schedule();
    if (!scheduleStatus.isOK()) {
        return scheduleStatus;
    }

    fetcher.wait();

    if (!status.isOK()) {
        return status;
    }

    return results;
}

StatusWith<BSONObj> ShardRegistry::runCommand(const HostAndPort& host,
                                              const std::string& dbName,
                                              const BSONObj& cmdObj) {
    StatusWith<RemoteCommandResponse> responseStatus =
        Status(ErrorCodes::InternalError, "Internal error running command");

    RemoteCommandRequest request(host, dbName, cmdObj, kConfigCommandTimeout);
    auto callStatus =
        _executor->scheduleRemoteCommand(request,
                                         [&responseStatus](const RemoteCommandCallbackArgs& args) {
                                             responseStatus = args.response;
                                         });
    if (!callStatus.isOK()) {
        return callStatus.getStatus();
    }

    // Block until the command is carried out
    _executor->wait(callStatus.getValue());

    if (!responseStatus.isOK()) {
        return responseStatus.getStatus();
    }

    return responseStatus.getValue().data;
}

}  // namespace mongo
