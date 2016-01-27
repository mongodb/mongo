/**
 *    Copyright (C) 2015 MongoDB Inc.
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

#include "mongo/platform/basic.h"

#include "mongo/client/connection_pool.h"

#include "mongo/client/connpool.h"
#include "mongo/db/auth/authorization_manager_global.h"
#include "mongo/db/auth/internal_user_auth.h"
#include "mongo/executor/network_connection_hook.h"
#include "mongo/executor/remote_command_request.h"
#include "mongo/executor/remote_command_response.h"
#include "mongo/rpc/reply_interface.h"
#include "mongo/rpc/unique_message.h"

namespace mongo {
namespace {

const Date_t kNeverTooStale = Date_t::max();

// TODO: Workaround for SERVER-19092. To be lowered back to 5 min / 30 sec once the bug is fixed
const Hours kCleanUpInterval(1);  // Note: Must be larger than kMaxConnectionAge below)
const Minutes kMaxConnectionAge(30);

}  // namespace

ConnectionPool::ConnectionPool(int messagingPortTags,
                               std::unique_ptr<executor::NetworkConnectionHook> hook)
    : _messagingPortTags(messagingPortTags), _hook(std::move(hook)) {}

ConnectionPool::ConnectionPool(int messagingPortTags)
    : ConnectionPool(messagingPortTags, nullptr) {}

ConnectionPool::~ConnectionPool() {
    cleanUpOlderThan(Date_t::max());

    invariant(_connections.empty());
    invariant(_inUseConnections.empty());
}

void ConnectionPool::cleanUpOlderThan(Date_t now) {
    stdx::lock_guard<stdx::mutex> lk(_mutex);

    HostConnectionMap::iterator hostConns = _connections.begin();
    while (hostConns != _connections.end()) {
        _cleanUpOlderThan_inlock(now, &hostConns->second);
        if (hostConns->second.empty()) {
            _connections.erase(hostConns++);
        } else {
            ++hostConns;
        }
    }
}

void ConnectionPool::_cleanUpOlderThan_inlock(Date_t now, ConnectionList* hostConns) {
    ConnectionList::iterator iter = hostConns->begin();
    while (iter != hostConns->end()) {
        if (_shouldKeepConnection(now, *iter)) {
            ++iter;
        } else {
            _destroyConnection_inlock(hostConns, iter++);
        }
    }
}

bool ConnectionPool::_shouldKeepConnection(Date_t now, const ConnectionInfo& connInfo) const {
    const Date_t expirationDate = connInfo.creationDate + kMaxConnectionAge;
    if (expirationDate <= now) {
        return false;
    }

    return true;
}

void ConnectionPool::closeAllInUseConnections() {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    for (ConnectionList::iterator iter = _inUseConnections.begin(); iter != _inUseConnections.end();
         ++iter) {
        iter->conn->port().shutdown();
    }
}

void ConnectionPool::_cleanUpStaleHosts_inlock(Date_t now) {
    if (now > _lastCleanUpTime + kCleanUpInterval) {
        for (HostLastUsedMap::iterator itr = _lastUsedHosts.begin(); itr != _lastUsedHosts.end();
             itr++) {
            if (itr->second <= _lastCleanUpTime) {
                ConnectionList connList = _connections.find(itr->first)->second;
                _cleanUpOlderThan_inlock(now, &connList);
                invariant(connList.empty());
                itr->second = kNeverTooStale;
            }
        }

        _lastCleanUpTime = now;
    }
}

ConnectionPool::ConnectionList::iterator ConnectionPool::acquireConnection(
    const HostAndPort& target, Date_t now, Milliseconds timeout) {
    stdx::unique_lock<stdx::mutex> lk(_mutex);

    // Clean up connections on stale/unused hosts
    _cleanUpStaleHosts_inlock(now);

    for (HostConnectionMap::iterator hostConns;
         (hostConns = _connections.find(target)) != _connections.end();) {
        // Clean up the requested host to remove stale/unused connections
        _cleanUpOlderThan_inlock(now, &hostConns->second);

        if (hostConns->second.empty()) {
            // prevent host from causing unnecessary cleanups
            _lastUsedHosts[hostConns->first] = kNeverTooStale;
            break;
        }

        _inUseConnections.splice(
            _inUseConnections.begin(), hostConns->second, hostConns->second.begin());

        const ConnectionList::iterator candidate = _inUseConnections.begin();
        lk.unlock();

        try {
            if (candidate->conn->isStillConnected()) {
                // setSoTimeout takes a double representing the number of seconds for send and
                // receive timeouts.  Thus, we must express 'timeout' in milliseconds and divide by
                // 1000.0 to get the number of seconds with a fractional part.
                candidate->conn->setSoTimeout(durationCount<Milliseconds>(timeout) / 1000.0);
                return candidate;
            }
        } catch (...) {
            lk.lock();
            _destroyConnection_inlock(&_inUseConnections, candidate);
            throw;
        }

        lk.lock();
        _destroyConnection_inlock(&_inUseConnections, candidate);
    }

    // No idle connection in the pool; make a new one.
    lk.unlock();

    std::unique_ptr<DBClientConnection> conn;
    if (_hook) {
        conn.reset(new DBClientConnection(
            false,  // auto reconnect
            0,      // socket timeout
            [this, target](const executor::RemoteCommandResponse& isMasterReply) {
                return _hook->validateHost(target, isMasterReply);
            }));
    } else {
        conn.reset(new DBClientConnection());
    }

    // setSoTimeout takes a double representing the number of seconds for send and receive
    // timeouts.  Thus, we must express 'timeout' in milliseconds and divide by 1000.0 to get
    // the number of seconds with a fractional part.
    conn->setSoTimeout(durationCount<Milliseconds>(timeout) / 1000.0);

    uassertStatusOK(conn->connect(target));
    conn->port().setTag(conn->port().getTag() | _messagingPortTags);

    if (isInternalAuthSet()) {
        conn->auth(getInternalUserAuthParamsWithFallback());
    }

    if (_hook) {
        auto postConnectRequest = uassertStatusOK(_hook->makeRequest(target));

        // We might not have a postConnectRequest
        if (postConnectRequest != boost::none) {
            auto start = Date_t::now();
            auto reply =
                conn->runCommandWithMetadata(postConnectRequest->dbname,
                                             postConnectRequest->cmdObj.firstElementFieldName(),
                                             postConnectRequest->metadata,
                                             postConnectRequest->cmdObj);

            auto rcr = executor::RemoteCommandResponse(reply->getCommandReply().getOwned(),
                                                       reply->getMetadata().getOwned(),
                                                       Date_t::now() - start);

            uassertStatusOK(_hook->handleReply(target, std::move(rcr)));
        }
    }

    lk.lock();
    return _inUseConnections.insert(_inUseConnections.begin(), ConnectionInfo(conn.release(), now));
}

void ConnectionPool::releaseConnection(ConnectionList::iterator iter, const Date_t now) {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    if (!_shouldKeepConnection(now, *iter)) {
        _destroyConnection_inlock(&_inUseConnections, iter);
        return;
    }

    ConnectionList& hostConns = _connections[iter->conn->getServerHostAndPort()];
    _cleanUpOlderThan_inlock(now, &hostConns);

    hostConns.splice(hostConns.begin(), _inUseConnections, iter);
    _lastUsedHosts[iter->conn->getServerHostAndPort()] = now;
}

void ConnectionPool::destroyConnection(ConnectionList::iterator iter) {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    _destroyConnection_inlock(&_inUseConnections, iter);
}

void ConnectionPool::_destroyConnection_inlock(ConnectionList* connList,
                                               ConnectionList::iterator iter) {
    delete iter->conn;
    connList->erase(iter);
}


//
// ConnectionPool::ConnectionPtr
//

ConnectionPool::ConnectionPtr::ConnectionPtr(ConnectionPool* pool,
                                             const HostAndPort& target,
                                             Date_t now,
                                             Milliseconds timeout)
    : _pool(pool), _connInfo(pool->acquireConnection(target, now, timeout)) {}

ConnectionPool::ConnectionPtr::~ConnectionPtr() {
    if (_pool) {
        _pool->destroyConnection(_connInfo);
    }
}

ConnectionPool::ConnectionPtr::ConnectionPtr(ConnectionPtr&& other)
    : _pool(std::move(other._pool)), _connInfo(std::move(other._connInfo)) {
    other._pool = nullptr;
}

ConnectionPool::ConnectionPtr& ConnectionPool::ConnectionPtr::operator=(ConnectionPtr&& other) {
    _pool = std::move(other._pool);
    _connInfo = std::move(other._connInfo);
    other._pool = nullptr;
    return *this;
}

void ConnectionPool::ConnectionPtr::done(Date_t now) {
    _pool->releaseConnection(_connInfo, now);
    _pool = NULL;
}

}  // namespace mongo
