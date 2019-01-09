
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kASIO

#include "mongo/platform/basic.h"

#include "mongo/executor/connection_pool.h"

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/executor/connection_pool_stats.h"
#include "mongo/executor/remote_command_request.h"
#include "mongo/stdx/memory.h"
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
     * Whenever a function enters a specific pool, the function needs to be guarded.
     * The presence of one of these guards will bump a counter on the specific pool
     * which will prevent the pool from removing itself from the map of pools.
     *
     * The complexity comes from the need to hold a lock when writing to the
     * _activeClients param on the specific pool.  Because the code beneath the client needs to lock
     * and unlock the parent mutex (and can leave unlocked), we want to start the client with the
     * lock acquired, move it into the client, then re-acquire to decrement the counter on the way
     * out.
     *
     * This callback also (perhaps overly aggressively) binds a shared pointer to the guard.
     * It is *always* safe to reference the original specific pool in the guarded function object.
     *
     * For a function object of signature:
     * R riskyBusiness(stdx::unique_lock<stdx::mutex>, ArgTypes...);
     *
     * It returns a function object of signature:
     * R safeCallback(ArgTypes...);
     */
    template <typename Callback>
    auto guardCallback(Callback&& cb) {
        return [ cb = std::forward<Callback>(cb), anchor = shared_from_this() ](auto&&... args) {
            stdx::unique_lock<stdx::mutex> lk(anchor->_parent->_mutex);
            ++(anchor->_activeClients);

            ON_BLOCK_EXIT([anchor]() {
                stdx::unique_lock<stdx::mutex> lk(anchor->_parent->_mutex);
                --(anchor->_activeClients);
            });

            return cb(std::move(lk), std::forward<decltype(args)>(args)...);
        };
    }

    SpecificPool(ConnectionPool* parent,
                 const HostAndPort& hostAndPort,
                 transport::ConnectSSLMode sslMode);
    ~SpecificPool();

    /**
     * Gets a connection from the specific pool. Sinks a unique_lock from the
     * parent to preserve the lock on _mutex
     */
    Future<ConnectionHandle> getConnection(Milliseconds timeout, stdx::unique_lock<stdx::mutex> lk);

    /**
     * Gets a connection from the specific pool if a connection is available and there are no
     * outstanding requests.
     */
    boost::optional<ConnectionHandle> tryGetConnection(const stdx::unique_lock<stdx::mutex>& lk);

    /**
     * Triggers the shutdown procedure. This function marks the state as kInShutdown
     * and calls processFailure below with the status provided. This may not immediately
     * delist or destruct this pool. However, both will happen eventually as ConnectionHandles
     * are deleted.
     */
    void triggerShutdown(const Status& status, stdx::unique_lock<stdx::mutex> lk);

    /**
     * Cascades a failure across existing connections and requests. Invoking
     * this function drops all current connections and fails all current
     * requests with the passed status.
     */
    void processFailure(const Status& status, stdx::unique_lock<stdx::mutex> lk);

    /**
     * Returns a connection to a specific pool. Sinks a unique_lock from the
     * parent to preserve the lock on _mutex
     */
    void returnConnection(ConnectionInterface* connection, stdx::unique_lock<stdx::mutex> lk);

    /**
     * Returns the number of connections currently checked out of the pool.
     */
    size_t inUseConnections(const stdx::unique_lock<stdx::mutex>& lk);

    /**
     * Returns the number of available connections in the pool.
     */
    size_t availableConnections(const stdx::unique_lock<stdx::mutex>& lk);

    /**
     * Returns the number of in progress connections in the pool.
     */
    size_t refreshingConnections(const stdx::unique_lock<stdx::mutex>& lk);

    /**
     * Returns the total number of connections ever created in this pool.
     */
    size_t createdConnections(const stdx::unique_lock<stdx::mutex>& lk);

    /**
     * Returns the total number of connections currently open that belong to
     * this pool. This is the sum of refreshingConnections, availableConnections,
     * and inUseConnections.
     */
    size_t openConnections(const stdx::unique_lock<stdx::mutex>& lk);

    /**
     * Return true if the tags on the specific pool match the passed in tags
     */
    bool matchesTags(const stdx::unique_lock<stdx::mutex>& lk,
                     transport::Session::TagMask tags) const {
        return !!(_tags & tags);
    }

    /**
     * Atomically manipulate the tags in the pool
     */
    void mutateTags(const stdx::unique_lock<stdx::mutex>& lk,
                    const stdx::function<transport::Session::TagMask(transport::Session::TagMask)>&
                        mutateFunc) {
        _tags = mutateFunc(_tags);
    }

    void fassertSSLModeIs(transport::ConnectSSLMode desired) const {
        if (desired != _sslMode) {
            severe() << "Mixing ssl modes for a single host is not supported";
            fassertFailedNoTrace(51043);
        }
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

    void addToReady(stdx::unique_lock<stdx::mutex>& lk, OwnedConnection conn);

    void fulfillRequests(stdx::unique_lock<stdx::mutex>& lk);

    void spawnConnections(stdx::unique_lock<stdx::mutex>& lk);

    // This internal helper is used both by tryGet and by fulfillRequests and differs in that it
    // skips some bookkeeping that the other callers do on their own
    boost::optional<ConnectionHandle> tryGetInternal(const stdx::unique_lock<stdx::mutex>& lk);

    template <typename OwnershipPoolType>
    typename OwnershipPoolType::mapped_type takeFromPool(
        OwnershipPoolType& pool, typename OwnershipPoolType::key_type connPtr);

    OwnedConnection takeFromProcessingPool(ConnectionInterface* connection);

    void updateStateInLock();

private:
    ConnectionPool* const _parent;

    const transport::ConnectSSLMode _sslMode;
    const HostAndPort _hostAndPort;

    LRUOwnershipPool _readyPool;
    OwnershipPool _processingPool;
    OwnershipPool _droppedProcessingPool;
    OwnershipPool _checkedOutPool;

    std::vector<Request> _requests;

    std::shared_ptr<TimerInterface> _requestTimer;
    Date_t _requestTimerExpiration;
    size_t _activeClients;
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

constexpr Milliseconds ConnectionPool::kDefaultHostTimeout;
size_t const ConnectionPool::kDefaultMaxConns = std::numeric_limits<size_t>::max();
size_t const ConnectionPool::kDefaultMinConns = 1;
size_t const ConnectionPool::kDefaultMaxConnecting = std::numeric_limits<size_t>::max();
constexpr Milliseconds ConnectionPool::kDefaultRefreshRequirement;
constexpr Milliseconds ConnectionPool::kDefaultRefreshTimeout;

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
        stdx::unique_lock<stdx::mutex> lk(_mutex);
        return _pools;
    }();

    for (const auto& pair : pools) {
        stdx::unique_lock<stdx::mutex> lk(_mutex);
        pair.second->triggerShutdown(
            Status(ErrorCodes::ShutdownInProgress, "Shutting down the connection pool"),
            std::move(lk));
    }
}

