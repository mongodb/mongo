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

#include <set>

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/client/connection_string.h"
#include "mongo/client/replica_set_monitor.h"
#include "mongo/db/client.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/server_options.h"
#include "mongo/executor/network_connection_hook.h"
#include "mongo/executor/network_interface_factory.h"
#include "mongo/executor/network_interface_thread_pool.h"
#include "mongo/executor/task_executor.h"
#include "mongo/executor/task_executor_pool.h"
#include "mongo/executor/thread_pool_task_executor.h"
#include "mongo/rpc/metadata/egress_metadata_hook_list.h"
#include "mongo/s/catalog/sharding_catalog_client.h"
#include "mongo/s/catalog/type_shard.h"
#include "mongo/s/client/shard.h"
#include "mongo/s/client/shard_connection.h"
#include "mongo/s/client/shard_factory.h"
#include "mongo/s/grid.h"
#include "mongo/stdx/memory.h"
#include "mongo/stdx/mutex.h"
#include "mongo/util/log.h"
#include "mongo/util/map_util.h"
#include "mongo/util/mongoutils/str.h"
#include "mongo/util/scopeguard.h"

namespace mongo {

using std::shared_ptr;
using std::set;
using std::string;
using std::unique_ptr;
using std::vector;

using executor::NetworkInterface;
using executor::NetworkInterfaceThreadPool;
using executor::TaskExecutorPool;
using executor::ThreadPoolTaskExecutor;
using executor::TaskExecutor;
using CallbackArgs = TaskExecutor::CallbackArgs;
using CallbackHandle = TaskExecutor::CallbackHandle;


namespace {
const Seconds kRefreshPeriod(30);
}  // namespace

ShardRegistry::ShardRegistry(std::unique_ptr<ShardFactory> shardFactory,
                             const ConnectionString& configServerCS)
    : _shardFactory(std::move(shardFactory)), _initConfigServerCS(configServerCS) {}

ShardRegistry::~ShardRegistry() {
    shutdown();
}

void ShardRegistry::shutdown() {
    if (_executor && !_isShutdown) {
        LOG(1) << "Shutting down task executor for reloading shard registry";
        _executor->shutdown();
        _executor->join();
        _isShutdown = true;
    }
}

ConnectionString ShardRegistry::getConfigServerConnectionString() const {
    return getConfigShard()->getConnString();
}

StatusWith<shared_ptr<Shard>> ShardRegistry::getShard(OperationContext* txn,
                                                      const ShardId& shardId) {
    // If we know about the shard, return it.
    auto shard = _data.findByShardId(shardId);
    if (shard) {
        return shard;
    }

    // If we can't find the shard, attempt to reload the ShardRegistry.
    bool didReload = reload(txn);
    shard = _data.findByShardId(shardId);

    // If we found the shard, return it.
    if (shard) {
        return shard;
    }

    // If we did not find the shard but performed the reload
    // ourselves, return, because it means the shard does not exist.
    if (didReload) {
        return {ErrorCodes::ShardNotFound, str::stream() << "Shard " << shardId << " not found"};
    }

    // If we did not perform the reload ourselves (because there was a concurrent reload), force a
    // reload again to ensure that we have seen data at least as up to date as our first reload.
    reload(txn);
    shard = _data.findByShardId(shardId);

    if (shard) {
        return shard;
    }

    return {ErrorCodes::ShardNotFound, str::stream() << "Shard " << shardId << " not found"};
}

shared_ptr<Shard> ShardRegistry::getShardNoReload(const ShardId& shardId) {
    return _data.findByShardId(shardId);
}

shared_ptr<Shard> ShardRegistry::getShardForHostNoReload(const HostAndPort& host) {
    return _data.findByHostAndPort(host);
}

shared_ptr<Shard> ShardRegistry::getConfigShard() const {
    auto shard = _data.getConfigShard();
    invariant(shard);
    return shard;
}

unique_ptr<Shard> ShardRegistry::createConnection(const ConnectionString& connStr) const {
    return _shardFactory->createUniqueShard(ShardId("<unnamed>"), connStr);
}

shared_ptr<Shard> ShardRegistry::lookupRSName(const string& name) const {
    return _data.findByRSName(name);
}

void ShardRegistry::getAllShardIds(vector<ShardId>* all) const {
    std::set<ShardId> seen;
    _data.getAllShardIds(seen);
    all->assign(seen.begin(), seen.end());
}

void ShardRegistry::toBSON(BSONObjBuilder* result) const {
    _data.toBSON(result);
}

void ShardRegistry::updateReplSetHosts(const ConnectionString& newConnString) {
    invariant(newConnString.type() == ConnectionString::SET ||
              newConnString.type() == ConnectionString::CUSTOM);  // For dbtests

    // to prevent update config shard connection string during init
    stdx::unique_lock<stdx::mutex> lock(_reloadMutex);
    _data.rebuildShardIfExists(newConnString, _shardFactory.get());
}

void ShardRegistry::init() {
    stdx::unique_lock<stdx::mutex> reloadLock(_reloadMutex);
    invariant(_initConfigServerCS.isValid());
    auto configShard = _shardFactory->createShard(ShardId("config"), _initConfigServerCS);
    _data.addConfigShard(configShard);
    // set to invalid so it cant be started more than once.
    _initConfigServerCS = ConnectionString();
}

void ShardRegistry::startup() {
    // startup() must be called only once
    invariant(!_executor);

    auto hookList = stdx::make_unique<rpc::EgressMetadataHookList>();
    // TODO SERVER-27750: add LogicalTimeMetadataHook

    // construct task executor
    auto net = executor::makeNetworkInterface("ShardRegistryUpdater", nullptr, std::move(hookList));
    auto netPtr = net.get();
    _executor = stdx::make_unique<ThreadPoolTaskExecutor>(
        stdx::make_unique<NetworkInterfaceThreadPool>(netPtr), std::move(net));
    LOG(1) << "Starting up task executor for periodic reloading of ShardRegistry";
    _executor->startup();

    auto status =
        _executor->scheduleWork([this](const CallbackArgs& cbArgs) { _internalReload(cbArgs); });

    if (status.getStatus() == ErrorCodes::ShutdownInProgress) {
        LOG(1) << "Cant schedule Shard Registry reload. "
               << "Executor shutdown in progress";
        return;
    }

    if (!status.isOK()) {
        severe() << "Can't schedule ShardRegistry reload due to " << causedBy(status.getStatus());
        fassertFailed(40252);
    }
}

void ShardRegistry::_internalReload(const CallbackArgs& cbArgs) {
    LOG(1) << "Reloading shardRegistry";
    if (!cbArgs.status.isOK()) {
        warning() << "cant reload ShardRegistry " << causedBy(cbArgs.status);
        return;
    }

    Client::initThreadIfNotAlready("shard registry reload");
    auto txn = cc().makeOperationContext();

    try {
        reload(txn.get());
    } catch (const DBException& e) {
        log() << "Periodic reload of shard registry failed " << causedBy(e) << "; will retry after "
              << kRefreshPeriod;
    }

    // reschedule itself
    auto status =
        _executor->scheduleWorkAt(_executor->now() + kRefreshPeriod,
                                  [this](const CallbackArgs& cbArgs) { _internalReload(cbArgs); });

    if (status.getStatus() == ErrorCodes::ShutdownInProgress) {
        LOG(1) << "Cant schedule ShardRegistry reload. "
               << "Executor shutdown in progress";
        return;
    }

    if (!status.isOK()) {
        severe() << "Can't schedule ShardRegistry reload due to " << causedBy(status.getStatus());
        fassertFailed(40253);
    }
}

bool ShardRegistry::isUp() const {
    stdx::unique_lock<stdx::mutex> reloadLock(_reloadMutex);
    return _isUp;
}

bool ShardRegistry::reload(OperationContext* txn) {
    stdx::unique_lock<stdx::mutex> reloadLock(_reloadMutex);

    if (_reloadState == ReloadState::Reloading) {
        // Another thread is already in the process of reloading so no need to do duplicate work.
        // There is also an issue if multiple threads are allowed to call getAllShards()
        // simultaneously because there is no good way to determine which of the threads has the
        // more recent version of the data.
        do {
            _inReloadCV.wait(reloadLock);
        } while (_reloadState == ReloadState::Reloading);

        if (_reloadState == ReloadState::Idle) {
            return false;
        }
        // else proceed to reload since an error occured on the last reload attempt.
        invariant(_reloadState == ReloadState::Failed);
    }

    _reloadState = ReloadState::Reloading;
    reloadLock.unlock();

    auto nextReloadState = ReloadState::Failed;

    auto failGuard = MakeGuard([&] {
        if (!reloadLock.owns_lock()) {
            reloadLock.lock();
        }
        _reloadState = nextReloadState;
        _inReloadCV.notify_all();
    });


    ShardRegistryData currData(txn, _shardFactory.get());
    currData.addConfigShard(_data.getConfigShard());
    _data.swap(currData);

    // Remove RSMs that are not in the catalog any more.
    std::set<ShardId> removedShardIds;
    currData.getAllShardIds(removedShardIds);
    _data.shardIdSetDifference(removedShardIds);

    for (auto& shardId : removedShardIds) {
        auto shard = currData.findByShardId(shardId);
        invariant(shard);

        auto name = shard->getConnString().getSetName();
        ReplicaSetMonitor::remove(name);
    }

    nextReloadState = ReloadState::Idle;
    // first successful reload means that registry is up
    _isUp = true;
    return true;
}

void ShardRegistry::replicaSetChangeShardRegistryUpdateHook(
    const std::string& setName, const std::string& newConnectionString) {
    // Inform the ShardRegsitry of the new connection string for the shard.
    auto connString = fassertStatusOK(28805, ConnectionString::parse(newConnectionString));
    invariant(setName == connString.getSetName());
    grid.shardRegistry()->updateReplSetHosts(connString);
}

void ShardRegistry::replicaSetChangeConfigServerUpdateHook(const std::string& setName,
                                                           const std::string& newConnectionString) {
    // This is run in it's own thread. Exceptions escaping would result in a call to terminate.
    Client::initThread("replSetChange");
    auto txn = cc().makeOperationContext();

    try {
        std::shared_ptr<Shard> s = Grid::get(txn.get())->shardRegistry()->lookupRSName(setName);
        if (!s) {
            LOG(1) << "shard not found for set: " << newConnectionString
                   << " when attempting to inform config servers of updated set membership";
            return;
        }

        if (s->isConfig()) {
            // No need to tell the config servers their own connection string.
            return;
        }

        auto status = Grid::get(txn.get())->catalogClient(txn.get())->updateConfigDocument(
            txn.get(),
            ShardType::ConfigNS,
            BSON(ShardType::name(s->getId().toString())),
            BSON("$set" << BSON(ShardType::host(newConnectionString))),
            false,
            ShardingCatalogClient::kMajorityWriteConcern);
        if (!status.isOK()) {
            error() << "RSChangeWatcher: could not update config db for set: " << setName
                    << " to: " << newConnectionString << causedBy(status.getStatus());
        }
    } catch (const std::exception& e) {
        warning() << "caught exception while updating config servers: " << e.what();
    } catch (...) {
        warning() << "caught unknown exception while updating config servers";
    }
}

////////////// ShardRegistryData //////////////////

ShardRegistryData::ShardRegistryData(OperationContext* txn, ShardFactory* shardFactory) {
    _init(txn, shardFactory);
}

void ShardRegistryData::_init(OperationContext* txn, ShardFactory* shardFactory) {
    auto shardsStatus =
        grid.catalogClient(txn)->getAllShards(txn, repl::ReadConcernLevel::kMajorityReadConcern);

    if (!shardsStatus.isOK()) {
        uasserted(shardsStatus.getStatus().code(),
                  str::stream() << "could not get updated shard list from config server due to "
                                << shardsStatus.getStatus().reason());
    }

    auto shards = std::move(shardsStatus.getValue().value);
    auto reloadOpTime = std::move(shardsStatus.getValue().opTime);

    LOG(1) << "found " << shards.size()
           << " shards listed on config server(s) with lastVisibleOpTime: "
           << reloadOpTime.toBSON();

    // Ensure targeter exists for all shards and take shard connection string from the targeter.
    // Do this before re-taking the mutex to avoid deadlock with the ReplicaSetMonitor updating
    // hosts for a given shard.
    std::vector<std::tuple<std::string, ConnectionString>> shardsInfo;
    for (const auto& shardType : shards) {
        // This validation should ideally go inside the ShardType::validate call. However, doing
        // it there would prevent us from loading previously faulty shard hosts, which might have
        // been stored (i.e., the entire getAllShards call would fail).
        auto shardHostStatus = ConnectionString::parse(shardType.getHost());
        if (!shardHostStatus.isOK()) {
            warning() << "Unable to parse shard host " << shardHostStatus.getStatus().toString();
            continue;
        }

        shardsInfo.push_back(std::make_tuple(shardType.getName(), shardHostStatus.getValue()));
    }

    for (auto& shardInfo : shardsInfo) {
        if (std::get<0>(shardInfo) == "config") {
            continue;
        }

        auto shard = shardFactory->createShard(std::move(std::get<0>(shardInfo)),
                                               std::move(std::get<1>(shardInfo)));

        _addShard_inlock(std::move(shard), false);
    }
}

void ShardRegistryData::swap(ShardRegistryData& other) {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    _lookup.swap(other._lookup);
    _rsLookup.swap(other._rsLookup);
    _hostLookup.swap(other._hostLookup);
    _configShard.swap(other._configShard);
}

shared_ptr<Shard> ShardRegistryData::getConfigShard() const {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    return _configShard;
}

void ShardRegistryData::addConfigShard(std::shared_ptr<Shard> shard) {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    _configShard = shard;
    _addShard_inlock(shard, true);
}

shared_ptr<Shard> ShardRegistryData::findByRSName(const string& name) const {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    auto i = _rsLookup.find(name);
    return (i != _rsLookup.end()) ? i->second : nullptr;
}

shared_ptr<Shard> ShardRegistryData::findByHostAndPort(const HostAndPort& hostAndPort) const {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    return mapFindWithDefault(_hostLookup, hostAndPort);
}

shared_ptr<Shard> ShardRegistryData::findByShardId(const ShardId& shardId) const {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    return _findByShardId_inlock(shardId);
}

shared_ptr<Shard> ShardRegistryData::_findByShardId_inlock(const ShardId& shardId) const {
    auto i = _lookup.find(shardId);
    return (i != _lookup.end()) ? i->second : nullptr;
}

void ShardRegistryData::toBSON(BSONObjBuilder* result) const {
    // Need to copy, then sort by shardId.
    std::vector<std::pair<ShardId, std::string>> shards;
    {
        stdx::lock_guard<stdx::mutex> lk(_mutex);
        shards.reserve(_lookup.size());
        for (auto&& shard : _lookup) {
            shards.emplace_back(shard.first, shard.second->getConnString().toString());
        }
    }

    std::sort(std::begin(shards), std::end(shards));

    BSONObjBuilder mapBob(result->subobjStart("map"));
    for (auto&& shard : shards) {
        mapBob.append(shard.first, shard.second);
    }
}

void ShardRegistryData::getAllShardIds(std::set<ShardId>& seen) const {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    for (auto i = _lookup.begin(); i != _lookup.end(); ++i) {
        const auto& s = i->second;
        if (s->getId().toString() == "config") {
            continue;
        }
        seen.insert(s->getId());
    }
}

void ShardRegistryData::shardIdSetDifference(std::set<ShardId>& diff) const {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    for (auto i = _lookup.begin(); i != _lookup.end(); ++i) {
        invariant(i->second);
        auto res = diff.find(i->second->getId());
        if (res != diff.end()) {
            diff.erase(res);
        }
    }
}

void ShardRegistryData::rebuildShardIfExists(const ConnectionString& newConnString,
                                             ShardFactory* factory) {
    stdx::unique_lock<stdx::mutex> updateConnStringLock(_mutex);
    auto it = _rsLookup.find(newConnString.getSetName());
    if (it == _rsLookup.end()) {
        return;
    }

    _rebuildShard_inlock(newConnString, factory);
}


void ShardRegistryData::_rebuildShard_inlock(const ConnectionString& newConnString,
                                             ShardFactory* factory) {
    auto it = _rsLookup.find(newConnString.getSetName());
    invariant(it->second);
    auto shard = factory->createShard(it->second->getId(), newConnString);
    _addShard_inlock(shard, true);
    if (shard->isConfig()) {
        _configShard = shard;
    }
}

void ShardRegistryData::_addShard_inlock(const std::shared_ptr<Shard>& shard, bool useOriginalCS) {
    const ShardId shardId = shard->getId();

    const ConnectionString connString =
        useOriginalCS ? shard->originalConnString() : shard->getConnString();

    auto currentShard = _findByShardId_inlock(shardId);
    if (currentShard) {
        auto oldConnString = currentShard->originalConnString();

        if (oldConnString.toString() != connString.toString()) {
            log() << "Updating ShardRegistry connection string for shard " << currentShard->getId()
                  << " from: " << oldConnString.toString() << " to: " << connString.toString();
        }

        for (const auto& host : oldConnString.getServers()) {
            _lookup.erase(host.toString());
            _hostLookup.erase(host);
        }
        _lookup.erase(oldConnString.toString());
    }

    _lookup[shard->getId()] = shard;

    LOG(3) << "Adding shard " << shard->getId() << ", with CS " << connString.toString();
    if (connString.type() == ConnectionString::SET) {
        _rsLookup[connString.getSetName()] = shard;
    } else if (connString.type() == ConnectionString::CUSTOM) {
        // CUSTOM connection strings (ie "$dummy:10000) become DBDirectClient connections which
        // always return "localhost" as their response to getServerAddress().  This is just for
        // making dbtest work.
        _lookup[ShardId("localhost")] = shard;
        _hostLookup[HostAndPort("localhost")] = shard;
    }

    // TODO: The only reason to have the shard host names in the lookup table is for the
    // setShardVersion call, which resolves the shard id from the shard address. This is
    // error-prone and will go away eventually when we switch all communications to go through
    // the remote command runner and all nodes are sharding aware by default.
    _lookup[connString.toString()] = shard;

    for (const HostAndPort& hostAndPort : connString.getServers()) {
        _lookup[hostAndPort.toString()] = shard;
        _hostLookup[hostAndPort] = shard;
    }
}

}  // namespace mongo
