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

#include <string>
#include <vector>

#include "mongo/client/dbclient_connection.h"
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
    using DBClientBase::query;

    bool connect(const char* hostName, StringData applicationName, std::string& errmsg);

    Status connect(const HostAndPort& host, StringData applicationName) override {
        std::string errmsg;
        if (!connect(host.toString().c_str(), applicationName, errmsg)) {
            return {ErrorCodes::HostNotFound, errmsg};
        }
        return Status::OK();
    }

    using DBClientBase::runCommandWithTarget;
    std::pair<rpc::UniqueReply, DBClientBase*> runCommandWithTarget(OpMsgRequest request) override;

    std::unique_ptr<mongo::DBClientCursor> query(const NamespaceStringOrUUID& nsOrUuid,
                                                 mongo::Query query = mongo::Query(),
                                                 int nToReturn = 0,
                                                 int nToSkip = 0,
                                                 const mongo::BSONObj* fieldsToReturn = nullptr,
                                                 int queryOptions = 0,
                                                 int batchSize = 0) override;

    uint64_t getSockCreationMicroSec() const override;

    void insert(const std::string& ns, BSONObj obj, int flags = 0) override;

    void insert(const std::string& ns, const std::vector<BSONObj>& objList, int flags = 0) override;

    void remove(const std::string& ns, Query query, int flags = 0) override;

    //
    // Getters
    //

    mongo::ConnectionString::ConnectionType type() const override;
    bool isFailed() const override;
    double getSoTimeout() const override;
    std::string getServerAddress() const override;
    std::string toString() const override;

    //
    // Unsupported methods (defined to get rid of virtual function was hidden error)
    //

    unsigned long long query(std::function<void(mongo::DBClientCursorBatchIterator&)> f,
                             const NamespaceStringOrUUID& nsOrUuid,
                             mongo::Query query,
                             const mongo::BSONObj* fieldsToReturn = nullptr,
                             int queryOptions = 0,
                             int batchSize = 0) override;

    //
    // Unsupported methods (these are pure virtuals in the base class)
    //

    void killCursor(const NamespaceString& ns, long long cursorID) override;
    bool call(mongo::Message& toSend,
              mongo::Message& response,
              bool assertOk,
              std::string* actualServer) override;
    void say(mongo::Message& toSend,
             bool isRetry = false,
             std::string* actualServer = nullptr) override;
    bool lazySupported() const override;

private:
    void checkConnection() override;

    MockRemoteDBServer::InstanceID _remoteServerInstanceID;
    MockRemoteDBServer* _remoteServer;
    bool _isFailed;
    uint64_t _sockCreationTime;
    bool _autoReconnect;
};
}
