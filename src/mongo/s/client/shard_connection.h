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

#include "mongo/client/connpool.h"

namespace mongo {

class ChunkManager;

class ShardConnection : public AScopedConnection {
public:
    /**
     * Instantiates a new sharded connection, which will be associated with the specified chunk
     * manager for versioning purposes.
     *
     * @param connectionString Connection string for the host to use.
     * @param ns Namespace to associate the version with.
     * @param manager The chunk manager, which should be used for obtaining shard version
     *  information to be set on the connection. This value can be nullptr, if the connection will
     *  not be versioned and not associated with a namespace, in which case only the
     *  setShardVersion command will be invoked to initialize the remote shard. Otherwise, the
     *  chunk manager will be used to obtain the shard version to set on the connection.
     */
    ShardConnection(OperationContext* opCtx,
                    const ConnectionString& connectionString,
                    const std::string& ns,
                    std::shared_ptr<ChunkManager> manager = nullptr);

    ~ShardConnection();

    void done();
    void kill();

    DBClientBase& conn() {
        _finishInit();
        verify(_conn);
        return *_conn;
    }

    DBClientBase* operator->() {
        _finishInit();
        verify(_conn);
        return _conn;
    }

    DBClientBase* get() {
        _finishInit();
        verify(_conn);
        return _conn;
    }

    /**
     * @return the connection object underneath without setting the shard version.
     * @throws AssertionException if _conn is uninitialized.
     */
    DBClientBase* getRawConn() const {
        verify(_conn);
        return _conn;
    }

    std::string getHost() const {
        return _cs.toString();
    }

    std::string getNS() const {
        return _ns;
    }

    std::shared_ptr<ChunkManager> getManager() const {
        return _manager;
    }

    bool setVersion() {
        _finishInit();
        return _setVersion;
    }

    void donotCheckVersion() {
        invariant(!_finishedInit);
        _finishedInit = true;
        _setVersion = false;
    }

    bool ok() const {
        return _conn != NULL;
    }

    /** reports all thread local connections on this instance */
    static void reportActiveClientConnections(BSONObjBuilder* builder);

    /** checks all of my thread local connections for the version of this ns */
    static void checkMyConnectionVersions(OperationContext* opCtx, const std::string& ns);

    /**
     * Returns all the current sharded connections to the pool.
     * Note: This is *dangerous* if we have GLE state.
     */
    static void releaseMyConnections();

    /**
     * Clears all connections in the sharded pool, including connections in the
     * thread local storage pool of the current thread.
     */
    static void clearPool();

    /**
     * Forgets a namespace to prevent future versioning.
     */
    static void forgetNS(const std::string& ns);

private:
    void _finishInit();

    const ConnectionString _cs;
    const std::string _ns;
    const std::shared_ptr<ChunkManager> _manager;

    bool _finishedInit{false};

    // Specifies whether the call to _finishInit established a version on the connection
    bool _setVersion{false};

    DBClientBase* _conn;
};

/**
 * Global sharded connection pool, used by all instances of ShardConnection.
 */
extern DBConnectionPool shardConnectionPool;

}  // namespace mongo