void ConnectionPool::dropConnections(const HostAndPort& hostAndPort) {
    stdx::unique_lock<stdx::mutex> lk(_mutex);

    auto iter = _pools.find(hostAndPort);

    if (iter == _pools.end())
        return;

    auto pool = iter->second;
    pool->processFailure(Status(ErrorCodes::PooledConnectionsDropped, "Pooled connections dropped"),
                         std::move(lk));
}

void ConnectionPool::dropConnections(transport::Session::TagMask tags) {
    // Grab all current pools (under the lock)
    auto pools = [&] {
        stdx::unique_lock<stdx::mutex> lk(_mutex);
        return _pools;
    }();

    for (const auto& pair : pools) {
        auto& pool = pair.second;

        stdx::unique_lock<stdx::mutex> lk(_mutex);
        if (pool->matchesTags(lk, tags))
            continue;

        pool->processFailure(
            Status(ErrorCodes::PooledConnectionsDropped, "Pooled connections dropped"),
            std::move(lk));
    }
}

void ConnectionPool::mutateTags(
    const HostAndPort& hostAndPort,
    const stdx::function<transport::Session::TagMask(transport::Session::TagMask)>& mutateFunc) {
    stdx::unique_lock<stdx::mutex> lk(_mutex);

    auto iter = _pools.find(hostAndPort);

    if (iter == _pools.end())
        return;

    auto pool = iter->second;
    pool->mutateTags(lk, mutateFunc);
}

void ConnectionPool::get_forTest(const HostAndPort& hostAndPort,
                                 Milliseconds timeout,
                                 GetConnectionCallback cb) {
    return get(hostAndPort, transport::kGlobalSSLMode, timeout).getAsync(std::move(cb));
}

boost::optional<ConnectionPool::ConnectionHandle> ConnectionPool::tryGet(
    const HostAndPort& hostAndPort, transport::ConnectSSLMode sslMode) {
    stdx::unique_lock<stdx::mutex> lk(_mutex);

    auto iter = _pools.find(hostAndPort);

    if (iter == _pools.end()) {
        return boost::none;
    }

    const auto& pool = iter->second;
    invariant(pool);
    pool->fassertSSLModeIs(sslMode);

    return pool->tryGetConnection(lk);
}

