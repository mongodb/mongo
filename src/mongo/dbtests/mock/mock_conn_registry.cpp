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

#include "mongo/dbtests/mock/mock_conn_registry.h"

#include "mongo/base/init.h"
#include "mongo/dbtests/mock/mock_dbclient_connection.h"

namespace mongo {

using std::string;

std::unique_ptr<MockConnRegistry> MockConnRegistry::_instance;

MONGO_INITIALIZER(MockConnRegistry)(InitializerContext* context) {
    return MockConnRegistry::init();
}

Status MockConnRegistry::init() {
    MockConnRegistry::_instance.reset(new MockConnRegistry());
    return Status::OK();
}

MockConnRegistry::MockConnRegistry() : _mockConnStrHook(this) {}

MockConnRegistry* MockConnRegistry::get() {
    return _instance.get();
}

ConnectionString::ConnectionHook* MockConnRegistry::getConnStrHook() {
    return &_mockConnStrHook;
}

void MockConnRegistry::addServer(MockRemoteDBServer* server) {
    stdx::lock_guard<stdx::mutex> sl(_registryMutex);

    const std::string hostName(server->getServerAddress());
    fassert(16533, _registry.count(hostName) == 0);

    _registry[hostName] = server;
}

bool MockConnRegistry::removeServer(const std::string& hostName) {
    stdx::lock_guard<stdx::mutex> sl(_registryMutex);
    return _registry.erase(hostName) == 1;
}

void MockConnRegistry::clear() {
    stdx::lock_guard<stdx::mutex> sl(_registryMutex);
    _registry.clear();
}

MockDBClientConnection* MockConnRegistry::connect(const std::string& connStr) {
    stdx::lock_guard<stdx::mutex> sl(_registryMutex);
    fassert(16534, _registry.count(connStr) == 1);
    return new MockDBClientConnection(_registry[connStr], true);
}

MockConnRegistry::MockConnHook::MockConnHook(MockConnRegistry* registry) : _registry(registry) {}

MockConnRegistry::MockConnHook::~MockConnHook() {}

mongo::DBClientBase* MockConnRegistry::MockConnHook::connect(const ConnectionString& connString,
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
