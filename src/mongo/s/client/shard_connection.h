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

#include <string>

#include "mongo/client/connpool.h"
#include "mongo/s/chunk_version.h"

namespace mongo {

    class ChunkManager;
    typedef std::shared_ptr<ChunkManager> ChunkManagerPtr;

    
    class ShardConnection : public AScopedConnection {
    public:
        ShardConnection(const ConnectionString& connectionString,
                        const std::string& ns,
                        std::shared_ptr<ChunkManager> manager = nullptr);

        ~ShardConnection();

        void done();
        void kill();

        DBClientBase& conn() {
            _finishInit();
            verify( _conn );
            return *_conn;
        }

        DBClientBase* operator->() {
            _finishInit();
            verify( _conn );
            return _conn;
        }

        DBClientBase* get() {
            _finishInit();
            verify( _conn );
            return _conn;
        }

        /**
         * @return the connection object underneath without setting the shard version.
         * @throws AssertionException if _conn is uninitialized.
         */
        DBClientBase* getRawConn() const {
            verify( _conn );
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

        static void sync();

        void donotCheckVersion() {
            _setVersion = false;
            _finishedInit = true;
        }
        
        bool ok() const { return _conn != NULL; }

        /** checks all of my thread local connections for the version of this ns */
        static void checkMyConnectionVersions( const std::string & ns );

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
        static void forgetNS( const std::string& ns );

    private:
        void _init();
        void _finishInit();

        const ConnectionString _cs;
        const std::string _ns;

        std::shared_ptr<ChunkManager> _manager;

        bool _finishedInit;

        DBClientBase* _conn;
        bool _setVersion;
    };


    /**
     * Sends the setShardVersion command on the specified connection.
     */
    bool setShardVersion(DBClientBase & conn,
                         const std::string& ns,
                         const std::string& configServerPrimary,
                         ChunkVersion version,
                         ChunkManager* manager,
                         bool authoritative,
                         BSONObj& result);


    typedef std::shared_ptr<ShardConnection> ShardConnectionPtr;

    extern DBConnectionPool shardConnectionPool;

} // namespace mongo
