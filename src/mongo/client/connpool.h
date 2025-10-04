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

#include "mongo/client/connection_string.h"
#include "mongo/client/dbclient_base.h"
#include "mongo/client/mongo_uri.h"
#include "mongo/executor/connection_pool_stats.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/stdx/mutex.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/background.h"
#include "mongo/util/duration.h"
#include "mongo/util/hierarchical_acquisition.h"
#include "mongo/util/time_support.h"

#include <cstdint>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <stack>
#include <string>
#include <vector>

#include <boost/move/utility_core.hpp>

namespace mongo {

class BSONObjBuilder;

class DBConnectionPool;

namespace executor {
struct ConnectionPoolStats;
}  // namespace executor

/**
 * The PoolForHost is responsible for storing a maximum of _maxPoolSize connections to a particular
 * host. It is not responsible for creating new connections; instead, when DBConnectionPool is asked
 * for a connection to a particular host, DBConnectionPool will check if any connections are
 * available in the PoolForHost for that host. If so, DBConnectionPool will check out a connection
 * from PoolForHost, and if not, DBConnectionPool will create a new connection itself, if we are
 * below the maximum allowed number of connections. If we have already created _maxPoolSize
 * connections, the calling thread will block until a new connection can be made for it.
 *
 * Once the connection is released back to DBConnectionPool, DBConnectionPool will attempt to
 * release the connection to PoolForHost. This is how connections enter PoolForHost for the first
 * time. If PoolForHost is below the _maxPoolSize limit, PoolForHost will take ownership of the
 * connection, otherwise PoolForHost will clean up and destroy the connection.
 *
 * Additionally, PoolForHost knows how to purge itself of stale connections (since a connection can
 * go stale while it is just sitting in the pool), but does not decide when to do so. Instead,
 * DBConnectionPool tells PoolForHost to purge stale connections periodically.
 *
 * PoolForHost is not thread-safe; thread safety is handled by DBConnectionPool.
 */
class PoolForHost {
    PoolForHost(const PoolForHost&) = delete;
    PoolForHost& operator=(const PoolForHost&) = delete;

public:
    // Sentinel value indicating pool has no cleanup limit
    static const int kPoolSizeUnlimited;

    friend class DBConnectionPool;

    PoolForHost();
    ~PoolForHost();

    /**
     * Returns the number of connections in this pool that went bad.
     */
    int getNumBadConns() const {
        return _badConns;
    }

    /**
     * Returns the maximum number of connections stored in the pool
     */
    int getMaxPoolSize() const {
        return _maxPoolSize;
    }

    /**
     * Sets the maximum number of connections stored in the pool
     */
    void setMaxPoolSize(int maxPoolSize) {
        _maxPoolSize = maxPoolSize;
    }

    /**
     * Sets the maximum number of in-use connections for this pool.
     */
    void setMaxInUse(int maxInUse) {
        _maxInUse = maxInUse;
    }

    /**
     * Sets the socket timeout on this host, in seconds, for reporting purposes only.
     */
    void setSocketTimeout(double socketTimeout) {
        _socketTimeoutSecs = socketTimeout;
    }

    int numAvailable() const {
        return (int)_pool.size();
    }

    int numInUse() const {
        return _checkedOut;
    }

    /**
     * Returns the number of open connections in this pool.
     */
    int openConnections() const {
        return numInUse() + numAvailable();
    }

    void createdOne(DBClientBase* base);
    long long numCreated() const {
        return _created;
    }

    ConnectionString::ConnectionType type() const {
        MONGO_verify(_created);
        return _type;
    }

    /**
     * gets a connection or return NULL
     */
    DBClientBase* get(DBConnectionPool* pool, double socketTimeout);

    // Deletes all connections in the pool
    void clear();

    /**
     * A concrete statement about the health of a DBClientBase connection
     */
    enum class ConnectionHealth {
        kReuseable,
        kTooMany,
        kFailed,
    };

    /**
     * Attempt to reclaim the underlying connection behind the DBClientBase
     */
    ConnectionHealth done(DBConnectionPool* pool, DBClientBase* c);

    void flush();

    void getStaleConnections(Date_t idleThreshold, std::vector<DBClientBase*>& stale);

    /**
     * Sets the lower bound for creation times that can be considered as
     *     good connections.
     */
    void reportBadConnectionAt(uint64_t microSec);

    /**
     * @return true if the given creation time is considered to be not
     *     good for use.
     */
    bool isBadSocketCreationTime(uint64_t microSec);

    /**
     * Sets the host name to a new one, only if it is currently empty.
     */
    void initializeHostName(const std::string& hostName);

    /**
     * If this pool has more than _maxPoolSize connections in use, blocks
     * the calling thread until a connection is returned to the pool or
     * is destroyed. If a non-zero timeout is given, this method will
     * throw if a free connection cannot be acquired within that amount of
     * time. Timeout is in seconds.
     */
    void waitForFreeConnection(int timeout, stdx::unique_lock<stdx::mutex>& lk);

    /**
     * Notifies any waiters that there are new connections available.
     */
    void notifyWaiters();