Future<ConnectionPool::ConnectionHandle> ConnectionPool::get(const HostAndPort& hostAndPort,
                                                             transport::ConnectSSLMode sslMode,
                                                             Milliseconds timeout) {
    std::shared_ptr<SpecificPool> pool;

    stdx::unique_lock<stdx::mutex> lk(_mutex);

    auto iter = _pools.find(hostAndPort);

    if (iter == _pools.end()) {
        pool = stdx::make_unique<SpecificPool>(this, hostAndPort, sslMode);
        _pools[hostAndPort] = pool;
    } else {
        pool = iter->second;
        pool->fassertSSLModeIs(sslMode);
    }

    invariant(pool);

    return pool->getConnection(timeout, std::move(lk));
}

void ConnectionPool::appendConnectionStats(ConnectionPoolStats* stats) const {
    stdx::unique_lock<stdx::mutex> lk(_mutex);

    for (const auto& kv : _pools) {
        HostAndPort host = kv.first;

        auto& pool = kv.second;
        ConnectionStatsPer hostStats{pool->inUseConnections(lk),
                                     pool->availableConnections(lk),
                                     pool->createdConnections(lk),
                                     pool->refreshingConnections(lk)};
        stats->updateStatsForHost(_name, host, hostStats);
    }
}

size_t ConnectionPool::getNumConnectionsPerHost(const HostAndPort& hostAndPort) const {
    stdx::unique_lock<stdx::mutex> lk(_mutex);
    auto iter = _pools.find(hostAndPort);
    if (iter != _pools.end()) {
        return iter->second->openConnections(lk);
    }

    return 0;
}

void ConnectionPool::returnConnection(ConnectionInterface* conn) {
    stdx::unique_lock<stdx::mutex> lk(_mutex);

    auto iter = _pools.find(conn->getHostAndPort());

    invariant(iter != _pools.end(),
              str::stream() << "Tried to return connection but no pool found for "
                            << conn->getHostAndPort());

    auto pool = iter->second;
    pool->returnConnection(conn, std::move(lk));
}

ConnectionPool::SpecificPool::SpecificPool(ConnectionPool* parent,
                                           const HostAndPort& hostAndPort,
                                           transport::ConnectSSLMode sslMode)
    : _parent(parent),
      _sslMode(sslMode),
      _hostAndPort(hostAndPort),
      _readyPool(std::numeric_limits<size_t>::max()),
      _requestTimer(parent->_factory->makeTimer()),
      _activeClients(0),
      _generation(0),
      _inFulfillRequests(false),
      _inSpawnConnections(false),
      _created(0),
      _state(State::kRunning) {}

ConnectionPool::SpecificPool::~SpecificPool() {
    DESTRUCTOR_GUARD(_requestTimer->cancelTimeout();)

    invariant(_requests.empty());
    invariant(_checkedOutPool.empty());
}

size_t ConnectionPool::SpecificPool::inUseConnections(const stdx::unique_lock<stdx::mutex>& lk) {
    return _checkedOutPool.size();
}

size_t ConnectionPool::SpecificPool::availableConnections(
    const stdx::unique_lock<stdx::mutex>& lk) {
    return _readyPool.size();
}

size_t ConnectionPool::SpecificPool::refreshingConnections(
    const stdx::unique_lock<stdx::mutex>& lk) {
    return _processingPool.size();
}

size_t ConnectionPool::SpecificPool::createdConnections(const stdx::unique_lock<stdx::mutex>& lk) {
    return _created;
}

size_t ConnectionPool::SpecificPool::openConnections(const stdx::unique_lock<stdx::mutex>& lk) {
    return _checkedOutPool.size() + _readyPool.size() + _processingPool.size();
}

Future<ConnectionPool::ConnectionHandle> ConnectionPool::SpecificPool::getConnection(
    Milliseconds timeout, stdx::unique_lock<stdx::mutex> lk) {
    invariant(_state != State::kInShutdown);

    if (timeout < Milliseconds(0) || timeout > _parent->_options.refreshTimeout) {
        timeout = _parent->_options.refreshTimeout;
    }

    const auto expiration = _parent->_factory->now() + timeout;
    auto pf = makePromiseFuture<ConnectionHandle>();

    _requests.push_back(make_pair(expiration, std::move(pf.promise)));
    std::push_heap(begin(_requests), end(_requests), RequestComparator{});

    updateStateInLock();

    spawnConnections(lk);
    fulfillRequests(lk);

    return std::move(pf.future);
}

