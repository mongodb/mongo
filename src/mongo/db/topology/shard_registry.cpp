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

#include "mongo/db/topology/shard_registry.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bson_field.h"
#include "mongo/client/replica_set_monitor.h"
#include "mongo/db/client.h"
#include "mongo/db/generic_argument_util.h"
#include "mongo/db/global_catalog/sharding_catalog_client.h"
#include "mongo/db/global_catalog/type_shard.h"
#include "mongo/db/logical_time.h"
#include "mongo/db/repl/optime_with.h"
#include "mongo/db/repl/read_concern_level.h"
#include "mongo/db/server_options.h"
#include "mongo/db/shard_role/transaction_resources.h"
#include "mongo/db/sharding_environment/client/config_shard_wrapper.h"
#include "mongo/db/sharding_environment/grid.h"
#include "mongo/db/topology/cluster_role.h"
#include "mongo/db/topology/vector_clock/vector_clock.h"
#include "mongo/db/topology/vector_clock/vector_clock_metadata_hook.h"
#include "mongo/executor/network_connection_hook.h"
#include "mongo/executor/network_interface_factory.h"
#include "mongo/executor/network_interface_thread_pool.h"
#include "mongo/executor/task_executor_pool.h"
#include "mongo/executor/thread_pool_task_executor.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_severity_suppressor.h"
#include "mongo/rpc/metadata/egress_metadata_hook_list.h"
#include "mongo/rpc/metadata/metadata_hook.h"
#include "mongo/util/cancellation.h"
#include "mongo/util/duration.h"
#include "mongo/util/future_impl.h"
#include "mongo/util/future_util.h"
#include "mongo/util/invalidating_lru_cache.h"
#include "mongo/util/observable_mutex_registry.h"
#include "mongo/util/str.h"
#include "mongo/util/time_support.h"

#include <algorithm>
#include <iterator>
#include <list>
#include <tuple>

#include <absl/container/node_hash_map.h>
#include <absl/meta/type_traits.h>
#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

namespace mongo {

namespace {

const Seconds kRefreshPeriod(30);

const Backoff kExponentialBackoff(Seconds(1), Milliseconds::max());

/**
 * Fetches shard documents from the catalog client without creating Shard instances.
 * Returns a map of shardId -> connectionString and the maximum topologyTime found.
 */
std::pair<ShardRegistryData::ShardIdToConnectionStringMap, Timestamp> fetchFromCatalogClient(
    OperationContext* opCtx) {
    auto const catalogClient = Grid::get(opCtx)->catalogClient();

    const auto shardsAndOpTime = [&] {
        try {
            return catalogClient->getAllShards(opCtx, repl::ReadConcernLevel::kSnapshotReadConcern);
        } catch (DBException& ex) {
            ex.addContext("could not get updated shard list from config server");
            throw;
        }
    }();

    auto shards = std::move(shardsAndOpTime.value);
    auto reloadOpTime = std::move(shardsAndOpTime.opTime);

    LOGV2_DEBUG(22731,
                1,
                "Successfully retrieved updated shard list from config server",
                "shardsNumber"_attr = shards.size(),
                "lastVisibleOpTime"_attr = reloadOpTime);

    ShardRegistryData::ShardIdToConnectionStringMap shardDocs;
    Timestamp maxTopologyTime{VectorClock::kInitialComponentTime.asTimestamp()};
    for (const auto& shardType : shards) {
        // This validation should ideally go inside the ShardType::validate call. However, doing
        // it there would prevent us from loading previously faulty shard hosts, which might have
        // been stored (i.e., the entire getAllShards call would fail).
        auto shardHostStatus = ConnectionString::parse(shardType.getHost());
        if (!shardHostStatus.isOK()) {
            LOGV2_WARNING(22735,
                          "Error parsing shard host",
                          "error"_attr = redact(shardHostStatus.getStatus()));
            continue;
        }

        if (auto thisTopologyTime = shardType.getTopologyTime();
            maxTopologyTime < thisTopologyTime) {
            maxTopologyTime = thisTopologyTime;
        }

        shardDocs.insert_or_assign(ShardId(shardType.getName()), shardHostStatus.getValue());
    }

    return {std::move(shardDocs), maxTopologyTime};
}

}  // namespace

namespace shard_registry_stats {

Counter64& blockedOpsGauge = *MetricBuilder<Counter64>{"mongos.shardRegistry.blockedOpsGauge"};

}  // namespace shard_registry_stats

using CallbackArgs = executor::TaskExecutor::CallbackArgs;

ShardRegistry::ShardRegistry(ServiceContext* service,
                             std::unique_ptr<ShardFactory> shardFactory,
                             const boost::optional<ConnectionString>& configServerCS,
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
          _service->getService(),
          _threadPool,
          [this](OperationContext* opCtx,
                 const Singleton& key,
                 const Cache::ValueHandle& cachedData,
                 const Time& timeInStore) { return _lookup(opCtx, key, cachedData, timeInStore); },
          1 /* cacheSize */)) {

    if (_initConfigServerCS) {
        invariant(_initConfigServerCS->isValid());
    }

    _threadPool.startup();

    ObservableMutexRegistry::get().add("Router Cache Mutexes", _cacheMutex);
    ObservableMutexRegistry::get().add("Router Cache Mutexes", _mutex);
}

