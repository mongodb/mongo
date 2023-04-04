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

// _ todo: reconnect?


#include "mongo/platform/basic.h"

#include "mongo/client/connpool.h"

#include <limits>
#include <string>

#include "mongo/base/init.h"
#include "mongo/client/connection_string.h"
#include "mongo/client/dbclient_connection.h"
#include "mongo/client/global_conn_pool.h"
#include "mongo/client/replica_set_monitor.h"
#include "mongo/config.h"
#include "mongo/db/server_feature_flags_gen.h"
#include "mongo/executor/connection_pool_stats.h"
#include "mongo/logv2/log.h"
#include "mongo/stdx/chrono.h"
#include "mongo/util/exit.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/net/socket_exception.h"

#if !defined(__has_feature)
#define __has_feature(x) 0
#endif

#if __has_feature(address_sanitizer)
#include <sanitizer/lsan_interface.h>
#endif

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kNetwork


namespace mongo {

namespace {
const int kDefaultIdleTimeout = std::numeric_limits<int>::max();
const int kDefaultMaxInUse = std::numeric_limits<int>::max();

auto makeDuration(double secs) {
    return Milliseconds(static_cast<Milliseconds::rep>(1000 * secs));
}

void recordWaitTime(PoolForHost& p, DBClientBase* conn, Date_t connRequestedAt) {
    // (Ignore FCV check): This feature flag doesn't have any upgrade/downgrade concerns.
    if (gFeatureFlagConnHealthMetrics.isEnabledAndIgnoreFCVUnsafe() && conn) {
        p.recordConnectionWaitTime(connRequestedAt);
    }
}
}  // namespace

MONGO_FAIL_POINT_DEFINE(injectWaitTimeForConnpoolAcquisition);

using std::endl;
using std::list;
using std::map;
using std::set;
using std::string;
using std::vector;

// ------ PoolForHost ------

PoolForHost::PoolForHost()
    : _created(0),
      _minValidCreationTimeMicroSec(0),
      _type(ConnectionString::ConnectionType::kInvalid),
      _maxPoolSize(kPoolSizeUnlimited),
      _maxInUse(kDefaultMaxInUse),
      _checkedOut(0),
      _badConns(0),
      _parentDestroyed(false),
      _inShutdown(false) {}

PoolForHost::~PoolForHost() {
    clear();
}

void PoolForHost::clear() {
    if (!_parentDestroyed) {
        LOGV2(24124,
              "Dropping all pooled connections to {connString} "
              "(with timeout of {timeoutSecs} seconds)",
              "Dropping all pooled connections to a host",
              "connString"_attr = _hostName,
              "socketTimeout"_attr = makeDuration(_socketTimeoutSecs));
    }

    _pool = decltype(_pool){};
}

auto PoolForHost::done(DBConnectionPool* pool, DBClientBase* c) -> ConnectionHealth {
    const bool isFailed = c->isFailed();

    --_checkedOut;

    // Remember that this host had a broken connection for later
    if (isFailed) {
        reportBadConnectionAt(c->getSockCreationMicroSec());
    }

    // Another (later) connection was reported as broken to this host
    bool isBroken = c->getSockCreationMicroSec() < _minValidCreationTimeMicroSec;
    if (isFailed || isBroken) {
        _badConns++;
        LOGV2(24125,
              "Ending connection to {connString} (with timeout of {socketTimeout}) "
              "due to bad connection status; {numOpenConns} connections to that host remain open",
              "Ending connection to a host due to a bad connection status",
              "connString"_attr = _hostName,
              "socketTimeout"_attr = makeDuration(_socketTimeoutSecs),
              "numOpenConns"_attr = openConnections());
        return ConnectionHealth::kFailed;
    } else if (_maxPoolSize >= 0 && static_cast<int>(_pool.size()) >= _maxPoolSize) {
        // We have a pool size that we need to enforce
        LOGV2(24126,
              "Ending idle connection to {connString} (with timeout of {socketTimeout}) "
              "because its pool meets constraints; "
              "{numOpenConns} connections to that host remain open",
              "Ending idle connection to a host because its pool mees constraints",
              "connString"_attr = _hostName,
              "socketTimeout"_attr = makeDuration(_socketTimeoutSecs),
              "numOpenConns"_attr = openConnections());
        return ConnectionHealth::kTooMany;
#ifdef MONGO_CONFIG_SSL
    } else if (c->isUsingTransientSSLParams()) {
        LOGV2(53064,
              "Ending idle connection to a host because it was authenticated with transient SSL "
              "params",
              "connString"_attr = _hostName,
              "socketTimeout"_attr = makeDuration(_socketTimeoutSecs),
              "numOpenConns"_attr = openConnections());
        return ConnectionHealth::kFailed;
#endif
    }

    // The connection is probably fine, save for later
    _pool.push(std::unique_ptr<DBClientBase>(c));
    return ConnectionHealth::kReuseable;
}

void PoolForHost::reportBadConnectionAt(uint64_t microSec) {
    if (microSec != DBClientBase::INVALID_SOCK_CREATION_TIME &&
        microSec > _minValidCreationTimeMicroSec) {
        _minValidCreationTimeMicroSec = microSec;
        LOGV2(24127,
              "Detected bad connection created at {currentTime}, "
              "clearing pool for {connString} of {numOpenConns} connections",
              "Detected bad connection, clearing pool for host",
              "currentTime"_attr =
                  Microseconds(static_cast<Microseconds::rep>(_minValidCreationTimeMicroSec)),
              "connString"_attr = _hostName,
              "numOpenConns"_attr = openConnections());
        clear();
    }
}

bool PoolForHost::isBadSocketCreationTime(uint64_t microSec) {
    return microSec != DBClientBase::INVALID_SOCK_CREATION_TIME &&
        microSec <= _minValidCreationTimeMicroSec;
}

DBClientBase* PoolForHost::get(DBConnectionPool* pool, double socketTimeout) {
    while (!_pool.empty()) {
        auto sc = std::move(_pool.top());
        _pool.pop();

        if (!sc.ok()) {
            _badConns++;
            pool->onDestroy(sc.conn.get());
            continue;
        }

        verify(sc.conn->getSoTimeout() == socketTimeout);
#ifdef MONGO_CONFIG_SSL
        invariant(!sc.conn->isUsingTransientSSLParams());
#endif

        ++_checkedOut;
        return sc.conn.release();
    }

    return nullptr;
}

void PoolForHost::flush() {
    clear();
}

void PoolForHost::getStaleConnections(Date_t idleThreshold, vector<DBClientBase*>& stale) {
    vector<StoredConnection> all;
    while (!_pool.empty()) {
        StoredConnection c = std::move(_pool.top());
        _pool.pop();

        if (c.ok() && !c.addedBefore(idleThreshold)) {
            all.push_back(std::move(c));
        } else {
            _badConns++;
            stale.emplace_back(c.conn.release());
        }
    }

    for (auto& conn : all) {
        _pool.push(std::move(conn));
    }
}


PoolForHost::StoredConnection::StoredConnection(std::unique_ptr<DBClientBase> c)
    : conn(std::move(c)), added(Date_t::now()) {}

bool PoolForHost::StoredConnection::ok() {
    // Poke the connection to see if we're still ok
    return conn->isStillConnected();
}

bool PoolForHost::StoredConnection::addedBefore(Date_t time) {
    return added < time;
}

void PoolForHost::createdOne(DBClientBase* base) {
    if (_created == 0)
        _type = base->type();
    ++_created;
    // _checkedOut is used to indicate the number of in-use connections so
    // though we didn't actually check this connection out, we bump it here.
    ++_checkedOut;
}

void PoolForHost::initializeHostName(const std::string& hostName) {
    if (_hostName.empty()) {
        _hostName = hostName;
    }
}

void PoolForHost::waitForFreeConnection(int timeout, stdx::unique_lock<Latch>& lk) {
    auto condition = [&] {
        return (numInUse() < _maxInUse || _inShutdown.load());
    };

    if (timeout > 0) {
        stdx::chrono::seconds timeoutSeconds{timeout};

        // If we timed out waiting without getting a new connection, throw.
        uassert(ErrorCodes::ExceededTimeLimit,
                str::stream() << "too many connections to " << _hostName << ":" << timeout,
                !_cv.wait_for(lk, timeoutSeconds, condition));
    } else {
        _cv.wait(lk, condition);
    }
}

void PoolForHost::notifyWaiters() {
    _cv.notify_one();
}

void PoolForHost::shutdown() {
    _inShutdown.store(true);
    _cv.notify_all();
}

// ------ DBConnectionPool::Detail ------

class DBConnectionPool::Detail {
public:
    template <typename Connect>
    static DBClientBase* get(DBConnectionPool* _this,
                             const std::string& host,
                             double timeout,
                             Connect connect) {
        auto connRequestedAt = Date_t::now();
        if (MONGO_unlikely(injectWaitTimeForConnpoolAcquisition.shouldFail())) {
            injectWaitTimeForConnpoolAcquisition.execute([&](const BSONObj& data) {
                sleepFor(Milliseconds(data["sleepTimeMillis"].numberInt()));
            });
        }

        while (!(_this->_inShutdown.load())) {
            // Get a connection from the pool, if there is one.
            std::unique_ptr<DBClientBase> c(_this->_get(host, timeout, connRequestedAt));
            if (c) {
                // This call may throw.
                _this->onHandedOut(c.get());
                return c.release();
            }

            // If there are no pooled connections for this host, create a new connection. If
            // there are too many connections in this pool to make a new one, block until a
            // connection is released.
            {
                stdx::unique_lock<Latch> lk(_this->_mutex);
                PoolForHost& p = _this->_pools[PoolKey(host, timeout)];

                if (p.openConnections() >= _this->_maxInUse) {
                    LOGV2(20112,
                          "Too many in-use connections; "
                          "waiting until there are fewer than {maxInUseConns}",
                          "Too many in-use connections; waiting until there are fewer than maximum",
                          "maxInUseConns"_attr = _this->_maxInUse);
                    p.waitForFreeConnection(timeout, lk);
                } else {
                    // Drop the lock here, so we can connect without holding it.
                    // _finishCreate will take the lock again.
                    lk.unlock();

                    // Create a new connection and return. All Connect functions
                    // should throw if they cannot create a connection.
                    auto c = connect();
                    invariant(c);
                    auto conn = _this->_finishCreate(host, timeout, c, connRequestedAt);
                    return conn;
                }
            }
        }

        // If we get here, we are in shutdown, and it does not matter what we return.
        invariant(_this->_inShutdown.load());
        uassert(ErrorCodes::ShutdownInProgress, "connection pool is in shutdown", false);
        MONGO_UNREACHABLE;
    }
};

// ------ DBConnectionPool ------

const int PoolForHost::kPoolSizeUnlimited(-1);

DBConnectionPool::DBConnectionPool()
    : _name("dbconnectionpool"),
      _maxPoolSize(PoolForHost::kPoolSizeUnlimited),
      _maxInUse(kDefaultMaxInUse),
      _idleTimeout(kDefaultIdleTimeout),
      _inShutdown(false),
      _hooks(new list<DBConnectionHook*>())

{}

void DBConnectionPool::shutdown() {
    if (!_inShutdown.swap(true)) {
        stdx::lock_guard<Latch> L(_mutex);
        for (auto i = _pools.begin(); i != _pools.end(); i++) {
            PoolForHost& p = i->second;
            p.shutdown();
        }
    }
}

DBClientBase* DBConnectionPool::_get(const string& ident,
                                     double socketTimeout,
                                     Date_t& connRequestedAt) {
    uassert(ErrorCodes::ShutdownInProgress,
            "Can't use connection pool during shutdown",
            !globalInShutdownDeprecated());
    stdx::lock_guard<Latch> L(_mutex);
    PoolForHost& p = _pools[PoolKey(ident, socketTimeout)];
    p.setMaxPoolSize(_maxPoolSize);
    p.setSocketTimeout(socketTimeout);
    p.initializeHostName(ident);
    auto c = p.get(this, socketTimeout);
    recordWaitTime(p, c, connRequestedAt);
    return c;
}

int DBConnectionPool::openConnections(const string& ident, double socketTimeout) {
    stdx::lock_guard<Latch> L(_mutex);
    PoolForHost& p = _pools[PoolKey(ident, socketTimeout)];
    return p.openConnections();
}

DBClientBase* DBConnectionPool::_finishCreate(const string& ident,
                                              double socketTimeout,
                                              DBClientBase* conn,
                                              Date_t& connRequestedAt) {
    {
        stdx::lock_guard<Latch> L(_mutex);
        PoolForHost& p = _pools[PoolKey(ident, socketTimeout)];
        p.setMaxPoolSize(_maxPoolSize);
        p.initializeHostName(ident);
        p.createdOne(conn);
        recordWaitTime(p, conn, connRequestedAt);
    }

    try {
        onCreate(conn);
        onHandedOut(conn);
    } catch (std::exception&) {
        delete conn;
        throw;
    }

    LOGV2(20113,
          "Successfully connected to {connString} "
          "({numOpenConns} connections now open to that host "
          "with a {socketTimeoutSecs} second timeout)",
          "Successfully connected to host",
          "connString"_attr = ident,
          "numOpenConns"_attr = openConnections(ident, socketTimeout),
          "socketTimeoutSecs"_attr = socketTimeout);

    return conn;
}

DBClientBase* DBConnectionPool::get(const ConnectionString& url, double socketTimeout) {
    auto connect = [&]() {
        auto c = url.connect(StringData(), socketTimeout);
        uassert(13328,
                fmt::format(
                    "{}: connect failed {} : {}", _name, url.toString(), c.getStatus().reason()),
                c.isOK());
        return c.getValue().release();
    };

    return Detail::get(this, url.toString(), socketTimeout, connect);
}

DBClientBase* DBConnectionPool::get(const string& host, double socketTimeout) {
    auto connect = [&] {
        const ConnectionString cs(uassertStatusOK(ConnectionString::parse(host)));

        auto swConn = cs.connect(StringData(), socketTimeout);
        if (!swConn.isOK()) {
            throwSocketError(SocketErrorKind::CONNECT_ERROR,
                             host,
                             fmt::format("{} error: {}", _name, swConn.getStatus().reason()));
        }

        return swConn.getValue().release();
    };

    return Detail::get(this, host, socketTimeout, connect);
}

DBClientBase* DBConnectionPool::get(const MongoURI& uri, double socketTimeout) {
    auto connect = [&] {
        string errmsg;
        std::unique_ptr<DBClientBase> c(
            uri.connect(uri.getAppName().value(), errmsg, socketTimeout));
        uassert(40356, fmt::format("{}: connect failed {} : {}", _name, uri.toString(), errmsg), c);
        return c.release();
    };

    return Detail::get(this, uri.toString(), socketTimeout, connect);
}

int DBConnectionPool::getNumAvailableConns(const string& host, double socketTimeout) const {
    stdx::lock_guard<Latch> L(_mutex);
    auto it = _pools.find(PoolKey(host, socketTimeout));
    return (it == _pools.end()) ? 0 : it->second.numAvailable();
}

int DBConnectionPool::getNumBadConns(const string& host, double socketTimeout) const {
    stdx::lock_guard<Latch> L(_mutex);
    auto it = _pools.find(PoolKey(host, socketTimeout));
    return (it == _pools.end()) ? 0 : it->second.getNumBadConns();
}

void DBConnectionPool::onRelease(DBClientBase* conn) {
    if (_hooks->empty()) {
        return;
    }

    for (list<DBConnectionHook*>::iterator i = _hooks->begin(); i != _hooks->end(); i++) {
        (*i)->onRelease(conn);
    }
}

void DBConnectionPool::release(const string& host, DBClientBase* c) {
    onRelease(c);

    PoolForHost* pool;
    PoolForHost::ConnectionHealth health;
    {
        // Grab our pool and update its accounting
        stdx::lock_guard lk(_mutex);
        pool = &_pools[PoolKey(host, c->getSoTimeout())];
        health = pool->done(this, c);
    }

    if (health != PoolForHost::ConnectionHealth::kReuseable) {
        // If the connection wasn't okay, destroy it
        onDestroy(c);
        delete c;
    }

    pool->notifyWaiters();
}

void DBConnectionPool::decrementEgress(const string& host, DBClientBase* c) {
    stdx::lock_guard L(_mutex);
    PoolForHost& p = _pools[PoolKey(host, c->getSoTimeout())];
    --p._checkedOut;
}

DBConnectionPool::~DBConnectionPool() {
    // Do not log in destruction, because global connection pools get
    // destroyed after the logging framework.
    stdx::lock_guard<Latch> L(_mutex);
    for (PoolMap::iterator i = _pools.begin(); i != _pools.end(); i++) {
        PoolForHost& p = i->second;
        p._parentDestroyed = true;
    }

#if __has_feature(address_sanitizer)
    __lsan_ignore_object(_hooks);
#endif
}

void DBConnectionPool::flush() {
    stdx::lock_guard<Latch> L(_mutex);
    for (PoolMap::iterator i = _pools.begin(); i != _pools.end(); i++) {
        PoolForHost& p = i->second;
        p.flush();
    }
}

void DBConnectionPool::clear() {
    stdx::lock_guard<Latch> L(_mutex);
    LOGV2_DEBUG(20114,
                2,
                "Removing connections on all pools owned by {poolName}",
                "Removing all connectionns associated with this set of pools",
                "poolName"_attr = _name);
    for (PoolMap::iterator iter = _pools.begin(); iter != _pools.end(); ++iter) {
        iter->second.clear();
    }
}

void DBConnectionPool::removeHost(const string& host) {
    stdx::lock_guard<Latch> L(_mutex);
    LOGV2_DEBUG(20115,
                2,
                "Removing connections from all pools to {connString}",
                "Removing connections from all pools to a host",
                "connString"_attr = host);
    for (PoolMap::iterator i = _pools.begin(); i != _pools.end(); ++i) {
        const string& poolHost = i->first.ident;
        if (!serverNameCompare()(host, poolHost) && !serverNameCompare()(poolHost, host)) {
            // hosts are the same
            i->second.clear();
        }
    }
}

void DBConnectionPool::addHook(DBConnectionHook* hook) {
    _hooks->push_back(hook);
}

void DBConnectionPool::onCreate(DBClientBase* conn) {
    if (_hooks->size() == 0)
        return;

    for (list<DBConnectionHook*>::iterator i = _hooks->begin(); i != _hooks->end(); i++) {
        (*i)->onCreate(conn);
    }
}

void DBConnectionPool::onHandedOut(DBClientBase* conn) {
    if (_hooks->size() == 0)
        return;

    for (list<DBConnectionHook*>::iterator i = _hooks->begin(); i != _hooks->end(); i++) {
        (*i)->onHandedOut(conn);
    }
}

void DBConnectionPool::onDestroy(DBClientBase* conn) {
    if (_hooks->size() == 0)
        return;

    for (list<DBConnectionHook*>::iterator i = _hooks->begin(); i != _hooks->end(); i++) {
        (*i)->onDestroy(conn);
    }
}

void DBConnectionPool::appendConnectionStats(executor::ConnectionPoolStats* stats) const {
    {
        stdx::lock_guard<Latch> lk(_mutex);
        for (PoolMap::const_iterator i = _pools.begin(); i != _pools.end(); ++i) {
            if (i->second.numCreated() == 0)
                continue;

            // Mongos may use either a replica set uri or a list of addresses as
            // the identifier here, so we always take the first server parsed out
            // as our label for connPoolStats. Note that these stats will collide
            // with any existing stats for the chosen host.
            auto uri = ConnectionString::parse(i->first.ident);
            invariant(uri.isOK());
            HostAndPort host = uri.getValue().getServers().front();

            executor::ConnectionStatsPer hostStats{static_cast<size_t>(i->second.numInUse()),
                                                   static_cast<size_t>(i->second.numAvailable()),
                                                   0,
                                                   static_cast<size_t>(i->second.numCreated()),
                                                   0,
                                                   0,
                                                   0,
                                                   0,
                                                   Milliseconds{0}};
            // (Ignore FCV check): This feature flag doesn't have any upgrade/downgrade concerns.
            if (gFeatureFlagConnHealthMetrics.isEnabledAndIgnoreFCVUnsafe()) {
                hostStats.acquisitionWaitTimes = i->second.connectionWaitTimeStats();
            }
            stats->updateStatsForHost("global", host, hostStats);
        }
    }
}

bool DBConnectionPool::serverNameCompare::operator()(const string& a, const string& b) const {
    const char* ap = a.c_str();
    const char* bp = b.c_str();

    while (true) {
        if (*ap == '\0' || *ap == '/') {
            if (*bp == '\0' || *bp == '/')
                return false;  // equal strings
            else
                return true;  // a is shorter
        }

        if (*bp == '\0' || *bp == '/')
            return false;  // b is shorter

        if (*ap < *bp)
            return true;
        else if (*ap > *bp)
            return false;

        ++ap;
        ++bp;
    }
    verify(false);
}

bool DBConnectionPool::poolKeyCompare::operator()(const PoolKey& a, const PoolKey& b) const {
    if (DBConnectionPool::serverNameCompare()(a.ident, b.ident))
        return true;

    if (DBConnectionPool::serverNameCompare()(b.ident, a.ident))
        return false;

    return a.timeout < b.timeout;
}

bool DBConnectionPool::isConnectionGood(const string& hostName, DBClientBase* conn) {
    if (conn == nullptr) {
        return false;
    }

    if (conn->isFailed()) {
        return false;
    }

    {
        stdx::lock_guard<Latch> sl(_mutex);
        PoolForHost& pool = _pools[PoolKey(hostName, conn->getSoTimeout())];
        if (pool.isBadSocketCreationTime(conn->getSockCreationMicroSec())) {
            return false;
        }
    }

    return true;
}

void DBConnectionPool::taskDoWork() {
    vector<DBClientBase*> toDelete;
    auto idleThreshold = Date_t::now() - _idleTimeout;
    {
        // we need to get the connections inside the lock
        // but we can actually delete them outside
        stdx::lock_guard<Latch> lk(_mutex);
        for (PoolMap::iterator i = _pools.begin(); i != _pools.end(); ++i) {
            i->second.getStaleConnections(idleThreshold, toDelete);
        }
    }

    for (size_t i = 0; i < toDelete.size(); i++) {
        try {
            onDestroy(toDelete[i]);
            delete toDelete[i];
        } catch (...) {
            // we don't care if there was a socket error
        }
    }
}

// ------ ScopedDbConnection ------

ScopedDbConnection::ScopedDbConnection(const std::string& host, double socketTimeout)
    : _host(host),
      _conn(globalConnPool.get(host, socketTimeout)),
      _socketTimeoutSecs(socketTimeout) {
    _setSocketTimeout();
}

ScopedDbConnection::ScopedDbConnection(const ConnectionString& host, double socketTimeout)
    : _host(host.toString()),
      _conn(globalConnPool.get(host, socketTimeout)),
      _socketTimeoutSecs(socketTimeout) {
    _setSocketTimeout();
}

ScopedDbConnection::ScopedDbConnection(const MongoURI& uri, double socketTimeout)
    : _host(uri.toString()),
      _conn(globalConnPool.get(uri, socketTimeout)),
      _socketTimeoutSecs(socketTimeout) {
    _setSocketTimeout();
}

void ScopedDbConnection::done() {
    if (!_conn) {
        return;
    }

    globalConnPool.release(_host, _conn);
    _conn = nullptr;
}

void ScopedDbConnection::kill() {
    globalConnPool.decrementEgress(_host, _conn);
    delete _conn;
    _conn = nullptr;
}

void ScopedDbConnection::_setSocketTimeout() {
    if (!_conn)
        return;

    if (_conn->type() == ConnectionString::ConnectionType::kStandalone)
        static_cast<DBClientConnection*>(_conn)->setSoTimeout(_socketTimeoutSecs);
}

ScopedDbConnection::~ScopedDbConnection() {
    if (_conn) {
        if (_conn->isFailed()) {
            if (_conn->getSockCreationMicroSec() == DBClientBase::INVALID_SOCK_CREATION_TIME) {
                kill();
            } else {
                // The pool takes care of deleting the failed connection - this
                // will also trigger disposal of older connections in the pool
                done();
            }
        } else {
            /* see done() comments above for why we log this line */
            LOGV2(24128,
                  "Scoped connection to {connString} not being returned to the pool",
                  "Scoped connection not being returned to the pool",
                  "connString"_attr = _conn->getServerAddress());
            kill();
        }
    }
}

void ScopedDbConnection::clearPool() {
    globalConnPool.clear();
}

AtomicWord<int> AScopedConnection::_numConnections;

MONGO_INITIALIZER(SetupDBClientBaseWithConnection)(InitializerContext*) {
    DBClientBase::withConnection_do_not_use = [](std::string host,
                                                 std::function<void(DBClientBase*)> cb) {
        ScopedDbConnection conn(host);
        cb(conn.get());
        conn.done();
    };
}

}  // namespace mongo
