// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/dbtests/mock/mock_conn_registry.h"

#include "mongo/base/init.h"  // IWYU pragma: keep
#include "mongo/base/initializer.h"
#include "mongo/dbtests/mock/mock_dbclient_connection.h"
#include "mongo/util/assert_util.h"

#include <mutex>
#include <string_view>
#include <utility>

#include <absl/container/node_hash_map.h>

namespace mongo {

using std::string;

std::unique_ptr<MockConnRegistry> MockConnRegistry::_instance;

MONGO_INITIALIZER(MockConnRegistry)(InitializerContext* context) {
    uassertStatusOK(MockConnRegistry::init());
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
    std::lock_guard<std::mutex> sl(_registryMutex);

    const std::string hostName(server->getServerAddress());
    fassert(16533, _registry.count(hostName) == 0);

    _registry[hostName] = server;
}

bool MockConnRegistry::removeServer(const std::string& hostName) {
    std::lock_guard<std::mutex> sl(_registryMutex);
    return _registry.erase(hostName) == 1;
}

MockRemoteDBServer* MockConnRegistry::getMockRemoteDBServer(const std::string& hostName) const {
    std::lock_guard lk(_registryMutex);
    auto iter = _registry.find(hostName);
    if (iter == _registry.end())
        return nullptr;

    return iter->second;
}

void MockConnRegistry::clear() {
    std::lock_guard<std::mutex> sl(_registryMutex);
    _registry.clear();
}

std::unique_ptr<MockDBClientConnection> MockConnRegistry::connect(const std::string& connStr) {
    std::lock_guard<std::mutex> sl(_registryMutex);
    fassert(16534, _registry.count(connStr) == 1);
    return std::make_unique<MockDBClientConnection>(_registry[connStr], true);
}

MockConnRegistry::MockConnHook::MockConnHook(MockConnRegistry* registry) : _registry(registry) {}

MockConnRegistry::MockConnHook::~MockConnHook() {}

std::unique_ptr<mongo::DBClientBase> MockConnRegistry::MockConnHook::connect(
    const ConnectionString& connString,
    std::string& errmsg,
    double socketTimeout,
    const ClientAPIVersionParameters* apiParameters) {
    const string hostName(connString.toString());
    auto conn = _registry->connect(hostName);

    if (!conn->connect(hostName.c_str(), std::string_view(), errmsg)) {
        // mimic ConnectionString::connect for kStandalone type connection to return NULL
        // if the destination is unreachable.
        return nullptr;
    }

    return std::move(conn);
}
}  // namespace mongo
