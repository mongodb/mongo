/* connpool.cpp
*/

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

// _ todo: reconnect?

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kNetwork

#include "mongo/platform/basic.h"

#include "mongo/client/connpool.h"

#include <string>

#include "mongo/client/connection_string.h"
#include "mongo/client/global_conn_pool.h"
#include "mongo/client/replica_set_monitor.h"
#include "mongo/executor/connection_pool_stats.h"
#include "mongo/util/exit.h"
#include "mongo/util/log.h"
#include "mongo/util/net/socket_exception.h"

namespace mongo {

using std::endl;
using std::list;
using std::map;
using std::set;
using std::string;
using std::vector;

// ------ PoolForHost ------

PoolForHost::~PoolForHost() {
    clear();
}

void PoolForHost::clear() {
    if (!_parentDestroyed) {
        logNoCache() << "Dropping all pooled connections to " << _hostName << "(with timeout of "
                     << _socketTimeout << " seconds)";
    }

    _pool = decltype(_pool){};
}

void PoolForHost::done(DBConnectionPool* pool, DBClientBase* c_raw) {
    std::unique_ptr<DBClientBase> c{c_raw};
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
        logNoCache() << "Ending connection to host " << _hostName << "(with timeout of "
                     << _socketTimeout << " seconds)"
                     << " due to bad connection status; " << openConnections()
                     << " connections to that host remain open";
        pool->onDestroy(c.get());
    } else if (_maxPoolSize >= 0 && static_cast<int>(_pool.size()) >= _maxPoolSize) {
        // We have a pool size that we need to enforce
        logNoCache() << "Ending idle connection to host " << _hostName << "(with timeout of "
                     << _socketTimeout << " seconds)"
                     << " because the pool meets constraints; " << openConnections()
                     << " connections to that host remain open";
        pool->onDestroy(c.get());
    } else {
        // The connection is probably fine, save for later
        _pool.push(std::move(c));
    }
}

void PoolForHost::reportBadConnectionAt(uint64_t microSec) {
    if (microSec != DBClientBase::INVALID_SOCK_CREATION_TIME &&
        microSec > _minValidCreationTimeMicroSec) {
        _minValidCreationTimeMicroSec = microSec;
        logNoCache() << "Detected bad connection created at " << _minValidCreationTimeMicroSec
                     << " microSec, clearing pool for " << _hostName << " of " << openConnections()
                     << " connections" << endl;
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

        ++_checkedOut;
        return sc.conn.release();
    }

    return nullptr;
}

void PoolForHost::flush() {
    clear();
}

