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

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/executor/connection_pool_stats.h"
#include "mongo/executor/remote_command_request.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/destructor_guard.h"
#include "mongo/util/log.h"
#include "mongo/util/lru_cache.h"
#include "mongo/util/scopeguard.h"

// One interesting implementation note herein concerns how setup() and
// refresh() are invoked outside of the global lock, but setTimeout is not.
// This implementation detail simplifies mocks, allowing them to return
// synchronously sometimes, whereas having timeouts fire instantly adds little
// value. In practice, dumping the locks is always safe (because we restrict
// ourselves to operations over the connection).

namespace mongo {
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

/**
 * A pool for a specific HostAndPort
 *
 * Pools come into existance the first time a connection is requested and
 * go out of existence after hostTimeout passes without any of their
 * connections being used.
 */
class ConnectionPool::SpecificPool final
    : public std::enable_shared_from_this<ConnectionPool::SpecificPool> {
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
     * Gets a connection from the specific pool. Sinks a unique_lock from the
     * parent to preserve the lock on _mutex
     */
    Future<ConnectionHandle> getConnection(Milliseconds timeout);

    /**
     * Triggers the shutdown procedure. This function marks the state as kInShutdown
     * and calls processFailure below with the status provided. This may not immediately
     * delist or destruct this pool. However, both will happen eventually as ConnectionHandles
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
     * Returns a connection to a specific pool. Sinks a unique_lock from the
     * parent to preserve the lock on _mutex
     */
    void returnConnection(ConnectionInterface* connection);

    /**
     * Returns the number of connections currently checked out of the pool.
     */
    size_t inUseConnections();

    /**
     * Returns the number of available connections in the pool.
     */
    size_t availableConnections();

    /**
     * Returns the number of in progress connections in the pool.
     */
    size_t refreshingConnections();

    /**
     * Returns the total number of connections ever created in this pool.
     */
    size_t createdConnections();

    /**
     * Returns the total number of connections currently open that belong to
     * this pool. This is the sum of refreshingConnections, availableConnections,
     * and inUseConnections.
     */
    size_t openConnections();

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

    void spawnConnections();

    template <typename CallableT>
    void runOnExecutor(CallableT&& cb) {
        ExecutorFuture(ExecutorPtr(_parent->_factory->getExecutor()))  //
            .getAsync([ anchor = shared_from_this(),
                        cb = std::forward<CallableT>(cb) ](Status && status) mutable {
                invariant(status);
                cb();
            });
    }

    void updateState();

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

    void finishRefresh(ConnectionInterface* connPtr, Status status);

    void addToReady(OwnedConnection conn);

    void fulfillRequests();

    // This internal helper is used both by get and by fulfillRequests and differs in that it
    // skips some bookkeeping that the other callers do on their own
    ConnectionHandle tryGetConnection();

    template <typename OwnershipPoolType>
    typename OwnershipPoolType::mapped_type takeFromPool(
        OwnershipPoolType& pool, typename OwnershipPoolType::key_type connPtr);

    OwnedConnection takeFromProcessingPool(ConnectionInterface* connection);

private:
    const std::shared_ptr<ConnectionPool> _parent;

    const transport::ConnectSSLMode _sslMode;
    const HostAndPort _hostAndPort;

    LRUOwnershipPool _readyPool;
    OwnershipPool _processingPool;
    OwnershipPool _droppedProcessingPool;
    OwnershipPool _checkedOutPool;

    std::vector<Request> _requests;

    std::shared_ptr<TimerInterface> _requestTimer;
    Date_t _requestTimerExpiration;
    size_t _generation;
    bool _inFulfillRequests;
    bool _inSpawnConnections;

    size_t _created;

    transport::Session::TagMask _tags = transport::Session::kPending;

    /**
     * The current state of the pool
     *
     * The pool begins in a running state. Moves to idle when no requests
     * are pending and no connections are checked out. It finally enters
     * shutdown after hostTimeout has passed (and waits there for current
     * refreshes to process out).
     *
     * At any point a new request sets the state back to running and
     * restarts all timers.
     */
    enum class State {
        // The pool is active
        kRunning,

        // No current activity, waiting for hostTimeout to pass
        kIdle,

        // hostTimeout is passed, we're waiting for any processing
        // connections to finish before shutting down
        kInShutdown,
    };

    State _state;
};

const Status ConnectionPool::kConnectionStateUnknown =
    Status(ErrorCodes::InternalError, "Connection is in an unknown state");

ConnectionPool::ConnectionPool(std::shared_ptr<DependentTypeFactoryInterface> impl,
                               std::string name,
                               Options options)
    : _name(std::move(name)),
      _options(std::move(options)),
      _factory(std::move(impl)),
      _manager(options.egressTagCloserManager) {
    if (_manager) {
        _manager->add(this);
    }
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
        pair.second->updateState();
    }
}

