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

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/config.h"  // IWYU pragma: keep
#include "mongo/executor/connection_pool_stats.h"
#include "mongo/executor/egress_connection_closer.h"
#include "mongo/executor/egress_connection_closer_manager.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/platform/compiler.h"
#include "mongo/stdx/mutex.h"
#include "mongo/stdx/unordered_map.h"
#include "mongo/transport/session.h"
#include "mongo/transport/transport_layer.h"
#include "mongo/util/cancellation.h"
#include "mongo/util/clock_source.h"
#include "mongo/util/duration.h"
#include "mongo/util/functional.h"
#include "mongo/util/future.h"
#include "mongo/util/hierarchical_acquisition.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/net/ssl_options.h"
#include "mongo/util/observable_mutex.h"
#include "mongo/util/out_of_line_executor.h"
#include "mongo/util/time_support.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <utility>
#include <vector>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {

class BSONObjBuilder;

namespace executor {

/**
 * The actual user visible connection pool.
 *
 * This pool is constructed with a DependentTypeFactoryInterface which provides the tools it
 * needs to generate connections and manage them over time.
 *
 * The overall workflow here is to manage separate pools for each unique
 * HostAndPort. See comments on the various Options for how the pool operates.
 */
class ConnectionPool : public EgressConnectionCloser,
                       public std::enable_shared_from_this<ConnectionPool> {
    class LimitController;

public:
    class SpecificPool;

    class ConnectionInterface;
    class DependentTypeFactoryInterface;
    class TimerInterface;
    class ControllerInterface;

    using ConnectionHandleDeleter = std::function<void(ConnectionInterface* connection)>;
    using ConnectionHandle = std::unique_ptr<ConnectionInterface, ConnectionHandleDeleter>;

    using RetrieveConnection = unique_function<SemiFuture<ConnectionHandle>()>;
    using GetConnectionCallback = unique_function<void(StatusWith<ConnectionHandle>)>;

    using PoolId = uint64_t;

    static constexpr size_t kDefaultMaxConns = std::numeric_limits<size_t>::max();
    static constexpr size_t kDefaultMinConns = 1;
    static constexpr size_t kDefaultMaxConnecting = 2;
    static constexpr Milliseconds kDefaultHostTimeout = Minutes(5);
    static constexpr Milliseconds kDefaultRefreshRequirement = Minutes(1);
    static constexpr Milliseconds kDefaultRefreshTimeout = Seconds(20);
    static constexpr Milliseconds kHostRetryTimeout = Seconds(1);

    /**
     * Default value for limiting the size of a connection requests queue.
     */
    static constexpr size_t kDefaultConnectionRequestsMaxQueueDepth = 0;

    static const Status kConnectionStateUnknown;

    /**
     * Make a vanilla LimitController as a decent default option
     */
    static std::shared_ptr<ControllerInterface> makeLimitController();

    struct Options {
        Options() {}

        /**
         * The minimum number of connections to keep alive while the pool is in
         * operation
         */
        size_t minConnections = kDefaultMinConns;

        /**
         * The maximum number of connections to spawn for a host. This includes
         * pending connections in setup and connections checked out of the pool
         * as well as the obvious live connections in the pool.
         */
        size_t maxConnections = kDefaultMaxConns;

        /**
         * The maximum number of processing connections for a host.  This includes pending
         * connections in setup/refresh. It's designed to rate limit connection storms rather than
         * steady state processing (as maxConnections does).
         */
        size_t maxConnecting = kDefaultMaxConnecting;

        /**
         * Amount of time to wait before timing out a refresh attempt
         */
        Milliseconds refreshTimeout = kDefaultRefreshTimeout;

        /**
         * Amount of time a connection may be idle before it cannot be returned
         * for a user request and must instead be checked out and refreshed
         * before handing to a user.
         */
        Milliseconds refreshRequirement = kDefaultRefreshRequirement;

        /**
         * Amount of time to keep a specific pool around without any checked
         * out connections or new requests
         */
        Milliseconds hostTimeout = kDefaultHostTimeout;

        /**
         * An egress tag closer manager which will provide global access to this connection pool.
         * The manager set's tags and potentially drops connections that don't match those tags.
         *
         * The manager will hold this pool for the lifetime of the pool.
         */
        EgressConnectionCloserManager* egressConnectionCloserManager = nullptr;

        /**
         * Connections created through this connection pool will not attempt to authenticate.
         */
        bool skipAuthentication = false;

#ifdef MONGO_CONFIG_SSL
        /**
         * Provides SSL params if the egress cluster connection requires custom SSL certificates
         * different from the global (default) certificates.
         */
        boost::optional<TransientSSLParams> transientSSLParams;
#endif

        std::function<std::shared_ptr<ControllerInterface>(void)> controllerFactory =
            &ConnectionPool::makeLimitController;

        /**
         * This parameter represents the limit on the size of connection requests queue. If this
         * parameter is 0 then no checks and no rejections will be performed.
         */
        size_t connectionRequestsMaxQueueDepth = kDefaultConnectionRequestsMaxQueueDepth;
    };

    /**
     * A set of flags describing the health of a host pool
     */
    struct HostHealth {
        /**
         * The pool is expired and can be shutdown by updateController
         *
         * This flag is set to true when there have been no connection requests or in use
         * connections for ControllerInterface::hostTimeout().
         *
         * This flag is set to false whenever a connection is requested.
         */
        bool isExpired = false;

        /**
         *  The pool has processed a failure and will not spawn new connections until requested
         *
         *  This flag is set to true by processFailure(), and thus also triggerShutdown().
         *
         *  This flag is set to false whenever a connection is requested.
         *
         *  As a further note, this prevents us from spamming a failed host with connection
         *  attempts. If an external user believes a host should be available, they can request
         *  again.
         */
        bool isFailed = false;

        /**
         * The pool is shutdown and will never be called by the ConnectionPool again.
         *
         * This flag is set to true by triggerShutdown() or updateController(). It is never unset.
         */
        bool isShutdown = false;
    };

    /**
     * The state of connection pooling for a single host
     *
     * This should only be constructed by the SpecificPool.
     */
    struct HostState {
        HostHealth health;
        size_t requests = 0;
        size_t pending = 0;
        size_t ready = 0;
        size_t active = 0;
        size_t leased = 0;

        std::string toString() const;
    };

    /**
     * A simple set of controls to direct a single host
     *
     * This should only be constructed by a ControllerInterface
     */
    struct ConnectionControls {
        size_t maxPendingConnections = kDefaultMaxConnecting;
        size_t targetConnections = 0;

        std::string toString() const;
    };

    /**
     * A set of hosts and a flag canShutdown for if the group can shutdown
     *
     * This should only be constructed by a ControllerInterface
     */
    struct HostGroupState {
        std::vector<HostAndPort> hosts;
        bool canShutdown = false;
    };

    explicit ConnectionPool(std::shared_ptr<DependentTypeFactoryInterface> impl,
                            std::string name,
                            Options options = Options{});

    ~ConnectionPool() override;

    void shutdown();

    /**
     * Drops connection to a specific target in the connection pool and relays a message as to
     * why the connection was dropped.
     */
    void dropConnections(const HostAndPort& target, const Status& status) override;
    void dropConnections(const HostAndPort& target) {
        dropConnections(
            target,
            Status(ErrorCodes::PooledConnectionsDropped, "Drop connections to a specific target"));
    }

    /**
     * Drops all connections, but if a certain SpecificPool (and therefore HostAndPort) is
     * marked as keep open, that connection will not be dropped.
     */
    void dropConnections(const Status& status) override;
    void dropConnections() {
        dropConnections(Status(ErrorCodes::PooledConnectionsDropped, "Drop all connections"));
    }

    /**
     * Marks SpecificPool to be kept open for dropConnections(), must acquire connection pool
     * mutex.
     */
    void setKeepOpen(const HostAndPort& hostAndPort, bool keepOpen) override;

    inline SemiFuture<ConnectionHandle> get(
        const HostAndPort& hostAndPort,
        transport::ConnectSSLMode sslMode,
        Milliseconds timeout,
        CancellationToken token = CancellationToken::uncancelable()) {
        return _get(hostAndPort, sslMode, timeout, false /*lease*/, std::move(token));
    }

    void get_forTest(const HostAndPort& hostAndPort,
                     Milliseconds timeout,
                     GetConnectionCallback cb);

    /**
     * "Lease" a connection from the pool.
     *
     * Connections retrieved via this method are not assumed to be in active use for the duration of
     * their lease and are reported separately in metrics. Otherwise, this method behaves similarly
     * to `ConnectionPool::get`.
     */
    inline SemiFuture<ConnectionHandle> lease(
        const HostAndPort& hostAndPort,
        transport::ConnectSSLMode sslMode,
        Milliseconds timeout,
        CancellationToken token = CancellationToken::uncancelable()) {
        return _get(hostAndPort, sslMode, timeout, true /*lease*/, std::move(token));
    }

    void lease_forTest(const HostAndPort& hostAndPort,
                       Milliseconds timeout,
                       GetConnectionCallback cb);

    void appendConnectionStats(ConnectionPoolStats* stats) const;

    size_t getNumConnectionsPerHost(const HostAndPort& hostAndPort) const;

    std::string getName() const {
        return _name;
    }

private:
    ClockSource* _getFastClockSource() const;

    SemiFuture<ConnectionHandle> _get(const HostAndPort& hostAndPort,
                                      transport::ConnectSSLMode sslMode,
                                      Milliseconds timeout,
                                      bool leased,
                                      const CancellationToken& token);

    void retrieve_forTest(RetrieveConnection retrieve, GetConnectionCallback cb);

    std::string _name;

    const std::shared_ptr<DependentTypeFactoryInterface> _factory;
    const Options _options;

    std::shared_ptr<ControllerInterface> _controller;

    // The global mutex for specific pool access and the generation counter
    mutable ObservableMutex<stdx::mutex> _mutex;
    PoolId _nextPoolId = 0;
    stdx::unordered_map<HostAndPort, std::shared_ptr<SpecificPool>> _pools;
    bool _isShutDown = false;

    // Preserves the total created connection count for SpecificPools that were destroyed.
    stdx::unordered_map<HostAndPort, size_t> _cachedCreatedConnections;

    EgressConnectionCloserManager* _manager;

    mutable ClockSource* _fastClockSource{nullptr};
    mutable std::once_flag _fastClkSrcInitFlag;
};

/**
 * Interface for a basic timer
 *
 * Minimal interface sets a timer with a callback and cancels the timer.
 */
class ConnectionPool::TimerInterface {
    TimerInterface(const TimerInterface&) = delete;
    TimerInterface& operator=(const TimerInterface&) = delete;

public:
    TimerInterface() = default;

