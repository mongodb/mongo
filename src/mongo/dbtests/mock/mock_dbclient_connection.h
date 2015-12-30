/*    Copyright 2012 10gen Inc.
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

    bool runCommand(const std::string& dbname,
                    const mongo::BSONObj& cmdObj,
                    mongo::BSONObj& info,
                    int options = 0);

    rpc::UniqueReply runCommandWithMetadata(StringData database,
                                            StringData command,
                                            const BSONObj& metadata,
                                            const BSONObj& commandArgs) final;

    std::unique_ptr<mongo::DBClientCursor> query(const std::string& ns,
                                                 mongo::Query query = mongo::Query(),
                                                 int nToReturn = 0,
                                                 int nToSkip = 0,
                                                 const mongo::BSONObj* fieldsToReturn = 0,
                                                 int queryOptions = 0,
                                                 int batchSize = 0);

    uint64_t getSockCreationMicroSec() const;

    virtual void insert(const std::string& ns, BSONObj obj, int flags = 0);

    virtual void insert(const std::string& ns, const std::vector<BSONObj>& objList, int flags = 0);

    virtual void remove(const std::string& ns, Query query, int flags = 0);

    //
    // Getters
    //

    mongo::ConnectionString::ConnectionType type() const;
    bool isFailed() const;
    double getSoTimeout() const;
    std::string getServerAddress() const;
    std::string toString() const;

    //
    // Unsupported methods (defined to get rid of virtual function was hidden error)
    //
    unsigned long long query(stdx::function<void(const mongo::BSONObj&)> f,
                             const std::string& ns,
                             mongo::Query query,
                             const mongo::BSONObj* fieldsToReturn = 0,
                             int queryOptions = 0);

    unsigned long long query(stdx::function<void(mongo::DBClientCursorBatchIterator&)> f,
                             const std::string& ns,
                             mongo::Query query,
                             const mongo::BSONObj* fieldsToReturn = 0,
                             int queryOptions = 0);

    //
    // Unsupported methods (these are pure virtuals in the base class)
    //

    void killCursor(long long cursorID);
    bool call(mongo::Message& toSend,
              mongo::Message& response,
              bool assertOk,
              std::string* actualServer);
    void say(mongo::Message& toSend, bool isRetry = false, std::string* actualServer = 0);
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
