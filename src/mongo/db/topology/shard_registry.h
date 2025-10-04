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

#pragma once

#include "mongo/base/status_with.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/timestamp.h"
#include "mongo/client/connection_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/server_options.h"
#include "mongo/db/service_context.h"
#include "mongo/db/sharding_environment/client/shard.h"
#include "mongo/db/sharding_environment/client/shard_factory.h"
#include "mongo/db/sharding_environment/shard_id.h"
#include "mongo/executor/task_executor.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/stdx/mutex.h"
#include "mongo/stdx/unordered_map.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/concurrency/thread_pool.h"
#include "mongo/util/concurrency/with_lock.h"
#include "mongo/util/future.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/out_of_line_executor.h"
#include "mongo/util/read_through_cache.h"

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <absl/container/node_hash_map.h>
#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr.hpp>

namespace mongo {

namespace shard_registry_stats {

extern Counter64& blockedOpsGauge;

}  // namespace shard_registry_stats

class ShardRegistryData {
public:
    using ShardMap = stdx::unordered_map<ShardId, std::shared_ptr<Shard>, ShardId::Hasher>;

    /**
     * Creates a basic ShardRegistryData, that only contains the config shard.  Needed during
     * initialization, when the config servers are contacted for the first time (ie. the first time
     * createFromCatalogClient() is called).
     */
    static ShardRegistryData createWithConfigShardOnly(std::shared_ptr<Shard> configShard);

    /**
     * Reads shards docs from the catalog client and fills in maps.
     */
    static std::pair<ShardRegistryData, Timestamp> createFromCatalogClient(
        OperationContext* opCtx, ShardFactory* shardFactory);

    /**
     * Merges alreadyCachedData and configServerData into a new ShardRegistryData.
     *
     * The merged data is the same as configServerData, except that for the host and connection
     * string based lookups, any values from alreadyCachedData will take precedence over those from
     * configServerData.
     *
     * Returns the merged data, as well as the shards that have been removed (ie. that are present
     * in alreadyCachedData but not configServerData) as a mapping from ShardId to
     * std::shared_ptr<Shard>.
     *
     * Called when reloading the shard registry. It is important to merge _hostLookup because
     * reloading the shard registry can interleave with updates to the shard registry passed by the
     * RSM.
     */
    static std::pair<ShardRegistryData, ShardMap> mergeExisting(
        const ShardRegistryData& alreadyCachedData, const ShardRegistryData& configServerData);

    /**
     * Create a duplicate of existingData, but additionally updates the shard for newConnString.
     * Used when notified by the RSM of a new connection string from a shard.
     */
    static ShardRegistryData createFromExisting(const ShardRegistryData& existingData,
                                                const ConnectionString& newConnString,
                                                ShardFactory* shardFactory);

    /**
     * Returns the shard with the given shard id, connection string, or host and port.
     *
     * Callers might pass in the connection string or HostAndPort rather than ShardId, so this
     * method will first look for the shard by ShardId, then connection string, then HostAndPort
     * stopping once it finds the shard.
     */
    std::shared_ptr<Shard> findShard(const ShardId& shardId) const;

    /**
     * Returns the shard with the given replica set name, or nullptr if no such shard.
     */
    std::shared_ptr<Shard> findByRSName(const std::string& name) const;

    /**
     * Returns the shard which contains a mongod with the given host and port, or nullptr if no such
     * shard.
     */
    std::shared_ptr<Shard> findByHostAndPort(const HostAndPort&) const;

    /**
     * Returns a vector of all known shard ids.
     * The order of the elements is not guaranteed.
     */
    std::vector<ShardId> getAllShardIds() const;

    /**
     * Returns a vector of all known shard objects.
     * The order of the elements is not guaranteed.
     */
    std::vector<std::shared_ptr<Shard>> getAllShards() const;