    using TimeoutCallback = std::function<void()>;

    virtual ~TimerInterface() = default;

    /**
     * Sets the timeout for the timer. Setting an already set timer should
     * override the previous timer.
     */
    virtual void setTimeout(Milliseconds timeout, TimeoutCallback cb) = 0;

    /**
     * It should be safe to cancel a previously canceled, or never set, timer.
     */
    virtual void cancelTimeout() = 0;

    /**
     * Returns the current time for the clock used by the timer
     */
    virtual Date_t now() = 0;
};

/**
 * Interface for connection pool connections
 *
 * Provides a minimal interface to manipulate connections within the pool,
 * specifically callbacks to set them up (connect + auth + whatever else),
 * refresh them (issue some kind of ping) and manage a timer.
 */
class ConnectionPool::ConnectionInterface : public TimerInterface {
    ConnectionInterface(const ConnectionInterface&) = delete;
    ConnectionInterface& operator=(const ConnectionInterface&) = delete;

    friend class ConnectionPool;

public:
    explicit ConnectionInterface(size_t generation) : _generation(generation) {}

    ~ConnectionInterface() override = default;

    /**
     * Indicates that the user is now done with this connection. Users MUST call either
     * this method or indicateFailure() before returning the connection to its pool.
     */
    void indicateSuccess();

