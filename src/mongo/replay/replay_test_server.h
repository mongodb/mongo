/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/bson/bsonobj.h"
#include "mongo/dbtests/mock/mock_conn_registry.h"
#include "mongo/dbtests/mock/mock_dbclient_connection.h"
#include "mongo/dbtests/mock/mock_remote_db_server.h"
#include "mongo/stdx/unordered_map.h"

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