    /**
     * Records the connection waittime in the connAcquisitionWaitTime histogram
     */
    inline void recordConnectionWaitTime(Date_t requestedAt) {
        auto connTime = Date_t::now() - requestedAt;
        _connAcquisitionWaitTimeStats.increment(connTime);
        _connTime = connTime;
    }

    /**
     * Returns the connAcquisitionWaitTime histogram
     */
    const executor::ConnectionWaitTimeHistogram& connectionWaitTimeStats() const {
        return _connAcquisitionWaitTimeStats;
    }

    /**
     * Shuts down this pool, notifying all waiters.
     */
    void shutdown();

private:
    struct StoredConnection {
        StoredConnection(std::unique_ptr<DBClientBase> c);

        bool ok();

        /**
         * Returns true if this connection was added before the given time.
         */
        bool addedBefore(Date_t time);

        std::unique_ptr<DBClientBase> conn;

        // The time when this connection was added to the pool. Will
        // be reset if the connection is checked out and re-added.
        Date_t added;
    };

    std::string _hostName;
    double _socketTimeoutSecs;
    std::stack<StoredConnection> _pool;

    int64_t _created;
    uint64_t _minValidCreationTimeMicroSec;
    ConnectionString::ConnectionType _type;

    // The maximum number of connections we'll save in the pool
    int _maxPoolSize;

    // The maximum number of connections allowed to be in-use in this pool
    int _maxInUse;

    // The number of currently active connections from this pool
    int _checkedOut;

    // The number of connections that we did not reuse because they went bad.
    int _badConns;

    // Whether our parent DBConnectionPool object is in destruction
    bool _parentDestroyed;

    // Time it took for the last connection to be established
    Milliseconds _connTime;

    executor::ConnectionWaitTimeHistogram _connAcquisitionWaitTimeStats{};

    stdx::condition_variable _cv;

    AtomicWord<bool> _inShutdown;
};

class DBConnectionHook {
public:
    virtual ~DBConnectionHook() {}
    virtual void onCreate(DBClientBase* conn) {}
    virtual void onHandedOut(DBClientBase* conn) {}
    virtual void onRelease(DBClientBase* conn) {}
    virtual void onDestroy(DBClientBase* conn) {}
};

/** Database connection pool.

    Generally, use ScopedDbConnection and do not call these directly.

    This class, so far, is suitable for use with unauthenticated connections.
    Support for authenticated connections requires some adjustments: please
    request...

    Usage:

    {
       ScopedDbConnection c("myserver");
       c.conn()...
    }
*/
class DBConnectionPool : public PeriodicTask {
public:
    DBConnectionPool();
    ~DBConnectionPool() override;

    /** right now just controls some asserts.  defaults to "dbconnectionpool" */
    void setName(const std::string& name) {
        _name = name;
    }

    /**
     * Returns the maximum number of connections pooled per-host
     *
     * This setting only applies to new host connection pools, previously-pooled host pools are
     * unaffected.
     */
    int getMaxPoolSize() const {
        return _maxPoolSize;
    }

    /**
     * Returns the number of connections to the given host pool.
     */
    int openConnections(const std::string& ident, double socketTimeout);

    /**
     * Sets the maximum number of connections pooled per-host.
     *
     * This setting only applies to new host connection pools, previously-pooled host pools are
     * unaffected.
     */
    void setMaxPoolSize(int maxPoolSize) {
        _maxPoolSize = maxPoolSize;
    }

    /**
     * Sets the maximum number of in-use connections per host.
     */
    void setMaxInUse(int maxInUse) {
        _maxInUse = maxInUse;
    }

    /**
     * Sets the timeout value for idle connections, after which we will remove them
     * from the pool. This value is in minutes.
     */
    void setIdleTimeout(int timeout) {
        _idleTimeout = Minutes(timeout);
    }

    void onCreate(DBClientBase* conn);
    void onHandedOut(DBClientBase* conn);
    void onDestroy(DBClientBase* conn);
    void onRelease(DBClientBase* conn);

    void flush();

    /**
     * Gets a connection to the given host with the given timeout, in seconds.
     */
    DBClientBase* get(const std::string& host, double socketTimeout = 0);
    DBClientBase* get(const ConnectionString& host, double socketTimeout = 0);
    DBClientBase* get(const MongoURI& uri, double socketTimeout = 0);

    /**
     * Gets the time it took for the last connection to be established from the PoolMap given a host
     * and timeout.
     */
    Milliseconds getPoolHostConnTime_forTest(const std::string& host, double timeout) const;

    /**
     * Gets the number of connections available in the pool.
     */
    int getNumAvailableConns(const std::string& host, double socketTimeout = 0) const;
    int getNumBadConns(const std::string& host, double socketTimeout = 0) const;

    void release(const std::string& host, DBClientBase* c);
    void decrementEgress(const std::string& host, DBClientBase* c);

    void addHook(DBConnectionHook* hook);  // we take ownership
    void appendConnectionStats(executor::ConnectionPoolStats* stats) const;