    void toBSON(BSONObjBuilder* result) const;
    void toBSON(BSONObjBuilder* map, BSONObjBuilder* hosts, BSONObjBuilder* connStrings) const;
    BSONObj toBSON() const;

private:
    /**
     * Returns the shard with the given shard id, or nullptr if no such shard.
     */
    std::shared_ptr<Shard> _findByShardId(const ShardId&) const;

    /**
     * Returns the shard with the given connection string, or nullptr if no such shard.
     */
    std::shared_ptr<Shard> _findByConnectionString(const std::string& connectionString) const;

    /**
     * Puts the given shard object into the lookup maps.
     */
    void _addShard(std::shared_ptr<Shard>);

    // Map of shardName -> Shard
    ShardMap _shardIdLookup;

    // Map from replica set name to shard corresponding to this replica set
    ShardMap _rsLookup;

    // Map of HostAndPort to Shard
    stdx::unordered_map<HostAndPort, std::shared_ptr<Shard>> _hostLookup;

    // Map of connection string to Shard
    std::map<std::string, std::shared_ptr<Shard>> _connStringLookup;
};

/**
 * Each node (router, shard server, config server, primaries and secondaries) has one instance of
 * this object. It is an in-memory cache mirroring the `config.shards` collection on the config
 * server, whose causal consistency is driven by the `topologyTime` component of the vector clock.
 * The collection (and thus the cache) contains an entry for each shard in the cluster. Each entry
 * contains the connection string for that shard.
 *
 * Retrieving a shard from the registry returns a `Shard` object. Using that object, one can access
 * more information about a shard and run commands against that shard. A `Shard` object can be
 * retrieved from the registry by using any of:
 * - The shard's name
 * - The replica set's name
 * - The HostAndPort object
 * - The connection string
 *
 * REFRESHES: The shard registry refreshes itself in these scenarios:
 * - Upon the node's start-up
 * - Upon completion of a background job that runs every thirty seconds
 * - Upon an attempt to retrieve a shard that doesn’t have a matching entry in the cache
 * - Upon calling the ShardRegistry’s reload function (ShardRegistry::reload())
 * - After an operation has gossipped-in a higher `topologyTime`
 *
 * The shard registry makes updates to the `config.shards` collection in one case. If the shard
 * registry discovers an updated connection string for another shard via a replica set topology
 * change, it will persist that update to `config.shards`.
 */
class ShardRegistry {
    ShardRegistry(const ShardRegistry&) = delete;
    ShardRegistry& operator=(const ShardRegistry&) = delete;

public:
    /**
     * A callback type for functions that can be called on shard removal.
     */
    using ShardRemovalHook = std::function<void(const ShardId&)>;

    /**
     * Used when informing updateReplSetHosts() of a new connection string for a shard.
     */
    enum class ConnectionStringUpdateType { kConfirmed, kPossible };

    /**
     * Instantiates a new shard registry.
     *
     * @param shardFactory      Makes shards
     * @param configServerCS    ConnectionString used for communicating with the config servers
     * @param shardRemovalHooks A list of hooks that will be called when a shard is removed. The
     *                          hook is expected not to throw. If it does throw, the process will be
     *                          terminated.
     */
    ShardRegistry(ServiceContext* service,
                  std::unique_ptr<ShardFactory> shardFactory,
                  const boost::optional<ConnectionString>& configServerCS,
                  std::vector<ShardRemovalHook> shardRemovalHooks = {});

    ~ShardRegistry();

    /**
     * Initializes ShardRegistry with config shard, if a connection string was provided at
     * construction.
     *
     * The creation of the config shard object will intialize the associated RSM monitor that in
     * turn will call ShardRegistry::updateReplSetHosts(). Hence the config shard object MUST be
     * created after the ShardRegistry is fully constructed.
     */
    void init();

    /**
     * Sets up the registry's config shard from the given connection string. Only takes effect if
     * the registry has not already done this.
     */
    void initConfigShardIfNecessary(const ConnectionString& configCS);

