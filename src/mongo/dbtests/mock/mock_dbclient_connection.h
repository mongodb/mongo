/*    Copyright 2012 10gen Inc.
 *
 *    Licensed under the Apache License, Version 2.0 (the "License");
 *    you may not use this file except in compliance with the License.
 *    You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing, software
 *    distributed under the License is distributed on an "AS IS" BASIS,
 *    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *    See the License for the specific language governing permissions and
 *    limitations under the License.
 */

#pragma once

#include <boost/shared_ptr.hpp>
#include <string>
#include <vector>

#include "mongo/client/dbclientinterface.h"
#include "mongo/dbtests/mock/mock_remote_db_server.h"

namespace mongo {
    /**
     * A simple class for mocking mongo::DBClientConnection.
     *
     * Also check out sample usage in dbtests/mock_dbclient_conn_test.cpp
     */
    class MockDBClientConnection : public mongo::DBClientConnection {
    public:
        /**
         * Create a mock connection to a mock server.
         *
         * @param remoteServer the remote server to connect to. The caller is
         *     responsible for making sure that the life of remoteServer is
         *     longer than this connection.
         * @param autoReconnect will automatically re-establish connection the
         *     next time an operation is requested when the last operation caused
         *     this connection to fall into a failed state.
         */
        MockDBClientConnection(MockRemoteDBServer* remoteServer, bool autoReconnect = false);
        virtual ~MockDBClientConnection();

        //
        // DBClientBase methods
        //

        bool connect(const char* hostName, std::string& errmsg);

        inline bool connect(const HostAndPort& host, std::string& errmsg) {
            return connect(host.toString().c_str(), errmsg);
        }

        bool runCommand(const std::string& dbname, const mongo::BSONObj& cmdObj,
                mongo::BSONObj &info, int options = 0);

        std::auto_ptr<mongo::DBClientCursor> query(const std::string &ns,
                mongo::Query query = mongo::Query(),
                int nToReturn = 0,
                int nToSkip = 0,
                const mongo::BSONObj* fieldsToReturn = 0,
                int queryOptions = 0,
                int batchSize = 0);

        uint64_t getSockCreationMicroSec() const;

        virtual void insert(const string& ns, BSONObj obj, int flags = 0);

        virtual void insert(const string& ns, const vector<BSONObj>& objList, int flags = 0);

        virtual void remove(const string& ns, Query query, bool justOne = false);

        virtual void remove(const string& ns, Query query, int flags = 0);

        //
        // Getters
        //

        mongo::ConnectionString::ConnectionType type() const;
        bool isFailed() const;
        double getSoTimeout() const;
        std::string getServerAddress() const;
        std::string toString();

        //
        // Unsupported methods (defined to get rid of virtual function was hidden error)
        //
        unsigned long long query(boost::function<void(const mongo::BSONObj&)> f,
                const std::string& ns, mongo::Query query,
                const mongo::BSONObj* fieldsToReturn = 0, int queryOptions = 0);

        unsigned long long query(boost::function<void(mongo::DBClientCursorBatchIterator&)> f,
                const std::string& ns, mongo::Query query,
                const mongo::BSONObj* fieldsToReturn = 0,
                int queryOptions = 0);

        //
        // Unsupported methods (these are pure virtuals in the base class)
        //

        void killCursor(long long cursorID);
        bool callRead(mongo::Message& toSend , mongo::Message& response);
        bool call(mongo::Message& toSend, mongo::Message& response, bool assertOk = true,
                std::string* actualServer = 0);
        void say(mongo::Message& toSend, bool isRetry = false, std::string* actualServer = 0);
        void sayPiggyBack(mongo::Message& toSend);
        bool lazySupported() const;

    private:
        void checkConnection();

        MockRemoteDBServer::InstanceID _remoteServerInstanceID;
        MockRemoteDBServer* _remoteServer;
        bool _isFailed;
        uint64_t _sockCreationTime;
        bool _autoReconnect;
    };
}
