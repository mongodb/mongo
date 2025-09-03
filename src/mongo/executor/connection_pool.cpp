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


#include "mongo/executor/connection_pool.h"

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/service_context.h"
#include "mongo/executor/connection_pool_stats.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_severity_suppressor.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/cancellation.h"
#include "mongo/util/debug_util.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/hierarchical_acquisition.h"
#include "mongo/util/lru_cache.h"
#include "mongo/util/observable_mutex_registry.h"
#include "mongo/util/str.h"

#include <algorithm>
#include <iterator>
#include <list>
#include <memory>
#include <queue>
#include <system_error>
#include <type_traits>

#include <absl/container/node_hash_map.h>
#include <absl/meta/type_traits.h>
#include <boost/move/utility_core.hpp>
#include <boost/smart_ptr.hpp>
#include <boost/tuple/tuple.hpp>
#include <fmt/format.h>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kConnectionPool


// One interesting implementation note herein concerns how setup() and
// refresh() are invoked outside of the global lock, but setTimeout is not.
// This implementation detail simplifies mocks, allowing them to return
// synchronously sometimes, whereas having timeouts fire instantly adds little
// value. In practice, dumping the locks is always safe (because we restrict
// ourselves to operations over the connection).

namespace mongo {

namespace {

MONGO_FAIL_POINT_DEFINE(refreshConnectionAfterEveryCommand);
MONGO_FAIL_POINT_DEFINE(forceExecutorConnectionPoolTimeout);
MONGO_FAIL_POINT_DEFINE(connectionPoolReturnsErrorOnGet);
MONGO_FAIL_POINT_DEFINE(connectionPoolDropConnectionsBeforeGetConnection);
MONGO_FAIL_POINT_DEFINE(connectionPoolDoesNotFulfillRequests);
MONGO_FAIL_POINT_DEFINE(connectionPoolRejectsConnectionRequests);
// This does not guarantee that a new connection will be returned, but rather that a request for one
// will be submitted.
MONGO_FAIL_POINT_DEFINE(connectionPoolAlwaysRequestsNewConn);

static const Status kCancelledStatus{ErrorCodes::CallbackCanceled,
                                     "Cancelled acquiring connection"};

auto makeSeveritySuppressor() {
    return std::make_unique<logv2::KeyedSeveritySuppressor<HostAndPort>>(
        Seconds{1}, logv2::LogSeverity::Log(), logv2::LogSeverity::Debug(2));
}

template <typename Map, typename Key>
auto& getOrInvariant(Map&& map, const Key& key) {
    auto it = map.find(key);
    invariant(it != map.end(), "Unable to find key in map");

    return it->second;
}

template <typename Map, typename... Args>
void emplaceOrInvariant(Map&& map, Args&&... args) {
    auto ret = std::forward<Map>(map).emplace(std::forward<Args>(args)...);
    invariant(ret.second, "Element already existed in map/set");
}

bool shouldInvariantOnPoolCorrectness() {
    return kDebugBuild;
}

}  // namespace

namespace executor {

void ConnectionPool::ConnectionInterface::indicateUsed() {
    // It is illegal to attempt to use a connection after calling indicateFailure().
    invariant(_status.isOK() || _status == ConnectionPool::kConnectionStateUnknown);
    _lastUsed = now();
    _timesUsed.fetchAndAddRelaxed(1);
}

void ConnectionPool::ConnectionInterface::indicateSuccess() {
    _status = Status::OK();
}

void ConnectionPool::ConnectionInterface::indicateFailure(Status status) {
    _status = std::move(status);
}

Date_t ConnectionPool::ConnectionInterface::getLastUsed() const {
    return _lastUsed;
}

size_t ConnectionPool::ConnectionInterface::getTimesUsed() const {
    return _timesUsed.loadRelaxed();
}

const Status& ConnectionPool::ConnectionInterface::getStatus() const {
    return _status;
}

void ConnectionPool::ConnectionInterface::resetToUnknown() {
    _status = ConnectionPool::kConnectionStateUnknown;
}

size_t ConnectionPool::ConnectionInterface::getGeneration() const {
    return _generation;
}

void ConnectionPool::ControllerInterface::init(ConnectionPool* pool) {
    invariant(pool);

    LOGV2_DEBUG(22558,
                2,
                "Initializing connection pool controller",
                "pool"_attr = pool->_name,
                "controller"_attr = name());
    _pool = pool;
}

std::string ConnectionPool::ConnectionControls::toString() const {
    return fmt::format(
        "{{ maxPending: {}, target: {}, }}", maxPendingConnections, targetConnections);
}

std::string ConnectionPool::HostState::toString() const {
    return fmt::format(
        "{{ requests: {}, ready: {}, pending: {}, active: {}, leased: {}, isExpired: {} }}",
        requests,
        ready,
        pending,
        active,
        leased,
        health.isExpired);
}

/**
 * Standard controller for the ConnectionPool
 *
 * This class uses the Options struct in the ConnectionPool to determine its controls.
 */
class ConnectionPool::LimitController final : public ConnectionPool::ControllerInterface {
public:
    void addHost(PoolId id, const HostAndPort& host) override {
        stdx::lock_guard lk(_mutex);
        PoolData poolData;
        poolData.host = host;

        emplaceOrInvariant(_poolData, id, std::move(poolData));
    }
    HostGroupState updateHost(PoolId id, const HostState& stats) override {
        stdx::lock_guard lk(_mutex);
        auto& data = getOrInvariant(_poolData, id);

        const auto minConns = getPool()->_options.minConnections;
        const auto maxConns = getPool()->_options.maxConnections;

        data.target = stats.requests + stats.active + stats.leased;
        if (data.target < minConns) {
            data.target = minConns;
        } else if (data.target > maxConns) {
            data.target = maxConns;
        }

        return {{data.host}, stats.health.isExpired};
    }
    void removeHost(PoolId id) override {
        stdx::lock_guard lk(_mutex);
        invariant(_poolData.erase(id));
    }

    ConnectionControls getControls(PoolId id) override {
        stdx::lock_guard lk(_mutex);
        const auto& data = getOrInvariant(_poolData, id);

        return {
            getPool()->_options.maxConnecting,
            data.target,
        };
    }

    Milliseconds hostTimeout() const override {
        return getPool()->_options.hostTimeout;
    }
    Milliseconds pendingTimeout() const override {
        return getPool()->_options.refreshTimeout;
    }
    Milliseconds toRefreshTimeout() const override {
        return getPool()->_options.refreshRequirement;
    }

    size_t connectionRequestsMaxQueueDepth() const override {
        return getPool()->_options.connectionRequestsMaxQueueDepth;
    }
    size_t maxConnections() const override {
        return getPool()->_options.maxConnections;
    }

    StringData name() const override {
        return "LimitController"_sd;
    }

    void updateConnectionPoolStats(ConnectionPoolStats* cps) const override {}

protected:
    struct PoolData {
        HostAndPort host;
        size_t target = 0;
    };

