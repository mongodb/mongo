/** @file connpool.h */

/*    Copyright 2009 10gen Inc.
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
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#pragma once

#include <cstdint>
#include <stack>

#include "mongo/client/dbclientinterface.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/util/background.h"
#include "mongo/util/concurrency/mutex.h"

namespace mongo {

class BSONObjBuilder;
class DBConnectionPool;

namespace executor {
struct ConnectionPoolStats;
}  // namespace executor

/**
 * not thread safe
 * thread safety is handled by DBConnectionPool
 */
class PoolForHost {
public:
    // Sentinel value indicating pool has no cleanup limit
    static const int kPoolSizeUnlimited;

    PoolForHost()
        : _created(0),
          _minValidCreationTimeMicroSec(0),
          _type(ConnectionString::INVALID),
          _maxPoolSize(kPoolSizeUnlimited),
          _checkedOut(0) {}

    PoolForHost(const PoolForHost& other)
        : _created(other._created),
          _minValidCreationTimeMicroSec(other._minValidCreationTimeMicroSec),
          _type(other._type),
          _maxPoolSize(other._maxPoolSize),
          _checkedOut(other._checkedOut) {
        verify(_created == 0);
        verify(other._pool.size() == 0);
    }

    ~PoolForHost();

    /**
     * Returns the maximum number of connections stored in the pool
     */
    int getMaxPoolSize() {
        return _maxPoolSize;
    }

    /**
     * Sets the maximum number of connections stored in the pool
     */
    void setMaxPoolSize(int maxPoolSize) {
        _maxPoolSize = maxPoolSize;
    }

    int numAvailable() const {
        return (int)_pool.size();
    }

    int numInUse() const {
        return _checkedOut;
    }

    void createdOne(DBClientBase* base);
    long long numCreated() const {
        return _created;
    }

    ConnectionString::ConnectionType type() const {
        verify(_created);
        return _type;
    }

    /**
     * gets a connection or return NULL
     */
    DBClientBase* get(DBConnectionPool* pool, double socketTimeout);

    // Deletes all connections in the pool
    void clear();

    void done(DBConnectionPool* pool, DBClientBase* c);

    void flush();

    void getStaleConnections(std::vector<DBClientBase*>& stale);

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

private:
    struct StoredConnection {
        StoredConnection(DBClientBase* c);

        bool ok(time_t now);

        DBClientBase* conn;
        time_t when;
    };

    std::string _hostName;
    std::stack<StoredConnection> _pool;

    int64_t _created;
    uint64_t _minValidCreationTimeMicroSec;
    ConnectionString::ConnectionType _type;

    // The maximum number of connections we'll save in the pool
    int _maxPoolSize;

    // The number of currently active connections from this pool
    int _checkedOut;
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
    ~DBConnectionPool();

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
    int getMaxPoolSize() {
        return _maxPoolSize;
    }

    /**
     * Sets the maximum number of connections pooled per-host.
     *
     * This setting only applies to new host connection pools, previously-pooled host pools are
     * unaffected.
     */
    void setMaxPoolSize(int maxPoolSize) {
        _maxPoolSize = maxPoolSize;
    }

    void onCreate(DBClientBase* conn);
    void onHandedOut(DBClientBase* conn);
    void onDestroy(DBClientBase* conn);
    void onRelease(DBClientBase* conn);

    void flush();

    DBClientBase* get(const std::string& host, double socketTimeout = 0);
    DBClientBase* get(const ConnectionString& host, double socketTimeout = 0);

    void release(const std::string& host, DBClientBase* c);

    void addHook(DBConnectionHook* hook);  // we take ownership
    void appendConnectionStats(executor::ConnectionPoolStats* stats) const;

    /**
     * Clears all connections for all host.
     */
    void clear();

    /**
     * Checks whether the connection for a given host is black listed or not.
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

    virtual std::string taskName() const {
        return "DBConnectionPool-cleaner";
    }
    virtual void taskDoWork();

private:
    DBConnectionPool(DBConnectionPool& p);

    DBClientBase* _get(const std::string& ident, double socketTimeout);

    DBClientBase* _finishCreate(const std::string& ident, double socketTimeout, DBClientBase* conn);

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

    PoolMap _pools;

    // pointers owned by me, right now they leak on shutdown
    // _hooks itself also leaks because it creates a shutdown race condition
    std::list<DBConnectionHook*>* _hooks;
};

class AScopedConnection {
    MONGO_DISALLOW_COPYING(AScopedConnection);

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
    static AtomicInt32 _numConnections;
};

/** Use to get a connection from the pool.  On exceptions things
   clean up nicely (i.e. the socket gets closed automatically when the
   scopeddbconnection goes out of scope).
*/
class ScopedDbConnection : public AScopedConnection {
public:
    /** the main constructor you want to use
        throws UserException if can't connect
        */
    explicit ScopedDbConnection(const std::string& host, double socketTimeout = 0);
    explicit ScopedDbConnection(const ConnectionString& host, double socketTimeout = 0);

    ScopedDbConnection() : _host(""), _conn(0), _socketTimeout(0) {}

    /* @param conn - bind to an existing connection */
    ScopedDbConnection(const std::string& host, DBClientBase* conn, double socketTimeout = 0)
        : _host(host), _conn(conn), _socketTimeout(socketTimeout) {
        _setSocketTimeout();
    }

    ~ScopedDbConnection();

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
    DBClientBase* get() {
        uassert(13102, "connection was returned to the pool already", _conn);
        return _conn;
    }

    bool ok() const {
        return _conn != NULL;
    }

    std::string getHost() const {
        return _host;
    }

    /** Force closure of the connection.  You should call this if you leave it in
        a bad state.  Destructor will do this too, but it is verbose.
    */
    void kill() {
        delete _conn;
        _conn = 0;
    }

    /** Call this when you are done with the connection.

        If you do not call done() before this object goes out of scope,
        we can't be sure we fully read all expected data of a reply on the socket.  so
        we don't try to reuse the connection in that situation.
    */
    void done();

private:
    void _setSocketTimeout();

    const std::string _host;
    DBClientBase* _conn;
    const double _socketTimeout;
};

}  // namespace mongo