    /**
     * Startup the periodic reloader of the ShardRegistry.
     * Can be called only after ShardRegistry::init()
     */
    void startupPeriodicReloader(OperationContext* opCtx);

    /**
     * Shutdown the periodic reloader of the ShardRegistry.
     */
    void shutdownPeriodicReloader();

    /**
     * Shuts down the threadPool. Needs to be called explicitly because ShardRegistry is never
     * destroyed as it's owned by the static grid object.
     */
    void shutdown();

    /**
     * This is invalid to use on the config server and will hit an invariant if it is done.
     * If the config server has need of a connection string for itself, it should get it from the
     * replication state.
     *
     * Returns the connection string for the config server.
     */
    ConnectionString getConfigServerConnectionString() const;

    /**
     * Returns shared pointer to the shard object representing the config servers.
     *
     * The config shard is always known, so this function never blocks.
     */
    std::shared_ptr<Shard> getConfigShard() const;

    /**
     * Returns a shared pointer to the shard object with the given shard id, or ShardNotFound error
     * otherwise.
     *
     * May refresh the shard registry if there's no cached information about the shard. The shardId
     * parameter can actually be the shard name or the HostAndPort for any server in the shard.
     */
    StatusWith<std::shared_ptr<Shard>> getShard(OperationContext* opCtx, const ShardId& shardId);

    SemiFuture<std::shared_ptr<Shard>> getShard(ExecutorPtr executor,
                                                const ShardId& shardId) noexcept;

    /**
     * Returns a vector containing all known shard IDs.
     * The order of the elements is not guaranteed.
     */
    std::vector<ShardId> getAllShardIds(OperationContext* opCtx);

    /**
     * Returns the number of shards.
     */
    int getNumShards(OperationContext* opCtx);

    /**
     * Takes a connection string describing either a shard or config server replica set, looks
     * up the corresponding Shard object based on the replica set name, then updates the
     * ShardRegistry's notion of what hosts make up that shard.
     */
    void updateReplSetHosts(const ConnectionString& givenConnString,
                            ConnectionStringUpdateType updateType);

    /**
     * Instantiates a new detached shard connection, which does not appear in the list of shards
     * tracked by the registry and as a result will not be returned by getAllShardIds.
     *
     * The caller owns the returned shard object and is responsible for disposing of it when done.
     *
     * @param connStr Connection string to the shard.
     */
    std::unique_ptr<Shard> createConnection(const ConnectionString& connStr) const;

    /**
     * Returns a ShardLocal for the config server that is not tracked by the registry. May only be
     * called on a config server node.
     */
    std::shared_ptr<Shard> createLocalConfigShard() const;

    void toBSON(BSONObjBuilder* result) const;

    /**
     * Force a reload of the ShardRegistry based on the contents of the config server's
     * config.shards collection.
     */
    void reload(OperationContext* opCtx);

    /**
     * To be used on mongod only. If the mongod is a primary, schedules a write to update the
     * connection string in the config.shards document for the shard it corresponds to. Upon an
     * error, retries if the mongod is still a primary.
     */
    static void scheduleReplicaSetUpdateOnConfigServerIfNeeded(
        const std::function<bool()>& isPrimaryFn) noexcept;

    /*
     * Returns true if the given host is part of the config server replica set.
     *
     * This method relies on the RSM to have pushed the correct CSRS membership information.
     */
    bool isConfigServer(const HostAndPort& host) const;

    /**
     * Returns an optional boolean indicating whether the config server is in the shard registry as
     * a shard.
     * - If there is cached data available, returns true if the config server is in the shard
     * registry, and false otherwise.
     * - If there is no cached data, returns boost::none.
     *
     * Note: This function does not refresh the shard registry or perform any network traffic. It is
     * a non-causally consistent call to the cached data from the shard registry.
     */
    boost::optional<bool> cachedClusterHasConfigShard() const;

