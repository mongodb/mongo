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

#include "mongo/platform/basic.h"

#include "mongo/client/dbclient_connection.h"

#include "mongo/base/checked_cast.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/unittest/integration_test.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

const auto sleepCmd = fromjson(R"({sleep: 1, locks: 'none', secs: 100})");
constexpr StringData kAppName = "DBClientConnectionTest"_sd;

class DBClientConnectionFixture : public unittest::Test {
public:
    static std::unique_ptr<DBClientConnection> makeConn(StringData name = kAppName) {
        auto swConn = unittest::getFixtureConnectionString().connect(name);
        uassertStatusOK(swConn.getStatus());

        auto conn = dynamic_cast<DBClientConnection*>(swConn.getValue().release());
        return std::unique_ptr<DBClientConnection>{conn};
    }

    void tearDown() override {
        // resmoke hangs if there are any ops still running on the server, so try to clean up after
        // ourselves.
        auto conn = makeConn(kAppName + "-cleanup");

        BSONObj currOp;
        if (!conn->runCommand(DatabaseName::kAdmin, BSON("currentOp" << 1), currOp))
            uassertStatusOK(getStatusFromCommandResult(currOp));

        for (auto&& op : currOp["inprog"].Obj()) {
            try {
                if (op["clientMetadata"]["application"]["name"].String() != kAppName) {
                    continue;
                }
            } catch (const DBException&) {
                // Ignore ops that don't have the right format since they can't be the one we are
                // looking for.
                continue;
            }

            // Ignore failures to clean up.
            BSONObj ignored;
            (void)conn->runCommand(
                DatabaseName::kAdmin, BSON("killOp" << 1 << "op" << op["opid"]), ignored);
        }
    }
};

TEST_F(DBClientConnectionFixture, shutdownWorksIfCalledFirst) {
    auto conn = makeConn();

    conn->shutdownAndDisallowReconnect();

    BSONObj reply;
    ASSERT_THROWS(conn->runCommand(DatabaseName::kAdmin, sleepCmd, reply),
                  ExceptionForCat<ErrorCategory::NetworkError>);  // Currently SocketException.
}

TEST_F(DBClientConnectionFixture, shutdownWorksIfRunCommandInProgress) {
    auto conn = makeConn();

    stdx::thread shutdownThread([&] {
        // Try to make this run while the connection is blocked in recv.
        sleepmillis(100);
        conn->shutdownAndDisallowReconnect();
    });
    ON_BLOCK_EXIT([&] { shutdownThread.join(); });

    BSONObj reply;
    ASSERT_THROWS(conn->runCommand(DatabaseName::kAdmin, sleepCmd, reply),
                  ExceptionForCat<ErrorCategory::NetworkError>);  // Currently HostUnreachable.
}

}  // namespace
}  // namespace mongo
