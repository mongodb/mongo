/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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

#include <boost/optional.hpp>

#include "mongo/base/status.h"
#include "mongo/client/replica_set_change_notifier.h"
#include "mongo/executor/connection_pool.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/platform/mutex.h"
#include "mongo/stdx/unordered_map.h"

namespace mongo {

class ShardRegistry;

/**
 * A special Controller for the sharding ConnectionPool
 *
 * This class has two special members:
 * * A global set of synchronized Parameters for the ShardingTaskExecutorPool server parameters
 * * A ReplicaSetChangeListener to inform it of changes to replica set membership
 *
 * When the MatchingStrategy from its Parameters is kDisabled, this class operates much like the
 * LimitController but with its limits allowed to shift at runtime (via Parameters).
 *
 * When the MatchingStrategy is kMatchPrimaryNode, the limits are obeyed but, when the pool for a
 * primary member calls updateHost, it can increase the targetConnections for the pool of each other
 * member of its replica set. Note that this will, at time of writing, follow the "hosts" field
 * from the primary isMaster combined with the seed list for the replica set. If the seed list were
 * to include arbiters or hidden members, then they would also be subject to these constraints.
 *
 * When the MatchingStrategy is kMatchBusiestNode, it operates like kMatchPrimaryNode, but any pool
 * can be responsible for increasing the targetConnections of each member of its set.
 *
 * Note that, in essence, there are three outside elements that can mutate the state of this class:
 * * The ReplicaSetChangeNotifier can notify the listener which updates the host groups
 * * The ServerParameters can update the Parameters which will used in the next update
 * * The SpecificPools for its ConnectionPool can updateHost with their individual States
 */
class ShardingTaskExecutorPoolController final
    : public executor::ConnectionPool::ControllerInterface {
    class ReplicaSetChangeListener;

public:
    using ConnectionPool = executor::ConnectionPool;

    enum class MatchingStrategy {
        kDisabled,
        kMatchPrimaryNode,
        kMatchBusiestNode,
    };

    friend StringData matchingStrategyToString(MatchingStrategy strategy) {
        switch (strategy) {
            case ShardingTaskExecutorPoolController::MatchingStrategy::kMatchPrimaryNode:
                return "matchPrimaryNode"_sd;
            case ShardingTaskExecutorPoolController::MatchingStrategy::kMatchBusiestNode:
                return "matchBusiestNode"_sd;
            case ShardingTaskExecutorPoolController::MatchingStrategy::kDisabled:
                return "disabled"_sd;
            default:
                MONGO_UNREACHABLE;
        }
    }

    class Parameters {
    public:
        AtomicWord<int> minConnections;
        AtomicWord<int> maxConnections;
        AtomicWord<int> maxConnecting;

        AtomicWord<int> hostTimeoutMS;
        AtomicWord<int> pendingTimeoutMS;
        AtomicWord<int> toRefreshTimeoutMS;

        synchronized_value<std::string> matchingStrategyString;
        AtomicWord<MatchingStrategy> matchingStrategy;

        AtomicWord<int> minConnectionsForConfigServers;
        AtomicWord<int> maxConnectionsForConfigServers;
    };

    static inline Parameters gParameters;

    /**
     * Validate that hostTimeoutMS is greater than the sum of pendingTimeoutMS and
     * toRefreshTimeoutMS
     */
    static Status validateHostTimeout(const int& hostTimeoutMS, const boost::optional<TenantId>&);

    /**
     * Validate that pendingTimeoutMS is less than toRefreshTimeoutMS
     */
    static Status validatePendingTimeout(const int& pendingTimeoutMS,
                                         const boost::optional<TenantId>&);

    /**
     *  Matches the matching strategy string against a set of literals
     *  and either sets gParameters.matchingStrategy or returns !Status::isOK().
     */
    static Status onUpdateMatchingStrategy(const std::string& str);

    explicit ShardingTaskExecutorPoolController(std::weak_ptr<ShardRegistry> shardRegistry)
        : _shardRegistry(std::move(shardRegistry)) {}
    ShardingTaskExecutorPoolController& operator=(ShardingTaskExecutorPoolController&&) = delete;

    void init(ConnectionPool* parent) override;

    void addHost(PoolId id, const HostAndPort& host) override;
    HostGroupState updateHost(PoolId id, const HostState& stats) override;
    void removeHost(PoolId id) override;

    ConnectionControls getControls(PoolId id) override;

    Milliseconds hostTimeout() const override;
    Milliseconds pendingTimeout() const override;
    Milliseconds toRefreshTimeout() const override;

    StringData name() const override {
        return "ShardingTaskExecutorPoolController"_sd;
    }

    void updateConnectionPoolStats(executor::ConnectionPoolStats* cps) const override;

private:
    void _addGroup(WithLock, const ReplicaSetChangeNotifier::State& state);
    void _removeGroup(WithLock, const std::string& key);

    /**
     * GroupData is a shared state for a set of hosts (a replica set).
     *
     * When the ReplicaSetChangeListener is informed of a change to a replica set,
     * it creates a new GroupData and fills it into _groupDatas[setName] and
     * _groupAndIds[memberHost].
     *
     * When a SpecificPool calls updateHost, it then will update target for its group according to
     * the MatchingStrategy. It will also postpone shutdown until all members of its group are ready
     * to shutdown.
     *
     * Note that a PoolData can find itself orphaned from its GroupData during a reconfig.
     */
    struct GroupData {
        // The members for this group
        std::vector<HostAndPort> members;

        // The primary member for this group
        HostAndPort primary;

        // Id for each pool in the set
        stdx::unordered_set<PoolId> poolIds;

        // The number of connections that all pools in the group should maintain
        size_t target = 0;
    };

    /**
     * PoolData represents the current state for a SpecificPool.
     *
     * It is mutated by addHost/updateHost/removeHost and used along with Parameters to form
     * Controls for getControls.
     */
    struct PoolData {
        // The host associated with this pool
        HostAndPort host;

        // A pool connected to a config server gets special treatment
        bool isConfigServer = false;

        // The GroupData associated with this pool.
        // Note that this will be invalid if there was a replica set change
        std::weak_ptr<GroupData> groupData;

        // The number of connections the host should maintain
        size_t target = 0;

        // This host is able to shutdown
        bool isAbleToShutdown = false;
    };

    /**
     * A GroupAndId allows incoming GroupData and PoolData to find each other
     *
     * Note that each side of the pair initializes independently. The side that is ctor'd last adds
     * the id to the GroupData's poolIds and a GroupData ptr to the PoolData for maybeId. Likewise,
     * the side that is dtor'd last removes the GroupAndId.
     */
    struct GroupAndId {
        std::shared_ptr<GroupData> groupData;
        boost::optional<PoolId> maybeId;
    };

    /** Needed by isConfigServer */
    std::weak_ptr<ShardRegistry> const _shardRegistry;

    std::shared_ptr<ReplicaSetChangeNotifier::Listener> _listener;

    Mutex _mutex = MONGO_MAKE_LATCH("ShardingTaskExecutorPoolController::_mutex");

    // Entires to _poolDatas are added by addHost() and removed by removeHost()
    stdx::unordered_map<PoolId, PoolData> _poolDatas;

    // Entries to _groupData are added by _addGroup() and removed by _removeGroup()
    stdx::unordered_map<std::string, std::shared_ptr<GroupData>> _groupDatas;

    // Entries to _groupAndIds are added by the first caller of either addHost() or _addGroup() and
    // removed by the last caller either removeHost() or _removeGroup(). This map exists to tie
    // together a pool and a group based on a HostAndPort. It is hopefully used once, because a
    // PoolId is much cheaper to index than a HostAndPort.
    stdx::unordered_map<HostAndPort, GroupAndId> _groupAndIds;
};
}  // namespace mongo