    // TODO SERVER-50206: Remove usage of these non-causally consistent accessors.
    //
    // Their most important current users are dispatching requests to hosts, and processing
    // responses from hosts.  These contexts need to know the shard that the host is associated
    // with, but usually have no access to any associated opCtx (if there even is one), and also
    // cannot tolerate waiting for further network activity (if the cache is stale and needs to be
    // refreshed via _lookup()).

    /**
     * Finds the Shard that the mongod listening at this HostAndPort is a member of. Will not
     * refresh the shard registry or otherwise perform any network traffic.
     */
    std::shared_ptr<Shard> getShardForHostNoReload(const HostAndPort& shardHost) const;

    /**
     * Reports statistics about the shard registry to be used by serverStatus
     */
    void report(BSONObjBuilder* builder) const;

private:
    friend class ShardRegistryTest;

    /**
     * The ShardRegistry uses the ReadThroughCache to handle refreshing itself.  The cache stores
     * a single entry, with key of Singleton, value of ShardRegistryData, and causal-consistency
     * time which is primarily Timestamp (based on the TopologyTime), but with additional
     * "increment"s that are used to flag additional refresh criteria.
     */

    using Increment = int64_t;

    struct Time {
        explicit Time() {}

        bool operator==(const Time& other) const {
            return tie() == other.tie();
        }
        bool operator!=(const Time& other) const {
            return tie() != other.tie();
        }
        bool operator>(const Time& other) const {
            return tie() > other.tie();
        }
        bool operator>=(const Time& other) const {
            return tie() >= other.tie();
        }
        bool operator<(const Time& other) const {
            return tie() < other.tie();
        }
        bool operator<=(const Time& other) const {
            return tie() <= other.tie();
        }

        std::string toString() const {
            BSONObjBuilder bob;
            bob.append("forceReloadIncrement", _forceReloadIncrement);
            bob.append("topologyTime", _topologyTime);
            return bob.obj().toString();
        }

        /**
         * Create a Time to trigger a forced reload.
         */
        static Time makeForForcedReload();

        /**
         * Create a Time with the latest known topologyTime.
         */
        static Time makeLatestKnown(ServiceContext*);

        /**
         * Create a Time which will cause merging of force reload requests that have been made
         * before 'lookupFn' is evaluated, and contain the topologyTime returned by 'lookupFn'.
         */
        static Time makeWithLookup(std::function<Timestamp(void)>&& lookupFn);

        /**
         * Get the topologyTime component.
         *
         * TODO (SERVER-102087): remove after 9.0 is branched.
         */
        const Timestamp& getTopologyTime() const {
            return _topologyTime;
        }

    private:
        explicit Time(Increment _forceReloadIncrement, Timestamp topologyTime)
            : _forceReloadIncrement(_forceReloadIncrement), _topologyTime(topologyTime) {}

        // Tie the Time components in a std::tuple, to leverage the total ordering comparators.
        // We want the _forceReloadIncrement to be the most significative component, so a refresh
        // is always forced when requested.
        std::tuple<Increment, Timestamp> tie() const {
            return std::tie(_forceReloadIncrement, _topologyTime);
        }

        // Source for the _forceReloadIncrement field.
        static AtomicWord<Increment> _forceReloadIncrementSource;

        // The _forceReloadIncrement is used to indicate that the latest data should be fetched
        // from the configsvrs regardless of the topologyTime (ie. when the topologyTime can't be
        // used for this, eg. in the first lookup, and in contexts like unittests where topologyTime
        // isn't gossipped but the ShardRegistry still needs to be reloaded).  This is how reload()
        // is able to force a refresh from the config servers - incrementing the
        // _forceReloadIncrement causes the cache to call _lookup() (rather than having reload()
        // attempt to do a synchronous refresh).
        Increment _forceReloadIncrement{0};
        Timestamp _topologyTime;
    };

