// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/client/client_api_version_parameters_gen.h"
#include "mongo/client/connection_string.h"
#include "mongo/client/dbclient_base.h"
#include "mongo/dbtests/mock/mock_dbclient_connection.h"
#include "mongo/dbtests/mock/mock_remote_db_server.h"
#include "mongo/stdx/unordered_map.h"
#include "mongo/util/modules.h"

#include <memory>
#include <mutex>
#include <string>

namespace mongo {
/**
 * Registry for storing mock servers and can create mock connections to these
 * servers.
 */
class [[MONGO_MOD_PUBLIC]] MockConnRegistry {
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
    mutable std::mutex _registryMutex;
    stdx::unordered_map<std::string, MockRemoteDBServer*> _registry;
};
}  // namespace mongo