ShardRegistry::~ShardRegistry() {
    shutdown();
}

void ShardRegistry::init() {
    invariant(!_isInitialized.load());

    /* The creation of the config shard object will intialize the associated RSM monitor that in
     * turn will call ShardRegistry::updateReplSetHosts(). Hence the config shard object MUST be
     * created after the ShardRegistry is fully constructed. This is why `_configShardData`
     * is initialized here rather than in the ShardRegistry constructor.
     */
    if (_initConfigServerCS) {
        LOGV2_DEBUG(
            5123000, 1, "Initializing ShardRegistry", "configServers"_attr = _initConfigServerCS);

        stdx::lock_guard lk(_mutex);
        _initConfigShard(lk, *_initConfigServerCS);
        _isInitialized.store(true);
    } else {
        LOGV2_DEBUG(
            7208800,
            1,
            "Deferring ShardRegistry initialization until local replica set config is known");
    }
}

ShardRegistry::Cache::LookupResult ShardRegistry::_lookup(OperationContext* opCtx,
                                                          const Singleton& key,
                                                          const Cache::ValueHandle& cachedData,
                                                          const Time& timeInStore) {
    invariant(key == _kSingleton);

    try {
        _stats.totalRefreshCount.fetchAndAdd(1);
        _stats.activeRefreshCount.fetchAndAdd(1);
        ON_BLOCK_EXIT([&]() { _stats.activeRefreshCount.fetchAndSubtract(1); });

        // This function can potentially block for a long time on network activity, so holding of
        // locks
        // is disallowed.
        tassert(7032320,
                "Can't perform ShardRegistry lookup while holding locks",
                !shard_role_details::getLocker(opCtx)->isLocked());

        LOGV2(11993900,
              "Started ShardRegistry lookup",
              "numShards"_attr = cachedData ? cachedData->getAllShardIds().size() : 0,
              "cachedDataTime"_attr = cachedData ? cachedData.getTime() : Time{},
              "latestKnownTime"_attr = timeInStore);
        LOGV2_DEBUG(11993901,
                    2,
                    "ShardRegistry lookup starting with",
                    "cachedData"_attr = cachedData ? cachedData->toBSON() : BSONObj{},
                    "cachedDataTime"_attr = cachedData ? cachedData.getTime() : Time{},
                    "latestKnownTime"_attr = timeInStore);

        // If the latest _forceReloadIncrement before the lookup is greater than the one in
        // timeInStore, at least one force reload request sneaked in while the lookup was
        // started. This lookup will be causally consistent with any force reload requests up to
        // this point, so it is fine to merge the requests to avoid extra lookups.

        // This also covers the case where no cachedData was available (null timeInStore). Using
        // the timeInStore._forceReloadIncrement (0) for the lookup result would cause an
        // unnecessary additional lookup on the next get request if the current lookup was in
        // fact caused by a forceReload (meaning _forceReloadIncrement >= 1).
        const auto [shardDocs, returnTime] = Time::makeWithLookup([&] {
            auto [docs, maxTopologyTime] = fetchFromCatalogClient(opCtx);

            // TODO (SERVER-102087): remove invariant relaxation after 9.0 is branched.
            {
                // Detect topologyTime corruption due to SERVER-63742. If config.shards has no
                // entries with a topologyTime, but there is a non-initial topologyTime being
                // gossiped, there has been corruption. This scenario is benign, and thus we
                // relax the time monotonicity invariant by making the expected topologyTime
                // accepted.
                //
                // For cases where there is a topologyTime in config.shards, but the gossiped
                // time is greater than the maximum topologyTime found there, or for cases where
                // config.shards has no shards but there is a gossiped topologyTime, we still
                // want to fail.
                //
                // Caveat: by design, force reloads install the actual config.shards
                // topologyTime as the timeInStore, which should be Timestamp(0, 1) in the
                // accepted scenario. Subsequent getData calls will advance the timeInStore to
                // the corrupted topologyTime, causing another refresh. Meaning that each forced
                // reload will end up causing two lookups.
                const Timestamp kInitialTopologyTime =
                    VectorClock::kInitialComponentTime.asTimestamp();
                if (MONGO_unlikely(!docs.empty() && maxTopologyTime == kInitialTopologyTime &&
                                   // '>' also ignores Timestamp(0, 0), used for force reloads,
                                   // or when the cache is empty.
                                   timeInStore.getTopologyTime() > kInitialTopologyTime)) {

                    // Log severity suppressor for inconsistent topology time message.
                    static logv2::SeveritySuppressor severitySuppressor{
                        Days{1}, logv2::LogSeverity::Warning(), logv2::LogSeverity::Debug(2)};

                    LOGV2_DEBUG(10173900,
                                severitySuppressor().toInt(),
                                "Inconsistent $topologyTime detected. 'config.shards' does not "
                                "contain any 'topologyTime' in its entries, but a non-initial "
                                "$topologyTime has been gossiped. An inconsistent $topologyTime "
                                "could have been created by SERVER-63742, while 'config.shards' "
                                "not containing any 'topologyTime' is expected when there have "
                                "been no add or remove shard operations made after upgrading to "
                                "version 5.0. This scenario is benign and the topologyTime is "
                                "accepted as valid.",
                                "topologyTime"_attr = timeInStore.getTopologyTime());

                    maxTopologyTime = timeInStore.getTopologyTime();
                }
            }

            return std::pair{std::move(docs), maxTopologyTime};
        });

        // Tear down removed shards before buildFromShardDocs, which creates new RSMs.
        // Otherwise a re-added shard whose RSM hasn't been cleaned up yet would collide.
        _tearDownRemovedShards(opCtx, cachedData, shardDocs);

        auto lookupData = ShardRegistryData::buildFromShardDocs(shardDocs, _shardFactory.get());

        // Merge with cached data or use lookup data directly.
        // If the recovery mode flag is set, return the lookup data without merging with the
        // cached data. This ensures that shard connection strings are fully refreshed from
        // the config server. Since recovery mode is set only during recovery operations, which
        // happen infrequently, there is no additional synchronization mechanism to ensure that
        // it is executed only by reloadForRecovery(). If a normal lookup occurs after the
        // recovery mode flag is set, a full refresh will be performed, which is expected
        // behavior.
        const bool isRecoveryMode = _isRecoveryMode.load();
        ShardRegistryData returnData = [&] {
            if (!cachedData || isRecoveryMode) {
                if (isRecoveryMode) {
                    LOGV2_WARNING(
                        11032906,
                        "Returning lookup data in recovery mode without merging with cache",
                        "lookupData"_attr = lookupData.toBSON().toString(),
                        "returnTopologyTime"_attr = returnTime.getTopologyTime());
                    _isRecoveryMode.store(false);
                }
                return lookupData;
            }
            return ShardRegistryData::mergeExisting(*cachedData, lookupData);
        }();

        // Always apply the latest conn strings.
        auto latestConnStrings = _getLatestConnStrings();

        for (const auto& latestConnString : latestConnStrings) {
            auto shard = returnData.findByRSName(latestConnString.first.toString());
            if (shard == nullptr || shard->getConnString() == latestConnString.second) {
                continue;
            }

            auto newData = ShardRegistryData::createFromExisting(
                returnData, latestConnString.second, _shardFactory.get());
            returnData = newData;
        }

        LOGV2_DEBUG(11993902,
                    2,
                    "ShardRegistry lookup completing with",
                    "returnData"_attr = returnData.toBSON(),
                    "returnTime"_attr = returnTime);
        LOGV2(11993903,
              "Completed ShardRegistry lookup",
              "numShards"_attr = cachedData ? cachedData->getAllShardIds().size() : 0);
        return Cache::LookupResult{returnData, returnTime};
    } catch (const DBException&) {
        _stats.failedRefreshCount.fetchAndAdd(1);
        throw;
    }
}