    /**
     * Clears all connections for all host.
     */
    void clear();

    /**
     * Checks whether the connection for a given host is deny listed or not.
     *
     * @param hostName the name of the host the connection connects to.
     * @param conn the connection to check.
     *
     * @return true if the connection is not bad, meaning, it is good to keep it for
     *     future use.
     */
    bool isConnectionGood(const std::string& host, DBClientBase* conn);

    // Removes and deletes all connections from the pool for the host (regardless of timeout)
    void removeHost(const std::string& host);

    /** compares server namees, but is smart about replica set names */
    struct serverNameCompare {
        bool operator()(const std::string& a, const std::string& b) const;
    };

    std::string taskName() const override {
        return "DBConnectionPool-cleaner";
    }
    void taskDoWork() override;

    /**
     * Shuts down the connection pool, unblocking any waiters on connections.
     */
    void shutdown();

private:
    class Detail;

    DBConnectionPool(DBConnectionPool& p);

    DBClientBase* _get(const std::string& ident, double socketTimeout, Date_t& connRequestedAt);

    DBClientBase* _finishCreate(const std::string& ident,
                                double socketTimeout,
                                DBClientBase* conn,
                                Date_t& connRequestedAt);

    struct PoolKey {
        PoolKey(const std::string& i, double t) : ident(i), timeout(t) {}
        std::string ident;
        double timeout;
    };

    struct poolKeyCompare {
        bool operator()(const PoolKey& a, const PoolKey& b) const;
    };

    typedef std::map<PoolKey, PoolForHost, poolKeyCompare> PoolMap;  // servername -> pool

    mutable stdx::mutex _mutex;
    std::string _name;

    // The maximum number of connections we'll save in the pool per-host
    // PoolForHost::kPoolSizeUnlimited is a sentinel value meaning "no limit"
    // 0 effectively disables the pool
    int _maxPoolSize;

    int _maxInUse;
    Minutes _idleTimeout;

    PoolMap _pools;

    AtomicWord<bool> _inShutdown;

    // pointers owned by me, right now they leak on shutdown
    // _hooks itself also leaks because it creates a shutdown race condition
    std::list<DBConnectionHook*>* _hooks;
};

class AScopedConnection {
    AScopedConnection(const AScopedConnection&) = delete;
    AScopedConnection& operator=(const AScopedConnection&) = delete;

public:
    AScopedConnection() {
        _numConnections.fetchAndAdd(1);
    }
    virtual ~AScopedConnection() {
        _numConnections.fetchAndAdd(-1);
    }

    virtual DBClientBase* get() = 0;
    virtual void done() = 0;
    virtual std::string getHost() const = 0;

    /**
     * @return true iff this has a connection to the db
     */
    virtual bool ok() const = 0;

    /**
     * @return total number of current instances of AScopedConnection
     */
    static int getNumConnections() {
        return _numConnections.load();
    }

private:
    static AtomicWord<int> _numConnections;
};

/** Use to get a connection from the pool.  On exceptions things
   clean up nicely (i.e. the socket gets closed automatically when the
   scopeddbconnection goes out of scope).
*/
class ScopedDbConnection : public AScopedConnection {
public:
    /** the main constructor you want to use
        throws AssertionException if can't connect
        */
    explicit ScopedDbConnection(const std::string& host, double socketTimeout = 0);
    explicit ScopedDbConnection(const ConnectionString& host, double socketTimeout = 0);
    explicit ScopedDbConnection(const MongoURI& host, double socketTimeout = 0);

    ScopedDbConnection() : _host(""), _conn(nullptr), _socketTimeoutSecs(0) {}

    /* @param conn - bind to an existing connection */
    ScopedDbConnection(const std::string& host, DBClientBase* conn, double socketTimeout = 0)
        : _host(host), _conn(conn), _socketTimeoutSecs(socketTimeout) {
        _setSocketTimeout();
    }

    ~ScopedDbConnection() override;

    static void clearPool();

    /** get the associated connection object */
    DBClientBase* operator->() {
        uassert(11004, "connection was returned to the pool already", _conn);
        return _conn;
    }

    /** get the associated connection object */
    DBClientBase& conn() {
        uassert(11005, "connection was returned to the pool already", _conn);
        return *_conn;
    }

    /** get the associated connection object */
    DBClientBase* get() override {
        uassert(13102, "connection was returned to the pool already", _conn);
        return _conn;
    }

    bool ok() const override {
        return _conn != nullptr;
    }

    std::string getHost() const override {
        return _host;
    }

    /** Force closure of the connection.  You should call this if you leave it in
        a bad state.  Destructor will do this too, but it is verbose.
    */
    void kill();

    /** Call this when you are done with the connection.

        If you do not call done() before this object goes out of scope,
        we can't be sure we fully read all expected data of a reply on the socket.  so
        we don't try to reuse the connection in that situation.
    */
    void done() override;

private:
    void _setSocketTimeout();

    const std::string _host;
    DBClientBase* _conn;
    const double _socketTimeoutSecs;
};

}  // namespace mongo