void ConnectionPool::dropConnections(const HostAndPort& hostAndPort) {
    stdx::lock_guard lk(_mutex);

    auto iter = _pools.find(hostAndPort);

    if (iter == _pools.end())
        return;

    auto pool = iter->second;
    pool->processFailure(
        Status(ErrorCodes::PooledConnectionsDropped, "Pooled connections dropped"));
    pool->updateState();
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

        pool->processFailure(
            Status(ErrorCodes::PooledConnectionsDropped, "Pooled connections dropped"));
        pool->updateState();
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
        pool = std::make_shared<SpecificPool>(shared_from_this(), hostAndPort, sslMode);
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
      _readyPool(std::numeric_limits<size_t>::max()),
      _generation(0),
      _inFulfillRequests(false),
      _inSpawnConnections(false),
      _created(0),
      _state(State::kRunning) {
    invariant(_parent);
    _requestTimer = _parent->_factory->makeTimer();
}

ConnectionPool::SpecificPool::~SpecificPool() {
    DESTRUCTOR_GUARD(_requestTimer->cancelTimeout();)

    invariant(_requests.empty());
    invariant(_checkedOutPool.empty());
}

size_t ConnectionPool::SpecificPool::inUseConnections() {
    return _checkedOutPool.size();
}

size_t ConnectionPool::SpecificPool::availableConnections() {
    return _readyPool.size();
}

size_t ConnectionPool::SpecificPool::refreshingConnections() {
    return _processingPool.size();
}

size_t ConnectionPool::SpecificPool::createdConnections() {
    return _created;
}

size_t ConnectionPool::SpecificPool::openConnections() {
    return _checkedOutPool.size() + _readyPool.size() + _processingPool.size();
}

Future<ConnectionPool::ConnectionHandle> ConnectionPool::SpecificPool::getConnection(
    Milliseconds timeout) {
    invariant(_state != State::kInShutdown);

    auto conn = tryGetConnection();

    if (conn) {
        return Future<ConnectionPool::ConnectionHandle>::makeReady(std::move(conn));
    }

    if (timeout < Milliseconds(0) || timeout > _parent->_options.refreshTimeout) {
        timeout = _parent->_options.refreshTimeout;
    }

    const auto expiration = _parent->_factory->now() + timeout;
    auto pf = makePromiseFuture<ConnectionHandle>();

    _requests.push_back(make_pair(expiration, std::move(pf.promise)));
    std::push_heap(begin(_requests), end(_requests), RequestComparator{});

    runOnExecutor([ this, anchor = shared_from_this() ]() { spawnConnections(); });

    return std::move(pf.future);
}

auto ConnectionPool::SpecificPool::makeHandle(ConnectionInterface* connection) -> ConnectionHandle {
    auto deleter = [ this, anchor = shared_from_this() ](ConnectionInterface * connection) {
        runOnExecutor([this, connection]() {
            stdx::lock_guard lk(_parent->_mutex);
            returnConnection(connection);
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
    if (_state == State::kInShutdown)
        return;

    // If we've exceeded the time limit, start a new connect,
    // rather than failing all operations.  We do this because the
    // various callers have their own time limit which is unrelated
    // to our internal one.
    if (status.code() == ErrorCodes::NetworkInterfaceExceededTimeLimit) {
        LOG(0) << "Pending connection to host " << _hostAndPort
               << " did not complete within the connection timeout,"
               << " retrying with a new connection;" << openConnections()
               << " connections to that host remain open";
        spawnConnections();
        return;
    }

    // Pass a failure on through
    if (!status.isOK()) {
        processFailure(status);
        return;
    }

    // If the host and port were dropped, let this lapse and spawn new connections
    if (conn->getGeneration() != _generation) {
        spawnConnections();
        return;
    }

    // If the connection refreshed successfully, throw it back in the ready pool
    addToReady(std::move(conn));

    fulfillRequests();
}

void ConnectionPool::SpecificPool::returnConnection(ConnectionInterface* connPtr) {
    auto needsRefreshTP = connPtr->getLastUsed() + _parent->_options.refreshRequirement;

    auto conn = takeFromPool(_checkedOutPool, connPtr);
    invariant(conn);

    if (conn->getGeneration() != _generation) {
        // If the connection is from an older generation, just return.
        return;
    }

    if (!conn->getStatus().isOK()) {
        // TODO: alert via some callback if the host is bad
        log() << "Ending connection to host " << _hostAndPort << " due to bad connection status; "
              << openConnections() << " connections to that host remain open";
        return;
    }

    auto now = _parent->_factory->now();
    if (needsRefreshTP <= now) {
        // If we need to refresh this connection

        if (_readyPool.size() + _processingPool.size() + _checkedOutPool.size() >=
            _parent->_options.minConnections) {
            // If we already have minConnections, just let the connection lapse
            log() << "Ending idle connection to host " << _hostAndPort
                  << " because the pool meets constraints; " << openConnections()
                  << " connections to that host remain open";
            return;
        }

        _processingPool[connPtr] = std::move(conn);

        connPtr->refresh(_parent->_options.refreshTimeout,
                         guardCallback([this](auto conn, auto status) {
                             finishRefresh(std::move(conn), std::move(status));
                         }));

        return;
    }

    // If it's fine as it is, just put it in the ready queue
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
    connPtr->setTimeout(_parent->_options.refreshRequirement, guardCallback([this, connPtr]() {
                            auto conn = takeFromPool(_readyPool, connPtr);

                            // We've already been checked out. We don't need to refresh
                            // ourselves.
                            if (!conn)
                                return;

                            // If we're in shutdown, we don't need to refresh connections
                            if (_state == State::kInShutdown)
                                return;

                            _checkedOutPool[connPtr] = std::move(conn);

                            connPtr->indicateSuccess();

                            returnConnection(connPtr);
                        }));
}

// Sets state to shutdown and kicks off the failure protocol to tank existing connections
void ConnectionPool::SpecificPool::triggerShutdown(const Status& status) {
    _state = State::kInShutdown;
    _droppedProcessingPool.clear();
    processFailure(status);
}

// Drop connections and fail all requests
void ConnectionPool::SpecificPool::processFailure(const Status& status) {
    // Bump the generation so we don't reuse any pending or checked out
    // connections
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
        if (_state != State::kInShutdown) {
            // If we're just dropping the pool, we can reuse them later
            _droppedProcessingPool[x.first] = std::move(x.second);
        }
    }
    _processingPool.clear();

    // Move the requests out so they aren't visible
    // in other threads
    decltype(_requests) requestsToFail;
    {
        using std::swap;
        swap(requestsToFail, _requests);
    }

    for (auto& request : requestsToFail) {
        request.second.setError(status);
    }
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

    spawnConnections();
}

// spawn enough connections to satisfy open requests and minpool, while
// honoring maxpool
void ConnectionPool::SpecificPool::spawnConnections() {
    // If some other thread (possibly this thread) is spawning connections,
    // don't keep padding the callstack.
    if (_inSpawnConnections)
        return;

    _inSpawnConnections = true;
    auto guard = makeGuard([&] { _inSpawnConnections = false; });

    // We want minConnections <= outstanding requests <= maxConnections
    auto target = [&] {
        return std::max(
            _parent->_options.minConnections,
            std::min(_requests.size() + _checkedOutPool.size(), _parent->_options.maxConnections));
    };

    // While all of our inflight connections are less than our target
    while ((_state != State::kInShutdown) &&
           (_readyPool.size() + _processingPool.size() + _checkedOutPool.size() < target()) &&
           (_processingPool.size() < _parent->_options.maxConnecting)) {
        if (_readyPool.empty() && _processingPool.empty()) {
            auto severity = MONGO_GET_LIMITED_SEVERITY(_hostAndPort, Seconds{1}, 0, 2);
            LOG(severity) << "Connecting to " << _hostAndPort;
        }

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
        handle->setup(_parent->_options.refreshTimeout,
                      guardCallback([this](auto conn, auto status) {
                          finishRefresh(std::move(conn), std::move(status));
                      }));

        // Note that this assumes that the refreshTimeout is sound for the
        // setupTimeout
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
        invariant(_state != State::kInShutdown);
        return conn;
    }

    return takeFromPool(_droppedProcessingPool, connPtr);
}


// Updates our state and manages the request timer
void ConnectionPool::SpecificPool::updateState() {
    if (_state == State::kInShutdown) {
        // If we're in shutdown, there is nothing to update. Our clients are all gone.
        if (_processingPool.empty()) {
            // If we have no more clients that require access to us, delist from the parent pool
            LOG(2) << "Delisting connection pool for " << _hostAndPort;
            _parent->_pools.erase(_hostAndPort);
        }
        return;
    }

    if (_requests.size()) {
        // We have some outstanding requests, we're live

        // If we were already running and the timer is the same as it was
        // before, nothing to do
        if (_state == State::kRunning && _requestTimerExpiration == _requests.front().first)
            return;

        _state = State::kRunning;

        _requestTimer->cancelTimeout();

        _requestTimerExpiration = _requests.front().first;

        auto timeout = _requests.front().first - _parent->_factory->now();

        // We set a timer for the most recent request, then invoke each timed
        // out request we couldn't service
        _requestTimer->setTimeout(
            timeout, guardCallback([this]() {
                auto now = _parent->_factory->now();

                while (_requests.size()) {
                    auto& x = _requests.front();

                    if (x.first <= now) {
                        auto promise = std::move(x.second);
                        std::pop_heap(begin(_requests), end(_requests), RequestComparator{});
                        _requests.pop_back();

                        promise.setError(Status(ErrorCodes::NetworkInterfaceExceededTimeLimit,
                                                "Couldn't get a connection within the time limit"));
                    } else {
                        break;
                    }
                }
            }));
    } else if (_checkedOutPool.size()) {
        // If we have no requests, but someone's using a connection, we just
        // hang around until the next request or a return

        _requestTimer->cancelTimeout();
        _state = State::kRunning;
        _requestTimerExpiration = _requestTimerExpiration.max();
    } else {
        // If we don't have any live requests and no one has checked out connections

        // If we used to be idle, just bail
        if (_state == State::kIdle)
            return;

        _state = State::kIdle;

        _requestTimer->cancelTimeout();

        _requestTimerExpiration = _parent->_factory->now() + _parent->_options.hostTimeout;

        auto timeout = _parent->_options.hostTimeout;

        // Set the shutdown timer, this gets reset on any request
        _requestTimer->setTimeout(
            timeout, guardCallback([this]() {
                if (_state != State::kIdle)
                    return;

                triggerShutdown(
                    Status(ErrorCodes::NetworkInterfaceExceededTimeLimit,
                           "Connection pool has been idle for longer than the host timeout"));
            }));
    }
}

}  // namespace executor
}  // namespace mongo
