/*
 *    Copyright (C) 2012 10gen Inc.
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
 */

#pragma once

#include "mongo/base/status.h"
#include "mongo/client/dbclientinterface.h"
#include "mongo/dbtests/mock/mock_dbclient_connection.h"
#include "mongo/dbtests/mock/mock_remote_db_server.h"
#include "mongo/platform/unordered_map.h"
#include "mongo/util/concurrency/mutex.h"

namespace mongo {
    /**
     * Registry for storing mock servers and can create mock connections to these
     * servers.
     */
    class MockConnRegistry {
    public:
        /**
         * Initializes the static instance.
         */
        static Status init();

        /**
         * @return the singleton registry. If this needs to be called before main(),
         *     then the initializer method should depend on "MockConnRegistry".
         */
        static MockConnRegistry* get();

        /**
         * Adds a server to this registry.
         *
         * @param server the server to add. Caller is responsible for disposing
         *     the server.
         */
        void addServer(MockRemoteDBServer* server);

        /**
         * Removes the server from this registry.
         *
         * @param hostName the host name of the server to remove.
         *
         * @return true if the server is in the registry and was removed.
         */
        bool removeServer(const std::string& hostName);

        /**
         * Clears the registry.
         */
        void clear();

        /**
         * @return a new mocked connection to a server with the given hostName.
         */
        MockDBClientConnection* connect(const std::string& hostName);

        /**
         * @return the hook that can be used with ConnectionString.
         */
        ConnectionString::ConnectionHook* getConnStrHook();

    private:
        class MockConnHook: public ConnectionString::ConnectionHook {
        public:
            /**
             * Creates a new connection hook for the ConnectionString class that
             * can create mock connections to mock replica set members using their
             * pseudo host names.
             *
             * @param replSet the mock replica set. Caller is responsible for managing
             *     replSet and making sure that it lives longer than this object.
             */
            MockConnHook(MockConnRegistry* registry);
            ~MockConnHook();

            mongo::DBClientBase* connect(
                    const mongo::ConnectionString& connString,
                    std::string& errmsg, double socketTimeout);

        private:
            MockConnRegistry* _registry;
        };

        MockConnRegistry();

        static boost::scoped_ptr<MockConnRegistry> _instance;

        MockConnHook _mockConnStrHook;

        // protects _registry
        mongo::mutex _registryMutex;
        unordered_map<std::string, MockRemoteDBServer*> _registry;
    };
}
