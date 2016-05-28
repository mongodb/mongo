/** *    Copyright (C) 2015 MongoDB Inc.
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

#pragma once

#include <memory>
#include <queue>
#include <unordered_map>

#include "mongo/base/disallow_copying.h"
#include "mongo/stdx/chrono.h"
#include "mongo/stdx/functional.h"
#include "mongo/stdx/mutex.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/time_support.h"

namespace mongo {

class BSONObjBuilder;

namespace executor {

struct ConnectionPoolStats;

/**
 * The actual user visible connection pool.
 *
 * This pool is constructed with a DependentTypeFactoryInterface which provides the tools it
 * needs to generate connections and manage them over time.
 *
 * The overall workflow here is to manage separate pools for each unique
 * HostAndPort. See comments on the various Options for how the pool operates.
 */
class ConnectionPool {
    class ConnectionHandleDeleter;
    class SpecificPool;

public:
    class ConnectionInterface;
    class DependentTypeFactoryInterface;
    class TimerInterface;

    using ConnectionHandle = std::unique_ptr<ConnectionInterface, ConnectionHandleDeleter>;

    using GetConnectionCallback = stdx::function<void(StatusWith<ConnectionHandle>)>;

    static const Milliseconds kDefaultRefreshTimeout;
    static const Milliseconds kDefaultRefreshRequirement;
    static const Milliseconds kDefaultHostTimeout;

    static const Status kConnectionStateUnknown;

    struct Options {
        Options() {}

        /**
         * The minimum number of connections to keep alive while the pool is in
         * operation
         */
        size_t minConnections = 1;

        /**
         * The maximum number of connections to spawn for a host. This includes
         * pending connections in setup and connections checked out of the pool
         * as well as the obvious live connections in the pool.
         */
        size_t maxConnections = std::numeric_limits<size_t>::max();

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
    };

    explicit ConnectionPool(std::unique_ptr<DependentTypeFactoryInterface> impl,
                            Options options = Options{});

    ~ConnectionPool();

    void dropConnections(const HostAndPort& hostAndPort);

    void get(const HostAndPort& hostAndPort, Milliseconds timeout, GetConnectionCallback cb);

    void appendConnectionStats(ConnectionPoolStats* stats) const;

private:
    void returnConnection(ConnectionInterface* connection);

    // Options are set at startup and never changed at run time, so these are
    // accessed outside the lock
    const Options _options;

    const std::unique_ptr<DependentTypeFactoryInterface> _factory;

    // The global mutex for specific pool access and the generation counter
    mutable stdx::mutex _mutex;
    std::unordered_map<HostAndPort, std::unique_ptr<SpecificPool>> _pools;
};

class ConnectionPool::ConnectionHandleDeleter {
public:
    ConnectionHandleDeleter() = default;
    ConnectionHandleDeleter(ConnectionPool* pool) : _pool(pool) {}

    void operator()(ConnectionInterface* connection) {
        if (_pool && connection)
            _pool->returnConnection(connection);
    }

private:
    ConnectionPool* _pool = nullptr;
};

/**
 * Interface for a basic timer
 *
 * Minimal interface sets a timer with a callback and cancels the timer.
 */
class ConnectionPool::TimerInterface {
    MONGO_DISALLOW_COPYING(TimerInterface);

public:
    TimerInterface() = default;

    using TimeoutCallback = stdx::function<void()>;

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
};

/**
 * Interface for connection pool connections
 *
 * Provides a minimal interface to manipulate connections within the pool,
 * specifically callbacks to set them up (connect + auth + whatever else),
 * refresh them (issue some kind of ping) and manage a timer.
 */
class ConnectionPool::ConnectionInterface : public TimerInterface {
    MONGO_DISALLOW_COPYING(ConnectionInterface);

    friend class ConnectionPool;

public:
    ConnectionInterface() = default;

    virtual ~ConnectionInterface() = default;

    /**
     * Indicates that the user is now done with this connection. Users MUST call either
     * this method or indicateFailure() before returning the connection to its pool.
     */
    virtual void indicateSuccess() = 0;

    /**
     * Indicates that a connection has failed. This will prevent the connection
     * from re-entering the connection pool. Users MUST call either this method or
     * indicateSuccess() before returning connections to the pool.
     */
    virtual void indicateFailure(Status status) = 0;

    /**
     * The HostAndPort for the connection. This should be the same as the
     * HostAndPort passed to DependentTypeFactoryInterface::makeConnection.
     */
    virtual const HostAndPort& getHostAndPort() const = 0;

    /**
     * Check if the connection is healthy using some implementation defined condition.
     */
    virtual bool isHealthy() = 0;

protected:
    /**
     * Making these protected makes the definitions available to override in
     * children.
     */
    using SetupCallback = stdx::function<void(ConnectionInterface*, Status)>;
    using RefreshCallback = stdx::function<void(ConnectionInterface*, Status)>;

private:
    /**
     * This method updates a 'liveness' timestamp to avoid unnecessarily refreshing
     * the connection.
     */
    virtual void indicateUsed() = 0;

    /**
     * Returns the last used time point for the connection
     */
    virtual Date_t getLastUsed() const = 0;

    /**
     * Returns the status associated with the connection. If the status is not
     * OK, the connection will not be returned to the pool.
     */
    virtual const Status& getStatus() const = 0;

    /**
     * Sets up the connection. This should include connection + auth + any
     * other associated hooks.
     */
    virtual void setup(Milliseconds timeout, SetupCallback cb) = 0;

    /**
     * Resets the connection's state to kConnectionStateUnknown for the next user.
     */
    virtual void resetToUnknown() = 0;

    /**
     * Refreshes the connection. This should involve a network round trip and
     * should strongly imply an active connection
     */
    virtual void refresh(Milliseconds timeout, RefreshCallback cb) = 0;

    /**
     * Get the generation of the connection. This is used to track whether to
     * continue using a connection after a call to dropConnections() by noting
     * if the generation on the specific pool is the same as the generation on
     * a connection (if not the connection is from a previous era and should
     * not be re-used).
     */
    virtual size_t getGeneration() const = 0;
};

/**
 * Implementation interface for the connection pool
 *
 * This factory provides generators for connections, timers and a clock for the
 * connection pool.
 */
class ConnectionPool::DependentTypeFactoryInterface {
    MONGO_DISALLOW_COPYING(DependentTypeFactoryInterface);

public:
    DependentTypeFactoryInterface() = default;

    virtual ~DependentTypeFactoryInterface() = default;

    /**
     * Makes a new connection given a host and port
     */
    virtual std::unique_ptr<ConnectionInterface> makeConnection(const HostAndPort& hostAndPort,
                                                                size_t generation) = 0;

    /**
     * Makes a new timer
     */
    virtual std::unique_ptr<TimerInterface> makeTimer() = 0;

    /**
     * Returns the current time point
     */
    virtual Date_t now() = 0;
};

}  // namespace executor
}  // namespace mongo
