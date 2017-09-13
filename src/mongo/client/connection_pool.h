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

#pragma once

#include <map>

#include "mongo/base/disallow_copying.h"
#include "mongo/client/dbclientinterface.h"
#include "mongo/platform/unordered_map.h"
#include "mongo/stdx/list.h"
#include "mongo/stdx/mutex.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/time_support.h"

namespace mongo {
namespace executor {
class NetworkConnectionHook;
}

/**
 * Represents a pool of connections to a MongoDB server. The pool is synchronized internally
 * and its methods can be called from multiple threads, however once a connection is obtained
 * it should only be used from one thread in accordance with the rules of DBClientBase.
 */
class ConnectionPool {
    MONGO_DISALLOW_COPYING(ConnectionPool);

public:
    /**
     * Information about a connection in the pool.
     */
    struct ConnectionInfo {
        ConnectionInfo(DBClientConnection* theConn, Date_t date)
            : conn(theConn), creationDate(date) {}

        // A connection in the pool.
        DBClientConnection* const conn;

        // The date at which the connection was created.
        const Date_t creationDate;
    };

    typedef stdx::list<ConnectionInfo> ConnectionList;
    typedef unordered_map<HostAndPort, ConnectionList> HostConnectionMap;
    typedef std::map<HostAndPort, Date_t> HostLastUsedMap;

    /**
     * RAII class for connections from the pool.  To use the connection pool, instantiate one of
     * these with a pointer to the pool, the identity of the target node and the timeout for
     * network operations, use it like a pointer to a connection, and then call done() on
     * successful completion.  Failure to call done() will lead to the connection being reaped
     * when the holder goes out of scope.
     */
    class ConnectionPtr {
        MONGO_DISALLOW_COPYING(ConnectionPtr);

    public:
        /**
         * Constructs a ConnectionPtr referring to a connection to "target" drawn from "pool",
         * with the network timeout set to "timeout".
         *
         * Throws DBExceptions if the connection cannot be established.
         */
        ConnectionPtr(ConnectionPool* pool,
                      const HostAndPort& target,
                      Date_t now,
                      Milliseconds timeout);

        /**
         * Destructor reaps the connection if it wasn't already returned to the pool by calling
         * done().
         */
        ~ConnectionPtr();

        // We need to provide user defined move operations as we need to set the pool
        // pointer to nullptr on the moved-from object.
        ConnectionPtr(ConnectionPtr&&);

        ConnectionPtr& operator=(ConnectionPtr&&);

        /**
         * Obtains the underlying connection which can be used for making calls to the server.
         */
        DBClientConnection* get() const {
            return _connInfo->conn;
        }

        /**
         * Releases the connection back to the pool from which it was drawn.
         */
        void done(Date_t now);

    private:
        ConnectionPool* _pool;
        ConnectionList::iterator _connInfo;
    };

    /**
     * Instantiates a new connection pool with the specified tags to be applied to the
     * messaging ports that it opens.
     *
     * @param messagingPortTags tags to be applied to the messaging ports for each of the
     *      connections. If no tags are required, use 0.
     */
    ConnectionPool(int messagingPortTags, std::unique_ptr<executor::NetworkConnectionHook> hook);
    ConnectionPool(int messagingPortTags);
    ~ConnectionPool();

    /**
     * Acquires a connection to "target" with the given "timeout", or throws a DBException.
     * Intended for use by ConnectionPtr.
     */
    ConnectionList::iterator acquireConnection(const HostAndPort& target,
                                               Date_t now,
                                               Milliseconds timeout);

    /**
     * Releases a connection back into the pool.
     * Intended for use by ConnectionPtr.
     * Call this for connections that can safely be reused.
     */
    void releaseConnection(ConnectionList::iterator iter, Date_t now);

    /**
     * Destroys a connection previously acquired from the pool.
     * Intended for use by ConnectionPtr.
     * Call this for connections that cannot be reused.
     */
    void destroyConnection(ConnectionList::iterator);

    /**
     * Closes all connections currently in use, to ensure that the network threads
     * terminate promptly during shutdown.
     */
    void closeAllInUseConnections();

    /**
     * Reaps all connections in the pool that are too old as of "now".
     */
    void cleanUpOlderThan(Date_t now);

private:
    /**
     * Returns true if the given connection is young enough to keep in the pool.
     */
    bool _shouldKeepConnection(Date_t now, const ConnectionInfo& connInfo) const;

    /**
     * Apply cleanup policy to any host(s) not active in the last kCleanupInterval milliseconds.
     */
    void _cleanUpStaleHosts_inlock(Date_t now);

    /**
     * Reaps connections in "hostConns" that are too old or have been in the pool too long as of
     * "now".  Expects _mutex to be held.
     */
    void _cleanUpOlderThan_inlock(Date_t now, ConnectionList* hostConns);

    /**
     * Destroys the connection associated with "iter" and removes "iter" fron connList.
     */
    static void _destroyConnection_inlock(ConnectionList* connList, ConnectionList::iterator iter);


    // Flags to apply to the opened connections
    const int _messagingPortTags;

    // Mutex guarding members of the connection pool
    stdx::mutex _mutex;

    // Map from HostAndPort to idle connections.
    HostConnectionMap _connections;

    // List of non-idle connections.
    ConnectionList _inUseConnections;

    // Map of HostAndPorts to when they were last used.
    HostLastUsedMap _lastUsedHosts;

    // Time representing when the connections were last cleaned.
    Date_t _lastCleanUpTime;

    // The connection hook for this pool. May be nullptr if there is no hook.
    const std::unique_ptr<executor::NetworkConnectionHook> _hook;
};

}  // namespace mongo