    stdx::mutex _mutex;
    stdx::unordered_map<PoolId, PoolData> _poolData;
};


auto ConnectionPool::makeLimitController() -> std::shared_ptr<ControllerInterface> {
    return std::make_shared<LimitController>();
}

/**
 * A pool for a specific HostAndPort
 *
 * Pools come into existance the first time a connection is requested and
 * go out of existence after hostTimeout passes without any of their
 * connections being used.
 */
class ConnectionPool::SpecificPool final
    : public std::enable_shared_from_this<ConnectionPool::SpecificPool> {
    static constexpr auto kDiagnosticLogLevel = 4;

public:
    /**
     * Whenever a function enters a specific pool, the function needs to be guarded by the lock.
     *
     * This callback also (perhaps overly aggressively) binds a shared pointer to the guard.
     * It is *always* safe to reference the original specific pool in the guarded function object.
     *
     * For a function object of signature:
     * void riskyBusiness(stdx::unique_lock<ObservableMutex<stdx::mutex>>&, ArgTypes...);
     *
     * It returns a function object of signature:
     * void safeCallback(ArgTypes...);
     */
    template <typename Callback>
    auto guardCallback(Callback&& cb) {
        return
            [this, cb = std::forward<Callback>(cb), anchor = shared_from_this()](auto&&... args) {
                stdx::unique_lock lk(_parent->_mutex);
                cb(lk, std::forward<decltype(args)>(args)...);
                invariant(lk.owns_lock(), "Callback released, but did not reacquire the lock.");
                updateState(lk);
            };
    }

    SpecificPool(std::shared_ptr<ConnectionPool> parent,
                 const HostAndPort& hostAndPort,
                 transport::ConnectSSLMode sslMode);
    ~SpecificPool();

    /**
     * Create and initialize a SpecificPool
     */
    static auto make(std::shared_ptr<ConnectionPool> parent,
                     const HostAndPort& hostAndPort,
                     transport::ConnectSSLMode sslMode);

    /**
     * Triggers a controller update, potentially changes the request timer,
     * and maybe delists from pool.
     */
    void updateState(WithLock);

    /**
     * Gets a connection from the specific pool.
     */
    Future<ConnectionHandle> getConnection(WithLock,
                                           Milliseconds timeout,
                                           bool lease,
                                           const CancellationToken& token);

    /**
     * Completely shuts down this pool. Use `initiateShutdown` and `processFailure` for
     * finer-grained control. Destruction is deferred to when the ConnectionHandles are deleted.
     */
    void shutdown(stdx::unique_lock<ObservableMutex<stdx::mutex>>& lk, const Status& status);

    /**
     * Initiates the shutdown process and returns true if it has not already been initiated.
     * Otherwise, does nothing and returns false. The caller is responsible for removing this pool
     * from the ConnectionPool's internal data structures. Call processFailure after that to
     * complete shutdown of any number of in progress SpecificPool shutdown operations.
     */
    bool initiateShutdown(WithLock);

    /**
     * Cascades a failure across existing connections and requests. Invoking
     * this function drops all current connections and fails all current
     * requests with the passed status.
     */
    void processFailure(stdx::unique_lock<ObservableMutex<stdx::mutex>>& lk, const Status& status);

    /**
     * Returns the number of connections currently checked out of the pool.
     */
    size_t inUseConnections(WithLock) const;

    /**
     * Returns the number of leased connections from the pool.
     */
    size_t leasedConnections(WithLock) const;

    /**
     * Returns the number of available connections in the pool.
     */
    size_t availableConnections(WithLock) const;

    /**
     * Returns the number of in progress connections in the pool.
     */
    size_t refreshingConnections(WithLock) const;

    /**
     * Returns the number of all refreshed connections in the pool.
     */
    size_t refreshedConnections(WithLock) const;

    /**
     * Returns the total number of connections ever created in this pool.
     */
    size_t createdConnections(WithLock) const;

    /**
     * Returns the number of connections that expire and are destroyed before they are ever used.
     */
    size_t neverUsedConnections(WithLock) const;

    /**
     * Returns the number of connections that were used only once before being destroyed.
     */
    size_t getOnceUsedConnections(WithLock) const;

    /**
     * Returns the cumulative amount of time connections were in use by operations.
     */
    Milliseconds getTotalConnUsageTime(WithLock) const;

    /**
     * Returns the total number of connections currently open that belong to
     * this pool. This is the sum of refreshingConnections, availableConnections,
     * inUseConnections, and leasedConnections.
     */
    size_t openConnections(WithLock) const;

    /**
     * Returns the number of unfulfilled requests pending.
     */
    size_t requestsPending(WithLock) const;

    /**
     * Returns the number of rejected requests.
     */
    size_t rejectedConnectionsRequests(WithLock) const;

    /**
     * Updates the ConnectionPool's _cachedCreatedConnections map with the number of created
     * connections that this SpecificPool made. This should be called before removing a SpecificPool
     * from the ConnectionPool.
     */
    void updateCachedCreatedConnections(WithLock);

    /**
     * Returns the number of cached created connections associated with _hostAndPort.
     */
    size_t getCachedCreatedConnections(WithLock) const;

    /**
     * Records the time it took to return the connection since it was requested, so that it can be
     * reported in the connection pool stats.
     */
    void recordConnectionWaitTime(WithLock, Date_t requestedAt) {
        _connAcquisitionWaitTimeStats.increment(_parent->_factory->now() - requestedAt);
    }

    /**
     * Returns connection acquisition wait time statistics to be included in the connection pool
     * stats.
     */
    const ConnectionWaitTimeHistogram& connectionWaitTimeStats(WithLock) {
        return _connAcquisitionWaitTimeStats;
    };

    /**
     * Returns the HostAndPort for this pool.
     */
    const HostAndPort& host(WithLock) const {
        return _hostAndPort;
    }

    /**
     * Return true if the specific pool should be kept open.
     */
    bool isKeepOpen(WithLock) const {
        return _keepOpen;
    }

    void setKeepOpen(WithLock, bool keepOpen) {
        _keepOpen = keepOpen;
    }

    void fassertSSLModeIs(WithLock, transport::ConnectSSLMode desired) const {
        if (desired != _sslMode) {
            LOGV2_FATAL_NOTRACE(51043, "Mixing ssl modes for a single host is not supported");
        }
    }

private:
    using OwnedConnection = std::shared_ptr<ConnectionInterface>;
    using OwnershipPool = stdx::unordered_map<ConnectionInterface*, OwnedConnection>;
    using LRUOwnershipPool = LRUCache<OwnershipPool::key_type, OwnershipPool::mapped_type>;
    struct Request {
        Request(std::uint64_t i, Promise<ConnectionHandle>&& p, bool l, const CancellationToken& t)
            : id(i), promise(std::move(p)), lease(l), source(t) {}
        std::uint64_t id;
        Promise<ConnectionHandle> promise;
        // Whether or not the requested connection should be "leased".
        bool lease;
        CancellationSource source;
    };
    using Requests = std::multimap<Date_t, Request>;

    // Enqueues a request, returning an iterator into _requests pointing to the request.
    Requests::iterator pushRequest(WithLock, Date_t expiration, Request request) {

        auto it = _requests.emplace(std::pair(expiration, std::move(request)));
        _requestsById[it->second.id] = it;
        return it;
    }

    // Removes the pointed-to request and returns its associated promise for the caller to fulfill.
    Promise<ConnectionHandle> popRequest(WithLock, Requests::iterator it) {
        auto promise = std::move(it->second.promise);
        _requestsById.erase(it->second.id);
        _requests.erase(it);
        return promise;
    }

    ConnectionHandle makeHandle(ConnectionInterface* connection, bool isLeased);

    /**
     * Given a uniquely-owned OwnedConnection, returns an OwnedConnection
     * pointing to the same object, but which gathers stats just before destruction.
     */
    OwnedConnection makeDeathNotificationWrapper(OwnedConnection h) {
        invariant(h.use_count() == 1);
        struct ConnWrap {
            ConnWrap(OwnedConnection conn, std::weak_ptr<SpecificPool> owner)
                : conn{std::move(conn)}, owner{std::move(owner)} {}
            ~ConnWrap() {
                if (conn->getTimesUsed() == 0) {
                    if (auto ownerSp = owner.lock())
                        ownerSp->_neverUsed.fetchAndAddRelaxed(1);
                } else if (conn->getTimesUsed() == 1) {
                    if (auto ownerSp = owner.lock())
                        ownerSp->_usedOnce.fetchAndAddRelaxed(1);
                }
            }
            const OwnedConnection conn;
            const std::weak_ptr<SpecificPool> owner;
        };
        ConnectionInterface* ptr = h.get();
        return {std::make_shared<ConnWrap>(std::move(h), shared_from_this()), ptr};
    }

    // Special case for getConnection when the forceExecutorConnectionPoolTimeout FailPoint is set.
    Future<ConnectionHandle> setGetConnectionTimeout(WithLock,
                                                     FailPoint::LockHandle sfp,
                                                     Milliseconds timeout,
                                                     bool lease,
                                                     const CancellationToken& token);

    /**
     * Establishes connections until the ControllerInterface's target is met.
     */
    void spawnConnections(WithLock lk);

    void finishRefresh(stdx::unique_lock<ObservableMutex<stdx::mutex>>& lk,
                       ConnectionInterface* connPtr,
                       Status status);

    void addToReady(WithLock, OwnedConnection conn);

    void fulfillRequests(stdx::unique_lock<ObservableMutex<stdx::mutex>>& lk);

    void returnConnection(stdx::unique_lock<ObservableMutex<stdx::mutex>>& lk,
                          ConnectionInterface* connPtr,
                          bool isLeased);

    // This internal helper is used both by get and by _fulfillRequests and differs in that it
    // skips some bookkeeping that the other callers do on their own
    ConnectionHandle tryGetConnection(WithLock, bool lease);

    template <typename OwnershipPoolType>
    typename OwnershipPoolType::mapped_type takeFromPool(
        WithLock, OwnershipPoolType& pool, typename OwnershipPoolType::key_type connPtr);

    OwnedConnection takeFromProcessingPool(WithLock, ConnectionInterface* connection);

    // Update the health struct and related variables
    void updateHealth();

    // Update the event timer for this host pool
    void updateEventTimer();

    // Update the controller and potentially change the controls
    void updateController(stdx::unique_lock<ObservableMutex<stdx::mutex>>& lk);

private:
    /**
     * Returns Status::OK if the request can be serviced.
     * Returns a non-OK Status if the request should be rejected instead.
     */
    Status _verifyCanServiceRequest(WithLock);

    const std::shared_ptr<ConnectionPool> _parent;

    const transport::ConnectSSLMode _sslMode;
    const HostAndPort _hostAndPort;

    const PoolId _id;

    LRUOwnershipPool _readyPool;
    OwnershipPool _processingPool;
    OwnershipPool _droppedProcessingPool;
    OwnershipPool _checkedOutPool;
    OwnershipPool _leasedPool;

    Requests _requests;
    stdx::unordered_map<std::uint64_t, Requests::iterator> _requestsById;
    Date_t _lastActiveTime;

    std::shared_ptr<TimerInterface> _eventTimer;
    Date_t _eventTimerExpiration;
    Date_t _hostExpiration;

    // The _generation is the set of connection objects we believe are healthy.
    // It increases when we process a failure. If a connection is from a previous generation,
    // it will be discarded on return/refresh.
    size_t _generation = 0;

    // When the pool needs to potentially die or spawn connections, updateController() is scheduled
    // onto the executor and this flag is set. When updateController() finishes running, this flag
    // is unset. This allows the pool to amortize the expensive spawning and hopefully do work once
    // it is closer to steady state.
    bool _updateScheduled = false;

    size_t _created = 0;

    size_t _refreshed = 0;

    AtomicWord<size_t> _neverUsed{0};

    AtomicWord<size_t> _usedOnce{0};

    Milliseconds _totalConnUsageTime{0};

    ConnectionWaitTimeHistogram _connAcquisitionWaitTimeStats{};

    // Indicates connections associated with this HostAndPort should be kept open.
    bool _keepOpen = true;

    HostHealth _health;

    std::uint64_t _nextRequestId{0};

    logv2::SeveritySuppressor _rejectedConnectionsLogSeverity{
        Seconds{5}, logv2::LogSeverity::Warning(), logv2::LogSeverity::Debug(5)};
    // The overall number of rejected connections used for stats.
    size_t _numberRejectedConnections{0};
};

auto ConnectionPool::SpecificPool::make(std::shared_ptr<ConnectionPool> parent,
                                        const HostAndPort& hostAndPort,
                                        transport::ConnectSSLMode sslMode) {
    auto& controller = *parent->_controller;

    auto pool = std::make_shared<SpecificPool>(std::move(parent), hostAndPort, sslMode);

    // Inform the controller that we exist
    controller.addHost(pool->_id, hostAndPort);

    // Set our timers and health
    pool->updateEventTimer();
    pool->updateHealth();
    return pool;
}

const Status ConnectionPool::kConnectionStateUnknown =
    Status(ErrorCodes::InternalError, "Connection is in an unknown state");

ConnectionPool::ConnectionPool(std::shared_ptr<DependentTypeFactoryInterface> impl,
                               std::string name,
                               Options options)
    : _name(std::move(name)),
      _factory(std::move(impl)),
      _options(std::move(options)),
      _controller(_options.controllerFactory()),
      _manager(_options.egressConnectionCloserManager) {
    ObservableMutexRegistry::get().add("ConnectionPool::_mutex", _mutex);

    if (_manager) {
        _manager->add(this);
    }

    invariant(_controller);
    _controller->init(this);
}

ConnectionPool::~ConnectionPool() {
    // If we're currently destroying the service context the _manager is already deleted and this
    // pointer dangles. No need for cleanup in that case.
    if (hasGlobalServiceContext() && _manager) {
        _manager->remove(this);
    }

    shutdown();
}

void ConnectionPool::shutdown() {
    stdx::unique_lock lk(_mutex);
    if (std::exchange(_isShutDown, true)) {
        return;
    }

    _factory->shutdown();

    // processFailure() releases the lock to fulfill promises. We know we want to destroy all the
    // pools, so to avoid issues with _pools changing while the lock is released, we'll move them
    // all into a local variable.
    std::vector<std::shared_ptr<SpecificPool>> pools;
    pools.reserve(_pools.size());

    // Delist all of the pools while still holding the lock. We need to ensure all of them are
    // delisted without releasing the lock so updateController doesn't see an inconsistent state
    // where some SpecificPools have been removed from _pools but have not been delisted.
    for (auto& pair : _pools) {
        if (MONGO_likely(pair.second->initiateShutdown(lk))) {
            pools.push_back(std::move(pair.second));
        }
    }

    _pools.clear();

    // Cascade a failure across all of the pools.
    for (const auto& pool : pools) {
        pool->processFailure(
            lk, Status(ErrorCodes::ShutdownInProgress, "Shutting down the connection pool"));
        invariant(lk.owns_lock(), "processFailure released, but did not reacquire the lock.");
    }
}

void ConnectionPool::dropConnections(const HostAndPort& target, const Status& status) {
    stdx::unique_lock lk(_mutex);

    auto iter = _pools.find(target);

    if (iter == _pools.end())
        return;

    auto& pool = iter->second;
    pool->shutdown(lk, status);
}

void ConnectionPool::dropConnections(const Status& status) {
    stdx::unique_lock lk(_mutex);

    // Grab all of the pools we're going to drop connections for. This is necessary because
    // processFailure() releases the lock, and we need to avoid the possibility of _pools
    // being modified while the lock is released.
    std::vector<std::shared_ptr<SpecificPool>> pools;
    pools.reserve(_pools.size());
    for (auto it = _pools.begin(); it != _pools.end();) {
        auto cur = it++;
        if (cur->second->isKeepOpen(lk)) {
            continue;
        }
        auto pool = std::move(cur->second);
        if (MONGO_likely(pool->initiateShutdown(lk))) {
            pools.push_back(std::move(pool));
        }
        _pools.erase(cur);
    }

    // Cascade the failure across all of the pools we're dropping.
    for (const auto& pool : pools) {
        pool->processFailure(lk, status);
        invariant(lk.owns_lock(), "processFailure released, but did not reacquire the lock.");
    }
}

void ConnectionPool::setKeepOpen(const HostAndPort& hostAndPort, bool keepOpen) {
    stdx::lock_guard lk(_mutex);

    auto iter = _pools.find(hostAndPort);

    if (iter == _pools.end())
        return;

    auto pool = iter->second;
    pool->setKeepOpen(lk, keepOpen);
}

void ConnectionPool::retrieve_forTest(RetrieveConnection retrieve, GetConnectionCallback cb) {
    // We kick ourselves onto the executor queue to prevent us from deadlocking with our own thread
    auto getConnectionFunc =
        [this, retrieve = std::move(retrieve), cb = std::move(cb)](Status&&) mutable {
            retrieve().thenRunOn(_factory->getExecutor()).getAsync(std::move(cb));
        };
    _factory->getExecutor()->schedule(std::move(getConnectionFunc));
}

void ConnectionPool::get_forTest(const HostAndPort& hostAndPort,
                                 Milliseconds timeout,
                                 GetConnectionCallback cb) {
    auto getConnectionFunc = [this, hostAndPort, timeout]() mutable {
        return get(hostAndPort, transport::kGlobalSSLMode, timeout);
    };
    retrieve_forTest(getConnectionFunc, std::move(cb));
}

void ConnectionPool::lease_forTest(const HostAndPort& hostAndPort,
                                   Milliseconds timeout,
                                   GetConnectionCallback cb) {
    auto getConnectionFunc = [this, hostAndPort, timeout]() mutable {
        return lease(hostAndPort, transport::kGlobalSSLMode, timeout);
    };
    retrieve_forTest(getConnectionFunc, std::move(cb));
}

SemiFuture<ConnectionPool::ConnectionHandle> ConnectionPool::_get(const HostAndPort& hostAndPort,
                                                                  transport::ConnectSSLMode sslMode,
                                                                  Milliseconds timeout,
                                                                  bool lease,
                                                                  const CancellationToken& token) {
    auto connRequestedAt = _factory->now();

    stdx::unique_lock lk(_mutex);

    if (_isShutDown) {
        return Status(ErrorCodes::ShutdownInProgress,
                      "Cannot retrieve connection because pool is shutting down");
    }

    auto& pool = _pools[hostAndPort];
    if (!pool) {
        pool = SpecificPool::make(shared_from_this(), hostAndPort, sslMode);
    } else {
        pool->fassertSSLModeIs(lk, sslMode);
    }

    invariant(pool);

    if (MONGO_unlikely(connectionPoolDropConnectionsBeforeGetConnection.shouldFail())) {
        if (auto sfp = connectionPoolDropConnectionsBeforeGetConnection.scoped();
            MONGO_unlikely(sfp.isActive())) {
            std::string nameToTimeout = sfp.getData()["instance"].String();
            if (_name.substr(0, nameToTimeout.size()) == nameToTimeout) {
                // Drop all connections so new connections can be set up via getConnection.
                pool->processFailure(lk,
                                     Status(ErrorCodes::HostUnreachable,
                                            "Test dropping connections before initial handshake"));
            }
        }
    }

    // In case of no timeout, set timeout to the refresh timeout.
    if (timeout < Milliseconds(0)) {
        timeout = _controller->pendingTimeout();
    }

    auto connFuture = pool->getConnection(lk, timeout, lease, token);
    pool->updateState(lk);

    if (lease) {
        return std::move(connFuture).semi();
    }

    std::shared_ptr<SpecificPool> poolAnchor = pool;
    lk.unlock();

    // Only count connections being checked-out for ordinary use, not lease, towards cumulative wait
    // time.
    return std::move(connFuture)
        .tap([this, connRequestedAt, pool = std::move(poolAnchor)](const auto& conn) {
            stdx::lock_guard lk(_mutex);
            pool->recordConnectionWaitTime(lk, connRequestedAt);
        })
        .semi();
}

void ConnectionPool::appendConnectionStats(ConnectionPoolStats* stats) const {
    stdx::unique_lock lk(_mutex);

    _controller->updateConnectionPoolStats(stats);
    for (const auto& kv : _pools) {
        HostAndPort host = kv.first;

        auto& pool = kv.second;
        ConnectionStatsPer hostStats{pool->inUseConnections(lk),
                                     pool->availableConnections(lk),
                                     pool->leasedConnections(lk),
                                     pool->createdConnections(lk),
                                     pool->refreshingConnections(lk),
                                     pool->refreshedConnections(lk),
                                     pool->neverUsedConnections(lk),
                                     pool->getOnceUsedConnections(lk),
                                     pool->getTotalConnUsageTime(lk),
                                     pool->rejectedConnectionsRequests(lk),
                                     pool->requestsPending(lk)};

        hostStats.acquisitionWaitTimes = pool->connectionWaitTimeStats(lk);
        stats->updateStatsForHost(_name, host, hostStats);
    }
    for (const auto& kv : _cachedCreatedConnections) {
        ConnectionStatsPer hostStats;
        hostStats.created = kv.second;

        stats->updateStatsForHost(_name, kv.first, hostStats);
    }
}

size_t ConnectionPool::getNumConnectionsPerHost(const HostAndPort& hostAndPort) const {
    stdx::unique_lock lk(_mutex);
    auto iter = _pools.find(hostAndPort);
    if (iter != _pools.end()) {
        return iter->second->openConnections(lk);
    }

    return 0;
}

ConnectionPool::SpecificPool::SpecificPool(std::shared_ptr<ConnectionPool> parent,
                                           const HostAndPort& hostAndPort,
                                           transport::ConnectSSLMode sslMode)
    : _parent(std::move(parent)),
      _sslMode(sslMode),
      _hostAndPort(hostAndPort),
      _id(_parent->_nextPoolId++),
      _readyPool(std::numeric_limits<size_t>::max()) {
    invariant(_parent);
    _eventTimer = _parent->_factory->makeTimer();
}

ConnectionPool::SpecificPool::~SpecificPool() {
    try {
        _eventTimer->cancelTimeout();
    } catch (...) {
        reportFailedDestructor(MONGO_SOURCE_LOCATION());
    }

    if (shouldInvariantOnPoolCorrectness()) {
        invariant(_requests.empty());
        invariant(_requestsById.empty());
        invariant(_checkedOutPool.empty());
        invariant(_leasedPool.empty());
    }
}

size_t ConnectionPool::SpecificPool::inUseConnections(WithLock) const {
    return _checkedOutPool.size();
}

size_t ConnectionPool::SpecificPool::availableConnections(WithLock) const {
    return _readyPool.size();
}

size_t ConnectionPool::SpecificPool::leasedConnections(WithLock) const {
    return _leasedPool.size();
}

size_t ConnectionPool::SpecificPool::refreshingConnections(WithLock) const {
    return _processingPool.size();
}

size_t ConnectionPool::SpecificPool::refreshedConnections(WithLock) const {
    return _refreshed;
}

size_t ConnectionPool::SpecificPool::createdConnections(WithLock) const {
    return _created;
}

size_t ConnectionPool::SpecificPool::neverUsedConnections(WithLock) const {
    return _neverUsed.loadRelaxed();
}

size_t ConnectionPool::SpecificPool::getOnceUsedConnections(WithLock) const {
    return _usedOnce.loadRelaxed();
}

Milliseconds ConnectionPool::SpecificPool::getTotalConnUsageTime(WithLock) const {
    return _totalConnUsageTime;
}

size_t ConnectionPool::SpecificPool::openConnections(WithLock) const {
    return _checkedOutPool.size() + _readyPool.size() + _processingPool.size() + _leasedPool.size();
}

size_t ConnectionPool::SpecificPool::requestsPending(WithLock) const {
    return _requests.size();
}

size_t ConnectionPool::SpecificPool::rejectedConnectionsRequests(WithLock) const {
    return _numberRejectedConnections;
}

Status ConnectionPool::SpecificPool::_verifyCanServiceRequest(WithLock) {
    if (MONGO_unlikely(connectionPoolRejectsConnectionRequests.shouldFail())) {
        return Status(
            ErrorCodes::PooledConnectionAcquisitionRejected,
            "Rejecting request due to 'connectionPoolRejectsConnectionRequests' failpoint");
    }

    const size_t connectionRequestsMaxQueueDepth =
        _parent->_controller->connectionRequestsMaxQueueDepth();

    // If the value of connectionRequestsMaxQueueDepth is 0, then the feature is disabled and no
    // further checks should be performed here: the request should NOT be rejected.
    if (connectionRequestsMaxQueueDepth > 0 &&
        _requests.size() >= connectionRequestsMaxQueueDepth) {
        return Status{ErrorCodes::PooledConnectionAcquisitionRejected,
                      fmt::format("Maximum request queue depth for host '{}' in pool '{}' was "
                                  "exceeded (max queue depth = {}, max connections = {})",
                                  _hostAndPort,
                                  _parent->getName(),
                                  connectionRequestsMaxQueueDepth,
                                  _parent->_controller->maxConnections())};
    }

    return Status::OK();
}

void ConnectionPool::SpecificPool::updateCachedCreatedConnections(WithLock lk) {
    auto totalCreated = createdConnections(lk) + getCachedCreatedConnections(lk);
    _parent->_cachedCreatedConnections[_hostAndPort] = totalCreated;
}

size_t ConnectionPool::SpecificPool::getCachedCreatedConnections(WithLock) const {
    auto elem = _parent->_cachedCreatedConnections.find(_hostAndPort);
    if (elem == _parent->_cachedCreatedConnections.end()) {
        return 0;
    }

    return elem->second;
}

Future<ConnectionPool::ConnectionHandle> ConnectionPool::SpecificPool::getConnection(
    WithLock lk, Milliseconds timeout, bool lease, const CancellationToken& token) {

    if (MONGO_unlikely(connectionPoolReturnsErrorOnGet.shouldFail())) {
        return Future<ConnectionPool::ConnectionHandle>::makeReady(
            Status(ErrorCodes::SocketException, "test"));
    }

    // Reset our activity timestamp
    auto now = _parent->_factory->now();
    _lastActiveTime = now;

    if (auto sfp = forceExecutorConnectionPoolTimeout.scoped(); MONGO_unlikely(sfp.isActive())) {
        return setGetConnectionTimeout(lk, std::move(sfp), timeout, lease, token);
    }

    // If we've already been cancelled or we do not have requests, then we can fulfill immediately.
    if (token.isCanceled()) {
        return Future<ConnectionPool::ConnectionHandle>::makeReady(kCancelledStatus);
    }

    // If a queue of requests for connections exceeds a certain size, do not accept
    // new requests and reject them.
    if (auto s = _verifyCanServiceRequest(lk); MONGO_unlikely(!s.isOK())) {
        _numberRejectedConnections++;

        LOGV2_DEBUG(9147901,
                    _rejectedConnectionsLogSeverity().toInt(),
                    "Rejecting connection acquisition attempt",
                    "name"_attr = _parent->getName(),
                    "hostAndPort"_attr = _hostAndPort,
                    "reason"_attr = s,
                    "totalRejectedRequests"_attr = _numberRejectedConnections);
        return s;
    }

    if (_requests.size() == 0 && MONGO_likely(!connectionPoolAlwaysRequestsNewConn.shouldFail())) {
        auto conn = tryGetConnection(lk, lease);

        if (conn) {
            LOGV2_DEBUG(22559,
                        kDiagnosticLogLevel,
                        "Using existing idle connection",
                        "hostAndPort"_attr = _hostAndPort);
            return Future<ConnectionPool::ConnectionHandle>::makeReady(std::move(conn));
        }
    }

    LOGV2_DEBUG(22560,
                kDiagnosticLogLevel,
                "Requesting new connection",
                "hostAndPort"_attr = _hostAndPort,
                "timeout"_attr = timeout);

    const auto expiration = now + timeout;
    auto pf = makePromiseFuture<ConnectionHandle>();

    auto requestId = _nextRequestId++;
    auto it = pushRequest(lk, expiration, Request(requestId, std::move(pf.promise), lease, token));

    it->second.source.token()
        .onCancel()
        .thenRunOn(_parent->_factory->getExecutor())
        .getAsync([this, requestId](Status s) {
            if (!s.isOK()) {
                return;
            }

            stdx::unique_lock lk(_parent->_mutex);

            auto it = _requestsById.find(requestId);
            if (it == _requestsById.end()) {
                return;
            }
            auto promise = popRequest(lk, it->second);
            lk.unlock();
            promise.setError(kCancelledStatus);
        });

    return std::move(pf.future);
}

auto ConnectionPool::SpecificPool::makeHandle(ConnectionInterface* connection, bool isLeased)
    -> ConnectionHandle {
    auto connUseStartedAt = _parent->_getFastClockSource()->now();
    auto deleter = [this, anchor = shared_from_this(), connUseStartedAt, isLeased](
                       ConnectionInterface* connection) {
        stdx::unique_lock lk(_parent->_mutex);

        // Leased connections don't count towards the pool's total connection usage time.
        if (!isLeased) {
            _totalConnUsageTime += _parent->_getFastClockSource()->now() - connUseStartedAt;
        }

        returnConnection(lk, connection, isLeased);
        _lastActiveTime = _parent->_factory->now();
        updateState(lk);
    };
    return ConnectionHandle(connection, std::move(deleter));
}

ConnectionPool::ConnectionHandle ConnectionPool::SpecificPool::tryGetConnection(WithLock,
                                                                                bool lease) {
    while (_readyPool.size()) {
        // _readyPool is an LRUCache, so its begin() object is the MRU item.
        auto iter = _readyPool.begin();

        // Grab the connection and cancel its timeout
        auto conn = std::move(iter->second);
        _readyPool.erase(iter);
        conn->cancelTimeout();

        if (!conn->maybeHealthy()) {
            LOGV2(22561,
                  "Dropping unhealthy pooled connection",
                  "hostAndPort"_attr = conn->getHostAndPort());

            // Drop the bad connection via scoped destruction and retry
            continue;
        }

        // Use a reference to the target map location as our connection,
        // so we're not juggling raw pointers any more than we need to.
        OwnedConnection& mappedConn = (lease ? _leasedPool : _checkedOutPool)[conn.get()];
        mappedConn = std::move(conn);

        // pass it to the user
        mappedConn->resetToUnknown();
        auto handle = makeHandle(mappedConn.get(), lease);
        return handle;
    }

    return {};
}

void ConnectionPool::SpecificPool::finishRefresh(
    stdx::unique_lock<ObservableMutex<stdx::mutex>>& lk,
    ConnectionInterface* connPtr,
    Status status) {
    auto conn = takeFromProcessingPool(lk, connPtr);

    // We increment the total number of refreshed connections right upfront to track all completed
    // refreshes.
    _refreshed++;

    // If we're in shutdown, we don't need refreshed connections
    if (_health.isShutdown) {
        return;
    }

    // If the error can be contained to one connection, drop the one connection.
    if (status.code() == ErrorCodes::ConnectionError) {
        LOGV2_DEBUG(6832901,
                    kDiagnosticLogLevel,
                    "Dropping single connection",
                    "hostAndPort"_attr = _hostAndPort,
                    "error"_attr = redact(status),
                    "numOpenConns"_attr = openConnections(lk));
        return;
    }

    // If the host and port were dropped, let this lapse and spawn new connections
    if (!conn || conn->getGeneration() != _generation) {
        LOGV2_DEBUG(22564,
                    kDiagnosticLogLevel,
                    "Dropping late refreshed connection",
                    "hostAndPort"_attr = _hostAndPort);
        return;
    }

    // Pass a failure on through
    if (!status.isOK()) {
        LOGV2_DEBUG(22563,
                    kDiagnosticLogLevel,
                    "Connection failed",
                    "hostAndPort"_attr = _hostAndPort,
                    "error"_attr = redact(status));
        processFailure(lk, status);
        return;
    }

    LOGV2_DEBUG(22565,
                kDiagnosticLogLevel,
                "Finishing connection refresh",
                "hostAndPort"_attr = _hostAndPort);

    // If the connection refreshed successfully, throw it back in the ready pool
    addToReady(lk, std::move(conn));

    fulfillRequests(lk);
}

void ConnectionPool::SpecificPool::returnConnection(
    stdx::unique_lock<ObservableMutex<stdx::mutex>>& lk,
    ConnectionInterface* connPtr,
    bool isLeased) {
    auto needsRefreshTP = connPtr->getLastUsed() + _parent->_controller->toRefreshTimeout();

    auto conn = takeFromPool(lk, isLeased ? _leasedPool : _checkedOutPool, connPtr);
    invariant(conn);

    if (_health.isShutdown) {
        // If we're in shutdown, then we don't care
        return;
    }

    if (conn->getGeneration() != _generation) {
        // If the connection is from an older generation, just return.
        return;
    }

    if (auto status = conn->getStatus(); !status.isOK()) {
        // Our error handling here is determined by the MongoDB SDAM specification for handling
        // application errors on established connections. In particular, if a network error occurs,
        // we must close all idle sockets in the connection pool for the server: "if one socket is
        // bad, it is likely that all are." However, if the error is just a network _timeout_ error,
        // we don't drop the connections because the timeout may indicate a slow operation rather
        // than an unavailable server. Additionally, if we can isolate the error to a single
        // socket/connection based on it's type, we won't drop other connections/sockets.
        //
        // See the spec for additional details:
        // https://github.com/mongodb/specifications/blob/master/source/server-discovery-and-monitoring/server-discovery-and-monitoring.rst#application-errors
        bool isSingleConnectionError = status.code() == ErrorCodes::ConnectionError;
        if (ErrorCodes::isNetworkError(status) && !isSingleConnectionError &&
            !ErrorCodes::isNetworkTimeoutError(status)) {
            LOGV2_DEBUG(7719500,
                        kDiagnosticLogLevel,
                        "Connection failed",
                        "hostAndPort"_attr = _hostAndPort,
                        "error"_attr = redact(status));
            processFailure(lk, status);
            return;
        }
        // Otherwise, drop the one connection.
        LOGV2(22566,
              "Ending connection due to bad connection status",
              "hostAndPort"_attr = _hostAndPort,
              "error"_attr = redact(status),
              "numOpenConns"_attr = openConnections(lk));
        return;
    }

    // If we need to refresh this connection
    bool shouldRefreshConnection = needsRefreshTP <= _parent->_factory->now();

    if (MONGO_unlikely(refreshConnectionAfterEveryCommand.shouldFail())) {
        LOGV2(5505501, "refresh connection after every command is on");
        shouldRefreshConnection = true;
    }

    if (shouldRefreshConnection) {
        auto controls = _parent->_controller->getControls(_id);
        if (openConnections(lk) >= controls.targetConnections) {
            // If we already have minConnections, just let the connection lapse
            LOGV2(22567,
                  "Ending idle connection because the pool meets constraints",
                  "hostAndPort"_attr = _hostAndPort,
                  "numOpenConns"_attr = openConnections(lk));
            return;
        }

        _processingPool[connPtr] = std::move(conn);

        LOGV2_DEBUG(
            22568, kDiagnosticLogLevel, "Refreshing connection", "hostAndPort"_attr = _hostAndPort);
        connPtr->refresh(_parent->_controller->pendingTimeout(),
                         guardCallback([this](auto& lk, auto conn, auto status) {
                             finishRefresh(lk, std::move(conn), std::move(status));
                         }));

        return;
    }

    // If it's fine as it is, just put it in the ready queue
    LOGV2_DEBUG(22569,
                kDiagnosticLogLevel,
                "Returning ready connection",
                "hostAndPort"_attr = _hostAndPort);
    addToReady(lk, std::move(conn));

    fulfillRequests(lk);
}

// Adds a live connection to the ready pool
void ConnectionPool::SpecificPool::addToReady(WithLock, OwnedConnection conn) {
    auto connPtr = conn.get();

    // This makes the connection the new most-recently-used connection.
    _readyPool.add(connPtr, std::move(conn));

    // Our strategy for refreshing connections is to check them out and
    // immediately check them back in (which kicks off the refresh logic in
    // returnConnection
    auto returnConnectionFunc = guardCallback([this, connPtr](auto& lk) {
        LOGV2_DEBUG(22570,
                    kDiagnosticLogLevel,
                    "Triggered refresh timeout",
                    "hostAndPort"_attr = _hostAndPort);
        auto conn = takeFromPool(lk, _readyPool, connPtr);

        // We've already been checked out. We don't need to refresh ourselves.
        if (!conn)
            return;

        // If we're in shutdown, we don't need to refresh connections
        if (_health.isShutdown)
            return;

        _checkedOutPool[connPtr] = std::move(conn);

        connPtr->indicateSuccess();

        returnConnection(lk, connPtr, false);
    });
    connPtr->setTimeout(_parent->_controller->toRefreshTimeout(), std::move(returnConnectionFunc));
}

bool ConnectionPool::SpecificPool::initiateShutdown(WithLock lk) {
    auto wasShutdown = std::exchange(_health.isShutdown, true);
    if (wasShutdown) {
        return false;
    }

    LOGV2_DEBUG(22571, 2, "Delisting connection pool", "hostAndPort"_attr = _hostAndPort);

    updateCachedCreatedConnections(lk);

    _parent->_controller->removeHost(_id);

    _droppedProcessingPool.clear();
    _eventTimer->cancelTimeout();

    return true;
}

// Sets state to shutdown and kicks off the failure protocol to tank existing connections
void ConnectionPool::SpecificPool::shutdown(stdx::unique_lock<ObservableMutex<stdx::mutex>>& lk,
                                            const Status& status) {
    if (!initiateShutdown(lk)) {
        return;
    }

    // Make sure the pool lifetime lasts until the end of this function, it could be only in the
    // map of pools.
    auto anchor = shared_from_this();
    _parent->_pools.erase(_hostAndPort);

    processFailure(lk, status);
}

// Drop connections and fail all requests
void ConnectionPool::SpecificPool::processFailure(
    stdx::unique_lock<ObservableMutex<stdx::mutex>>& lk, const Status& status) {
    // Bump the generation so we don't reuse any pending or checked out connections
    _generation++;

    if (!_readyPool.empty() || !_processingPool.empty()) {
        static auto& bumpedSeverity = *makeSeveritySuppressor().release();
        LOGV2_DEBUG(22572,
                    bumpedSeverity(_hostAndPort).toInt(),
                    "Dropping all pooled connections",
                    "hostAndPort"_attr = _hostAndPort,
                    "error"_attr = redact(status));
    }

    // When a connection enters the ready pool, its timer is set to eventually refresh the
    // connection. This requires a lifetime extension of the specific pool because the connection
    // timer is tied to the lifetime of the connection, not the pool. That said, we can destruct
    // all of the connections and thus timers of which we have ownership.
    // In short, clearing the ready pool helps the SpecificPool drain.
    _readyPool.clear();

    // Migrate processing connections to the dropped pool, unless we're shutting down.
    if (!_health.isShutdown) {
        for (auto&& x : _processingPool) {
            // If we're just dropping the pool, we can reuse them later
            _droppedProcessingPool[x.first] = std::move(x.second);
        }
    }
    _processingPool.clear();

    // Mark ourselves as failed so we don't immediately respawn
    _health.isFailed = true;

    if (_requests.empty()) {
        return;
    }

    // We're going to fulfill all of the requests at once, outside the lock. To avoid asynchronous
    // cancellations racing against us to fulfill the promise while we're outside the lock,
    // we'll clear _requestsById, making those cancellations no-ops.
    decltype(_requests) requests;
    swap(_requests, requests);
    _requestsById.clear();

    LOGV2_DEBUG(22573, kDiagnosticLogLevel, "Failing requests", "hostAndPort"_attr = _hostAndPort);

    ScopedUnlock guard(lk);
    for (auto& p : requests) {
        p.second.promise.setError(status);
    }
}

// fulfills as many outstanding requests as possible
void ConnectionPool::SpecificPool::fulfillRequests(
    stdx::unique_lock<ObservableMutex<stdx::mutex>>& lk) {
    if (MONGO_unlikely(connectionPoolDoesNotFulfillRequests.shouldFail())) {
        return;
    }

    std::vector<std::pair<Promise<ConnectionHandle>, ConnectionHandle>> toFulfill;
    toFulfill.reserve(_requests.size());
    ScopeGuard fulfillPromises([&] {
        ScopedUnlock guard(lk);
        for (auto& p : toFulfill) {
            p.first.emplaceValue(std::move(p.second));
        }
    });

    while (_requests.size() > 0) {
        auto it = _requests.begin();
        // Marking this as our newest active time
        _lastActiveTime = _parent->_factory->now();

        // Caution: If this returns with a value, it's important that we not throw until we've
        // emplaced the promise (as returning a connection would attempt to take the lock and would
        // deadlock).
        auto conn = tryGetConnection(lk, it->second.lease);
        if (!conn) {
            break;
        }

        // We're going to fulfill all of the promises outside of the lock. popRequest removes
        // the request from _requestsById, so any asynchronous cancellations of that request
        // won't race against us fulfilling the promise outside of the lock.
        toFulfill.emplace_back(popRequest(lk, it), std::move(conn));
    }
}

Future<ConnectionPool::ConnectionHandle> ConnectionPool::SpecificPool::setGetConnectionTimeout(
    WithLock,
    FailPoint::LockHandle sfp,
    Milliseconds timeout,
    bool lease,
    const CancellationToken& token) {
    const Milliseconds failpointTimeout{sfp.getData()["timeout"].numberInt()};
    const Status failpointStatus{
        ErrorCodes::PooledConnectionAcquisitionExceededTimeLimit,
        "Connection timed out due to forceExecutorConnectionPoolTimeout failpoint"};
    if (failpointTimeout == Milliseconds(0)) {
        return Future<ConnectionPool::ConnectionHandle>::makeReady(failpointStatus);
    }
    auto pf = makePromiseFuture<ConnectionHandle>();
    auto request = std::make_shared<Request>(0, std::move(pf.promise), false, token);
    auto timeoutTimer = _parent->_factory->makeTimer();
    timeoutTimer->setTimeout(failpointTimeout, [request, timeoutTimer, failpointStatus]() mutable {
        request->promise.setError(failpointStatus);
    });
    // We don't support cancellation when the forceExecutorConnectionPoolTimeout failpoint is
    // enabled. The purpose of this failpoint is to evaluate the behavior of ConnectionPool
    // when a connection cannot be retrieved within the timeout, so cancellation shouldn't
    // ever be necessary in this context. Further, implementing cancellation would require
    // invasive changes to the timer system that is used to implement the failpoint logic.
    request->source.token().onCancel().unsafeToInlineFuture().getAsync([](Status s) {
        if (!s.isOK()) {
            return;
        }
        LOGV2(9257003, "Ignoring cancellation due to forceExecutorConnectionPoolTimeout failpoint");
    });
    return std::move(pf.future);
}

// spawn enough connections to satisfy open requests and minpool, while honoring maxpool
void ConnectionPool::SpecificPool::spawnConnections(WithLock lk) {
    if (_health.isShutdown) {
        // Dead pools spawn no conns
        return;
    }

    if (_health.isFailed) {
        LOGV2_DEBUG(22574,
                    kDiagnosticLogLevel,
                    "Pool has failed recently, postponing any attempts to spawn connections",
                    "hostAndPort"_attr = _hostAndPort);
        return;
    }

    auto controls = _parent->_controller->getControls(_id);
    LOGV2_DEBUG(22575,
                kDiagnosticLogLevel,
                "Comparing connection state to controls",
                "hostAndPort"_attr = _hostAndPort,
                "poolControls"_attr = controls);

    auto pendingConnections = refreshingConnections(lk);
    if (pendingConnections >= controls.maxPendingConnections) {
        return;
    }

    auto totalConnections = openConnections(lk);
    if (totalConnections >= controls.targetConnections) {
        return;
    }

    static auto& bumpedSeverity = *makeSeveritySuppressor().release();
    LOGV2_DEBUG(22576,
                bumpedSeverity(_hostAndPort).toInt(),
                "Connecting",
                "hostAndPort"_attr = _hostAndPort);

    auto allowance = std::min(controls.targetConnections - totalConnections,
                              controls.maxPendingConnections - pendingConnections);
    LOGV2_DEBUG(22577,
                kDiagnosticLogLevel,
                "Spawning connections",
                "connAllowance"_attr = allowance,
                "hostAndPort"_attr = _hostAndPort);

    for (decltype(allowance) i = 0; i < allowance; ++i) {
        OwnedConnection handle;
        try {
            // make a new connection and put it in processing
            handle = _parent->_factory->makeConnection(_hostAndPort, _sslMode, _generation);
        } catch (std::system_error& e) {
            LOGV2_FATAL(40336,
                        "Failed to construct a new connection object: {reason}",
                        "reason"_attr = e.what());
        }

        handle = makeDeathNotificationWrapper(std::move(handle));
        _processingPool[handle.get()] = handle;
        ++_created;

        // Run the setup callback
        handle->setup(_parent->_controller->pendingTimeout(),
                      guardCallback([this](auto& lk, auto conn, auto status) {
                          finishRefresh(lk, std::move(conn), std::move(status));
                      }),
                      _parent->getName());
    }
}

template <typename OwnershipPoolType>
typename OwnershipPoolType::mapped_type ConnectionPool::SpecificPool::takeFromPool(
    WithLock, OwnershipPoolType& pool, typename OwnershipPoolType::key_type connPtr) {
    auto iter = pool.find(connPtr);
    if (iter == pool.end())
        return typename OwnershipPoolType::mapped_type();

    auto conn = std::move(iter->second);
    pool.erase(iter);
    return conn;
}

ConnectionPool::SpecificPool::OwnedConnection ConnectionPool::SpecificPool::takeFromProcessingPool(
    WithLock lk, ConnectionInterface* connPtr) {
    auto conn = takeFromPool(lk, _processingPool, connPtr);
    if (conn) {
        return conn;
    }

    return takeFromPool(lk, _droppedProcessingPool, connPtr);
}

void ConnectionPool::SpecificPool::updateHealth() {
    const auto now = _parent->_factory->now();

    // We're expired if we have no sign of connection use and are past our expiry
    _health.isExpired = _requests.empty() && _checkedOutPool.empty() && _leasedPool.empty() &&
        (_hostExpiration <= now);

    // We're failed until we get new requests or our timer triggers
    if (_health.isFailed) {
        _health.isFailed = _requests.empty();
    }
}

void ConnectionPool::SpecificPool::updateEventTimer() {
    const auto now = _parent->_factory->now();

    // If our pending event has triggered, then schedule a retry as the next event
    auto nextEventTime = _eventTimerExpiration;
    if (nextEventTime <= now) {
        nextEventTime = now + kHostRetryTimeout;
    }

    // If our expiration comes before our next event, then it is the next event
    if (_requests.empty() && _checkedOutPool.empty() && _leasedPool.empty()) {
        _hostExpiration = _lastActiveTime + _parent->_controller->hostTimeout();
        if ((_hostExpiration > now) && (_hostExpiration < nextEventTime)) {
            nextEventTime = _hostExpiration;
        }
    }

    // If a request would timeout before the next event, then it is the next event
    if (_requests.size() && (_requests.begin()->first < nextEventTime)) {
        nextEventTime = _requests.begin()->first;
    }

    // Clamp next event time to be either now or in the future. Next event time
    // can be in the past anytime we wait a long time between invocations of
    // updateState; in these cases, we want to set our event timer to expire
    // immediately.
    if (nextEventTime < now) {
        nextEventTime = now;
    }

    // If our timer is already set to the next event, then we're done
    if (nextEventTime == _eventTimerExpiration) {
        return;
    }

    _eventTimerExpiration = nextEventTime;
    // TODO Our timeout can be a Date_t after SERVER-41459
    const auto timeout = _eventTimerExpiration - now;

    _eventTimer->cancelTimeout();

    // Set our event timer to timeout requests, refresh the state, and potentially expire this pool
    auto deferredStateUpdateFunc = guardCallback([this](auto& lk) {
        auto now = _parent->_factory->now();

        _health.isFailed = false;

        std::vector<Promise<ConnectionHandle>> toError;
        while (_requests.size() > 0 && _requests.begin()->first <= now) {
            toError.push_back(popRequest(lk, _requests.begin()));

            // Since we've failed a request, we've interacted with external users
            _lastActiveTime = now;
        }

        ScopedUnlock guard(lk);
        for (auto& promise : toError) {
            promise.setError(Status(ErrorCodes::PooledConnectionAcquisitionExceededTimeLimit,
                                    "Couldn't get a connection within the time limit"));
        }
    });
    _eventTimer->setTimeout(timeout, std::move(deferredStateUpdateFunc));
}

void ConnectionPool::SpecificPool::updateController(
    stdx::unique_lock<ObservableMutex<stdx::mutex>>& lk) {
    if (_health.isShutdown) {
        return;
    }

    auto& controller = *_parent->_controller;

    // Update our own state
    HostState state{
        _health,
        requestsPending(lk),
        refreshingConnections(lk),
        availableConnections(lk),
        inUseConnections(lk),
        leasedConnections(lk),
    };
    LOGV2_DEBUG(22578,
                kDiagnosticLogLevel,
                "Updating pool controller",
                "hostAndPort"_attr = _hostAndPort,
                "poolState"_attr = state);
    auto hostGroup = controller.updateHost(_id, std::move(state));

    // If we can shutdown, then do so
    if (hostGroup.canShutdown) {
        std::vector<std::shared_ptr<SpecificPool>> toShutdown;
        for (const auto& host : hostGroup.hosts) {
            auto it = _parent->_pools.find(host);
            if (it == _parent->_pools.end()) {
                continue;
            }

            auto& pool = it->second;
            if (!pool->_health.isExpired) {
                // Just because a HostGroup "canShutdown" doesn't mean that a SpecificPool should
                // shutdown. For example, it is always inappropriate to shutdown a SpecificPool with
                // connections in use or requests outstanding unless its parent ConnectionPool is
                // also shutting down.
                LOGV2_WARNING(4293001,
                              "Controller requested shutdown but connections still in use, "
                              "connection pool will stay active.",
                              "hostAndPort"_attr = pool->_hostAndPort);
                continue;
            }

            // At the moment, controllers will never mark for shutdown a pool with active
            // connections or pending requests. isExpired is never true if these invariants are
            // false. That's not to say that it's a terrible idea, but if this happens then we
            // should review what it means to be expired.

            if (shouldInvariantOnPoolCorrectness()) {
                invariant(pool->_checkedOutPool.empty());
                invariant(pool->_requests.empty());
                invariant(pool->_requestsById.empty());
                invariant(pool->_leasedPool.empty());
            }

            toShutdown.push_back(pool);
        }

        for (const auto& pool : toShutdown) {
            const auto& host = pool->_hostAndPort.host();
            pool->shutdown(lk,
                           Status(ErrorCodes::ConnectionPoolExpired,
                                  str::stream() << "Pool for " << host << " has expired."));
        }
        return;
    }


    // Make sure all related hosts exist
    for (const auto& host : hostGroup.hosts) {
        if (auto& pool = _parent->_pools[host]; !pool) {
            pool = SpecificPool::make(_parent, host, _sslMode);
        }
    }

    spawnConnections(lk);
}

// Updates our state and manages the request timer
void ConnectionPool::SpecificPool::updateState(WithLock) {
    if (_health.isShutdown) {
        // If we're in shutdown, there is nothing to update. Our clients are all gone.
        LOGV2_DEBUG(22579, kDiagnosticLogLevel, "Pool is dead", "hostAndPort"_attr = _hostAndPort);
        return;
    }

    updateEventTimer();
    updateHealth();

    if (std::exchange(_updateScheduled, true)) {
        return;
    }

    ExecutorFuture(ExecutorPtr(_parent->_factory->getExecutor()))  //
        .getAsync([this, anchor = shared_from_this()](Status&& status) mutable {
            invariant(status);

            stdx::unique_lock lk(_parent->_mutex);
            _updateScheduled = false;
            updateController(lk);
        });
}

ClockSource* ConnectionPool::DependentTypeFactoryInterface::getFastClockSource() {
    return getGlobalServiceContext()->getFastClockSource();
}

}  // namespace executor
}  // namespace mongo