    struct Stats {
        AtomicWord<long long> activeRefreshCount{0};
        AtomicWord<long long> totalRefreshCount{0};
        AtomicWord<long long> failedRefreshCount{0};

        void report(BSONObjBuilder* builder) const;
    } _stats;

    enum class Singleton { Only };
    static constexpr auto _kSingleton = Singleton::Only;

    using Cache = ReadThroughCache<Singleton, ShardRegistryData, Time>;

    Cache::LookupResult _lookup(OperationContext* opCtx,
                                const Singleton& key,
                                const Cache::ValueHandle& cachedData,
                                const Time& timeInStore);

    /**
     * Gets a causally-consistent (ie. latest-known) copy of the ShardRegistryData, refreshing from
     * the config servers if necessary.
     */
    Cache::ValueHandle _getData(OperationContext* opCtx);

    /**
     * Gets a causally-consistent (ie. latest-known) copy of the ShardRegistryData asynchronously,
     * refreshing from the config servers if necessary.
     */
    SharedSemiFuture<Cache::ValueHandle> _getDataAsync();

    SharedSemiFuture<Cache::ValueHandle> _getDataAsyncCommon();

    /**
     * Updates the timeInStore with the latest known topologyTime, or a time which would force a
     * reload if 'forceReload' is true.
     */
    void _updateTimeInStore(bool forceReload = false);

    /**
     * Triggers a reload without waiting for it to complete.
     */
    void _scheduleForcedLookup();

    /**
     * Trigger a lookup only if the cache is out of sync with the latest known topologyTime.
     */
    void _scheduleLookupIfRequired();

    /**
     * Gets the latest-cached copy of the ShardRegistryData.  Never fetches from the config servers.
     * Only used by the "NoReload" accessors.
     * TODO SERVER-50206: Remove usage of this non-causally consistent accessor.
     */
    Cache::ValueHandle _getCachedData() const;

    using LatestConnStrings = stdx::unordered_map<ShardId, ConnectionString, ShardId::Hasher>;

    std::vector<LatestConnStrings::value_type> _getLatestConnStrings() const;

    void _removeReplicaSet(const std::string& setName);

    void _initializeCacheIfNecessary() const;

    void _initConfigShard(WithLock, const ConnectionString& configCS);

    SharedSemiFuture<Cache::ValueHandle> _reloadAsync();

    Status _pingForNewTopologyTime(OperationContext* opCtx);

    ServiceContext* _service{nullptr};

    /**
     * Factory to create shards.  Never changed after startup so safe to access outside of _mutex.
     */
    const std::unique_ptr<ShardFactory> _shardFactory;

    /**
     * Specified in the ShardRegistry c-tor. It's used only in init() to initialize the config
     * shard.
     */
    const boost::optional<ConnectionString> _initConfigServerCS;

    /**
     * A list of callbacks to be called asynchronously when it has been discovered that a shard was
     * removed.
     */
    const std::vector<ShardRemovalHook> _shardRemovalHooks;

    // Thread pool used when looking up new values for the cache (ie. in which _lookup() runs).
    ThreadPool _threadPool;

    // Executor for periodically reloading the registry (ie. in which _periodicReload() runs).
    std::shared_ptr<executor::TaskExecutor> _executor{};

    mutable stdx::mutex _cacheMutex;
    std::unique_ptr<Cache> _cache;

    // Protects _configShardData, and _latestNewConnStrings.
    mutable stdx::mutex _mutex;

    // Store a reference to the configShard.
    ShardRegistryData _configShardData;

    // The key is replset name (the type is ShardId just to take advantage of its hasher).
    LatestConnStrings _latestConnStrings;

    AtomicWord<bool> _isInitialized{false};

    // Set to true in shutdown call to prevent calling it twice.
    AtomicWord<bool> _isShutdown{false};
};

}  // namespace mongo
