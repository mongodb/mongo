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

#include "mongo/base/status.h"
#include "mongo/client/client_api_version_parameters_gen.h"
#include "mongo/client/connection_string.h"
#include "mongo/client/dbclient_base.h"
#include "mongo/dbtests/mock/mock_dbclient_connection.h"
#include "mongo/dbtests/mock/mock_remote_db_server.h"
#include "mongo/stdx/mutex.h"
#include "mongo/stdx/unordered_map.h"

#include <memory>
#include <string>

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
     * @return the pointer to MockRemoteDBServer for the given hostname if available in
     * the registry. Otherwise, returns nullptr.
     */
    MockRemoteDBServer* getMockRemoteDBServer(const std::string& hostName) const;

    /**
     * Clears the registry.
     */
    void clear();

    /**
     * @return a new mocked connection to a server with the given hostName.
     */
    std::unique_ptr<MockDBClientConnection> connect(const std::string& hostName);

    /**
     * @return the hook that can be used with ConnectionString.
     */
    ConnectionString::ConnectionHook* getConnStrHook();

private:
    class MockConnHook : public ConnectionString::ConnectionHook {
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
        ~MockConnHook() override;

        std::unique_ptr<mongo::DBClientBase> connect(
            const mongo::ConnectionString& connString,
            std::string& errmsg,
            double socketTimeout,
            const ClientAPIVersionParameters* apiParameters = nullptr) override;

    private:
        MockConnRegistry* _registry;
    };

    MockConnRegistry();

    static std::unique_ptr<MockConnRegistry> _instance;

    MockConnHook _mockConnStrHook;

    // protects _registry
    mutable stdx::mutex _registryMutex;
    stdx::unordered_map<std::string, MockRemoteDBServer*> _registry;
};
}  // namespace mongo