void PoolForHost::getStaleConnections(vector<DBClientBase*>& stale) {
    vector<StoredConnection> all;
    while (!_pool.empty()) {
        StoredConnection c = std::move(_pool.top());
        _pool.pop();

        if (c.ok()) {
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
    : conn(std::move(c)), when(time(nullptr)) {}

bool PoolForHost::StoredConnection::ok() {
    // Poke the connection to see if we're still ok
    return conn->isStillConnected();
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

// ------ DBConnectionPool ------

const int PoolForHost::kPoolSizeUnlimited(-1);

DBConnectionPool::DBConnectionPool()
    : _name("dbconnectionpool"),
      _maxPoolSize(PoolForHost::kPoolSizeUnlimited),
      _hooks(new list<DBConnectionHook*>()) {}

DBClientBase* DBConnectionPool::_get(const string& ident, double socketTimeout) {
    uassert(17382, "Can't use connection pool during shutdown", !globalInShutdownDeprecated());
    stdx::lock_guard<stdx::mutex> L(_mutex);
    PoolForHost& p = _pools[PoolKey(ident, socketTimeout)];
    p.setMaxPoolSize(_maxPoolSize);
    p.setSocketTimeout(socketTimeout);
    p.initializeHostName(ident);
    return p.get(this, socketTimeout);
}

int DBConnectionPool::openConnections(const string& ident, double socketTimeout) {
    stdx::lock_guard<stdx::mutex> L(_mutex);
    PoolForHost& p = _pools[PoolKey(ident, socketTimeout)];
    return p.openConnections();
}

DBClientBase* DBConnectionPool::_finishCreate(const string& ident,
                                              double socketTimeout,
                                              DBClientBase* conn) {
    {
        stdx::lock_guard<stdx::mutex> L(_mutex);
        PoolForHost& p = _pools[PoolKey(ident, socketTimeout)];
        p.setMaxPoolSize(_maxPoolSize);
        p.initializeHostName(ident);
        p.createdOne(conn);
    }

    try {
        onCreate(conn);
        onHandedOut(conn);
    } catch (std::exception&) {
        delete conn;
        throw;
    }

    log() << "Successfully connected to " << ident << " (" << openConnections(ident, socketTimeout)
          << " connections now open to " << ident << " with a " << socketTimeout
          << " second timeout)";

    return conn;
}

DBClientBase* DBConnectionPool::get(const ConnectionString& url, double socketTimeout) {
    // If a connection for this host is available from the underlying PoolForHost, use the
    // connection in the pool.
    DBClientBase* c = _get(url.toString(), socketTimeout);
    if (c) {
        try {
            onHandedOut(c);
        } catch (std::exception&) {
            delete c;
            throw;
        }
        return c;
    }

    // If no connections for this host are available in the PoolForHost (that is, all the
    // connections have been checked out, or none have been created yet), create a new connection.
    string errmsg;
    c = url.connect(StringData(), errmsg, socketTimeout);
    uassert(13328, _name + ": connect failed " + url.toString() + " : " + errmsg, c);

    return _finishCreate(url.toString(), socketTimeout, c);
}

DBClientBase* DBConnectionPool::get(const string& host, double socketTimeout) {
    DBClientBase* c = _get(host, socketTimeout);
    if (c) {
        try {
            onHandedOut(c);
        } catch (std::exception&) {
            delete c;
            throw;
        }
        return c;
    }

    const ConnectionString cs(uassertStatusOK(ConnectionString::parse(host)));

    string errmsg;
    c = cs.connect(StringData(), errmsg, socketTimeout);
    if (!c)
        throw SocketException(SocketException::CONNECT_ERROR,
                              host,
                              11002,
                              str::stream() << _name << " error: " << errmsg);

    return _finishCreate(host, socketTimeout, c);
}

DBClientBase* DBConnectionPool::get(const MongoURI& uri, double socketTimeout) {
    std::unique_ptr<DBClientBase> c(_get(uri.toString(), socketTimeout));
    if (c) {
        onHandedOut(c.get());
        return c.release();
    }

    string errmsg;
    c = std::unique_ptr<DBClientBase>(uri.connect(StringData(), errmsg, socketTimeout));
    uassert(40356, _name + ": connect failed " + uri.toString() + " : " + errmsg, c);

    return _finishCreate(uri.toString(), socketTimeout, c.release());
}

int DBConnectionPool::getNumAvailableConns(const string& host, double socketTimeout) const {
    stdx::lock_guard<stdx::mutex> L(_mutex);
    auto it = _pools.find(PoolKey(host, socketTimeout));
    return (it == _pools.end()) ? 0 : it->second.numAvailable();
}

int DBConnectionPool::getNumBadConns(const string& host, double socketTimeout) const {
    stdx::lock_guard<stdx::mutex> L(_mutex);
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

    stdx::lock_guard<stdx::mutex> L(_mutex);
    _pools[PoolKey(host, c->getSoTimeout())].done(this, c);
}


DBConnectionPool::~DBConnectionPool() {
    // Do not log in destruction, because global connection pools get
    // destroyed after the logging framework.
    stdx::lock_guard<stdx::mutex> L(_mutex);
    for (PoolMap::iterator i = _pools.begin(); i != _pools.end(); i++) {
        PoolForHost& p = i->second;
        p._parentDestroyed = true;
    }
}

void DBConnectionPool::flush() {
    stdx::lock_guard<stdx::mutex> L(_mutex);
    for (PoolMap::iterator i = _pools.begin(); i != _pools.end(); i++) {
        PoolForHost& p = i->second;
        p.flush();
    }
}

void DBConnectionPool::clear() {
    stdx::lock_guard<stdx::mutex> L(_mutex);
    LOG(2) << "Removing connections on all pools owned by " << _name << endl;
    for (PoolMap::iterator iter = _pools.begin(); iter != _pools.end(); ++iter) {
        iter->second.clear();
    }
}

void DBConnectionPool::removeHost(const string& host) {
    stdx::lock_guard<stdx::mutex> L(_mutex);
    LOG(2) << "Removing connections from all pools for host: " << host << endl;
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
        stdx::lock_guard<stdx::mutex> lk(_mutex);
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
                                                   static_cast<size_t>(i->second.numCreated()),
                                                   0};
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
    if (conn == NULL) {
        return false;
    }

    if (conn->isFailed()) {
        return false;
    }

    {
        stdx::lock_guard<stdx::mutex> sl(_mutex);
        PoolForHost& pool = _pools[PoolKey(hostName, conn->getSoTimeout())];
        if (pool.isBadSocketCreationTime(conn->getSockCreationMicroSec())) {
            return false;
        }
    }

    return true;
}

void DBConnectionPool::taskDoWork() {
    vector<DBClientBase*> toDelete;

    {
        // we need to get the connections inside the lock
        // but we can actually delete them outside
        stdx::lock_guard<stdx::mutex> lk(_mutex);
        for (PoolMap::iterator i = _pools.begin(); i != _pools.end(); ++i) {
            i->second.getStaleConnections(toDelete);
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
    : _host(host), _conn(globalConnPool.get(host, socketTimeout)), _socketTimeout(socketTimeout) {
    _setSocketTimeout();
}

ScopedDbConnection::ScopedDbConnection(const ConnectionString& host, double socketTimeout)
    : _host(host.toString()),
      _conn(globalConnPool.get(host, socketTimeout)),
      _socketTimeout(socketTimeout) {
    _setSocketTimeout();
}

ScopedDbConnection::ScopedDbConnection(const MongoURI& uri, double socketTimeout)
    : _host(uri.toString()),
      _conn(globalConnPool.get(uri, socketTimeout)),
      _socketTimeout(socketTimeout) {
    _setSocketTimeout();
}

void ScopedDbConnection::done() {
    if (!_conn) {
        return;
    }

    globalConnPool.release(_host, _conn);
    _conn = NULL;
}

void ScopedDbConnection::_setSocketTimeout() {
    if (!_conn)
        return;

    if (_conn->type() == ConnectionString::MASTER)
        static_cast<DBClientConnection*>(_conn)->setSoTimeout(_socketTimeout);
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
            logNoCache() << "scoped connection to " << _conn->getServerAddress()
                         << " not being returned to the pool" << endl;
            kill();
        }
    }
}

void ScopedDbConnection::clearPool() {
    globalConnPool.clear();
}

AtomicInt32 AScopedConnection::_numConnections;

}  // namespace mongo
