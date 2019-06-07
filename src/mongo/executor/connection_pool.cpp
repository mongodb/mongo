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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kConnectionPool

#include "mongo/platform/basic.h"

#include "mongo/executor/connection_pool.h"

#include <fmt/format.h>

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/executor/connection_pool_stats.h"
#include "mongo/executor/remote_command_request.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/destructor_guard.h"
#include "mongo/util/log.h"
#include "mongo/util/lru_cache.h"
#include "mongo/util/scopeguard.h"

using namespace fmt::literals;

// One interesting implementation note herein concerns how setup() and
// refresh() are invoked outside of the global lock, but setTimeout is not.
// This implementation detail simplifies mocks, allowing them to return
// synchronously sometimes, whereas having timeouts fire instantly adds little
// value. In practice, dumping the locks is always safe (because we restrict
// ourselves to operations over the connection).

namespace mongo {

namespace {

template <typename Map, typename Key>
auto& getOrInvariant(Map&& map, const Key& key) noexcept {
    auto it = std::forward<Map>(map).find(key);
    invariant(it != std::forward<Map>(map).end(), "Unable to find key in map");

    return it->second;
}

template <typename Map, typename... Args>
void emplaceOrInvariant(Map&& map, Args&&... args) noexcept {
    auto ret = std::forward<Map>(map).emplace(std::forward<Args>(args)...);
    invariant(ret.second, "Element already existed in map/set");
}

}  // anonymous

namespace executor {

void ConnectionPool::ConnectionInterface::indicateUsed() {
    // It is illegal to attempt to use a connection after calling indicateFailure().
    invariant(_status.isOK() || _status == ConnectionPool::kConnectionStateUnknown);
    _lastUsed = now();
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

    LOG(2) << "Controller for " << pool->_name << " is " << name();
    _pool = pool;
}

std::string ConnectionPool::ConnectionControls::toString() const {
    return "{{ maxPending: {}, target: {}, }}"_format(maxPendingConnections, targetConnections);
}

std::string ConnectionPool::HostState::toString() const {
    return "{{ requests: {}, ready: {}, pending: {}, active: {}, isExpired: {} }}"_format(
        requests, ready, pending, active, health.isExpired);
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

        data.target = stats.requests + stats.active;
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
            getPool()->_options.maxConnecting, data.target,
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

    StringData name() const override {
        return "LimitController"_sd;
    }

protected:
    struct PoolData {
        HostAndPort host;
        size_t target = 0;
    };

    stdx::mutex _mutex;
    stdx::unordered_map<PoolId, PoolData> _poolData;
};

/**
 * A pool for a specific HostAndPort
 *
 * Pools come into existance the first time a connection is requested and
 * go out of existence after hostTimeout passes without any of their
 * connections being used.
 */
class ConnectionPool::SpecificPool final
    : public std::enable_shared_from_this<ConnectionPool::SpecificPool> {
    static constexpr int kDiagnosticLogLevel = 3;

public:
    /**
     * Whenever a function enters a specific pool, the function needs to be guarded by the lock.
     *
     * This callback also (perhaps overly aggressively) binds a shared pointer to the guard.
     * It is *always* safe to reference the original specific pool in the guarded function object.
     *
     * For a function object of signature:
     * void riskyBusiness(ArgTypes...);
     *
     * It returns a function object of signature:
     * void safeCallback(ArgTypes...);
     */
    template <typename Callback>
    auto guardCallback(Callback&& cb) {
        return
            [ this, cb = std::forward<Callback>(cb), anchor = shared_from_this() ](auto&&... args) {
            stdx::lock_guard lk(_parent->_mutex);
            cb(std::forward<decltype(args)>(args)...);
            updateState();
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
     * and maybe delists from pool
     *
     * This should only be called by the ConnectionPool or StateLock
     */
    void updateState();

    /**
     * Gets a connection from the specific pool. Sinks a unique_lock from the
     * parent to preserve the lock on _mutex
     */
    Future<ConnectionHandle> getConnection(Milliseconds timeout);

    /**
     * Triggers the shutdown procedure. This function sets isShutdown to true
     * and calls processFailure below with the status provided. This immediately removes this pool
     * from the ConnectionPool. The actual destruction will happen eventually as ConnectionHandles
     * are deleted.
     */
    void triggerShutdown(const Status& status);

    /**
     * Cascades a failure across existing connections and requests. Invoking
     * this function drops all current connections and fails all current
     * requests with the passed status.
     */
    void processFailure(const Status& status);

    /**
     * Returns the number of connections currently checked out of the pool.
     */
    size_t inUseConnections() const;

    /**
     * Returns the number of available connections in the pool.
     */
    size_t availableConnections() const;

    /**
     * Returns the number of in progress connections in the pool.
     */
    size_t refreshingConnections() const;

    /**
     * Returns the total number of connections ever created in this pool.
     */
    size_t createdConnections() const;

    /**
     * Returns the total number of connections currently open that belong to
     * this pool. This is the sum of refreshingConnections, availableConnections,
     * and inUseConnections.
     */
    size_t openConnections() const;

    /**
     * Returns the number of unfulfilled requests pending.
     */
    size_t requestsPending() const;

    /**
     * Returns the HostAndPort for this pool.
     */
    const HostAndPort& host() const {
        return _hostAndPort;
    }

    /**
     * Return true if the tags on the specific pool match the passed in tags
     */
    bool matchesTags(transport::Session::TagMask tags) const {
        return !!(_tags & tags);
    }

    /**
     * Atomically manipulate the tags in the pool
     */
    void mutateTags(const stdx::function<transport::Session::TagMask(transport::Session::TagMask)>&
                        mutateFunc) {
        _tags = mutateFunc(_tags);
    }

    void fassertSSLModeIs(transport::ConnectSSLMode desired) const {
        if (desired != _sslMode) {
            severe() << "Mixing ssl modes for a single host is not supported";
            fassertFailedNoTrace(51043);
        }
    }

    template <typename CallableT>
    void runOnExecutor(CallableT&& cb) {
        ExecutorFuture(ExecutorPtr(_parent->_factory->getExecutor()))  //
            .getAsync([ anchor = shared_from_this(),
                        cb = std::forward<CallableT>(cb) ](Status && status) mutable {
                invariant(status);
                cb();
            });
    }

private:
    using OwnedConnection = std::shared_ptr<ConnectionInterface>;
    using OwnershipPool = stdx::unordered_map<ConnectionInterface*, OwnedConnection>;
    using LRUOwnershipPool = LRUCache<OwnershipPool::key_type, OwnershipPool::mapped_type>;
    using Request = std::pair<Date_t, Promise<ConnectionHandle>>;
    struct RequestComparator {
        bool operator()(const Request& a, const Request& b) {
            return a.first > b.first;
        }
    };

    ConnectionHandle makeHandle(ConnectionInterface* connection);

    /**
     * Establishes connections until the ControllerInterface's target is met.
     */
    void spawnConnections();

    void finishRefresh(ConnectionInterface* connPtr, Status status);

    void addToReady(OwnedConnection conn);

    void fulfillRequests();

    void returnConnection(ConnectionInterface* connPtr);

    // This internal helper is used both by get and by _fulfillRequests and differs in that it
    // skips some bookkeeping that the other callers do on their own
    ConnectionHandle tryGetConnection();

    template <typename OwnershipPoolType>
    typename OwnershipPoolType::mapped_type takeFromPool(
        OwnershipPoolType& pool, typename OwnershipPoolType::key_type connPtr);

    OwnedConnection takeFromProcessingPool(ConnectionInterface* connection);

    // Update the health struct and related variables
    void updateHealth();

    // Update the event timer for this host pool
    void updateEventTimer();

    // Update the controller and potentially change the controls
    void updateController();

private:
    const std::shared_ptr<ConnectionPool> _parent;

    const transport::ConnectSSLMode _sslMode;
    const HostAndPort _hostAndPort;

    const PoolId _id;

    LRUOwnershipPool _readyPool;
    OwnershipPool _processingPool;
    OwnershipPool _droppedProcessingPool;
    OwnershipPool _checkedOutPool;

    std::vector<Request> _requests;
    Date_t _lastActiveTime;

    std::shared_ptr<TimerInterface> _eventTimer;
    Date_t _eventTimerExpiration;
    Date_t _hostExpiration;

    // The _generation is the set of connection objects we believe are healthy.
    // It increases when we process a failure. If a connection is from a previous generation,
    // it will be discarded on return/refresh.
    size_t _generation = 0;

    bool _inFulfillRequests = false;
    bool _inControlLoop = false;

    size_t _created = 0;

    transport::Session::TagMask _tags = transport::Session::kPending;

    HostHealth _health;
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
      _controller(std::move(_options.controller)),
      _manager(options.egressTagCloserManager) {
    if (_manager) {
        _manager->add(this);
    }

    if (!_controller) {
        _controller = std::make_shared<LimitController>();
    }
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
    _factory->shutdown();

    // Grab all current pools (under the lock)
    auto pools = [&] {
        stdx::lock_guard lk(_mutex);
        return _pools;
    }();

    for (const auto& pair : pools) {
        stdx::lock_guard lk(_mutex);
        pair.second->triggerShutdown(
            Status(ErrorCodes::ShutdownInProgress, "Shutting down the connection pool"));
    }
}

void ConnectionPool::dropConnections(const HostAndPort& hostAndPort) {
    stdx::lock_guard lk(_mutex);

    auto iter = _pools.find(hostAndPort);

    if (iter == _pools.end())
        return;

    auto pool = iter->second;
    pool->triggerShutdown(
        Status(ErrorCodes::PooledConnectionsDropped, "Pooled connections dropped"));
}

void ConnectionPool::dropConnections(transport::Session::TagMask tags) {
    // Grab all current pools (under the lock)
    auto pools = [&] {
        stdx::lock_guard lk(_mutex);
        return _pools;
    }();

    for (const auto& pair : pools) {
        auto& pool = pair.second;

        stdx::lock_guard lk(_mutex);
        if (pool->matchesTags(tags))
            continue;

        pool->triggerShutdown(
            Status(ErrorCodes::PooledConnectionsDropped, "Pooled connections dropped"));
    }
}

void ConnectionPool::mutateTags(
    const HostAndPort& hostAndPort,
    const stdx::function<transport::Session::TagMask(transport::Session::TagMask)>& mutateFunc) {
    stdx::lock_guard lk(_mutex);

    auto iter = _pools.find(hostAndPort);

    if (iter == _pools.end())
        return;

    auto pool = iter->second;
    pool->mutateTags(mutateFunc);
}

void ConnectionPool::get_forTest(const HostAndPort& hostAndPort,
                                 Milliseconds timeout,
                                 GetConnectionCallback cb) {
    // We kick ourselves onto the executor queue to prevent us from deadlocking with our own thread
    auto getConnectionFunc = [ this, hostAndPort, timeout, cb = std::move(cb) ](Status &&) mutable {
        get(hostAndPort, transport::kGlobalSSLMode, timeout)
            .thenRunOn(_factory->getExecutor())
            .getAsync(std::move(cb));
    };
    _factory->getExecutor()->schedule(std::move(getConnectionFunc));
}

SemiFuture<ConnectionPool::ConnectionHandle> ConnectionPool::get(const HostAndPort& hostAndPort,
                                                                 transport::ConnectSSLMode sslMode,
                                                                 Milliseconds timeout) {
    stdx::lock_guard lk(_mutex);

    auto& pool = _pools[hostAndPort];
    if (!pool) {
        pool = SpecificPool::make(shared_from_this(), hostAndPort, sslMode);
    } else {
        pool->fassertSSLModeIs(sslMode);
    }

    invariant(pool);

    auto connFuture = pool->getConnection(timeout);
    pool->updateState();

    return std::move(connFuture).semi();
}

void ConnectionPool::appendConnectionStats(ConnectionPoolStats* stats) const {
    stdx::lock_guard lk(_mutex);

    for (const auto& kv : _pools) {
        HostAndPort host = kv.first;

        auto& pool = kv.second;
        ConnectionStatsPer hostStats{pool->inUseConnections(),
                                     pool->availableConnections(),
                                     pool->createdConnections(),
                                     pool->refreshingConnections()};
        stats->updateStatsForHost(_name, host, hostStats);
    }
}

size_t ConnectionPool::getNumConnectionsPerHost(const HostAndPort& hostAndPort) const {
    stdx::lock_guard lk(_mutex);
    auto iter = _pools.find(hostAndPort);
    if (iter != _pools.end()) {
        return iter->second->openConnections();
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
    DESTRUCTOR_GUARD(_eventTimer->cancelTimeout();)

    invariant(_requests.empty());
    invariant(_checkedOutPool.empty());
}

size_t ConnectionPool::SpecificPool::inUseConnections() const {
    return _checkedOutPool.size();
}

size_t ConnectionPool::SpecificPool::availableConnections() const {
    return _readyPool.size();
}

size_t ConnectionPool::SpecificPool::refreshingConnections() const {
    return _processingPool.size();
}

size_t ConnectionPool::SpecificPool::createdConnections() const {
    return _created;
}

size_t ConnectionPool::SpecificPool::openConnections() const {
    return _checkedOutPool.size() + _readyPool.size() + _processingPool.size();
}

size_t ConnectionPool::SpecificPool::requestsPending() const {
    return _requests.size();
}

Future<ConnectionPool::ConnectionHandle> ConnectionPool::SpecificPool::getConnection(
    Milliseconds timeout) {

    // Reset our activity timestamp
    auto now = _parent->_factory->now();
    _lastActiveTime = now;

    // If we do not have requests, then we can fulfill immediately
    if (_requests.size() == 0) {
        auto conn = tryGetConnection();

        if (conn) {
            LOG(kDiagnosticLogLevel) << "Requesting new connection to " << _hostAndPort
                                     << "--using existing idle connection";
            return Future<ConnectionPool::ConnectionHandle>::makeReady(std::move(conn));
        }
    }

    auto pendingTimeout = _parent->_controller->pendingTimeout();
    if (timeout < Milliseconds(0) || timeout > pendingTimeout) {
        timeout = pendingTimeout;
    }
    LOG(kDiagnosticLogLevel) << "Requesting new connection to " << _hostAndPort << " with timeout "
                             << timeout;

    const auto expiration = now + timeout;
    auto pf = makePromiseFuture<ConnectionHandle>();

    _requests.push_back(make_pair(expiration, std::move(pf.promise)));
    std::push_heap(begin(_requests), end(_requests), RequestComparator{});

    return std::move(pf.future);
}

auto ConnectionPool::SpecificPool::makeHandle(ConnectionInterface* connection) -> ConnectionHandle {
    auto deleter = [ this, anchor = shared_from_this() ](ConnectionInterface * connection) {
        runOnExecutor([this, connection]() {
            stdx::lock_guard lk(_parent->_mutex);

            returnConnection(connection);

            _lastActiveTime = _parent->_factory->now();
            updateState();
        });
    };
    return ConnectionHandle(connection, std::move(deleter));
}

ConnectionPool::ConnectionHandle ConnectionPool::SpecificPool::tryGetConnection() {
    while (_readyPool.size()) {
        // _readyPool is an LRUCache, so its begin() object is the MRU item.
        auto iter = _readyPool.begin();

        // Grab the connection and cancel its timeout
        auto conn = std::move(iter->second);
        _readyPool.erase(iter);
        conn->cancelTimeout();

        if (!conn->isHealthy()) {
            log() << "dropping unhealthy pooled connection to " << conn->getHostAndPort();

            // Drop the bad connection via scoped destruction and retry
            continue;
        }

        auto connPtr = conn.get();

        // check out the connection
        _checkedOutPool[connPtr] = std::move(conn);

        // pass it to the user
        connPtr->resetToUnknown();
        auto handle = makeHandle(connPtr);
        return handle;
    }

    return {};
}

void ConnectionPool::SpecificPool::finishRefresh(ConnectionInterface* connPtr, Status status) {
    auto conn = takeFromProcessingPool(connPtr);

    // If we're in shutdown, we don't need refreshed connections
    if (_health.isShutdown) {
        return;
    }

    // If we've exceeded the time limit, start a new connect,
    // rather than failing all operations.  We do this because the
    // various callers have their own time limit which is unrelated
    // to our internal one.
    if (status.code() == ErrorCodes::NetworkInterfaceExceededTimeLimit) {
        LOG(kDiagnosticLogLevel) << "Pending connection to host " << _hostAndPort
                                 << " did not complete within the connection timeout,"
                                 << " retrying with a new connection;" << openConnections()
                                 << " connections to that host remain open";
        return;
    }

    // Pass a failure on through
    if (!status.isOK()) {
        LOG(kDiagnosticLogLevel) << "Connection failed to " << _hostAndPort << " due to "
                                 << redact(status);
        processFailure(status);
        return;
    }

    // If the host and port were dropped, let this lapse and spawn new connections
    if (!conn || conn->getGeneration() != _generation) {
        LOG(kDiagnosticLogLevel) << "Dropping late refreshed connection to " << _hostAndPort;
        return;
    }

    LOG(kDiagnosticLogLevel) << "Finishing connection refresh for " << _hostAndPort;

    // If the connection refreshed successfully, throw it back in the ready pool
    addToReady(std::move(conn));

    fulfillRequests();
}

void ConnectionPool::SpecificPool::returnConnection(ConnectionInterface* connPtr) {
    auto needsRefreshTP = connPtr->getLastUsed() + _parent->_controller->toRefreshTimeout();

    auto conn = takeFromPool(_checkedOutPool, connPtr);
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
        // TODO: alert via some callback if the host is bad
        log() << "Ending connection to host " << _hostAndPort
              << " due to bad connection status: " << redact(status) << "; " << openConnections()
              << " connections to that host remain open";
        return;
    }

    auto now = _parent->_factory->now();
    if (needsRefreshTP <= now) {
        // If we need to refresh this connection

        auto controls = _parent->_controller->getControls(_id);
        if (_readyPool.size() + _processingPool.size() + _checkedOutPool.size() >=
            controls.targetConnections) {
            // If we already have minConnections, just let the connection lapse
            log() << "Ending idle connection to host " << _hostAndPort
                  << " because the pool meets constraints; " << openConnections()
                  << " connections to that host remain open";
            return;
        }

        _processingPool[connPtr] = std::move(conn);

        LOG(kDiagnosticLogLevel) << "Refreshing connection to " << _hostAndPort;
        connPtr->refresh(_parent->_controller->pendingTimeout(),
                         guardCallback([this](auto conn, auto status) {
                             finishRefresh(std::move(conn), std::move(status));
                         }));

        return;
    }

    // If it's fine as it is, just put it in the ready queue
    LOG(kDiagnosticLogLevel) << "Returning ready connection to " << _hostAndPort;
    addToReady(std::move(conn));

    fulfillRequests();
}

// Adds a live connection to the ready pool
void ConnectionPool::SpecificPool::addToReady(OwnedConnection conn) {
    auto connPtr = conn.get();

    // This makes the connection the new most-recently-used connection.
    _readyPool.add(connPtr, std::move(conn));

    // Our strategy for refreshing connections is to check them out and
    // immediately check them back in (which kicks off the refresh logic in
    // returnConnection
    auto returnConnectionFunc = guardCallback([this, connPtr]() {
        LOG(kDiagnosticLogLevel) << "Triggered refresh timeout for " << _hostAndPort;
        auto conn = takeFromPool(_readyPool, connPtr);

        // We've already been checked out. We don't need to refresh ourselves.
        if (!conn)
            return;

        // If we're in shutdown, we don't need to refresh connections
        if (_health.isShutdown)
            return;

        _checkedOutPool[connPtr] = std::move(conn);

        connPtr->indicateSuccess();

        returnConnection(connPtr);
    });
    connPtr->setTimeout(_parent->_controller->toRefreshTimeout(), std::move(returnConnectionFunc));
}

// Sets state to shutdown and kicks off the failure protocol to tank existing connections
void ConnectionPool::SpecificPool::triggerShutdown(const Status& status) {
    _health.isShutdown = true;

    LOG(2) << "Delisting connection pool for " << _hostAndPort;
    _parent->_controller->removeHost(_id);
    _parent->_pools.erase(_hostAndPort);

    processFailure(status);

    _droppedProcessingPool.clear();
    _eventTimer->cancelTimeout();
}

// Drop connections and fail all requests
void ConnectionPool::SpecificPool::processFailure(const Status& status) {
    // Bump the generation so we don't reuse any pending or checked out connections
    _generation++;

    if (!_readyPool.empty() || !_processingPool.empty()) {
        auto severity = MONGO_GET_LIMITED_SEVERITY(_hostAndPort, Seconds{1}, 0, 2);
        LOG(severity) << "Dropping all pooled connections to " << _hostAndPort << " due to "
                      << redact(status);
    }

    // When a connection enters the ready pool, its timer is set to eventually refresh the
    // connection. This requires a lifetime extension of the specific pool because the connection
    // timer is tied to the lifetime of the connection, not the pool. That said, we can destruct
    // all of the connections and thus timers of which we have ownership.
    // In short, clearing the ready pool helps the SpecificPool drain.
    _readyPool.clear();

    // Migrate processing connections to the dropped pool
    for (auto&& x : _processingPool) {
        // If we're just dropping the pool, we can reuse them later
        _droppedProcessingPool[x.first] = std::move(x.second);
    }
    _processingPool.clear();

    // Mark ourselves as failed so we don't immediately respawn
    _health.isFailed = true;

    if (_requests.empty()) {
        return;
    }

    for (auto& request : _requests) {
        request.second.setError(status);
    }

    LOG(kDiagnosticLogLevel) << "Failing requests to " << _hostAndPort;
    _requests.clear();
}

// fulfills as many outstanding requests as possible
void ConnectionPool::SpecificPool::fulfillRequests() {
    // If some other thread (possibly this thread) is fulfilling requests,
    // don't keep padding the callstack.
    if (_inFulfillRequests)
        return;

    _inFulfillRequests = true;
    auto guard = makeGuard([&] { _inFulfillRequests = false; });
    while (_requests.size()) {
        // Marking this as our newest active time
        _lastActiveTime = _parent->_factory->now();

        // Caution: If this returns with a value, it's important that we not throw until we've
        // emplaced the promise (as returning a connection would attempt to take the lock and would
        // deadlock).
        //
        // None of the heap manipulation code throws, but it's something to keep in mind.
        auto conn = tryGetConnection();

        if (!conn) {
            break;
        }

        // Grab the request and callback
        auto promise = std::move(_requests.front().second);
        std::pop_heap(begin(_requests), end(_requests), RequestComparator{});
        _requests.pop_back();

        promise.emplaceValue(std::move(conn));
    }
}

// spawn enough connections to satisfy open requests and minpool, while honoring maxpool
void ConnectionPool::SpecificPool::spawnConnections() {
    if (_health.isShutdown) {
        // Dead pools spawn no conns
        return;
    }

    if (_health.isFailed) {
        LOG(kDiagnosticLogLevel)
            << "Pool for " << _hostAndPort
            << " has failed recently. Postponing any attempts to spawn connections";
        return;
    }

    auto controls = _parent->_controller->getControls(_id);
    LOG(kDiagnosticLogLevel) << "Comparing connection state for " << _hostAndPort
                             << " to Controls: " << controls;

    auto pendingConnections = refreshingConnections();
    if (pendingConnections >= controls.maxPendingConnections) {
        return;
    }

    auto totalConnections = openConnections();
    if (totalConnections >= controls.targetConnections) {
        return;
    }

    auto severity = MONGO_GET_LIMITED_SEVERITY(_hostAndPort, Seconds{1}, 0, 2);
    LOG(severity) << "Connecting to " << _hostAndPort;

    auto allowance = std::min(controls.targetConnections - totalConnections,
                              controls.maxPendingConnections - pendingConnections);
    LOG(kDiagnosticLogLevel) << "Spawning " << allowance << " connections to " << _hostAndPort;
    for (decltype(allowance) i = 0; i < allowance; ++i) {
        OwnedConnection handle;
        try {
            // make a new connection and put it in processing
            handle = _parent->_factory->makeConnection(_hostAndPort, _sslMode, _generation);
        } catch (std::system_error& e) {
            severe() << "Failed to construct a new connection object: " << e.what();
            fassertFailed(40336);
        }

        _processingPool[handle.get()] = handle;
        ++_created;

        // Run the setup callback
        handle->setup(_parent->_controller->pendingTimeout(),
                      guardCallback([this](auto conn, auto status) {
                          finishRefresh(std::move(conn), std::move(status));
                      }));
    }
}

template <typename OwnershipPoolType>
typename OwnershipPoolType::mapped_type ConnectionPool::SpecificPool::takeFromPool(
    OwnershipPoolType& pool, typename OwnershipPoolType::key_type connPtr) {
    auto iter = pool.find(connPtr);
    if (iter == pool.end())
        return typename OwnershipPoolType::mapped_type();

    auto conn = std::move(iter->second);
    pool.erase(iter);
    return conn;
}

ConnectionPool::SpecificPool::OwnedConnection ConnectionPool::SpecificPool::takeFromProcessingPool(
    ConnectionInterface* connPtr) {
    auto conn = takeFromPool(_processingPool, connPtr);
    if (conn) {
        return conn;
    }

    return takeFromPool(_droppedProcessingPool, connPtr);
}

void ConnectionPool::SpecificPool::updateHealth() {
    const auto now = _parent->_factory->now();

    // We're expired if we have no sign of connection use and are past our expiry
    _health.isExpired = _requests.empty() && _checkedOutPool.empty() && (_hostExpiration <= now);

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
    if (_requests.empty() && _checkedOutPool.empty()) {
        _hostExpiration = _lastActiveTime + _parent->_controller->hostTimeout();
        if ((_hostExpiration > now) && (_hostExpiration < nextEventTime)) {
            nextEventTime = _hostExpiration;
        }
    }

    // If a request would timeout before the next event, then it is the next event
    if (_requests.size() && (_requests.front().first < nextEventTime)) {
        nextEventTime = _requests.front().first;
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
    auto deferredStateUpdateFunc = guardCallback([this]() {
        auto now = _parent->_factory->now();

        _health.isFailed = false;

        while (_requests.size() && (_requests.front().first <= now)) {
            std::pop_heap(begin(_requests), end(_requests), RequestComparator{});

            auto& request = _requests.back();
            request.second.setError(Status(ErrorCodes::NetworkInterfaceExceededTimeLimit,
                                           "Couldn't get a connection within the time limit"));
            _requests.pop_back();

            // Since we've failed a request, we've interacted with external users
            _lastActiveTime = now;
        }
    });
    _eventTimer->setTimeout(timeout, std::move(deferredStateUpdateFunc));
}

void ConnectionPool::SpecificPool::updateController() {
    if (std::exchange(_inControlLoop, true)) {
        return;
    }
    const auto guard = makeGuard([&] { _inControlLoop = false; });

    auto& controller = *_parent->_controller;

    // Update our own state
    HostState state{
        _health,
        requestsPending(),
        refreshingConnections(),
        availableConnections(),
        inUseConnections(),
    };
    LOG(kDiagnosticLogLevel) << "Updating controller for " << _hostAndPort
                             << " with State: " << state;
    auto hostGroup = controller.updateHost(_id, std::move(state));

    // If we can shutdown, then do so
    if (hostGroup.canShutdown) {
        for (const auto& host : hostGroup.hosts) {
            auto it = _parent->_pools.find(host);
            if (it == _parent->_pools.end()) {
                continue;
            }

            auto& pool = it->second;

            // At the moment, controllers will never mark for shutdown a pool with active
            // connections or pending requests. isExpired is never true if these invariants are
            // false. That's not to say that it's a terrible idea, but if this happens then we
            // should review what it means to be expired.

            invariant(pool->_checkedOutPool.empty());
            invariant(pool->_requests.empty());

            pool->triggerShutdown(Status(ErrorCodes::ShutdownInProgress,
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

    runOnExecutor([ this, anchor = shared_from_this() ]() { spawnConnections(); });
}

// Updates our state and manages the request timer
void ConnectionPool::SpecificPool::updateState() {
    if (_health.isShutdown) {
        // If we're in shutdown, there is nothing to update. Our clients are all gone.
        LOG(kDiagnosticLogLevel) << _hostAndPort << " is dead";
        return;
    }

    updateEventTimer();
    updateHealth();
    updateController();
}

}  // namespace executor
}  // namespace mongo