    /**
     * Indicates that a connection has failed. This will prevent the connection
     * from re-entering the connection pool. Users MUST call either this method or
     * indicateSuccess() before returning connections to the pool.
     */
    void indicateFailure(Status status);

    /**
     * This method updates a 'liveness' timestamp to avoid unnecessarily refreshing
     * the connection.
     *
     * This method should be invoked whenever we perform an operation on the connection that must
     * have done work.  I.e. actual networking was performed.  If a connection was checked out, then
     * back in without use, one would expect an indicateSuccess without an indicateUsed.  Only if we
     * checked it out and did work would we call indicateUsed.
     */
    void indicateUsed();

    /**
     * The HostAndPort for the connection. This should be the same as the
     * HostAndPort passed to DependentTypeFactoryInterface::makeConnection.
     */
    virtual const HostAndPort& getHostAndPort() const = 0;
    virtual transport::ConnectSSLMode getSslMode() const = 0;

    /**
     * Check if the connection is healthy using some implementation defined condition.
     */
    virtual bool isHealthy() = 0;

    /**
     * The implementation may choose to override this method to provide a quick check for
     * connection health (e.g., by periodically caching the return value of the last invocation).
     * Callers should be aware that a "true" return value does not always indicate a healthy
     * connection.
     */
    virtual bool maybeHealthy() {
        return isHealthy();
    }

    /**
     * Returns the last used time point for the connection
     */
    Date_t getLastUsed() const;

    /**
     * Returns the number of times the connection was used by operations.
     */
    size_t getTimesUsed() const;

    /**
     * Returns the status associated with the connection. If the status is not
     * OK, the connection will not be returned to the pool.
     */
    const Status& getStatus() const;

    /**
     * Get the generation of the connection. This is used to track whether to
     * continue using a connection after a call to dropConnections() by noting
     * if the generation on the specific pool is the same as the generation on
     * a connection (if not the connection is from a previous era and should
     * not be re-used).
     */
    size_t getGeneration() const;

protected:
    /**
     * Making these protected makes the definitions available to override in
     * children.
     */
    using SetupCallback = unique_function<void(ConnectionInterface*, Status)>;
    using RefreshCallback = unique_function<void(ConnectionInterface*, Status)>;

