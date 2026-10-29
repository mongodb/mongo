// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/dbtests/mock/mock_conn_registry.h"
#include "mongo/dbtests/mock/mock_dbclient_connection.h"
#include "mongo/dbtests/mock/mock_remote_db_server.h"
#include "mongo/stdx/unordered_map.h"
#include "mongo/util/modules.h"

#include <memory>
#include <string>

namespace mongo {
class ReplayTestServer {
public:
    explicit ReplayTestServer(std::string = "$local:12345");
    explicit ReplayTestServer(const std::vector<std::string>&,
                              const std::vector<std::string>&,
                              std::string = "$local:12345");
    ~ReplayTestServer();
    const std::string& getFakeResponse(const std::string&) const;
    std::string getConnectionString() const;
    void setupServerResponse(const std::string& name, const std::string& response);
    bool checkResponse(const std::string& name, const BSONObj& response) const;

private:
    void setUp();
    void tearDown();

    std::string _hostName;
    std::unique_ptr<MockRemoteDBServer> _mockServer;
    int _originalSeverity;
    stdx::unordered_map<std::string, std::string> _fakeResponseMap;
};

}  // namespace mongo