boost::optional<ConnectionPool::ConnectionHandle> ConnectionPool::SpecificPool::tryGetConnection(
    const stdx::unique_lock<stdx::mutex>& lk) {
    invariant(_state != State::kInShutdown);

    if (_requests.size()) {
        return boost::none;
    }

    auto conn = tryGetInternal(lk);

    updateStateInLock();

    return conn;
}

boost::optional<ConnectionPool::ConnectionHandle> ConnectionPool::SpecificPool::tryGetInternal(
    const stdx::unique_lock<stdx::mutex>&) {

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
        return ConnectionHandle(connPtr,
                                guardCallback([this](stdx::unique_lock<stdx::mutex> localLk,
                                                     ConnectionPool::ConnectionInterface* conn) {
                                    returnConnection(conn, std::move(localLk));
                                }));
    }

    return boost::none;
}

void ConnectionPool::SpecificPool::returnConnection(ConnectionInterface* connPtr,
                                                    stdx::unique_lock<stdx::mutex> lk) {
    auto needsRefreshTP = connPtr->getLastUsed() + _parent->_options.refreshRequirement;

    auto conn = takeFromPool(_checkedOutPool, connPtr);
    invariant(conn);

    updateStateInLock();

    if (conn->getGeneration() != _generation) {
        // If the connection is from an older generation, just return.
        return;
    }

    if (!conn->getStatus().isOK()) {
        // TODO: alert via some callback if the host is bad
        log() << "Ending connection to host " << _hostAndPort << " due to bad connection status; "
              << openConnections(lk) << " connections to that host remain open";
        return;
    }

    auto now = _parent->_factory->now();
    if (needsRefreshTP <= now) {
        // If we need to refresh this connection

        if (_readyPool.size() + _processingPool.size() + _checkedOutPool.size() >=
            _parent->_options.minConnections) {
            // If we already have minConnections, just let the connection lapse
            log() << "Ending idle connection to host " << _hostAndPort
                  << " because the pool meets constraints; " << openConnections(lk)
                  << " connections to that host remain open";
            return;
        }

        _processingPool[connPtr] = std::move(conn);

        // Unlock in case refresh can occur immediately
        lk.unlock();
        connPtr->refresh(_parent->_options.refreshTimeout,
                         guardCallback([this](stdx::unique_lock<stdx::mutex> lk,
                                              ConnectionInterface* connPtr,
                                              Status status) {
                             auto conn = takeFromProcessingPool(connPtr);

                             // If we're in shutdown, we don't need refreshed connections
                             if (_state == State::kInShutdown)
                                 return;

                             // If the connection refreshed successfully, throw it back in
                             // the ready pool
                             if (status.isOK()) {
                                 // If the host and port were dropped, let this lapse
                                 if (conn->getGeneration() == _generation) {
                                     addToReady(lk, std::move(conn));
                                 }
                                 spawnConnections(lk);
                                 return;
                             }

                             // If we've exceeded the time limit, start a new connect,
                             // rather than failing all operations.  We do this because the
                             // various callers have their own time limit which is unrelated
                             // to our internal one.
                             if (status.code() == ErrorCodes::NetworkInterfaceExceededTimeLimit) {
                                 log() << "Pending connection to host " << _hostAndPort
                                       << " did not complete within the connection timeout,"
                                       << " retrying with a new connection;" << openConnections(lk)
                                       << " connections to that host remain open";
                                 spawnConnections(lk);
                                 return;
                             }

                             // Otherwise pass the failure on through
                             processFailure(status, std::move(lk));
                         }));
        lk.lock();
    } else {
        // If it's fine as it is, just put it in the ready queue
        addToReady(lk, std::move(conn));
    }

    updateStateInLock();
}

// Adds a live connection to the ready pool
void ConnectionPool::SpecificPool::addToReady(stdx::unique_lock<stdx::mutex>& lk,
                                              OwnedConnection conn) {
    auto connPtr = conn.get();

    // This makes the connection the new most-recently-used connection.
    _readyPool.add(connPtr, std::move(conn));

    // Our strategy for refreshing connections is to check them out and
    // immediately check them back in (which kicks off the refresh logic in
    // returnConnection
    connPtr->setTimeout(_parent->_options.refreshRequirement,
                        guardCallback([this, connPtr](stdx::unique_lock<stdx::mutex> lk) {
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

                            returnConnection(connPtr, std::move(lk));
                        }));

    fulfillRequests(lk);
}