void ShardRegistry::startupPeriodicReloader(OperationContext* opCtx) {
    // startupPeriodicReloader() must be called only once
    invariant(!_executor);

    auto hookList = std::make_unique<rpc::EgressMetadataHookList>();
    hookList->addHook(std::make_unique<rpc::VectorClockMetadataHook>(opCtx->getServiceContext()));

    // construct task executor
    auto net = executor::makeNetworkInterface("ShardRegistryUpdater", nullptr, std::move(hookList));
    auto netPtr = net.get();
    _executor = executor::ThreadPoolTaskExecutor::create(
        std::make_unique<executor::NetworkInterfaceThreadPool>(netPtr), std::move(net));
    LOGV2_DEBUG(22724, 1, "Starting up task executor for periodic reloading of ShardRegistry");
    _executor->startup();

    AsyncTry([this] {
        ThreadClient tc("Periodic ShardRegistry pinger", getGlobalServiceContext()->getService());
        auto opCtx = cc().makeOperationContext();

        LOGV2_DEBUG(9112100, 2, "Periodic ping to CSRS for ShardRegistry topology time update");
        uassertStatusOK(_pingForNewTopologyTime(opCtx.get()));

        const auto time = VectorClock::get(opCtx.get())->getTime();
        LOGV2_DEBUG(9112101,
                    2,
                    "VectorClock after periodic ShardRegistry ping",
                    "clusterTime"_attr = time.clusterTime(),
                    "configTime"_attr = time.configTime(),
                    "topologyTime"_attr = time.topologyTime());

        // Schedule a lookup if the topologyTime has been advanced. This is only necessary because
        // the "connPoolStats" command depends on replica set monitors being instantiated. Even if
        // a node becomes aware of a newer topologyTime, if there are no calls to ShardRegistry
        // functions no lookup is done, and the RSMs are not instantiated. There are some tests
        // which hang due to this, so presumably some users might already be relying on this
        // behaviour.
        _scheduleLookupIfRequired();
    })
        .until([](auto&& status) {
            if (!status.isOK()) {
                LOGV2(22727,
                      "Error running periodic reload of shard registry",
                      "error"_attr = redact(status),
                      "shardRegistryReloadInterval"_attr = kRefreshPeriod);
            }
            // Continue until the _executor will shutdown
            return false;
        })
        .withDelayBetweenIterations(kRefreshPeriod)  // This call is optional.
        .on(_executor, CancellationToken::uncancelable())
        .getAsync([](auto&& status) {
            LOGV2_DEBUG(22725,
                        1,
                        "Exiting periodic shard registry reloader",
                        "reason"_attr = redact(status));
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
    stdx::lock_guard lk(_mutex);
    auto configShard = _configShardData.findShard(ShardId::kConfigServerId);
    // Note this should only throw if the local node has not learned its replica set config yet.
    uassert(ErrorCodes::NotYetInitialized, "Config shard has not been set up yet", configShard);
    return std::make_shared<ConfigShardWrapper>(configShard);
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
        stdx::lock_guard lk(_mutex);
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
                stdx::lock_guard lk(_mutex);
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

std::vector<ShardRegistry::LatestConnStrings::value_type> ShardRegistry::_getLatestConnStrings()
    const {
    stdx::unique_lock lock(_mutex);
    return {_latestConnStrings.begin(), _latestConnStrings.end()};
}

void ShardRegistry::_removeReplicaSet(const std::string& setName) {
    stdx::lock_guard lk(_mutex);
    _latestConnStrings.erase(setName);
}

void ShardRegistry::_tearDownRemovedShards(
    OperationContext* opCtx,
    const Cache::ValueHandle& cachedData,
    const ShardRegistryData::ShardIdToConnectionStringMap& shardDocs) {
    if (!cachedData) {
        return;
    }

    for (const auto& cachedShard : cachedData->getAllShards()) {
        auto cachedId = cachedShard->getId();
        if (shardDocs.contains(cachedId)) {
            continue;
        }

        auto rsName = cachedShard->getConnString().getSetName();
        if (cachedId != ShardId::kConfigServerId) {
            ReplicaSetMonitor::remove(rsName);
        }
        _removeReplicaSet(rsName);

        for (auto& callback : _shardRemovalHooks) {
            ExecutorFuture<void>(Grid::get(opCtx)->getExecutorPool()->getFixedExecutor())
                .getAsync([=](const Status&) { callback(cachedId); });
        }
    }
}

void ShardRegistry::updateReplSetHosts(const ConnectionString& givenConnString,
                                       ConnectionStringUpdateType updateType) {
    invariant(givenConnString.type() == ConnectionString::ConnectionType::kReplicaSet ||
              givenConnString.type() == ConnectionString::ConnectionType::kCustom);  // For dbtests

    auto setName = givenConnString.getSetName();

    {
        stdx::lock_guard lk(_mutex);

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
        }

        LOGV2_DEBUG(9310104,
                    2,
                    "Forcing a reload after receiving updated connection string",
                    "newConnString"_attr = newConnString);
    }

    // Schedule a lookup, to incorporate the new connection string.
    _scheduleForcedLookup();
}

std::unique_ptr<Shard> ShardRegistry::createConnection(const ConnectionString& connStr) const {
    return _shardFactory->createUniqueShard(ShardId("<unnamed>"), connStr);
}

std::shared_ptr<Shard> ShardRegistry::createLocalConfigShard() const {
    invariant(serverGlobalParams.clusterRole.has(ClusterRole::ConfigServer));
    std::shared_ptr<Shard> configShard =
        _shardFactory->createShard(ShardId::kConfigServerId, ConnectionString::forLocal());
    return std::make_shared<ConfigShardWrapper>(configShard);
}

void ShardRegistry::toBSON(BSONObjBuilder* result) const {
    BSONObjBuilder map;
    BSONObjBuilder hosts;
    BSONObjBuilder connStrings;
    if (auto data = _getCachedData()) {
        data->toBSON(&map, &hosts, &connStrings);
        result->append("timeInStore", data.getTime().toString());
    }
    {
        stdx::lock_guard lk(_mutex);
        _configShardData.toBSON(&map, &hosts, &connStrings);
    }
    result->append("map", map.obj());
    result->append("hosts", hosts.obj());
    result->append("connStrings", connStrings.obj());
}

void ShardRegistry::reload(OperationContext* opCtx) {
    _reloadAsync().get(opCtx);
}

std::tuple<Timestamp, int64_t, Timestamp, int64_t> ShardRegistry::reloadForRecovery(
    OperationContext* opCtx) {
    Timestamp cachedTimeBefore = Timestamp::min();
    Timestamp cachedTimeAfter = Timestamp::min();
    Increment forceReloadIncrementBefore = 0;
    Increment forceReloadIncrementAfter = 0;
    auto cachedData = _getCachedData();
    if (cachedData) {
        auto beforeTime = cachedData.getTime();
        forceReloadIncrementBefore = beforeTime.getForceReloadIncrement();
        cachedTimeBefore = beforeTime.getTopologyTime();
    }
    _clearForRecovery(opCtx);
    // Set the recovery mode flag to true to prevent merging of connection strings with existing
    // connection strings, ensuring a full refresh of shard connection strings from the config
    // server.
    _isRecoveryMode.store(true);
    reload(opCtx);

    cachedData = _getCachedData();
    if (cachedData) {
        auto afterTime = cachedData.getTime();
        forceReloadIncrementAfter = afterTime.getForceReloadIncrement();
        cachedTimeAfter = afterTime.getTopologyTime();
    }

    return std::make_tuple(
        cachedTimeBefore, forceReloadIncrementBefore, cachedTimeAfter, forceReloadIncrementAfter);
}

void ShardRegistry::_clearForRecovery(OperationContext* opCtx) {

    LOGV2(11032902, "Recovering shard registry - clearing cached connection strings and RSMs");

    {
        stdx::lock_guard lk(_mutex);
        // Clear the cached latest connection strings.
        _latestConnStrings.clear();
    }

    // Helper lambda to safely remove an RSM
    auto removeRSM = [](const ShardId& shardId, const std::shared_ptr<Shard>& shardPtr) {
        if (!shardPtr) {
            return;
        }

        auto setName = shardPtr->getConnString().getSetName();
        if (setName.empty()) {
            return;
        }

        try {
            LOGV2_INFO(11032903,
                       "Removing RSM during recovery",
                       "shardId"_attr = shardId,
                       "setName"_attr = setName);
            ReplicaSetMonitor::remove(setName);
        } catch (const DBException& ex) {
            LOGV2_WARNING(11032904,
                          "Failed to remove RSM during shard registry recovery",
                          "shardId"_attr = shardId,
                          "setName"_attr = setName,
                          "error"_attr = ex);
        }
    };

    // Clean RSMs for regular shards in cache
    if (auto cachedData = _getCachedData()) {
        for (const auto& shardPtr : cachedData->getAllShards()) {
            if (shardPtr->getId() != ShardId::kConfigServerId) {
                removeRSM(shardPtr->getId(), shardPtr);
            }
        }
    }

    LOGV2(11032905, "Shard registry recovery cleanup completed");
}

SharedSemiFuture<ShardRegistry::Cache::ValueHandle> ShardRegistry::_reloadAsync() {
    _updateTimeInStore(/*forceReload=*/true);
    return _getDataAsyncCommon();
}

Status ShardRegistry::_pingForNewTopologyTime(OperationContext* opCtx) {
    return getConfigShard()
        ->runCommandWithIndefiniteRetries(opCtx,
                                          ReadPreferenceSetting(ReadPreference::PrimaryPreferred),
                                          DatabaseName::kAdmin,
                                          BSON("ping" << 1),
                                          Shard::RetryPolicy::kNoRetry)
        .getStatus();
}

void ShardRegistry::scheduleReplicaSetUpdateOnConfigServerIfNeeded(
    const std::function<bool()>& isPrimaryFn) noexcept {
    if (!isPrimaryFn()) {
        return;
    }

    auto executor = Grid::get(getGlobalServiceContext())->getExecutorPool()->getFixedExecutor();
    AsyncTry([] {
        ThreadClient tc("UpdateReplicaSetOnConfigServer", getGlobalServiceContext()->getService());
        auto opCtxHolder = cc().makeOperationContext();
        auto opCtx = opCtxHolder.get();

        auto grid = Grid::get(opCtx);
        // Gets a copy of the replica set config which will be used to update the config server.
        const repl::ReplSetConfig rsConfig = repl::ReplicationCoordinator::get(opCtx)->getConfig();
        auto connStr = rsConfig.getConnectionString();

        grid->shardRegistry()->updateReplSetHosts(
            connStr, ShardRegistry::ConnectionStringUpdateType::kPossible);

        auto swRegistryData = grid->shardRegistry()->_getDataAsync().getNoThrow(opCtx);
        if (!swRegistryData.isOK()) {
            LOGV2_ERROR(6791401,
                        "Error updating replica set on config server. Failed to fetch shard."
                        "registry data",
                        "replicaSetConnectionStr"_attr = connStr,
                        "error"_attr = swRegistryData.getStatus());
            return swRegistryData.getStatus();
        }

        auto shard = swRegistryData.getValue()->findByRSName(connStr.getSetName());
        if (!shard) {
            LOGV2_ERROR(6791402,
                        "Error updating replica set on config server. Couldn't find shard.",
                        "replicaSetConnectionStr"_attr = connStr);
            return Status::OK();
        }

        auto replSetConfigVersion = rsConfig.getConfigVersion();
        // Specify the config version in the filter and the update to prevent overwriting a
        // newer connection string when there are concurrent updates.
        auto filter =
            BSON(ShardType::name()
                 << shard->getId().toString() << "$or"
                 << BSON_ARRAY(BSON(ShardType::replSetConfigVersion() << BSON("$exists" << false))
                               << BSON(ShardType::replSetConfigVersion()
                                       << BSON("$lt" << replSetConfigVersion))));
        auto update = BSON("$set" << BSON(ShardType::host()
                                          << connStr.toString() << ShardType::replSetConfigVersion()
                                          << replSetConfigVersion));
        auto swWasUpdated =
            grid->catalogClient()->updateConfigDocument(opCtx,
                                                        NamespaceString::kConfigsvrShardsNamespace,
                                                        filter,
                                                        update,
                                                        false /* upsert */,
                                                        defaultMajorityWriteConcernDoNotUse());
        auto status = swWasUpdated.getStatus();
        if (!status.isOK()) {
            LOGV2_ERROR(2118501,
                        "Error updating replica set on config server.",
                        "replicaSetConnectionStr"_attr = connStr,
                        "error"_attr = redact(status));
            return status;
        }
        return Status::OK();
    })
        .until([isPrimaryFn](Status status) {
            // Stop if the update succeeds or if this node is no longer the primary since the new
            // primary will issue this update anyway.
            return status.isOK() || !isPrimaryFn();
        })
        .withBackoffBetweenIterations(kExponentialBackoff)
        .on(executor, CancellationToken::uncancelable())
        .getAsync([](auto) {});
}

SharedSemiFuture<ShardRegistry::Cache::ValueHandle> ShardRegistry::_getDataAsync() {
    _updateTimeInStore();
    return _getDataAsyncCommon();
}

SharedSemiFuture<ShardRegistry::Cache::ValueHandle> ShardRegistry::_getDataAsyncCommon() {
    shard_registry_stats::blockedOpsGauge.increment();
    return _cache->acquireAsync(_kSingleton, CacheCausalConsistency::kLatestKnown)
        .thenRunOn(Grid::get(_service)->getExecutorPool()->getFixedExecutor())
        .unsafeToInlineFuture()
        .tapAll([this](auto status) { shard_registry_stats::blockedOpsGauge.decrement(); })
        .share();
}

void ShardRegistry::_updateTimeInStore(bool forceReload) {
    auto newTime = forceReload ? Time::makeForForcedReload() : Time::makeLatestKnown(_service);
    LOGV2_DEBUG(9310103,
                2,
                "Advancing ShardRegistry timeInStore",
                "forceReload"_attr = forceReload,
                "newTime"_attr = newTime);
    _cache->advanceTimeInStore(_kSingleton, newTime);
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

boost::optional<bool> ShardRegistry::cachedClusterHasConfigShard() const {
    auto data = _getCachedData();
    if (!data) {
        return boost::none;
    }

    return data->findShard(ShardId::kConfigServerId) != nullptr;
}

std::shared_ptr<Shard> ShardRegistry::getShardForHostNoReload(const HostAndPort& host) const {
    // First check if this is a config shard lookup.
    {
        stdx::lock_guard lk(_mutex);
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

void ShardRegistry::report(BSONObjBuilder* builder) const {
    BSONObjBuilder statsBuilder = builder->subobjStart("shardRegistry");
    _stats.report(&statsBuilder);
}

void ShardRegistry::Stats::report(BSONObjBuilder* builder) const {
    builder->append("activeRefreshCount", activeRefreshCount.load());
    builder->append("totalRefreshCount", totalRefreshCount.load());
    builder->append("failedRefreshCount", failedRefreshCount.load());
}

void ShardRegistry::_scheduleForcedLookup() {
    _reloadAsync()
        .thenRunOn(Grid::get(_service)->getExecutorPool()->getFixedExecutor())
        .ignoreValue()
        .getAsync([](const Status& status) {
            if (!status.isOK()) {
                LOGV2(4620201,
                      "Error running reload of ShardRegistry for RSM update",
                      "error"_attr = redact(status));
            }
        });
}

void ShardRegistry::_scheduleLookupIfRequired() {
    _getDataAsync()
        .thenRunOn(Grid::get(_service)->getExecutorPool()->getFixedExecutor())
        .ignoreValue()
        .getAsync([](const Status& status) {
            if (!status.isOK()) {
                LOGV2(9310100, "Error during scheduled lookup", "error"_attr = redact(status));
            }
        });
}

void ShardRegistry::_initConfigShard(WithLock wl, const ConnectionString& configCS) {
    _configShardData = ShardRegistryData::createWithConfigShardOnly(
        _shardFactory->createShard(ShardId::kConfigServerId, configCS));
    _latestConnStrings[configCS.getSetName()] = configCS;
}

void ShardRegistry::initConfigShardIfNecessary(const ConnectionString& configCS) {
    {
        stdx::lock_guard lk(_mutex);
        if (_isInitialized.load()) {
            return;
        }

        _initConfigShard(lk, configCS);
        _isInitialized.store(true);
    }

    // Lookup can succeed now that the registry has a real config shard, so schedule one right away.
    _scheduleForcedLookup();
}

////////////// ShardRegistryData //////////////////

ShardRegistryData ShardRegistryData::createWithConfigShardOnly(std::shared_ptr<Shard> configShard) {
    ShardRegistryData data;
    data._addShard(configShard);
    return data;
}

ShardRegistryData ShardRegistryData::buildFromShardDocs(
    const ShardIdToConnectionStringMap& shardDocs, ShardFactory* shardFactory) {
    ShardRegistryData data;
    for (const auto& [shardId, connString] : shardDocs) {
        data._addShard(shardFactory->createShard(shardId, connString));
    }
    return data;
}

ShardRegistryData ShardRegistryData::mergeExisting(const ShardRegistryData& alreadyCachedData,
                                                   const ShardRegistryData& configServerData) {
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

    LOGV2_DEBUG(9310101,
                2,
                "Merge ShardRegistryData",
                "cached"_attr = alreadyCachedData.toBSON(),
                "lookup"_attr = configServerData.toBSON(),
                "merged"_attr = mergedData.toBSON());
    return mergedData;
}

ShardRegistryData ShardRegistryData::createFromExisting(const ShardRegistryData& existingData,
                                                        const ConnectionString& newConnString,
                                                        ShardFactory* shardFactory) {
    ShardRegistryData data(existingData);

    LOGV2_DEBUG(9310102,
                2,
                "ShardRegistryData::createFromExisting",
                "existing"_attr = existingData.toBSON(),
                "newConnString"_attr = newConnString);

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

////////////// ShardRegistry::Time
AtomicWord<ShardRegistry::Increment> ShardRegistry::Time::_forceReloadIncrementSource{0};

ShardRegistry::Time ShardRegistry::Time::makeForForcedReload() {
    // An empty topologyTime signifies that the refresh was due to a forced reload. This is
    // just for diagnostics. Once the lookup is finished, the proper topology time from the
    // CSRS response is used to update the timeInStore.
    const auto newForceReload = _forceReloadIncrementSource.addAndFetch(1);
    return Time{newForceReload, Timestamp{}};
}

ShardRegistry::Time ShardRegistry::Time::makeLatestKnown(ServiceContext* svcCtx) {
    // Make a Time from the latest known topologyTime.
    auto now = VectorClock::get(svcCtx)->getTime();
    Timestamp latestKnownTopologyTime = now.topologyTime().asTimestamp();
    return Time{_forceReloadIncrementSource.load(), latestKnownTopologyTime};
}

std::pair<ShardRegistryData::ShardIdToConnectionStringMap, ShardRegistry::Time>
ShardRegistry::Time::makeWithLookup(LookupFn&& lookupFn) {
    // It is important that this value is loaded before the lookup to ensure force reload requests
    // are not incorrectly merged.
    const auto forceReloadBeforeLookup = _forceReloadIncrementSource.load();
    auto [shardDocs, topologyTime] = lookupFn();
    return {std::move(shardDocs), Time{forceReloadBeforeLookup, topologyTime}};
}

}  // namespace mongo
