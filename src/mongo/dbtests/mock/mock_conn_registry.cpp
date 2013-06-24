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

#include "mongo/dbtests/mock/mock_conn_registry.h"

#include "mongo/base/init.h"
#include "mongo/dbtests/mock/mock_dbclient_connection.h"

namespace mongo {
    boost::scoped_ptr<MockConnRegistry> MockConnRegistry::_instance;

    MONGO_INITIALIZER(MockConnRegistry)(InitializerContext* context) {
        return MockConnRegistry::init();
    }

    Status MockConnRegistry::init() {
        MockConnRegistry::_instance.reset(new MockConnRegistry());
        return Status::OK();
    }

    MockConnRegistry::MockConnRegistry():
            _mockConnStrHook(this),
            _registryMutex("mockConnRegistryMutex") {
    }

    MockConnRegistry* MockConnRegistry::get() {
        return _instance.get();
    }

    ConnectionString::ConnectionHook* MockConnRegistry::getConnStrHook() {
        return &_mockConnStrHook;
    }

    void MockConnRegistry::addServer(MockRemoteDBServer* server) {
        scoped_lock sl(_registryMutex);

        const std::string hostName(server->getServerAddress());
        fassert(16533, _registry.count(hostName) == 0);

        _registry[hostName] = server;
    }

    bool MockConnRegistry::removeServer(const std::string& hostName) {
        scoped_lock sl(_registryMutex);
        return _registry.erase(hostName) == 1;
    }

    void MockConnRegistry::clear() {
        scoped_lock sl(_registryMutex);
        _registry.clear();
    }

    MockDBClientConnection* MockConnRegistry::connect(const std::string& connStr) {
        scoped_lock sl(_registryMutex);
        fassert(16534, _registry.count(connStr) == 1);
        return new MockDBClientConnection(_registry[connStr], true);
    }

    MockConnRegistry::MockConnHook::MockConnHook(MockConnRegistry* registry):
            _registry(registry) {
    }

    MockConnRegistry::MockConnHook::~MockConnHook() {
    }

    mongo::DBClientBase* MockConnRegistry::MockConnHook::connect(
                const ConnectionString& connString,
                std::string& errmsg,
                double socketTimeout) {
        const string hostName(connString.toString());
        MockDBClientConnection* conn = _registry->connect(hostName);

        if (!conn->connect(hostName.c_str(), errmsg)) {
            // Assumption: connect never throws, so no leak.
            delete conn;

            // mimic ConnectionString::connect for MASTER type connection to return NULL
            // if the destination is unreachable.
            return NULL;
        }

        return conn;
    }
}