// Sets state to shutdown and kicks off the failure protocol to tank existing connections
void ConnectionPool::SpecificPool::triggerShutdown(const Status& status,
                                                   stdx::unique_lock<stdx::mutex> lk) {
    _state = State::kInShutdown;
    _droppedProcessingPool.clear();
    processFailure(status, std::move(lk));
}

// Drop connections and fail all requests
void ConnectionPool::SpecificPool::processFailure(const Status& status,
                                                  stdx::unique_lock<stdx::mutex> lk) {
    // Bump the generation so we don't reuse any pending or checked out
    // connections
    _generation++;

    // When a connection enters the ready pool, its timer is set to eventually refresh the
    // connection. This requires a lifetime extension of the specific pool because the connection
    // timer is tied to the lifetime of the connection, not the pool. That said, we can destruct
    // all of the connections and thus timers of which we have ownership.
    // In short, clearing the ready pool helps the SpecificPool drain.
    _readyPool.clear();

    // Log something helpful
    log() << "Dropping all pooled connections to " << _hostAndPort << " due to " << status;

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

    // Update state to reflect the lack of requests
    updateStateInLock();

    // Drop the lock and process all of the requests
    // with the same failed status
    lk.unlock();

    for (auto& request : requestsToFail) {
        request.second.setError(status);
    }
}

// fulfills as many outstanding requests as possible
void ConnectionPool::SpecificPool::fulfillRequests(stdx::unique_lock<stdx::mutex>& lk) {
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
        auto conn = tryGetInternal(lk);

        if (!conn) {
            break;
        }

        // Grab the request and callback
        auto promise = std::move(_requests.front().second);
        std::pop_heap(begin(_requests), end(_requests), RequestComparator{});
        _requests.pop_back();

        lk.unlock();
        promise.emplaceValue(std::move(*conn));
        lk.lock();

        updateStateInLock();
    }

    spawnConnections(lk);
}

// spawn enough connections to satisfy open requests and minpool, while
// honoring maxpool
void ConnectionPool::SpecificPool::spawnConnections(stdx::unique_lock<stdx::mutex>& lk) {
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
        lk.unlock();
        handle->setup(
            _parent->_options.refreshTimeout,
            guardCallback([this](
                stdx::unique_lock<stdx::mutex> lk, ConnectionInterface* connPtr, Status status) {
                auto conn = takeFromProcessingPool(connPtr);

                // If we're in shutdown, we don't need this conn
                if (_state == State::kInShutdown)
                    return;

                if (status.isOK()) {
                    // If the host and port was dropped, let the connection lapse
                    if (conn->getGeneration() == _generation) {
                        addToReady(lk, std::move(conn));
                    }
                    spawnConnections(lk);
                } else if (status.code() == ErrorCodes::NetworkInterfaceExceededTimeLimit) {
                    // If we've exceeded the time limit, restart the connect, rather than
                    // failing all operations.  We do this because the various callers
                    // have their own time limit which is unrelated to our internal one.
                    spawnConnections(lk);
                } else {
                    // If the setup failed, cascade the failure edge
                    processFailure(status, std::move(lk));
                }
            }));
        // Note that this assumes that the refreshTimeout is sound for the
        // setupTimeout

        lk.lock();
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
void ConnectionPool::SpecificPool::updateStateInLock() {
    if (_state == State::kInShutdown) {
        // If we're in shutdown, there is nothing to update. Our clients are all gone.
        if (_processingPool.empty() && !_activeClients) {
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
            timeout, guardCallback([this](stdx::unique_lock<stdx::mutex> lk) {
                auto now = _parent->_factory->now();

                while (_requests.size()) {
                    auto& x = _requests.front();

                    if (x.first <= now) {
                        auto promise = std::move(x.second);
                        std::pop_heap(begin(_requests), end(_requests), RequestComparator{});
                        _requests.pop_back();

                        lk.unlock();
                        promise.setError(Status(ErrorCodes::NetworkInterfaceExceededTimeLimit,
                                                "Couldn't get a connection within the time limit"));
                        lk.lock();
                    } else {
                        break;
                    }
                }

                updateStateInLock();
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
        _requestTimer->setTimeout(timeout, [ this, anchor = shared_from_this() ]() {
            stdx::unique_lock<stdx::mutex> lk(anchor->_parent->_mutex);
            if (_state != State::kIdle)
                return;

            triggerShutdown(
                Status(ErrorCodes::NetworkInterfaceExceededTimeLimit,
                       "Connection pool has been idle for longer than the host timeout"),
                std::move(lk));
        });
    }
}

}  // namespace executor
}  // namespace mongo