    /**
     * Sets up the connection. This should include connection + auth + any
     * other associated hooks.
     */
    virtual void setup(Milliseconds timeout, SetupCallback cb, std::string instanceName) = 0;

    /**
     * Resets the connection's state to kConnectionStateUnknown for the next user.
     */
    void resetToUnknown();

    /**
     * Refreshes the connection. This should involve a network round trip and
     * should strongly imply an active connection
     */
    virtual void refresh(Milliseconds timeout, RefreshCallback cb) = 0;

private:
    size_t _generation;
    Date_t _lastUsed;
    AtomicWord<size_t> _timesUsed{0};
    Status _status = ConnectionPool::kConnectionStateUnknown;
};

/**
 * An implementation of ControllerInterface directs the behavior of a SpecificPool
 *
 * Generally speaking, a Controller will be given HostState via updateState and then return Controls
 * via getControls. A Controller is expected to not directly mutate its SpecificPool, including via
 * its ConnectionPool pointer. A Controller is expected to be given to only one ConnectionPool.
 */
class ConnectionPool::ControllerInterface {
public:
    using SpecificPool = typename ConnectionPool::SpecificPool;
    using HostState = typename ConnectionPool::HostState;
    using ConnectionControls = typename ConnectionPool::ConnectionControls;
    using HostGroupState = typename ConnectionPool::HostGroupState;
    using PoolId = typename ConnectionPool::PoolId;

    virtual ~ControllerInterface() = default;

    /**
     * Initialize this ControllerInterface using the given ConnectionPool
     *
     * ConnectionPools provide access to Executors and other DTF-provided objects.
     */
    virtual void init(ConnectionPool* parent);

    /**
     * Inform this Controller that a pool should be tracked
     */
    virtual void addHost(PoolId id, const HostAndPort& host) = 0;

    /**
     * Inform this Controller of a new State for a pool
     *
     * This function returns the state of the group of hosts to which this host belongs.
     */
    virtual HostGroupState updateHost(PoolId id, const HostState& stats) = 0;

    /**
     * Inform this Controller that a pool is no longer tracked
     */
    virtual void removeHost(PoolId id) = 0;

    /**
     * Get controls for the given pool
     */
    virtual ConnectionControls getControls(PoolId id) = 0;

    /**
     * Get the various timeouts that this controller suggests
     */
    virtual Milliseconds hostTimeout() const = 0;
    virtual Milliseconds pendingTimeout() const = 0;
    virtual Milliseconds toRefreshTimeout() const = 0;

    virtual size_t connectionRequestsMaxQueueDepth() const = 0;
    virtual size_t maxConnections() const = 0;

    /**
     * Get the name for this controller
     *
     * This function is intended to provide increased visibility into which controller is in use
     */
    virtual StringData name() const = 0;

    const ConnectionPool* getPool() const {
        return _pool;
    }

    Options getPoolOptions() const {
        return _pool->_options;
    }

    virtual void updateConnectionPoolStats([[maybe_unused]] ConnectionPoolStats* cps) const = 0;

protected:
    ConnectionPool* _pool = nullptr;
};

/**
 * Implementation interface for the connection pool
 *
 * This factory provides generators for connections, timers and a clock for the
 * connection pool.
 */
class ConnectionPool::DependentTypeFactoryInterface {
    DependentTypeFactoryInterface(const DependentTypeFactoryInterface&) = delete;
    DependentTypeFactoryInterface& operator=(const DependentTypeFactoryInterface&) = delete;

public:
    DependentTypeFactoryInterface() = default;

    virtual ~DependentTypeFactoryInterface() = default;

    /**
     * Makes a new connection given a host and port
     */
    virtual std::shared_ptr<ConnectionInterface> makeConnection(const HostAndPort& hostAndPort,
                                                                transport::ConnectSSLMode sslMode,
                                                                size_t generation) = 0;

    /**
     *  Return the executor for use with this factory
     */
    virtual const std::shared_ptr<OutOfLineExecutor>& getExecutor() = 0;

    /**
     * Makes a new timer
     */
    virtual std::shared_ptr<TimerInterface> makeTimer() = 0;

    /**
     * Returns the current time point
     */
    virtual Date_t now() = 0;

    /**
     * Returns the fast clock source.
     * The default implementation gets it from the global service context.
     */
    virtual ClockSource* getFastClockSource();

    /**
     * shutdown
     */
    virtual void shutdown() = 0;
};

inline ClockSource* ConnectionPool::_getFastClockSource() const {
    if (MONGO_unlikely(!_fastClockSource)) {
        std::call_once(_fastClkSrcInitFlag,
                       [&]() { _fastClockSource = _factory->getFastClockSource(); });
    }
    return _fastClockSource;
}

}  // namespace executor
}  // namespace mongo
