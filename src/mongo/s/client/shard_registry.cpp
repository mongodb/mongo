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

#include "mongo/s/client/shard_registry.h"

#include "mongo/client/replica_set_monitor.h"
#include "mongo/db/client.h"
#include "mongo/db/vector_clock.h"
#include "mongo/db/vector_clock_metadata_hook.h"
#include "mongo/executor/network_interface_factory.h"
#include "mongo/executor/network_interface_thread_pool.h"
#include "mongo/executor/task_executor_pool.h"
#include "mongo/executor/thread_pool_task_executor.h"
#include "mongo/logv2/log.h"
#include "mongo/rpc/metadata/egress_metadata_hook_list.h"
#include "mongo/s/catalog/sharding_catalog_client.h"
#include "mongo/s/catalog/type_shard.h"
#include "mongo/s/grid.h"
#include "mongo/util/future_util.h"
#include "mongo/util/str.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding


namespace mongo {

namespace {

const Seconds kRefreshPeriod(30);

const Backoff kExponentialBackoff(Seconds(1), Milliseconds::max());

}  // namespace

using CallbackArgs = executor::TaskExecutor::CallbackArgs;

ShardRegistry::ShardRegistry(ServiceContext* service,
                             std::unique_ptr<ShardFactory> shardFactory,
                             const ConnectionString& configServerCS,
                             std::vector<ShardRemovalHook> shardRemovalHooks)
    : _service(service),
      _shardFactory(std::move(shardFactory)),
      _initConfigServerCS(configServerCS),
      _shardRemovalHooks(std::move(shardRemovalHooks)),
      _threadPool([] {
          ThreadPool::Options options;
          options.poolName = "ShardRegistry";
          options.minThreads = 0;
          options.maxThreads = 1;
          return options;
      }()),
      _cache(std::make_unique<Cache>(
          _cacheMutex,
          _service,
          _threadPool,
          [this](OperationContext* opCtx,
                 const Singleton& key,
                 const Cache::ValueHandle& cachedData,
                 const Time& timeInStore) { return _lookup(opCtx, key, cachedData, timeInStore); },
          1 /* cacheSize */)) {

    invariant(_initConfigServerCS.isValid());
    _threadPool.startup();
}

ShardRegistry::~ShardRegistry() {
    shutdown();
}

void ShardRegistry::init() {
    invariant(!_isInitialized.load());

    LOGV2_DEBUG(5123000,
                1,
                "Initializing ShardRegistry",
                "configServers"_attr = _initConfigServerCS.toString());

    /* The creation of the config shard object will intialize the associated RSM monitor that in
     * turn will call ShardRegistry::updateReplSetHosts(). Hence the config shard object MUST be
     * created after the ShardRegistry is fully constructed. This is why `_configShardData`
     * is initialized here rather than in the ShardRegistry constructor.
     */
    {
        stdx::lock_guard<Latch> lk(_mutex);
        _configShardData = ShardRegistryData::createWithConfigShardOnly(
            _shardFactory->createShard(ShardId::kConfigServerId, _initConfigServerCS));
        _latestConnStrings[_initConfigServerCS.getSetName()] = _initConfigServerCS;
    }

    _isInitialized.store(true);
}

ShardRegistry::Cache::LookupResult ShardRegistry::_lookup(OperationContext* opCtx,
                                                          const Singleton& key,
                                                          const Cache::ValueHandle& cachedData,
                                                          const Time& timeInStore) {
    invariant(key == _kSingleton);

    auto lastForcedReloadIncrement = _forceReloadIncrement.load();

    LOGV2_DEBUG(4620250,
                2,
                "Starting ShardRegistry::_lookup",
                "cachedData"_attr = cachedData ? cachedData->toBSON() : BSONObj{},
                "cachedData.getTime()"_attr = cachedData ? cachedData.getTime() : Time{},
                "timeInStore"_attr = timeInStore,
                "lastForcedReloadIncrement"_attr = lastForcedReloadIncrement);

    // Check if we need to refresh from the configsvrs.  If so, then do that and get the results,
    // otherwise (this is a lookup only to incorporate updated connection strings from the RSM),
    // then get the equivalent values from the previously cached data.
    auto [returnData, returnTopologyTime, removedShards] =
        [&]() -> std::tuple<ShardRegistryData, Timestamp, ShardRegistryData::ShardMap> {
        if (!cachedData) {
            auto [reloadedData, maxTopologyTime] =
                ShardRegistryData::createFromCatalogClient(opCtx, _shardFactory.get());

            return {std::move(reloadedData), std::move(maxTopologyTime), {}};
        } else if (timeInStore.topologyTime > cachedData.getTime().topologyTime ||
                   lastForcedReloadIncrement > cachedData.getTime().forceReloadIncrement) {
            auto [reloadedData, maxTopologyTime] =
                ShardRegistryData::createFromCatalogClient(opCtx, _shardFactory.get());

            auto [mergedData, removedShards] =
                ShardRegistryData::mergeExisting(*cachedData, reloadedData);

            return {std::move(mergedData), std::move(maxTopologyTime), std::move(removedShards)};
        } else {
            return {*cachedData, cachedData.getTime().topologyTime, {}};
        }
    }();

    // Always apply the latest conn strings.
    auto [latestConnStrings, rsmIncrementForConnStrings] = _getLatestConnStrings();

    for (const auto& latestConnString : latestConnStrings) {
        auto shard = returnData.findByRSName(latestConnString.first.toString());
        if (shard == nullptr || shard->getConnString() == latestConnString.second) {
            continue;
        }

        auto newData = ShardRegistryData::createFromExisting(
            returnData, latestConnString.second, _shardFactory.get());
        returnData = newData;
    }

    // Remove RSMs that are not in the catalog any more.
    for (auto& pair : removedShards) {
        auto& shardId = pair.first;
        auto& shard = pair.second;
        invariant(shard);

        auto name = shard->getConnString().getSetName();
        ReplicaSetMonitor::remove(name);
        _removeReplicaSet(name);
        for (auto& callback : _shardRemovalHooks) {
            // Run callbacks asynchronously.
            // TODO SERVER-50906: Consider running these callbacks synchronously.
            ExecutorFuture<void>(Grid::get(opCtx)->getExecutorPool()->getFixedExecutor())
                .getAsync([=](const Status&) { callback(shardId); });
        }
    }

    Time returnTime{returnTopologyTime, rsmIncrementForConnStrings, lastForcedReloadIncrement};
    LOGV2_DEBUG(4620251,
                2,
                "Finished ShardRegistry::_lookup",
                "returnData"_attr = returnData.toBSON(),
                "returnTime"_attr = returnTime);
    return Cache::LookupResult{returnData, returnTime};
}

void ShardRegistry::startupPeriodicReloader(OperationContext* opCtx) {
    // startupPeriodicReloader() must be called only once
    invariant(!_executor);

    auto hookList = std::make_unique<rpc::EgressMetadataHookList>();
    hookList->addHook(std::make_unique<rpc::VectorClockMetadataHook>(opCtx->getServiceContext()));

    // construct task executor
    auto net = executor::makeNetworkInterface("ShardRegistryUpdater", nullptr, std::move(hookList));
    auto netPtr = net.get();
    _executor = std::make_shared<executor::ThreadPoolTaskExecutor>(
        std::make_unique<executor::NetworkInterfaceThreadPool>(netPtr), std::move(net));
    LOGV2_DEBUG(22724, 1, "Starting up task executor for periodic reloading of ShardRegistry");
    _executor->startup();

    AsyncTry([this] {
        LOGV2_DEBUG(22726, 1, "Reloading shardRegistry");
        return _reloadAsync();
    })
        .until([](auto&& sw) {
            if (!sw.isOK()) {
                LOGV2(22727,
                      "Error running periodic reload of shard registry",
                      "error"_attr = redact(sw.getStatus()),
                      "shardRegistryReloadInterval"_attr = kRefreshPeriod);
            }
            // Continue until the _executor will shutdown
            return false;
        })
        .withDelayBetweenIterations(kRefreshPeriod)  // This call is optional.
        .on(_executor, CancellationToken::uncancelable())
        .getAsync([](auto&& sw) {
            LOGV2_DEBUG(22725,
                        1,
                        "Exiting periodic shard registry reloader",
                        "reason"_attr = redact(sw.getStatus()));
        });
}

void ShardRegistry::shutdownPeriodicReloader() {
    if (_executor) {
        LOGV2_DEBUG(22723, 1, "Shutting down task executor for reloading shard registry");
        _executor->shutdown();
        _executor->join();
        _executor.reset();
    }
}

void ShardRegistry::shutdown() {
    if (!_isShutdown.load()) {
        LOGV2_DEBUG(4620235, 1, "Shutting down shard registry");
        _threadPool.shutdown();

        shutdownPeriodicReloader();

        _threadPool.join();
        _isShutdown.store(true);
    }
}

ConnectionString ShardRegistry::getConfigServerConnectionString() const {
    return getConfigShard()->getConnString();
}

std::shared_ptr<Shard> ShardRegistry::getConfigShard() const {
    stdx::lock_guard<Latch> lk(_mutex);
    return _configShardData.findShard(ShardId::kConfigServerId);
}

StatusWith<std::shared_ptr<Shard>> ShardRegistry::getShard(OperationContext* opCtx,
                                                           const ShardId& shardId) {
    // First check if this is a non config shard lookup
    // This call will may be blocking if there is an ongoing or a need of a cache rebuild
    if (auto shard = _getData(opCtx)->findShard(shardId)) {
        return shard;
    }

    // then check if this is a config shard (this call is blocking in any case)
    {
        stdx::lock_guard<Latch> lk(_mutex);
        if (auto shard = _configShardData.findShard(shardId)) {
            return shard;
        }
    }

    // Reload and try again if the shard was not in the registry
    reload(opCtx);
    if (auto shard = _getData(opCtx)->findShard(shardId)) {
        return shard;
    }

    return {ErrorCodes::ShardNotFound, str::stream() << "Shard " << shardId << " not found"};
}

SemiFuture<std::shared_ptr<Shard>> ShardRegistry::getShard(ExecutorPtr executor,
                                                           const ShardId& shardId) noexcept {

    // Fetch the shard registry data associated to the latest known topology time
    return _getDataAsync()
        .thenRunOn(executor)
        .then([this, executor, shardId](auto&& cachedData) {
            // First check if this is a non config shard lookup
            if (auto shard = cachedData->findShard(shardId)) {
                return SemiFuture<std::shared_ptr<Shard>>::makeReady(std::move(shard));
            }

            // then check if this is a config shard (this call is blocking in any case)
            {
                stdx::lock_guard<Latch> lk(_mutex);
                if (auto shard = _configShardData.findShard(shardId)) {
                    return SemiFuture<std::shared_ptr<Shard>>::makeReady(std::move(shard));
                }
            }

            // If the shard was not found, force reload the shard regitry data and try again.
            //
            // This is to cover the following scenario:
            // 1. Primary of the replicaset fetch the list of shards and store it on disk
            // 2. Primary crash before the latest VectorClock topology time is majority written to
            //    disk
            // 3. A new primary with a stale ShardRegistry is elected and read the set of shards
            //    from disk and calls ShardRegistry::getShard

            return _reloadAsync()
                .thenRunOn(executor)
                .then([this, executor, shardId](auto&& cachedData) -> std::shared_ptr<Shard> {
                    auto shard = cachedData->findShard(shardId);
                    uassert(ErrorCodes::ShardNotFound,
                            str::stream() << "Shard " << shardId << " not found",
                            shard);
                    return shard;
                })
                .semi();
        })
        .semi();
}

std::vector<ShardId> ShardRegistry::getAllShardIds(OperationContext* opCtx) {
    auto shardIds = _getData(opCtx)->getAllShardIds();
    if (shardIds.empty()) {
        reload(opCtx);
        shardIds = _getData(opCtx)->getAllShardIds();
    }
    return shardIds;
}

int ShardRegistry::getNumShards(OperationContext* opCtx) {
    return getAllShardIds(opCtx).size();
}

std::pair<std::vector<ShardRegistry::LatestConnStrings::value_type>, ShardRegistry::Increment>
ShardRegistry::_getLatestConnStrings() const {
    stdx::unique_lock<Latch> lock(_mutex);
    return {{_latestConnStrings.begin(), _latestConnStrings.end()}, _rsmIncrement.load()};
}

void ShardRegistry::_removeReplicaSet(const std::string& setName) {
    stdx::lock_guard<Latch> lk(_mutex);
    _latestConnStrings.erase(setName);
}

void ShardRegistry::updateReplSetHosts(const ConnectionString& givenConnString,
                                       ConnectionStringUpdateType updateType) {
    invariant(givenConnString.type() == ConnectionString::ConnectionType::kReplicaSet ||
              givenConnString.type() == ConnectionString::ConnectionType::kCustom);  // For dbtests

    auto setName = givenConnString.getSetName();

    {
        stdx::lock_guard<Latch> lk(_mutex);

        ConnectionString newConnString =
            (updateType == ConnectionStringUpdateType::kPossible &&
             _latestConnStrings.find(setName) != _latestConnStrings.end())
            ? _latestConnStrings[setName].makeUnionWith(givenConnString)
            : givenConnString;

        LOGV2_DEBUG(5123001,
                    1,
                    "Updating ShardRegistry connection string",
                    "updateType"_attr = updateType == ConnectionStringUpdateType::kPossible
                        ? "possible"
                        : "confirmed",
                    "currentConnString"_attr = _latestConnStrings[setName].toString(),
                    "givenConnString"_attr = givenConnString.toString(),
                    "newConnString"_attr = newConnString.toString());

        _latestConnStrings[setName] = newConnString;

        if (auto shard = _configShardData.findByRSName(setName)) {
            auto newData = ShardRegistryData::createFromExisting(
                _configShardData, newConnString, _shardFactory.get());
            _configShardData = newData;

        } else {
            auto value = _rsmIncrement.addAndFetch(1);
            LOGV2_DEBUG(4620252,
                        2,
                        "Incrementing the RSM timestamp after receiving updated connection string",
                        "newConnString"_attr = newConnString,
                        "newRSMIncrement"_attr = value);
        }
    }

    // Schedule a lookup, to incorporate the new connection string.
    _getDataAsync()
        .thenRunOn(Grid::get(_service)->getExecutorPool()->getFixedExecutor())
        .ignoreValue()
        .getAsync([](const Status& status) {
            if (!status.isOK()) {
                LOGV2(4620201,
                      "Error running reload of ShardRegistry for RSM update, caused by {error}",
                      "Error running reload of ShardRegistry for RSM update",
                      "error"_attr = redact(status));
            }
        });
}

std::unique_ptr<Shard> ShardRegistry::createConnection(const ConnectionString& connStr) const {
    return _shardFactory->createUniqueShard(ShardId("<unnamed>"), connStr);
}

void ShardRegistry::toBSON(BSONObjBuilder* result) const {
    BSONObjBuilder map;
    BSONObjBuilder hosts;
    BSONObjBuilder connStrings;
    if (auto data = _getCachedData()) {
        data->toBSON(&map, &hosts, &connStrings);
    }
    {
        stdx::lock_guard<Latch> lk(_mutex);
        _configShardData.toBSON(&map, &hosts, &connStrings);
    }
    result->append("map", map.obj());
    result->append("hosts", hosts.obj());
    result->append("connStrings", connStrings.obj());
}

void ShardRegistry::reload(OperationContext* opCtx) {
    _reloadAsync().get(opCtx);
}

SharedSemiFuture<ShardRegistry::Cache::ValueHandle> ShardRegistry::_reloadAsync() {
    // Make the next acquire do a lookup.
    auto value = _forceReloadIncrement.addAndFetch(1);
    LOGV2_DEBUG(4620253, 2, "Forcing ShardRegistry reload", "newForceReloadIncrement"_attr = value);

    // Force it to actually happen now.
    return _getDataAsync();
}

void ShardRegistry::updateReplicaSetOnConfigServer(ServiceContext* serviceContext,
                                                   const ConnectionString& connStr) noexcept {
    ThreadClient tc("UpdateReplicaSetOnConfigServer", serviceContext);

    auto opCtx = tc->makeOperationContext();
    auto const grid = Grid::get(opCtx.get());
    auto sr = grid->shardRegistry();

    // First check if this is a config shard lookup.
    {
        stdx::lock_guard<Latch> lk(sr->_mutex);
        if (auto shard = sr->_configShardData.findByRSName(connStr.getSetName())) {
            // No need to tell the config servers their own connection string.
            return;
        }
    }

    auto swRegistryData = sr->_getDataAsync().getNoThrow(opCtx.get());
    if (!swRegistryData.isOK()) {
        LOGV2_DEBUG(
            6791401,
            1,
            "Error updating replica set on config servers. Failed to fetch shard registry data",
            "replicaSetConnectionStr"_attr = connStr,
            "error"_attr = swRegistryData.getStatus());
        return;
    }

    auto shard = swRegistryData.getValue()->findByRSName(connStr.getSetName());
    if (!shard) {
        LOGV2_DEBUG(6791402,
                    1,
                    "Error updating replica set on config servers. Couldn't find shard",
                    "replicaSetConnectionStr"_attr = connStr);
        return;
    }

    auto swWasUpdated = grid->catalogClient()->updateConfigDocument(
        opCtx.get(),
        NamespaceString::kConfigsvrShardsNamespace,
        BSON(ShardType::name(shard->getId().toString())),
        BSON("$set" << BSON(ShardType::host(connStr.toString()))),
        false,
        ShardingCatalogClient::kMajorityWriteConcern);
    auto status = swWasUpdated.getStatus();
    if (!status.isOK()) {
        LOGV2_ERROR(22736,
                    "Error updating replica set on config server",
                    "replicaSetConnectionStr"_attr = connStr,
                    "error"_attr = redact(status));
    }
}

SharedSemiFuture<ShardRegistry::Cache::ValueHandle> ShardRegistry::_getDataAsync() {
    // Update the time the cache should be aiming for.
    auto now = VectorClock::get(_service)->getTime();
    // The topologyTime should be advanced to the gossiped topologyTime.
    Timestamp topologyTime = now.topologyTime().asTimestamp();
    _cache->advanceTimeInStore(
        _kSingleton, Time(topologyTime, _rsmIncrement.load(), _forceReloadIncrement.load()));

    return _cache->acquireAsync(_kSingleton, CacheCausalConsistency::kLatestKnown);
}

ShardRegistry::Cache::ValueHandle ShardRegistry::_getData(OperationContext* opCtx) {
    return _getDataAsync().get(opCtx);
}

bool ShardRegistry::isConfigServer(const HostAndPort& host) const {
    const auto configsvrConnString = getConfigServerConnectionString();
    const auto& configsvrHosts = configsvrConnString.getServers();
    return std::find(configsvrHosts.begin(), configsvrHosts.end(), host) != configsvrHosts.end();
}

// TODO SERVER-50206: Remove usage of these non-causally consistent accessors.

ShardRegistry::Cache::ValueHandle ShardRegistry::_getCachedData() const {
    return _cache->peekLatestCached(_kSingleton);
}

std::shared_ptr<Shard> ShardRegistry::getShardForHostNoReload(const HostAndPort& host) const {
    // First check if this is a config shard lookup.
    {
        stdx::lock_guard<Latch> lk(_mutex);
        if (auto shard = _configShardData.findByHostAndPort(host)) {
            return shard;
        }
    }
    auto data = _getCachedData();
    if (!data) {
        return nullptr;
    }

    return data->findByHostAndPort(host);
}

////////////// ShardRegistryData //////////////////

ShardRegistryData ShardRegistryData::createWithConfigShardOnly(std::shared_ptr<Shard> configShard) {
    ShardRegistryData data;
    data._addShard(configShard);
    return data;
}

std::pair<ShardRegistryData, Timestamp> ShardRegistryData::createFromCatalogClient(
    OperationContext* opCtx, ShardFactory* shardFactory) {
    auto const catalogClient = Grid::get(opCtx)->catalogClient();

    auto shardsAndOpTime = uassertStatusOKWithContext(
        catalogClient->getAllShards(opCtx, repl::ReadConcernLevel::kMajorityReadConcern),
        "could not get updated shard list from config server");

    auto shards = std::move(shardsAndOpTime.value);
    auto reloadOpTime = std::move(shardsAndOpTime.opTime);

    LOGV2_DEBUG(22731,
                1,
                "Found {shardsNumber} shards listed on config server(s) with lastVisibleOpTime: "
                "{lastVisibleOpTime}",
                "Succesfully retrieved updated shard list from config server",
                "shardsNumber"_attr = shards.size(),
                "lastVisibleOpTime"_attr = reloadOpTime);

    // Ensure targeter exists for all shards and take shard connection string from the targeter.
    // Do this before re-taking the mutex to avoid deadlock with the ReplicaSetMonitor updating
    // hosts for a given shard.
    std::vector<std::tuple<std::string, ConnectionString>> shardsInfo;
    Timestamp maxTopologyTime;
    for (const auto& shardType : shards) {
        // This validation should ideally go inside the ShardType::validate call. However, doing
        // it there would prevent us from loading previously faulty shard hosts, which might have
        // been stored (i.e., the entire getAllShards call would fail).
        auto shardHostStatus = ConnectionString::parse(shardType.getHost());
        if (!shardHostStatus.isOK()) {
            LOGV2_WARNING(22735,
                          "Error parsing shard host caused by {error}",
                          "Error parsing shard host",
                          "error"_attr = redact(shardHostStatus.getStatus()));
            continue;
        }

        if (auto thisTopologyTime = shardType.getTopologyTime();
            maxTopologyTime < thisTopologyTime) {
            maxTopologyTime = thisTopologyTime;
        }

        shardsInfo.push_back(std::make_tuple(shardType.getName(), shardHostStatus.getValue()));
    }

    ShardRegistryData data;
    for (auto& shardInfo : shardsInfo) {
        if (std::get<0>(shardInfo) == "config") {
            continue;
        }

        auto shard = shardFactory->createShard(std::move(std::get<0>(shardInfo)),
                                               std::move(std::get<1>(shardInfo)));

        data._addShard(std::move(shard));
    }
    return {data, maxTopologyTime};
}

std::pair<ShardRegistryData, ShardRegistryData::ShardMap> ShardRegistryData::mergeExisting(
    const ShardRegistryData& alreadyCachedData, const ShardRegistryData& configServerData) {
    ShardRegistryData mergedData(configServerData);

    // For connstrings and hosts, prefer values from alreadyCachedData to whatever might have been
    // fetched from the configsvrs.
    for (auto it = alreadyCachedData._connStringLookup.begin();
         it != alreadyCachedData._connStringLookup.end();
         ++it) {
        mergedData._connStringLookup[it->first] = it->second;
    }
    for (auto it = alreadyCachedData._hostLookup.begin(); it != alreadyCachedData._hostLookup.end();
         ++it) {
        mergedData._hostLookup[it->first] = it->second;
    }

    // Find the shards that are no longer present.
    ShardMap removedShards;
    for (auto i = alreadyCachedData._shardIdLookup.begin();
         i != alreadyCachedData._shardIdLookup.end();
         ++i) {
        invariant(i->second);
        if (mergedData._shardIdLookup.find(i->second->getId()) == mergedData._shardIdLookup.end()) {
            removedShards[i->second->getId()] = i->second;
        }
    }

    return {mergedData, removedShards};
}

ShardRegistryData ShardRegistryData::createFromExisting(const ShardRegistryData& existingData,
                                                        const ConnectionString& newConnString,
                                                        ShardFactory* shardFactory) {
    ShardRegistryData data(existingData);

    auto it = data._rsLookup.find(newConnString.getSetName());
    if (it == data._rsLookup.end()) {
        return data;
    }
    invariant(it->second);
    auto updatedShard = shardFactory->createShard(it->second->getId(), newConnString);
    data._addShard(updatedShard);

    return data;
}

std::shared_ptr<Shard> ShardRegistryData::findByRSName(const std::string& name) const {
    auto i = _rsLookup.find(name);
    return (i != _rsLookup.end()) ? i->second : nullptr;
}

std::shared_ptr<Shard> ShardRegistryData::_findByConnectionString(
    const std::string& connectionString) const {
    auto i = _connStringLookup.find(connectionString);
    return (i != _connStringLookup.end()) ? i->second : nullptr;
}

std::shared_ptr<Shard> ShardRegistryData::findByHostAndPort(const HostAndPort& hostAndPort) const {
    auto i = _hostLookup.find(hostAndPort);
    return (i != _hostLookup.end()) ? i->second : nullptr;
}

std::shared_ptr<Shard> ShardRegistryData::_findByShardId(const ShardId& shardId) const {
    auto i = _shardIdLookup.find(shardId);
    return (i != _shardIdLookup.end()) ? i->second : nullptr;
}

std::shared_ptr<Shard> ShardRegistryData::findShard(const ShardId& shardId) const {
    auto shard = _findByShardId(shardId);
    if (shard) {
        return shard;
    }

    shard = _findByConnectionString(shardId.toString());
    if (shard) {
        return shard;
    }

    StatusWith<HostAndPort> swHostAndPort = HostAndPort::parse(shardId.toString());
    if (swHostAndPort.isOK()) {
        shard = findByHostAndPort(swHostAndPort.getValue());
        if (shard) {
            return shard;
        }
    }

    return nullptr;
}

std::vector<std::shared_ptr<Shard>> ShardRegistryData::getAllShards() const {
    std::vector<std::shared_ptr<Shard>> result;
    result.reserve(_shardIdLookup.size());
    for (auto&& shard : _shardIdLookup) {
        result.emplace_back(shard.second);
    }
    return result;
}

std::vector<ShardId> ShardRegistryData::getAllShardIds() const {
    std::vector<ShardId> shardIds;
    shardIds.reserve(_shardIdLookup.size());
    std::transform(_shardIdLookup.begin(),
                   _shardIdLookup.end(),
                   std::back_inserter(shardIds),
                   [](const auto& shard) { return shard.second->getId(); });
    return shardIds;
}

void ShardRegistryData::_addShard(std::shared_ptr<Shard> shard) {
    const ShardId shardId = shard->getId();
    const ConnectionString connString = shard->getConnString();

    auto currentShard = findShard(shardId);
    if (currentShard) {
        for (const auto& host : connString.getServers()) {
            _hostLookup.erase(host);
        }
        _connStringLookup.erase(connString.toString());
    }

    _shardIdLookup[shard->getId()] = shard;

    LOGV2_DEBUG(22733,
                3,
                "Adding new shard {shardId} with connection string {shardConnectionString} to "
                "shard registry",
                "Adding new shard to shard registry",
                "shardId"_attr = shard->getId(),
                "shardConnectionString"_attr = connString);
    if (connString.type() == ConnectionString::ConnectionType::kReplicaSet) {
        _rsLookup[connString.getSetName()] = shard;
    } else if (connString.type() == ConnectionString::ConnectionType::kCustom) {
        // kCustom connection strings (ie "$dummy:10000) become DBDirectClient connections which
        // always return "localhost" as their response to getServerAddress().  This is just for
        // making dbtest work.
        _shardIdLookup[ShardId("localhost")] = shard;
        _hostLookup[HostAndPort("localhost")] = shard;
    }

    _connStringLookup[connString.toString()] = shard;

    for (const HostAndPort& hostAndPort : connString.getServers()) {
        _hostLookup[hostAndPort] = shard;
    }
}

void ShardRegistryData::toBSON(BSONObjBuilder* map,
                               BSONObjBuilder* hosts,
                               BSONObjBuilder* connStrings) const {
    auto shards = getAllShards();

    std::sort(std::begin(shards),
              std::end(shards),
              [](std::shared_ptr<const Shard> lhs, std::shared_ptr<const Shard> rhs) {
                  return lhs->getId() < rhs->getId();
              });

    if (map) {
        for (auto&& shard : shards) {
            map->append(shard->getId(), shard->getConnString().toString());
        }
    }

    if (hosts) {
        for (const auto& hostIt : _hostLookup) {
            hosts->append(hostIt.first.toString(), hostIt.second->getId());
        }
    }

    if (connStrings) {
        for (const auto& connStringIt : _connStringLookup) {
            connStrings->append(connStringIt.first, connStringIt.second->getId());
        }
    }
}

void ShardRegistryData::toBSON(BSONObjBuilder* result) const {
    auto shards = getAllShards();

    std::sort(std::begin(shards),
              std::end(shards),
              [](std::shared_ptr<const Shard> lhs, std::shared_ptr<const Shard> rhs) {
                  return lhs->getId() < rhs->getId();
              });

    BSONObjBuilder mapBob(result->subobjStart("map"));
    for (auto&& shard : shards) {
        mapBob.append(shard->getId(), shard->getConnString().toString());
    }
    mapBob.done();

    BSONObjBuilder hostsBob(result->subobjStart("hosts"));
    for (const auto& hostIt : _hostLookup) {
        hostsBob.append(hostIt.first.toString(), hostIt.second->getId());
    }
    hostsBob.done();

    BSONObjBuilder connStringsBob(result->subobjStart("connStrings"));
    for (const auto& connStringIt : _connStringLookup) {
        connStringsBob.append(connStringIt.first, connStringIt.second->getId());
    }
    connStringsBob.done();
}

BSONObj ShardRegistryData::toBSON() const {
    BSONObjBuilder bob;
    toBSON(&bob);
    return bob.obj();
}

}  // namespace mongo
